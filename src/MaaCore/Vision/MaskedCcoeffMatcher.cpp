#include "MaskedCcoeffMatcher.h"

#include "MaaUtils/NoWarningCV.hpp"

#include "Utils/Logger.hpp"

#include <array>
#include <vector>

namespace asst
{
namespace
{
// 稀疏路径使用的每个有效 mask 像素的条目
struct SparseEntry
{
    int16_t dx, dy;   // 相对模板左上角的偏移
    float T_prime[3]; // M*(T_c - μT_c)，per-channel
};

struct FftWorkspace
{
    cv::Mat padded;
    cv::Mat image_dft;
    cv::Mat product_spectrum;
    cv::Mat result_buf;
    cv::Mat sum_MI_buf;
    cv::Mat sum_MI2_buf;
    cv::Mat src_d;

    bool prepare(int dft_rows, int dft_cols, int result_rows, int result_cols, cv::Size image_size)
    {
        if (padded.rows == dft_rows && padded.cols == dft_cols && sum_MI_buf.rows == result_rows &&
            sum_MI_buf.cols == result_cols && src_d.size() == image_size) {
            return false;
        }
        padded.create(dft_rows, dft_cols, CV_64F);
        image_dft.create(dft_rows, dft_cols, CV_64F);
        product_spectrum.create(dft_rows, dft_cols, CV_64F);
        result_buf.create(dft_rows, dft_cols, CV_64F);
        sum_MI_buf.create(result_rows, result_cols, CV_64F);
        sum_MI2_buf.create(result_rows, result_cols, CV_64F);
        src_d.create(image_size, CV_64F);
        return true;
    }
};
}

struct MaskedCcoeffMatcher::TemplatePlan
{
    cv::Mat M;                      // CV_32F mask, 0 or 1
    std::array<cv::Mat, 3> T_prime; // M*(T_c - μT_c)，per-channel
    uint64_t cache_revision = 0;
    double sigma_T_sq = 0.0;
    double mask_area = 0.0;
    std::vector<SparseEntry> sparse_entries; // 非零 mask 位置列表
    int K = 0;                               // sparse_entries.size()
};

struct MaskedCcoeffMatcher::TemplateSpectrum
{
    cv::Mat M_dft;
    std::array<cv::Mat, 3> T_prime_dft;
};

MaskedCcoeffMatcher& MaskedCcoeffMatcher::get_instance()
{
    static MaskedCcoeffMatcher instance;
    return instance;
}

void MaskedCcoeffMatcher::sync_cache_revision(const uint64_t revision)
{
    if (m_cache_revision.load(std::memory_order_acquire) == revision) {
        return;
    }
    std::lock_guard lk(m_cache_mtx);
    if (m_cache_revision.load(std::memory_order_relaxed) == revision) {
        return;
    }
    // 清一下缓存
    m_template_plan_cache.clear();
    m_lru_list.clear();
    m_cache_total_bytes = 0;
    m_spectrum_cache.clear();
    m_spectrum_lru_list.clear();
    m_spectrum_cache_total_bytes = 0;
#if ASST_MASKED_MATCHER_STATS
    m_spectrum_cache_bytes.store(0, std::memory_order_relaxed);
#endif
    m_cache_revision.store(revision, std::memory_order_release);
}

MaskedCcoeffMatcher::Stats MaskedCcoeffMatcher::stats() const
{
    return {
        .opencv_fallbacks = m_opencv_fallbacks.load(std::memory_order_relaxed),
        .sparse_calls = m_sparse_calls.load(std::memory_order_relaxed),
        .fft_calls = m_fft_calls.load(std::memory_order_relaxed),
        .spectrum_cache_hits = m_spectrum_cache_hits.load(std::memory_order_relaxed),
        .spectrum_cache_misses = m_spectrum_cache_misses.load(std::memory_order_relaxed),
        .workspace_rebuilds = m_workspace_rebuilds.load(std::memory_order_relaxed),
        .spectrum_cache_bytes = m_spectrum_cache_bytes.load(std::memory_order_relaxed),
        .image_preparations = m_image_preparations.load(std::memory_order_relaxed),
    };
}

void MaskedCcoeffMatcher::reset_stats()
{
    m_opencv_fallbacks.store(0, std::memory_order_relaxed);
    m_sparse_calls.store(0, std::memory_order_relaxed);
    m_fft_calls.store(0, std::memory_order_relaxed);
    m_spectrum_cache_hits.store(0, std::memory_order_relaxed);
    m_spectrum_cache_misses.store(0, std::memory_order_relaxed);
    m_workspace_rebuilds.store(0, std::memory_order_relaxed);
    m_image_preparations.store(0, std::memory_order_relaxed);
}

std::optional<MaskedCcoeffMatcher::PreparedImage> MaskedCcoeffMatcher::prepare_image(const cv::Mat& image_rgb)
{
    if (image_rgb.empty() || image_rgb.channels() != 3) {
        return std::nullopt;
    }
    PreparedImage result;
    result.size = image_rgb.size();
    cv::Mat image_f32;
    image_rgb.convertTo(image_f32, CV_32F);
    cv::split(image_f32, result.channels.data());
#if ASST_MASKED_MATCHER_STATS
    m_image_preparations.fetch_add(1, std::memory_order_relaxed);
#endif
    return result;
}

void MaskedCcoeffMatcher::fnv1a_update(uint64_t& h, const void* data, size_t size)
{
    const auto* ptr = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= ptr[i];
        h *= 1'099'511'628'211ULL;
    }
}

