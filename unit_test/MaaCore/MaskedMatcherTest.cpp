#include <catch2/catch_test_macros.hpp>

#include "MaaUtils/NoWarningCV.hpp"
#include "Vision/MaskedCcoeffMatcher.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace
{
using asst::MaskedCcoeffMatcher;

cv::Mat random_mat(cv::Size size, int type, uint64_t seed)
{
    cv::Mat result(size, type);
    cv::RNG rng(seed);
    rng.fill(result, cv::RNG::UNIFORM, 0, 256);
    return result;
}

// keep_percent% 的像素保留为 255，其余为 0
cv::Mat random_mask(cv::Size size, uint64_t seed, int keep_percent)
{
    const cv::Mat values = random_mat(size, CV_8UC1, seed);
    cv::Mat mask;
    cv::threshold(values, mask, 255 - keep_percent * 255 / 100, 255, cv::THRESH_BINARY);
    return mask;
}

std::vector<cv::Point> mask_pixels(const cv::Mat& mask)
{
    std::vector<cv::Point> pixels;
    for (int y = 0; y < mask.rows; ++y) {
        for (int x = 0; x < mask.cols; ++x) {
            if (mask.at<uint8_t>(y, x) != 0) {
                pixels.emplace_back(x, y);
            }
        }
    }
    return pixels;
}

// masked TM_CCOEFF_NORMED 的暴力参考实现。
//
// 定义（M ∈ {0,1}，N = |M|，均值都在 mask 内取）：
//   result(y,x) = Σ_c Σ_p M(p)(T_c(p)-μT_c)(I_c(p+y,x)-μI_c)
//                 / sqrt( Σ_c Σ_p M(p)(T_c(p)-μT_c)² · Σ_c Σ_p M(p)(I_c(p+y,x)-μI_c)² )
//
// 把 μT_c = ST_c/(255N)、μI_c = SI_c/(255N) 代入，1/(255N)² 因子在分子分母里完全抵消：
//   a_c(p) = T_c(p)·N - ST_c,  b_c(p) = I_c(p)·N - SI_c
//   result  = Σ_c Σ_p a_c(p)b_c(p) / sqrt( (Σ_c Σ_p a_c(p)²)·(Σ_c Σ_p b_c(p)²) )
// 因此整个参考实现可以只用整数累加，不引入任何浮点误差——它给出的就是数学真值，
// 不是"另一个近似实现"。分母为 0（模板或窗口完全均匀）时约定为 0。
class BruteForceReference
{
public:
    BruteForceReference(const cv::Mat& templ, const cv::Mat& mask) :
        m_pixels(mask_pixels(mask)),
        m_n(static_cast<int64_t>(m_pixels.size()))
    {
        REQUIRE(m_n > 0);
        REQUIRE(m_n <= 20'000); // 保证 int64 累加不溢出：195'075·N³ < 2^63
        m_a.resize(m_pixels.size());
        for (size_t k = 0; k < m_pixels.size(); ++k) {
            const cv::Vec3b value = templ.at<cv::Vec3b>(m_pixels[k].y, m_pixels[k].x);
            for (int c = 0; c < 3; ++c) {
                m_sum_t[c] += value[c];
            }
        }
        for (size_t k = 0; k < m_pixels.size(); ++k) {
            const cv::Vec3b value = templ.at<cv::Vec3b>(m_pixels[k].y, m_pixels[k].x);
            for (int c = 0; c < 3; ++c) {
                const int64_t a = static_cast<int64_t>(value[c]) * m_n - m_sum_t[c];
                m_a[k][c] = a;
                m_sigma_t_sq += a * a;
            }
        }
    }

    double at(const cv::Mat& image, int wy, int wx) const
    {
        std::array<int64_t, 3> sum_i { 0, 0, 0 };
        for (size_t k = 0; k < m_pixels.size(); ++k) {
            const cv::Vec3b value = image.at<cv::Vec3b>(wy + m_pixels[k].y, wx + m_pixels[k].x);
            for (int c = 0; c < 3; ++c) {
                sum_i[c] += value[c];
            }
        }

        int64_t numerator = 0;
        int64_t sigma_i_sq = 0;
        for (size_t k = 0; k < m_pixels.size(); ++k) {
            const cv::Vec3b value = image.at<cv::Vec3b>(wy + m_pixels[k].y, wx + m_pixels[k].x);
            for (int c = 0; c < 3; ++c) {
                const int64_t b = static_cast<int64_t>(value[c]) * m_n - sum_i[c];
                numerator += m_a[k][c] * b;
                sigma_i_sq += b * b;
            }
        }

        const long double denominator =
            std::sqrt(static_cast<long double>(m_sigma_t_sq) * static_cast<long double>(sigma_i_sq));
        if (denominator == 0) {
            return 0.0;
        }
        return static_cast<double>(static_cast<long double>(numerator) / denominator);
    }

    cv::Mat match(const cv::Mat& image, cv::Size templ_size) const
    {
        const int rh = image.rows - templ_size.height + 1;
        const int rw = image.cols - templ_size.width + 1;
        cv::Mat result(rh, rw, CV_64F);
        for (int y = 0; y < rh; ++y) {
            for (int x = 0; x < rw; ++x) {
                result.at<double>(y, x) = at(image, y, x);
            }
        }
        return result;
    }

private:
    std::vector<cv::Point> m_pixels;
    int64_t m_n = 0;
    std::vector<std::array<int64_t, 3>> m_a;
    std::array<int64_t, 3> m_sum_t { 0, 0, 0 };
    int64_t m_sigma_t_sq = 0;
};

uint64_t revision = 1;

struct CaseResult
{
    MaskedCcoeffMatcher::MatchStrategy strategy = MaskedCcoeffMatcher::MatchStrategy::OpenCV;
    double max_error = 0;
    double max_abs = 0;
    bool empty = false;
};

CaseResult run_case(const std::string& name, const cv::Mat& image, const cv::Mat& templ, const cv::Mat& mask)
{
    INFO("case: " << name);

    const BruteForceReference reference(templ, mask);
    const cv::Mat expected = reference.match(image, templ.size());

    const int mask_pixels = cv::countNonZero(mask);
    const int result_positions = (image.rows - templ.rows + 1) * (image.cols - templ.cols + 1);

    CaseResult result;
    result.strategy = MaskedCcoeffMatcher::choose_strategy(mask_pixels, result_positions);

    auto& matcher = MaskedCcoeffMatcher::get_instance();
    matcher.sync_cache_revision(revision++);
    const cv::Mat actual = matcher.match(image, templ, mask);
    result.empty = actual.empty();

    cv::minMaxLoc(expected, nullptr, &result.max_abs);
    if (actual.empty()) {
        // OpenCV 路径下 match() 返回空矩阵，由 Matcher 落到 cv::matchTemplate
        REQUIRE(result.strategy == MaskedCcoeffMatcher::MatchStrategy::OpenCV);
        return result;
    }

    cv::Mat actual64;
    actual.convertTo(actual64, CV_64F);
    result.max_error = cv::norm(expected, actual64, cv::NORM_INF);

    // 结果必须是有限值且落在相关系数的值域内
    for (int y = 0; y < actual64.rows; ++y) {
        for (int x = 0; x < actual64.cols; ++x) {
            const double value = actual64.at<double>(y, x);
            REQUIRE(std::isfinite(value));
            REQUIRE(value >= -1.000001);
            REQUIRE(value <= 1.000001);
        }
    }
    return result;
}

// 良态输入下三条路径都应逼近数学真值；近简并窗口上分母趋零，允许更宽但仍远小于
// 任何匹配阈值的误差（见 MaskedCcoeffMatcher::match 中 variance_eps 的说明）。
constexpr double kWellConditionedTolerance = 1e-5;
constexpr double kDegenerateTolerance = 1e-3;
} // namespace

TEST_CASE("Masked matcher matches the brute-force CCOEFF_NORMED reference on every strategy")
{
    SECTION("sparse path")
    {
        // result_positions < 1000 且 mask_pixels < 2000 -> sparse
        const cv::Mat image = random_mat({ 40, 40 }, CV_8UC3, 0x1001);
        const cv::Mat templ = random_mat({ 16, 16 }, CV_8UC3, 0x1002);

        for (const int keep : { 6, 50, 96 }) {
            const auto result = run_case(
                "sparse keep=" + std::to_string(keep) + "%",
                image,
                templ,
                random_mask(templ.size(), 0x1003 + keep, keep));
            REQUIRE(result.strategy == MaskedCcoeffMatcher::MatchStrategy::Sparse);
            REQUIRE_FALSE(result.empty);
            REQUIRE(result.max_error <= kWellConditionedTolerance);
        }

        // 奇数尺寸
        const cv::Mat odd_image = random_mat({ 43, 41 }, CV_8UC3, 0x1004);
        const cv::Mat odd_templ = random_mat({ 15, 17 }, CV_8UC3, 0x1005);
        const auto odd = run_case("sparse odd sizes", odd_image, odd_templ, random_mask(odd_templ.size(), 0x1006, 50));
        REQUIRE(odd.strategy == MaskedCcoeffMatcher::MatchStrategy::Sparse);
        REQUIRE(odd.max_error <= kWellConditionedTolerance);

        // 全 mask（M 恒为 1）
        const auto full = run_case("sparse full mask", image, templ, cv::Mat(16, 16, CV_8UC1, cv::Scalar(255)));
        REQUIRE(full.strategy == MaskedCcoeffMatcher::MatchStrategy::Sparse);
        REQUIRE(full.max_error <= kWellConditionedTolerance);

        // 单像素 mask：μT 就是该像素本身，分子分母同时为 0，约定为 0
        cv::Mat single = cv::Mat::zeros(16, 16, CV_8UC1);
        single.at<uint8_t>(8, 8) = 255;
        const auto one_pixel = run_case("sparse single-pixel mask", image, templ, single);
        REQUIRE(one_pixel.strategy == MaskedCcoeffMatcher::MatchStrategy::Sparse);
        REQUIRE(one_pixel.max_abs == 0.0);
        REQUIRE(one_pixel.max_error == 0.0);
    }

    SECTION("fft path")
    {
        // mask_pixels >= 2000 且 result_positions >= 12000 -> fft
        const cv::Mat image = random_mat({ 150, 200 }, CV_8UC3, 0x2001);
        const cv::Mat templ = random_mat({ 50, 50 }, CV_8UC3, 0x2002);

        // 确定性 mask：每 6 个像素留 5 个
        cv::Mat mask(50, 50, CV_8UC1);
        for (int y = 0; y < mask.rows; ++y) {
            for (int x = 0; x < mask.cols; ++x) {
                mask.at<uint8_t>(y, x) = (x + 2 * y) % 6 == 0 ? 0 : 255;
            }
        }
        REQUIRE(cv::countNonZero(mask) >= 2000);

        const auto result = run_case("fft dense mask", image, templ, mask);
        REQUIRE(result.strategy == MaskedCcoeffMatcher::MatchStrategy::Fft);
        REQUIRE_FALSE(result.empty);
        REQUIRE(result.max_error <= kWellConditionedTolerance);
    }

    SECTION("opencv path")
    {
        // mask_pixels·result_positions < 25e6 -> 交给 OpenCV
        const cv::Mat image = random_mat({ 120, 120 }, CV_8UC3, 0x3001);
        const cv::Mat templ = random_mat({ 32, 32 }, CV_8UC3, 0x3002);
        const auto result = run_case("opencv fallback", image, templ, random_mask(templ.size(), 0x3003, 50));
        REQUIRE(result.strategy == MaskedCcoeffMatcher::MatchStrategy::OpenCV);
        REQUIRE(result.empty);
    }
}

TEST_CASE("Masked matcher returns the documented value on degenerate inputs")
{
    const cv::Mat image = random_mat({ 40, 40 }, CV_8UC3, 0x4001);
    const cv::Mat templ = random_mat({ 16, 16 }, CV_8UC3, 0x4002);
    const cv::Mat mask = random_mask(templ.size(), 0x4003, 50);

    SECTION("constant template has zero variance")
    {
        const cv::Mat constant_templ(16, 16, CV_8UC3, cv::Scalar(77, 88, 99));
        const auto result = run_case("constant template", image, constant_templ, mask);
        REQUIRE(result.max_abs == 0.0);
        REQUIRE(result.max_error == 0.0);
    }

    SECTION("constant image window has zero variance")
    {
        const cv::Mat constant_image(40, 40, CV_8UC3, cv::Scalar(10, 20, 30));
        const auto result = run_case("constant image", constant_image, templ, mask);
        REQUIRE(result.max_abs == 0.0);
        REQUIRE(result.max_error == 0.0);
    }

    SECTION("window constant except a single one-level pixel")
    {
        cv::Mat near_constant(40, 40, CV_8UC3, cv::Scalar(100, 100, 100));
        near_constant.at<cv::Vec3b>(20, 20)[0] = 101;
        const auto result = run_case("near-constant window", near_constant, templ, mask);
        REQUIRE(result.max_error <= kDegenerateTolerance);
    }

    SECTION("window identical to the template scores exactly 1")
    {
        cv::Mat perfect(40, 40, CV_8UC3, cv::Scalar(100, 100, 100));
        templ.copyTo(perfect(cv::Rect(10, 10, templ.cols, templ.rows)));
        const auto result = run_case("window == template", perfect, templ, mask);
        REQUIRE(result.max_error <= kWellConditionedTolerance);
        // 完美匹配位置的相关系数必须是 1
        auto& matcher = MaskedCcoeffMatcher::get_instance();
        matcher.sync_cache_revision(revision++);
        const cv::Mat actual = matcher.match(perfect, templ, mask);
        REQUIRE(actual.at<float>(10, 10) == 1.0f);
    }

    // 回归：窗口在 mask 内完全均匀时相关系数是 0/0，必须约定为 0。
    // 若把 sigma_I² 的相减噪声当成分母，这里会得到被裁剪成 ±1 的假阳性。
    SECTION("window constant over the mask scores zero, not +-1")
    {
        const cv::Mat constant_image(40, 40, CV_8UC3, cv::Scalar(100, 100, 100));
        const auto result = run_case("constant-over-mask window", constant_image, templ, mask);
        REQUIRE(result.max_abs == 0.0);
        REQUIRE(result.max_error == 0.0);
    }
}

// 回归测试：窗口方差取到 8-bit 输入可达的最小非零值时，方差下限不能把真实相关系数清零。
// 构造方式：mask 内模板取黑白棋盘（σT² 逼近理论上限 3·K·0.25），窗口除一个像素外完全均匀。
//
// K < 2050 时 σT²·1e-8 < (K-1)/K·(1/255)²，下限严格小于可达的最小非零方差，
// 因此这里必须逐点等于数学真值。K 更大时（只可能走 FFT 路径）下限会越过这个最小值，
// 但那些位置的数学真值只有 O(1/√K)，见下面的 FFT 用例。
TEST_CASE("Masked matcher does not suppress a reachable window variance")
{
    constexpr int side = 50;

    // mask 内模板取黑白棋盘，mask 覆盖 1971 个像素（< 2000，走 sparse 路径）
    cv::Mat templ(side, side, CV_8UC3);
    cv::Mat mask(side, side, CV_8UC1);
    cv::RNG rng(0x5001);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const bool in_mask = rng.uniform(0, 100) < 80;
            mask.at<uint8_t>(y, x) = in_mask ? 255 : 0;
            const uint8_t value = in_mask && (x + y) % 2 == 1 ? 255 : 0;
            templ.at<cv::Vec3b>(y, x) = cv::Vec3b(value, value, value);
        }
    }
    const int masked = cv::countNonZero(mask);
    REQUIRE(masked >= 1900);
    REQUIRE(masked < 2000);

    // 让窗口内唯一的非均匀像素落在模板偏离均值最大的位置
    int deviated_x = 0;
    int deviated_y = 0;
    double best = -1;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            if (mask.at<uint8_t>(y, x) == 0) {
                continue;
            }
            const double deviation = std::abs(static_cast<double>(templ.at<cv::Vec3b>(y, x)[0]) / 255.0 - 0.5);
            if (deviation > best) {
                best = deviation;
                deviated_x = x;
                deviated_y = y;
            }
        }
    }

    // 40x40 图 / 50x50 模板放不下，用 75x75（结果矩阵 26x26 = 676 < 1000 -> sparse）
    cv::Mat image(75, 75, CV_8UC3, cv::Scalar(100, 100, 100));
    image.at<cv::Vec3b>(10 + deviated_y, 10 + deviated_x)[0] = 101;

    const auto result = run_case("reachable minimum variance (sparse)", image, templ, mask);
    REQUIRE(result.strategy == MaskedCcoeffMatcher::MatchStrategy::Sparse);
    // 该位置的相关系数不为 0，说明方差下限没有误杀真实方差
    REQUIRE(result.max_abs > 1e-3);
    // sparse 路径的分子用 CV_32F 累加：近简并窗口上真实分子是大数相消后的小残差，
    // float32 的相对误差被放大，绝对误差上界约 1e-3。但受影响的分数本身只有 O(1/√K)，
    // 远低于任何匹配阈值（MAA 的阈值都在 0.6 以上），因此断言"误差有界"而非"误差极小"。
    REQUIRE(result.max_abs < 0.05);
    REQUIRE(result.max_error < 1e-3);
}