uint64_t MaskedCcoeffMatcher::hash_mat(const cv::Mat& mat)
{
    uint64_t hash = 14'695'981'039'346'656'037ULL;
    const int meta[] = { mat.rows, mat.cols, mat.type() };
    fnv1a_update(hash, meta, sizeof(meta));

    const size_t row_bytes = static_cast<size_t>(mat.cols) * mat.elemSize();
    for (int y = 0; y < mat.rows; ++y) {
        fnv1a_update(hash, mat.ptr(y), row_bytes);
    }
    return hash;
}

MaskedCcoeffMatcher::TemplateKey
    MaskedCcoeffMatcher::make_template_key(const cv::Mat& templ_rgb, const cv::Mat& mask_u8, uint64_t revision)
{
    TemplateKey key;
    key.revision = revision;
    key.template_hash = hash_mat(templ_rgb);
    key.mask_hash = hash_mat(mask_u8);
    key.template_rows = templ_rgb.rows;
    key.template_cols = templ_rgb.cols;
    key.template_type = templ_rgb.type();
    key.mask_rows = mask_u8.rows;
    key.mask_cols = mask_u8.cols;
    key.mask_type = mask_u8.type();
    return key;
}

bool MaskedCcoeffMatcher::is_binary_mask(const cv::Mat& mask_u8)
{
    // TemplatePlan 用 T' = M*(T - μT) 表示中心化模板，只有当 M ∈ {0, 1} 时
    // Σ(T')² 才等于 Σ M*(T - μT)²；稀疏路径也只覆盖 M > 0.5 的像素。
    // 非二值 mask 会让权重和与稀疏支撑集互相矛盾，因此直接拒绝。
    cv::Mat binary;
    cv::compare(mask_u8, 0, binary, cv::CMP_NE); // 非零 -> 255
    cv::Mat mismatch;
    cv::compare(binary, mask_u8, mismatch, cv::CMP_NE);
    return cv::countNonZero(mismatch) == 0;
}

size_t MaskedCcoeffMatcher::TemplateKeyHash::operator()(const TemplateKey& key) const noexcept
{
    const auto hash_u64 = std::hash<uint64_t> {};
    const auto hash_int = std::hash<int> {};
    size_t result = hash_u64(key.revision);
    const auto combine = [&result](size_t value) {
        result ^= value + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
    };
    combine(hash_u64(key.template_hash));
    combine(hash_u64(key.mask_hash));
    combine(hash_int(key.template_rows));
    combine(hash_int(key.template_cols));
    combine(hash_int(key.template_type));
    combine(hash_int(key.mask_rows));
    combine(hash_int(key.mask_cols));
    combine(hash_int(key.mask_type));
    return result;
}

size_t MaskedCcoeffMatcher::SpectrumKeyHash::operator()(const SpectrumKey& key) const noexcept
{
    size_t result = TemplateKeyHash {}(key.template_key);
    const auto hash_int = std::hash<int> {};
    result ^= hash_int(key.dft_rows) + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
    result ^= hash_int(key.dft_cols) + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
    return result;
}