// 同一个构造放大到 FFT 路径（K >= 2000）：σT²·1e-8 会越过可达的最小非零方差，
// 因此允许更宽的误差；但被影响的位置其数学真值只有 O(1/√K)，远低于任何匹配阈值。
TEST_CASE("Masked matcher variance floor stays far below matching thresholds on the fft path")
{
    constexpr int side = 60;
    cv::Mat templ(side, side, CV_8UC3);
    cv::Mat mask(side, side, CV_8UC1);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const bool in_mask = (x + 2 * y) % 7 != 0;
            mask.at<uint8_t>(y, x) = in_mask ? 255 : 0;
            const uint8_t value = in_mask && (x + y) % 2 == 1 ? 255 : 0;
            templ.at<cv::Vec3b>(y, x) = cv::Vec3b(value, value, value);
        }
    }
    REQUIRE(cv::countNonZero(mask) >= 2000);

    cv::Mat image(200, 150, CV_8UC3, cv::Scalar(100, 100, 100));
    image.at<cv::Vec3b>(60, 60)[0] = 101;

    const auto result = run_case("reachable minimum variance (fft)", image, templ, mask);
    REQUIRE(result.strategy == MaskedCcoeffMatcher::MatchStrategy::Fft);
    // MAA 的匹配阈值都在 0.6 以上，这个量级的偏差不参与任何判定
    REQUIRE(result.max_abs < 0.05);
    REQUIRE(result.max_error < 0.05);

    // 窗口在 mask 内完全均匀：真值 0/0，必须约定为 0，不能出现被裁剪成 ±1 的假阳性
    const cv::Mat constant_image(200, 150, CV_8UC3, cv::Scalar(100, 100, 100));
    const auto constant_result = run_case("constant-over-mask window (fft)", constant_image, templ, mask);
    REQUIRE(constant_result.strategy == MaskedCcoeffMatcher::MatchStrategy::Fft);
    REQUIRE(constant_result.max_abs < 0.05);
    REQUIRE(constant_result.max_error < 0.05);
}

TEST_CASE("Masked matcher cache is keyed by template and mask content")
{
    const cv::Mat image = random_mat({ 40, 40 }, CV_8UC3, 0x6001);
    const cv::Mat templ = random_mat({ 16, 16 }, CV_8UC3, 0x6002);
    const cv::Mat mask_a = random_mask(templ.size(), 0x6003, 40);
    const cv::Mat mask_b = random_mask(templ.size(), 0x6004, 40);
    REQUIRE(cv::countNonZero(mask_a) != cv::countNonZero(mask_b));

    const BruteForceReference reference_a(templ, mask_a);
    const BruteForceReference reference_b(templ, mask_b);

    // 同尺寸、不同内容：这是"两个模板产生同一 cache identity"最现实的路径
    const cv::Mat templ_b = random_mat({ 16, 16 }, CV_8UC3, 0x6005);
    REQUIRE(templ_b.size() == templ.size());
    REQUIRE(cv::norm(templ_b, templ, cv::NORM_INF) > 0);
    const BruteForceReference reference_c(templ_b, mask_a);

    auto& matcher = MaskedCcoeffMatcher::get_instance();
    matcher.sync_cache_revision(revision++);

    // 交替调用：必须各自命中自己的 plan，而不是复用另一个模板/另一个 mask 的
    for (int round = 0; round < 2; ++round) {
        const cv::Mat actual_a = matcher.match(image, templ, mask_a);
        const cv::Mat actual_b = matcher.match(image, templ, mask_b);
        const cv::Mat actual_c = matcher.match(image, templ_b, mask_a);
        REQUIRE_FALSE(actual_a.empty());
        REQUIRE_FALSE(actual_b.empty());
        REQUIRE_FALSE(actual_c.empty());

        cv::Mat actual_a64;
        cv::Mat actual_b64;
        cv::Mat actual_c64;
        actual_a.convertTo(actual_a64, CV_64F);
        actual_b.convertTo(actual_b64, CV_64F);
        actual_c.convertTo(actual_c64, CV_64F);
        REQUIRE(
            cv::norm(reference_a.match(image, templ.size()), actual_a64, cv::NORM_INF) <= kWellConditionedTolerance);
        REQUIRE(
            cv::norm(reference_b.match(image, templ.size()), actual_b64, cv::NORM_INF) <= kWellConditionedTolerance);
        REQUIRE(
            cv::norm(reference_c.match(image, templ_b.size()), actual_c64, cv::NORM_INF) <= kWellConditionedTolerance);
    }

    // revision 变化后缓存作废，但结果必须不变
    matcher.sync_cache_revision(revision++);
    const cv::Mat after_revision = matcher.match(image, templ, mask_a);
    cv::Mat after_revision64;
    after_revision.convertTo(after_revision64, CV_64F);
    REQUIRE(
        cv::norm(reference_a.match(image, templ.size()), after_revision64, cv::NORM_INF) <= kWellConditionedTolerance);
}