size_t MaskedCcoeffMatcher::calc_plan_bytes(const TemplatePlan& plan)
{
    size_t bytes = plan.M.total() * plan.M.elemSize();
    for (const auto& template_channel : plan.T_prime) {
        bytes += template_channel.total() * template_channel.elemSize();
    }
    bytes += plan.sparse_entries.size() * sizeof(SparseEntry);
    return bytes;
}

std::shared_ptr<const MaskedCcoeffMatcher::TemplatePlan> MaskedCcoeffMatcher::get_or_build_template_plan(
    const TemplateKey& template_key,
    const cv::Mat& templ_rgb,
    const cv::Mat& mask_u8)
{
    // 记录 miss 时的 revision，插入前校验是否已被清缓存
    const uint64_t revision_at_miss = template_key.revision;
    {
        std::lock_guard lock(m_cache_mtx);
        if (auto it = m_template_plan_cache.find(template_key); it != m_template_plan_cache.end()) {
            m_lru_list.splice(m_lru_list.begin(), m_lru_list, it->second.lru_it);
            return it->second.plan;
        }
    }

    cv::Mat templ_f32;
    cv::Mat mask_f32;
    templ_rgb.convertTo(templ_f32, CV_32F);
    mask_u8.convertTo(mask_f32, CV_32F, 1.0 / 255.0);

    auto plan = std::make_shared<TemplatePlan>();
    plan->cache_revision = revision_at_miss;
    plan->M = mask_f32;
    // mask_area 只能从 mask 内容推导：缓存键只包含 mask 内容，调用方另外传入的计数
    // 既不在键里，也无法与内容校验，会让同一份 mask 复用到一个错误的 μT / σT²。
    plan->mask_area = cv::sum(mask_f32)[0];
    if (plan->mask_area < 1.0) {
        return {};
    }

    std::vector<cv::Mat> template_channels(3);
    cv::split(templ_f32, template_channels);

    for (int channel = 0; channel < 3; ++channel) {
        const double mean_template = cv::sum(plan->M.mul(template_channels[channel]))[0] / plan->mask_area;
        plan->T_prime[channel] = plan->M.mul(template_channels[channel] - mean_template);
        plan->sigma_T_sq += cv::sum(plan->T_prime[channel].mul(plan->T_prime[channel]))[0];
    }

    for (int y = 0; y < templ_f32.rows; ++y) {
        for (int x = 0; x < templ_f32.cols; ++x) {
            if (plan->M.at<float>(y, x) > 0.5f) {
                SparseEntry entry {};
                entry.dx = static_cast<int16_t>(x);
                entry.dy = static_cast<int16_t>(y);
                for (int channel = 0; channel < 3; ++channel) {
                    entry.T_prime[channel] = plan->T_prime[channel].at<float>(y, x);
                }
                plan->sparse_entries.push_back(entry);
            }
        }
    }
    plan->K = static_cast<int>(plan->sparse_entries.size());

    const size_t new_bytes = calc_plan_bytes(*plan);
    if (new_bytes > k_max_cache_bytes) {
        return plan;
    }

    std::lock_guard lock(m_cache_mtx);

    // 二次检查：另一线程可能已插入同一 key，虽然当前设计使用的是单线程 Runner
    if (auto it = m_template_plan_cache.find(template_key); it != m_template_plan_cache.end()) {
        m_lru_list.splice(m_lru_list.begin(), m_lru_list, it->second.lru_it);
        return it->second.plan;
    }

    // revision 已变说明缓存在 build 期间被清过，旧数据不缓存
    if (m_cache_revision.load(std::memory_order_relaxed) != revision_at_miss) {
        return plan;
    }

    // 从尾部淘汰直到满足内存上限
    while (m_cache_total_bytes + new_bytes > k_max_cache_bytes && !m_lru_list.empty()) {
        const TemplateKey& victim = m_lru_list.back();
        const size_t victim_bytes = m_template_plan_cache.at(victim).bytes;
        Log.debug(
            "MaskedCcoeffMatcher | evict template",
            victim.template_hash,
            victim.mask_hash,
            victim_bytes / 1024,
            "KB, total",
            m_cache_total_bytes / 1024,
            "KB");
        m_cache_total_bytes -= victim_bytes;
        m_template_plan_cache.erase(victim);
        m_lru_list.pop_back();
    }

    auto list_it = m_lru_list.insert(m_lru_list.begin(), template_key);
    m_template_plan_cache.emplace(template_key, CacheEntry { plan, list_it, new_bytes });
    m_cache_total_bytes += new_bytes;
    return plan;
}