TEST_CASE("Masked matcher reuses a prepared image across templates")
{
    const cv::Mat image = random_mat({ 40, 40 }, CV_8UC3, 0x7001);
    const cv::Mat templ_a = random_mat({ 16, 16 }, CV_8UC3, 0x7002);
    const cv::Mat templ_b = random_mat({ 12, 20 }, CV_8UC3, 0x7003);
    const cv::Mat mask_a = random_mask(templ_a.size(), 0x7004, 50);
    const cv::Mat mask_b = random_mask(templ_b.size(), 0x7005, 50);

    auto& matcher = MaskedCcoeffMatcher::get_instance();
    matcher.sync_cache_revision(revision++);
    const auto prepared = matcher.prepare_image(image);
    REQUIRE(prepared.has_value());

    const cv::Mat actual_a = matcher.match(*prepared, templ_a, mask_a);
    const cv::Mat actual_b = matcher.match(*prepared, templ_b, mask_b);
    REQUIRE_FALSE(actual_a.empty());
    REQUIRE_FALSE(actual_b.empty());

    cv::Mat actual_a64;
    cv::Mat actual_b64;
    actual_a.convertTo(actual_a64, CV_64F);
    actual_b.convertTo(actual_b64, CV_64F);
    REQUIRE(
        cv::norm(BruteForceReference(templ_a, mask_a).match(image, templ_a.size()), actual_a64, cv::NORM_INF) <=
        kWellConditionedTolerance);
    REQUIRE(
        cv::norm(BruteForceReference(templ_b, mask_b).match(image, templ_b.size()), actual_b64, cv::NORM_INF) <=
        kWellConditionedTolerance);
}