std::shared_ptr<const MaskedCcoeffMatcher::TemplateSpectrum> MaskedCcoeffMatcher::get_or_build_template_spectrum(
    const TemplateKey& template_key,
    const TemplatePlan& plan,
    int dft_rows,
    int dft_cols)
{
    const uint64_t revision_at_miss = plan.cache_revision;
    const SpectrumKey spectrum_key { template_key, dft_rows, dft_cols };
    {
        std::lock_guard lock(m_cache_mtx);
        if (auto it = m_spectrum_cache.find(spectrum_key); it != m_spectrum_cache.end()) {
            m_spectrum_lru_list.splice(m_spectrum_lru_list.begin(), m_spectrum_lru_list, it->second.lru_it);
#if ASST_MASKED_MATCHER_STATS
            m_spectrum_cache_hits.fetch_add(1, std::memory_order_relaxed);
#endif
            return it->second.spectrum;
        }
    }
#if ASST_MASKED_MATCHER_STATS
    m_spectrum_cache_misses.fetch_add(1, std::memory_order_relaxed);
#endif

    auto spectrum = std::make_shared<TemplateSpectrum>();
    cv::Mat padded(dft_rows, dft_cols, CV_64F, cv::Scalar(0));
    cv::Mat source_double;
    auto make_dft = [&](const cv::Mat& source, cv::Mat& output) {
        padded.setTo(0.0);
        source.convertTo(source_double, CV_64F);
        source_double.copyTo(padded(cv::Rect(0, 0, source_double.cols, source_double.rows)));
        cv::dft(padded, output);
    };
    make_dft(plan.M, spectrum->M_dft);
    for (int channel = 0; channel < 3; ++channel) {
        make_dft(plan.T_prime[channel], spectrum->T_prime_dft[channel]);
    }

    size_t new_bytes = spectrum->M_dft.total() * spectrum->M_dft.elemSize();
    for (const auto& dft : spectrum->T_prime_dft) {
        new_bytes += dft.total() * dft.elemSize();
    }
    if (new_bytes > k_max_spectrum_cache_bytes) {
        return spectrum;
    }

    std::lock_guard lock(m_cache_mtx);
    if (auto it = m_spectrum_cache.find(spectrum_key); it != m_spectrum_cache.end()) {
        m_spectrum_lru_list.splice(m_spectrum_lru_list.begin(), m_spectrum_lru_list, it->second.lru_it);
        return it->second.spectrum;
    }
    if (m_cache_revision.load(std::memory_order_relaxed) != revision_at_miss) {
        return spectrum;
    }
    while (m_spectrum_cache_total_bytes + new_bytes > k_max_spectrum_cache_bytes && !m_spectrum_lru_list.empty()) {
        const SpectrumKey& victim = m_spectrum_lru_list.back();
        const size_t victim_bytes = m_spectrum_cache.at(victim).bytes;
        m_spectrum_cache_total_bytes -= victim_bytes;
        m_spectrum_cache.erase(victim);
        m_spectrum_lru_list.pop_back();
    }
    auto list_it = m_spectrum_lru_list.insert(m_spectrum_lru_list.begin(), spectrum_key);
    m_spectrum_cache.emplace(spectrum_key, SpectrumCacheEntry { spectrum, list_it, new_bytes });
    m_spectrum_cache_total_bytes += new_bytes;
#if ASST_MASKED_MATCHER_STATS
    m_spectrum_cache_bytes.store(m_spectrum_cache_total_bytes, std::memory_order_relaxed);
#endif
    return spectrum;
}

MaskedCcoeffMatcher::MatchStrategy MaskedCcoeffMatcher::choose_strategy(int mask_pixels, int result_positions)
{
    if (mask_pixels <= 0) {
        return MatchStrategy::OpenCV;
    }

    // 神秘调参值
    // - 极小 result + 低 K：稀疏路径整体工作量极小，留给 sparse
    // - 极小 result + 高 K（如 138×130/105×105）：K 超稀疏阈值，FFT 在小 DFT size 反而不如 OpenCV
    // - 中等 result 配中等 K：Windows 上 OpenCV 紧凑 SIMD 快；Android 上 OpenCV 慢约 300x，阈值大幅收紧
    const bool prefer_opencv = [&] {
        if (result_positions < 1000 && mask_pixels < 2000) {
            return false;
        }

#ifdef __ANDROID__
        if (result_positions < 3000 && mask_pixels >= 500) {
            return true;
        }
        return static_cast<long long>(mask_pixels) * result_positions < 8'000'000LL;
#else
        if (result_positions < 12'000 && mask_pixels >= 500) {
            return true;
        }
        return static_cast<long long>(mask_pixels) * result_positions < 25'000'000LL;
#endif
    }();
    if (prefer_opencv) {
        return MatchStrategy::OpenCV;
    }

    constexpr int sparse_mask_limit = 2000;
    constexpr long long sparse_work_limit = 30'000'000LL;
    if (mask_pixels < sparse_mask_limit && static_cast<long long>(mask_pixels) * result_positions < sparse_work_limit) {
        return MatchStrategy::Sparse;
    }
    return MatchStrategy::Fft;
}

// 用 cv::dft 直接实现，消除冗余 FFT
//
// 当前 9 次 matchTemplate 的冗余：
//   FFT(I_c)  每通道算两次（分别用于 xcorr(T'_c, I_c) 和 xcorr(M, I_c)）
//   FFT(M)    每通道算两次（分别用于 xcorr(M, I_c) 和 xcorr(M, I_c²)）
//
// 优化后：
//   FFT(I_c) 和 FFT(I_c²) 每通道各算一次并复用
//   TemplatePlan（T'_c、稀疏列表）通过缓存跨调用复用
//
// 等价于 cv::matchTemplate(image, templ, result, TM_CCOEFF_NORMED, mask)
cv::Mat MaskedCcoeffMatcher::match(
    const cv::Mat& image_rgb, // CV_8UC3
    const cv::Mat& templ_rgb, // CV_8UC3
    const cv::Mat& mask_u8)   // CV_8UC1, 0 or 255
{
    // cv::countNonZero 要求单通道，先挡掉类型不对的输入（其余校验交给下面那个重载）
    if (image_rgb.empty() || templ_rgb.empty() || mask_u8.empty() || mask_u8.type() != CV_8UC1) {
        return {};
    }

    const int rh = image_rgb.rows - templ_rgb.rows + 1;
    const int rw = image_rgb.cols - templ_rgb.cols + 1;
    if (rh <= 0 || rw <= 0) {
        return {};
    }

    const MatchStrategy strategy = choose_strategy(cv::countNonZero(mask_u8), rh * rw);
    if (strategy == MatchStrategy::OpenCV) {
#if ASST_MASKED_MATCHER_STATS
        m_opencv_fallbacks.fetch_add(1, std::memory_order_relaxed);
#endif
        return {};
    }

    auto prepared = prepare_image(image_rgb);
    if (!prepared) {
        return {};
    }
    return match(*prepared, templ_rgb, mask_u8);
}