TEST_CASE("Masked matcher rejects unsupported inputs")
{
    const cv::Mat image = random_mat({ 40, 40 }, CV_8UC3, 0x8001);
    const cv::Mat templ = random_mat({ 16, 16 }, CV_8UC3, 0x8002);
    const cv::Mat mask = random_mask(templ.size(), 0x8003, 50);

    auto& matcher = MaskedCcoeffMatcher::get_instance();
    matcher.sync_cache_revision(revision++);

    SECTION("four-channel image is rejected before preparation")
    {
        const cv::Mat rgba(32, 32, CV_8UC4, cv::Scalar(0, 0, 0, 255));
        REQUIRE_FALSE(matcher.prepare_image(rgba).has_value());
    }

    SECTION("non 8UC3 template falls back to OpenCV")
    {
        cv::Mat float_templ;
        templ.convertTo(float_templ, CV_32FC3);
        REQUIRE(matcher.match(image, float_templ, mask).empty());
    }

    SECTION("mask size mismatch falls back to OpenCV")
    {
        const cv::Mat wrong_size = random_mask({ 8, 8 }, 0x8004, 50);
        REQUIRE(matcher.match(image, templ, wrong_size).empty());
    }

    SECTION("multi-channel mask falls back to OpenCV instead of throwing")
    {
        cv::Mat rgb_mask;
        cv::cvtColor(random_mask(templ.size(), 0x8007, 50), rgb_mask, cv::COLOR_GRAY2BGR);
        REQUIRE(rgb_mask.channels() == 3);
        REQUIRE(matcher.match(image, templ, rgb_mask).empty());
    }

    SECTION("non-binary mask falls back to OpenCV")
    {
        // 二值 mask 里塞一个 128：像素集合不变，但不再是 0/255
        cv::Mat soft_mask = random_mask(templ.size(), 0x8005, 50);
        soft_mask.at<uint8_t>(4, 4) = 128;
        REQUIRE(cv::countNonZero(soft_mask) == cv::countNonZero(random_mask(templ.size(), 0x8005, 50)));
        REQUIRE(matcher.match(image, templ, soft_mask).empty());
    }

    SECTION("empty mask falls back to OpenCV")
    {
        REQUIRE(matcher.match(image, templ, cv::Mat::zeros(templ.size(), CV_8UC1)).empty());
    }

    SECTION("template larger than image falls back to OpenCV")
    {
        const cv::Mat small_image = random_mat({ 8, 8 }, CV_8UC3, 0x8006);
        REQUIRE(matcher.match(small_image, templ, mask).empty());
    }
}