cv::Mat MaskedCcoeffMatcher::match(const PreparedImage& image, const cv::Mat& templ_rgb, const cv::Mat& mask_u8)
{
    if (templ_rgb.empty() || mask_u8.empty() || templ_rgb.type() != CV_8UC3 || mask_u8.type() != CV_8UC1 ||
        templ_rgb.size() != mask_u8.size()) {
        return {};
    }

    const int rh = image.size.height - templ_rgb.rows + 1;
    const int rw = image.size.width - templ_rgb.cols + 1;
    if (rh <= 0 || rw <= 0) {
        return {};
    }

    const int mask_pixels = cv::countNonZero(mask_u8);
    const MatchStrategy strategy = choose_strategy(mask_pixels, rh * rw);
    if (strategy == MatchStrategy::OpenCV) {
#if ASST_MASKED_MATCHER_STATS
        m_opencv_fallbacks.fetch_add(1, std::memory_order_relaxed);
#endif
        return {};
    }
    if (!is_binary_mask(mask_u8)) {
        Log.error("MaskedCcoeffMatcher | mask must be binary 0/255");
        return {};
    }

    // 模板键只算一次：plan 与 spectrum 必须用同一个键，否则 revision 在两者之间变化时
    // spectrum 会按另一个键构建，既漏缓存又让 plan/spectrum 的 revision 不一致。
    const TemplateKey template_key =
        make_template_key(templ_rgb, mask_u8, m_cache_revision.load(std::memory_order_acquire));
    const auto template_plan = get_or_build_template_plan(template_key, templ_rgb, mask_u8);
    if (!template_plan) {
        return {};
    }

    const double mask_area = template_plan->mask_area;
    const double sigma_T_sq = template_plan->sigma_T_sq;

    const auto& I_ch = image.channels;

    // 稀疏直接相关（小模板快路径，比如基建任务中那种就很合适）。
    // planner 已经综合 K 和 result_positions 做出选择。
    if (strategy == MatchStrategy::Sparse && template_plan->K > 0) {
#if ASST_MASKED_MATCHER_STATS
        m_sparse_calls.fetch_add(1, std::memory_order_relaxed);
#endif
        cv::Mat numerator = cv::Mat::zeros(rh, rw, CV_32F);
        // 用 CV_64F 避免大数相减时的 float32 catastrophic cancellation：
        cv::Mat sum_MI_r = cv::Mat::zeros(rh, rw, CV_64F);
        cv::Mat sum_MI_g = cv::Mat::zeros(rh, rw, CV_64F);
        cv::Mat sum_MI_b = cv::Mat::zeros(rh, rw, CV_64F);
        cv::Mat sum_MI2 = cv::Mat::zeros(rh, rw, CV_64F); // Σ_c I_c²

        for (const auto& [dx, dy, T_prime] : template_plan->sparse_entries) {
            for (int y = 0; y < rh; ++y) {
                const float* Ir = I_ch[0].ptr<float>(y + dy) + dx;
                const float* Ig = I_ch[1].ptr<float>(y + dy) + dx;
                const float* Ib = I_ch[2].ptr<float>(y + dy) + dx;
                auto* num_p = numerator.ptr<float>(y);
                auto* smir_p = sum_MI_r.ptr<double>(y);
                auto* smig_p = sum_MI_g.ptr<double>(y);
                auto* smib_p = sum_MI_b.ptr<double>(y);
                auto* smi2_p = sum_MI2.ptr<double>(y);

                // 编译器会自动向量化的
                for (int x = 0; x < rw; ++x) {
                    const float r = Ir[x], g = Ig[x], b = Ib[x];
                    num_p[x] += T_prime[0] * r + T_prime[1] * g + T_prime[2] * b;
                    smir_p[x] += r;
                    smig_p[x] += g;
                    smib_p[x] += b;
                    smi2_p[x] += r * r + g * g + b * b;
                }
            }
        }

        // sigma_I² = sum_MI2 - (sum_MI_r² + sum_MI_g² + sum_MI_b²) / mask_area
        // 全程保持 CV_64F，防止大数相减精度损失
        cv::Mat sq_sum, sq_g, sq_b;
        cv::multiply(sum_MI_r, sum_MI_r, sq_sum);
        cv::multiply(sum_MI_g, sum_MI_g, sq_g);
        cv::multiply(sum_MI_b, sum_MI_b, sq_b);
        cv::add(sq_sum, sq_g, sq_sum);
        cv::add(sq_sum, sq_b, sq_sum);
        cv::Mat sigma_I_sq_64;
        cv::subtract(sum_MI2, sq_sum * (1.0 / mask_area), sigma_I_sq_64);
        cv::max(sigma_I_sq_64, 0.0, sigma_I_sq_64);

        // sigma_I² 是两个同量级大数相减的结果，被减数（Σ_c Σ_p I_c²）远大于窗口真实方差时，
        // 差值只剩舍入噪声（约 1e-16·sum_MI2，这里 sum_MI2 本身是 CV_64F 精确累加，
        // 误差来自乘以 1/mask_area）。这种窗口的相关系数在数学上是 0/0，若直接当分母会
        // 把噪声放大成 ±1，从而产生假阳性。以被减数为尺度设下限（留 1e4 倍余量）：
        // 8-bit 输入下可达的最小非零 sigma_I² 约为 (K-1)/K，而
        // 1e-12·Σ_c Σ_p I_c² ≤ 1e-12·3·255²·K，sparse 路径 K < 2000 时仍低几个数量级。
        cv::Mat variance_floor;
        cv::multiply(sum_MI2, 1e-12, variance_floor);
        sigma_I_sq_64.setTo(0.0, sigma_I_sq_64 < variance_floor);

        cv::Mat sigma_I_sq;
        sigma_I_sq_64.convertTo(sigma_I_sq, CV_32F);

        cv::Mat denom;
        cv::sqrt(sigma_I_sq * sigma_T_sq, denom);
        cv::Mat result;
        cv::divide(numerator, denom, result);
        cv::patchNaNs(result, 0.0);
        // 上面已按被减数量级清掉噪声方差，这里再兜一层 sigma_I² ≈ 0 的判据
        // （denom < √σ_T²·1e-5 等价于 σ_I² < 1e-10，σ_T² > 0）。
        const auto sigma_T_norm = static_cast<float>(std::sqrt(sigma_T_sq));
        result.setTo(0.0f, denom < sigma_T_norm * 1e-5f);
        cv::min(result, 1.0f, result);
        cv::max(result, -1.0f, result);
        return result;
    }

    // DFT 的填充尺寸：仅在确认走 FFT 路径后才需要
    const int dft_rows = cv::getOptimalDFTSize(image.size.height + templ_rgb.rows - 1);
    const int dft_cols = cv::getOptimalDFTSize(image.size.width + templ_rgb.cols - 1);
#if ASST_MASKED_MATCHER_STATS
    m_fft_calls.fetch_add(1, std::memory_order_relaxed);
#endif
    const auto template_spectrum = get_or_build_template_spectrum(template_key, *template_plan, dft_rows, dft_cols);

    // Keep small workspaces hot, but do not retain unbounded CV_64F buffers per worker.
    // ponytail: 64 MB per-thread ceiling; larger requests use a call-scoped workspace.
    constexpr size_t max_thread_local_workspace_bytes = 64ULL * 1024 * 1024;
    const size_t dft_elements = static_cast<size_t>(dft_rows) * static_cast<size_t>(dft_cols);
    const size_t result_elements = static_cast<size_t>(rh) * static_cast<size_t>(rw);
    const size_t image_elements = static_cast<size_t>(image.size.height) * static_cast<size_t>(image.size.width);
    const size_t workspace_bytes = (4 * dft_elements + 2 * result_elements + image_elements) * sizeof(double);
    thread_local FftWorkspace thread_workspace;
    FftWorkspace local_workspace;
    FftWorkspace& workspace = workspace_bytes <= max_thread_local_workspace_bytes ? thread_workspace : local_workspace;
    if (workspace.prepare(dft_rows, dft_cols, rh, rw, image.size)) {
#if ASST_MASKED_MATCHER_STATS
        m_workspace_rebuilds.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    auto make_dft_into = [&](const cv::Mat& src, cv::Mat& out) {
        workspace.padded.setTo(0.0);
        src.convertTo(workspace.src_d, CV_64F);
        workspace.src_d.copyTo(workspace.padded(cv::Rect(0, 0, workspace.src_d.cols, workspace.src_d.rows)));
        cv::dft(workspace.padded, out);
    };

    auto xcorr_into = [&](const cv::Mat& dft_A, const cv::Mat& dft_B, cv::Mat& out) {
        cv::mulSpectrums(dft_A, dft_B, workspace.product_spectrum, 0, true);
        cv::dft(
            workspace.product_spectrum,
            workspace.result_buf,
            cv::DFT_INVERSE | cv::DFT_REAL_OUTPUT | cv::DFT_SCALE);
        workspace.result_buf(cv::Rect(0, 0, rw, rh)).copyTo(out);
    };
    auto xcorr_add = [&](const cv::Mat& dft_A, const cv::Mat& dft_B, cv::Mat& accum) {
        cv::mulSpectrums(dft_A, dft_B, workspace.product_spectrum, 0, true);
        cv::dft(
            workspace.product_spectrum,
            workspace.result_buf,
            cv::DFT_INVERSE | cv::DFT_REAL_OUTPUT | cv::DFT_SCALE);
        cv::add(accum, workspace.result_buf(cv::Rect(0, 0, rw, rh)), accum);
    };

    cv::Mat numerator = cv::Mat::zeros(rh, rw, CV_64F);
    cv::Mat sigma_I_sq = cv::Mat::zeros(rh, rw, CV_64F);

    // σ_I²(x,y) = Σ_c σ_I_c² = Σ_c [(M ⋆ I_c²) - (M ⋆ I_c)² / N]
    //          = Σ_c (M ⋆ I_c²)        - (1/N) Σ_c (M ⋆ I_c)²
    //          ↑ 把这一项的三通道求和提到卷积外面
    //
    // 利用卷积对加法线性：Σ_c (M ⋆ I_c²) = M ⋆ (Σ_c I_c²)
    // 在空域里先把三通道平方加起来再做一次卷积，比每通道各做一次再相加少 2 次 FFT + 2 次 IFFT
    // 第二项 Σ_c (M ⋆ I_c)² 因为有平方，不能这样合并（平方对加法非线性），仍逐通道算
    cv::Mat I_sq_sum = I_ch[0].mul(I_ch[0]) + I_ch[1].mul(I_ch[1]) + I_ch[2].mul(I_ch[2]);
    make_dft_into(I_sq_sum, workspace.image_dft);
    xcorr_into(workspace.image_dft, template_spectrum->M_dft, workspace.sum_MI2_buf);
    cv::add(sigma_I_sq, workspace.sum_MI2_buf, sigma_I_sq);

    for (int c = 0; c < 3; ++c) {
        make_dft_into(I_ch[c], workspace.image_dft);

        // numerator += xcorr(T'_c, I_c)
        xcorr_add(workspace.image_dft, template_spectrum->T_prime_dft[c], numerator);

        // sigma_I² 第二项：-Σ_c (sum_MI_c)² / mask_area，逐通道累加
        xcorr_into(workspace.image_dft, template_spectrum->M_dft, workspace.sum_MI_buf);
        cv::Mat var_d;
        cv::multiply(workspace.sum_MI_buf, workspace.sum_MI_buf, var_d, -1.0 / mask_area);
        cv::add(sigma_I_sq, var_d, sigma_I_sq);
    }

    cv::max(sigma_I_sq, 0.0, sigma_I_sq);
    // 图像块方差远低于模板方差时相关系数无定义（均匀区域，分母≈0），直接归零。
    // sigma_I² 是 (M ⋆ Σ_c I_c²) - Σ_c (M ⋆ I_c)²/N 相减得到的，大数相减必须留一个下限；
    // FFT 路径的误差比稀疏路径的直接累加大，所以下限比稀疏路径的 denom 判据更宽。
    // 输入保持 8-bit 的 [0,255] 尺度：σ_I² 要么为 0，要么至少是 (K-1)/K，
    // 而 variance_eps = σ_T²·1e-8 ≤ 3·K·(255²/4)·1e-8 ≈ 4.88e-4·K。K < 2050 时下限严格
    // 小于可达的最小非零方差；更大的 K 只在"模板方差取到理论上限、窗口仅差 1 个色阶"的
    // 构造上才会越界，而那里的真实相关系数是 O(1/√K) ≤ 0.03，远低于任何匹配阈值。
    const double variance_eps = sigma_T_sq * 1e-8;
    sigma_I_sq.setTo(0.0, sigma_I_sq < variance_eps);

    cv::Mat denom;
    cv::sqrt(sigma_I_sq * sigma_T_sq, denom);

    cv::Mat result;
    cv::divide(numerator, denom, result);
    // patchNaNs 不支持 CV_64F，用 CMP_NE 自检替代（NaN != NaN）
    cv::Mat nan_mask;
    cv::compare(result, result, nan_mask, cv::CMP_NE);
    result.setTo(0.0, nan_mask);
    const double sigma_T_norm = std::sqrt(sigma_T_sq);
    result.setTo(0.0, denom < sigma_T_norm * 1e-5);
    cv::min(result, 1.0, result);
    cv::max(result, -1.0, result);

    cv::Mat result_f32;
    result.convertTo(result_f32, CV_32F);
    return result_f32;
}
}
