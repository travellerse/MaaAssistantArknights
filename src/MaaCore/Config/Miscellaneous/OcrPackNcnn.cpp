#if defined(ASST_WITH_NCNN) && ASST_WITH_NCNN

#include "OcrPack.h"

#include "OcrPackNcnn.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <ranges>
#include <thread>

#include "MaaUtils/NoWarningCV.hpp"

#include "Utils/Demangle.hpp"
#include "Utils/Logger.hpp"
#include "Utils/Platform.hpp"

#include <cpu.h>

namespace
{
int cpu_thread_count()
{
    const int logical = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    if (logical <= 2) {
        return 1;
    }
    if (logical <= 4) {
        return 2;
    }
    if (logical <= 12) {
        return 3;
    }
    return 4;
}

#if ASST_WITH_NCNN_VULKAN_OCR
constexpr bool k_ncnn_vulkan_ocr_enabled = true;
#else
constexpr bool k_ncnn_vulkan_ocr_enabled = false;
#endif

}

namespace asst
{

struct OcrPack::Impl
{
    std::shared_ptr<OcrPackNcnn> ncnn;
    std::filesystem::path det_model_path;
    std::filesystem::path rec_model_path;
    std::filesystem::path rec_label_path;
    // 实际生效的后端，仅在 session 加载成功后写入。
    std::optional<detail::NcnnOcrRuntimeOptions> applied_runtime;
    // Vulkan 请求失败后的降级标记。只作用于本 pack（WordOcr / CharOcr 各自独立），
    // 不写任何进程级状态；use_gpu() / use_cpu() / load() 会清除它，
    // 因此显式重新配置后端或重新加载资源后仍会重试 Vulkan。
    bool vulkan_downgraded = false;
    std::mutex mutex;
};

OcrPack::OcrPack() :
    m_impl(std::make_unique<Impl>())
{
}

OcrPack::~OcrPack()
{
    LogTraceFunction;
}

bool OcrPack::is_using_gpu()
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->applied_runtime && m_impl->applied_runtime->backend == detail::NcnnOcrBackend::Vulkan;
}

bool OcrPack::load(const std::filesystem::path& path)
{
    LogTraceFunction;
    Log.info("load", path.lexically_relative(UserDir.get()));

    std::lock_guard lock(m_impl->mutex);

    using namespace asst::utils::path_literals;

    const auto det_dir = path / "det"_p;
    const auto det_model_file = det_dir / "det.ncnn.param"_p;
    const auto det_bin_file = det_dir / "det.ncnn.bin"_p;
    const auto rec_dir = path / "rec"_p;
    const auto rec_model_file = rec_dir / "rec.ncnn.param"_p;
    const auto rec_bin_file = rec_dir / "rec.ncnn.bin"_p;
    const auto rec_label_file = rec_dir / "keys.txt"_p;

    const bool complete = std::filesystem::exists(det_model_file) && std::filesystem::exists(det_bin_file) &&
                          std::filesystem::exists(rec_model_file) && std::filesystem::exists(rec_bin_file) &&
                          std::filesystem::exists(rec_label_file);
    if (!complete) {
        m_impl->det_model_path.clear();
        m_impl->rec_model_path.clear();
        m_impl->rec_label_path.clear();
        m_impl->ncnn = nullptr;
        m_impl->applied_runtime.reset();
        m_impl->vulkan_downgraded = false;
        return false;
    }

    if (m_impl->det_model_path != det_model_file || m_impl->rec_model_path != rec_model_file ||
        m_impl->rec_label_path != rec_label_file) {
        m_impl->det_model_path = det_model_file;
        m_impl->rec_model_path = rec_model_file;
        m_impl->rec_label_path = rec_label_file;
        m_impl->ncnn = nullptr;
        m_impl->applied_runtime.reset();
    }
    m_impl->vulkan_downgraded = false;
    return true;
}

OcrPack::ResultsVec OcrPack::recognize(const cv::Mat& image, bool without_det, const std::optional<Rect>& base_roi)
{
    auto start_time = std::chrono::steady_clock::now();

    std::optional<int> requested_gpu;
    {
        std::lock_guard lock(m_impl->mutex);
        if (!m_impl->vulkan_downgraded) {
            requested_gpu = m_gpu_selector ? m_gpu_selector->resolve_device_id() : std::nullopt;
        }
    }
    const auto requested_runtime =
        detail::select_ncnn_ocr_runtime(requested_gpu, cpu_thread_count(), k_ncnn_vulkan_ocr_enabled);

    auto run = [this, &image, without_det](const detail::NcnnOcrRuntimeOptions& runtime) {
        auto session = ensure_session(runtime);
        if (!session) {
            return std::optional<ResultsVec> {};
        }
        return session->recognize(image, without_det);
    };

    auto result = run(requested_runtime);
    if (!result && requested_runtime.backend == detail::NcnnOcrBackend::Vulkan) {
        // Vulkan 失败只降级本 pack：不写进程级状态，另一个 OcrPack 仍会各自尝试 Vulkan。
        // 降级标记让后续 recognize() 直接走 CPU，避免每次调用都重新初始化 Vulkan。
        Log.warn(
            "NCNN Vulkan OCR failed, falling back to CPU for this OCR pack",
            "device",
            requested_runtime.device_id);
        {
            std::lock_guard lock(m_impl->mutex);
            m_impl->vulkan_downgraded = true;
        }
        result = run(detail::ncnn_ocr_cpu_fallback(requested_runtime));
    }
    if (!result) {
        Log.error(__FUNCTION__, "NCNN OCR inference failed");
        return {};
    }
    auto raw_results = std::move(*result);

    auto costs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
    std::string class_type = utils::demangle(typeid(*this).name());
    if (!base_roi) {
        Log.trace(class_type, raw_results, without_det ? "by OCR Rec" : "by OCR Pipeline", ", cost", costs, "ms");
    }
    else {
        std::string output = "[";
        for (size_t i = 0; i != raw_results.size(); ++i) {
            if (i != 0) {
                output += ", ";
            }
            output += std::format(
                "{{ text: {}, rect: [ {} ({}), {} ({}), {}, {} ], score: {:.6f} }}",
                raw_results[i].text,
                raw_results[i].rect.x,
                raw_results[i].rect.x + base_roi->x,
                raw_results[i].rect.y,
                raw_results[i].rect.y + base_roi->y,
                raw_results[i].rect.width,
                raw_results[i].rect.height,
                raw_results[i].score);
        }
        output += "]";
        Log.trace(class_type, output, without_det ? "by OCR Rec" : "by OCR Pipeline", ", cost", costs, "ms");
    }
    return raw_results;
}

std::shared_ptr<OcrPackNcnn> OcrPack::ensure_session(const detail::NcnnOcrRuntimeOptions& runtime)
{
    // 模型加载（文件 IO + ncnn/Vulkan 初始化）必须在配置锁内完成：session 的发布与
    // use_gpu() / use_cpu() / load() 的配置变更必须原子，否则会在配置已经变化之后
    // 发布一个过期后端的 session。代价是并发的 recognize() 只在首次加载时排队；
    // 稳态推理由 OcrPackNcnn::m_inference_mutex 串行化，不经过这把锁。
    std::lock_guard lock(m_impl->mutex);
    if (m_impl->ncnn && m_impl->ncnn->initialized() && m_impl->applied_runtime == runtime) {
        return m_impl->ncnn;
    }

    LogTraceFunction;

#ifdef __ANDROID__
    {
        // ncnn 的 powersave 是进程级执行策略：它设置调用线程的 CPU 亲和性并改写全局
        // OpenMP 线程数，影响本进程内所有 ncnn 使用者（其他 OCR pack、其他 ncnn 模型）。
        // 这里在首次加载 session 前设置一次，语义等价于"本进程的 ncnn 一律绑大核"。
        static std::mutex cpu_runtime_mutex;
        std::scoped_lock cpu_lock(cpu_runtime_mutex);
        ncnn::set_cpu_powersave(2);
    }
#endif

    m_impl->ncnn = nullptr;
    m_impl->applied_runtime.reset();
    auto session = std::make_shared<OcrPackNcnn>();
    session->set_cpu_threads(runtime.cpu_threads);
    session->set_runtime(runtime);

    const auto det_bin = std::filesystem::path(m_impl->det_model_path).replace_extension("bin");
    const auto rec_bin = std::filesystem::path(m_impl->rec_model_path).replace_extension("bin");
    const bool ok =
        session->load(m_impl->det_model_path, det_bin, m_impl->rec_model_path, rec_bin, m_impl->rec_label_path);

    Log.info(
        "ncnn ocr inited",
        ok,
        "backend",
        runtime.backend == detail::NcnnOcrBackend::Vulkan ? "Vulkan" : "CPU",
        "device",
        runtime.device_id);
    if (!ok) {
        return nullptr;
    }

    m_impl->ncnn = std::move(session);
    m_impl->applied_runtime = runtime;
    return m_impl->ncnn;
}

} // namespace asst

// ================================================================================
//  OcrPackNcnn implementation
// ================================================================================

#include <array>
#include <fstream>
#include <utility>

#include <ncnn/net.h>
#if ASST_WITH_NCNN_VULKAN_OCR && NCNN_VULKAN
#include <ncnn/gpu.h>
#endif

#include <polyclipping/clipper.hpp>

#if ASST_WITH_NCNN_VULKAN_OCR
namespace asst::detail
{
// NCNN's Vulkan instance is process-wide. NCNN registers its own exit handler
// after create_gpu_instance(), so this context intentionally does not destroy it.
class NcnnVulkanContext
{
public:
    bool initialize();
    int gpu_count();
    const char* device_name(int device_id);

private:
    std::once_flag m_once;
    bool m_initialized = false;
};

bool NcnnVulkanContext::initialize()
{
#if NCNN_VULKAN
    std::call_once(m_once, [this] { m_initialized = ncnn::create_gpu_instance() == 0; });
    return m_initialized;
#else
    return false;
#endif
}

int NcnnVulkanContext::gpu_count()
{
#if NCNN_VULKAN
    return initialize() ? ncnn::get_gpu_count() : 0;
#else
    return 0;
#endif
}

const char* NcnnVulkanContext::device_name(int device_id)
{
#if NCNN_VULKAN
    return initialize() ? ncnn::get_gpu_info(device_id).device_name() : "";
#else
    (void)device_id;
    return "";
#endif
}

NcnnVulkanContext& ncnn_vulkan_context()
{
    static NcnnVulkanContext context;
    return context;
}
}

#endif

void asst::OcrPack::use_cpu()
{
    std::lock_guard lock(m_impl->mutex);
    m_gpu_selector = std::nullopt;
    m_gpu_active = false;
    m_impl->ncnn = nullptr;
    m_impl->applied_runtime.reset();
    m_impl->vulkan_downgraded = false;
}

void asst::OcrPack::use_gpu(asst::GpuDeviceSelector selector)
{
    // 只记录请求，不在这里校验设备：真正的后端解析与可用性验证发生在
    // ensure_session()（加载模型）与 recognize()（推理）阶段。
    std::lock_guard lock(m_impl->mutex);
    m_gpu_selector = std::move(selector);
    m_impl->ncnn = nullptr;
    m_impl->applied_runtime.reset();
    m_impl->vulkan_downgraded = false;
}

namespace
{
constexpr int kDetLimitSideLen = 960;
constexpr float kDetMean[3] = { 0.485f * 255.f, 0.456f * 255.f, 0.406f * 255.f };
constexpr float kDetNorm[3] = { 1.f / (0.229f * 255.f), 1.f / (0.224f * 255.f), 1.f / (0.225f * 255.f) };

constexpr float kDetThresh = 0.3f;
constexpr float kDetBoxThresh = 0.6f;
constexpr float kDetUnclipRatio = 1.5f;
constexpr int kDetMinSize = 3;
constexpr int kDetMaxCandidates = 1000;

constexpr int kRecImgH = 48;
constexpr int kRecImgW = 320;
constexpr float kRecMean[3] = { 127.5f, 127.5f, 127.5f };
constexpr float kRecNorm[3] = { 1.f / 127.5f, 1.f / 127.5f, 1.f / 127.5f };

cv::Mat ensure_continuous_bgr(const cv::Mat& img)
{
    cv::Mat m = img;
    if (m.channels() == 4) {
        cv::cvtColor(m, m, cv::COLOR_BGRA2BGR);
    }
    else if (m.channels() == 1) {
        cv::cvtColor(m, m, cv::COLOR_GRAY2BGR);
    }
    if (!m.isContinuous()) {
        m = m.clone();
    }
    return m;
}

void order_box(const cv::Point2f in[4], cv::Point2f out[4])
{
    std::array<cv::Point2f, 4> pts = { in[0], in[1], in[2], in[3] };
    std::ranges::sort(pts, [](const cv::Point2f& a, const cv::Point2f& b) { return a.x < b.x; });
    cv::Point2f tl, bl, tr, br;
    if (pts[1].y > pts[0].y) {
        tl = pts[0];
        bl = pts[1];
    }
    else {
        tl = pts[1];
        bl = pts[0];
    }
    if (pts[3].y > pts[2].y) {
        tr = pts[2];
        br = pts[3];
    }
    else {
        tr = pts[3];
        br = pts[2];
    }
    out[0] = tl;
    out[1] = tr;
    out[2] = br;
    out[3] = bl;
}

// 对齐 fastdeploy
float poly_score(const cv::Mat& prob, const std::vector<cv::Point>& contour)
{
    const int w = prob.cols;
    const int h = prob.rows;
    int xmin = w, xmax = 0, ymin = h, ymax = 0;
    for (const auto& p : contour) {
        xmin = std::min(xmin, p.x);
        xmax = std::max(xmax, p.x);
        ymin = std::min(ymin, p.y);
        ymax = std::max(ymax, p.y);
    }
    xmin = std::clamp(xmin, 0, w - 1);
    xmax = std::clamp(xmax, 0, w - 1);
    ymin = std::clamp(ymin, 0, h - 1);
    ymax = std::clamp(ymax, 0, h - 1);
    if (xmax < xmin || ymax < ymin) {
        return 0.f;
    }
    cv::Mat mask = cv::Mat::zeros(ymax - ymin + 1, xmax - xmin + 1, CV_8UC1);
    std::vector<cv::Point> shifted;
    shifted.reserve(contour.size());
    for (const auto& p : contour) {
        shifted.emplace_back(p.x - xmin, p.y - ymin);
    }
    cv::fillPoly(mask, std::vector<std::vector<cv::Point>> { shifted }, cv::Scalar(1));
    cv::Mat region = prob(cv::Rect(xmin, ymin, xmax - xmin + 1, ymax - ymin + 1));
    return static_cast<float>(cv::mean(region, mask)[0]);
}

float poly_len(const cv::Point2f box[4])
{
    float s = 0.f;
    for (int i = 0; i < 4; ++i) {
        s += static_cast<float>(cv::norm(box[i] - box[(i + 1) % 4]));
    }
    return s;
}
} // namespace

asst::OcrPackNcnn::OcrPackNcnn() = default;

asst::OcrPackNcnn::~OcrPackNcnn() = default;

bool asst::OcrPackNcnn::load(
    const std::filesystem::path& det_param,
    const std::filesystem::path& det_bin,
    const std::filesystem::path& rec_param,
    const std::filesystem::path& rec_bin,
    const std::filesystem::path& keys_path)
{
    LogTraceFunction;

    m_loaded = false;
    m_rec.reset();
    m_det.reset();
#if ASST_WITH_NCNN_VULKAN_OCR
    if (m_runtime.backend == detail::NcnnOcrBackend::Vulkan) {
        auto& context = detail::ncnn_vulkan_context();
        if (!context.initialize()) {
            Log.error("OcrPackNcnn failed to initialize Vulkan");
            return false;
        }
        if (!detail::is_valid_ncnn_vulkan_device(m_runtime.device_id, context.gpu_count())) {
            Log.error("OcrPackNcnn invalid Vulkan device", m_runtime.device_id, "gpu_count", context.gpu_count());
            return false;
        }
        Log.info("OcrPackNcnn Vulkan device", m_runtime.device_id, context.device_name(m_runtime.device_id));
    }
#endif
    auto make_net =
        [this](const std::filesystem::path& param, const std::filesystem::path& bin) -> std::unique_ptr<ncnn::Net> {
        auto net = std::make_unique<ncnn::Net>();
#if ASST_WITH_NCNN_VULKAN_OCR
        const bool use_vulkan = m_runtime.backend == detail::NcnnOcrBackend::Vulkan;
#else
        constexpr bool use_vulkan = false;
#endif
        net->opt.use_vulkan_compute = use_vulkan;
        net->opt.num_threads = m_cpu_threads;
        net->opt.use_fp16_packed = false;
        net->opt.use_fp16_storage = false;
        net->opt.use_fp16_arithmetic = false;
#if ASST_WITH_NCNN_VULKAN_OCR && NCNN_VULKAN
        if (use_vulkan) {
            net->set_vulkan_device(m_runtime.device_id);
        }
#elif ASST_WITH_NCNN_VULKAN_OCR
        if (use_vulkan) {
            return nullptr;
        }
#endif
        if (net->load_param(platform::path_to_utf8_string(param).c_str()) != 0) {
            Log.error("OcrPackNcnn load_param failed:", param);
            return nullptr;
        }
        if (!detail::is_ncnn_ocr_runtime_applied(m_runtime, net->opt.use_vulkan_compute)) {
            Log.error("OcrPackNcnn Vulkan initialization failed:", param);
            return nullptr;
        }
        if (net->load_model(platform::path_to_utf8_string(bin).c_str()) != 0) {
            Log.error("OcrPackNcnn load_model failed:", bin);
            return nullptr;
        }
        return net;
    };

    m_det = make_net(det_param, det_bin);
    m_rec = make_net(rec_param, rec_bin);
    if (!m_det || !m_rec) {
        m_loaded = false;
        return false;
    }

    std::vector<std::string> keys;
    {
        std::ifstream ifs(keys_path, std::ios::binary);
        if (!ifs.is_open()) {
            Log.error("OcrPackNcnn open keys failed:", keys_path);
            m_loaded = false;
            return false;
        }
        std::string line;
        bool first = true;
        while (std::getline(ifs, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (first) {
                first = false;
                if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                    static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
                    line.erase(0, 3);
                }
            }
            keys.push_back(line);
        }
    }

    int num_classes = 0;
    {
        ncnn::Mat dummy(kRecImgW, kRecImgH, 3);
        dummy.fill(0.f);
        ncnn::Extractor ex = m_rec->create_extractor();
        if (ex.input("in0", dummy) != 0) {
            Log.error("OcrPackNcnn rec probe input failed");
            m_loaded = false;
            return false;
        }
        ncnn::Mat out;
        if (ex.extract("out0", out) != 0 || out.empty() || out.w <= 0 || out.h <= 0) {
            Log.error("OcrPackNcnn rec probe extract failed");
            m_loaded = false;
            return false;
        }
        num_classes = out.w;
        const float* row0 = out.row(0);
        float s = 0.f;
        float mn = row0[0];
        for (int c = 0; c < num_classes; ++c) {
            s += row0[c];
            mn = std::min(mn, row0[c]);
        }
        m_rec_output_is_prob = (mn >= -1e-6f) && (std::abs(s - 1.f) < 1e-2f);
    }

    m_charset.clear();
    m_charset.reserve(num_classes);
    m_charset.emplace_back("<blank>");
    for (auto& k : keys) {
        m_charset.emplace_back(k);
    }
    if (num_classes == static_cast<int>(m_charset.size()) + 1) {
        m_charset.emplace_back(" ");
    }
    if (num_classes != static_cast<int>(m_charset.size())) {
        Log.warn("OcrPackNcnn charset size", m_charset.size(), "!= num_classes", num_classes, "; keys", keys.size());
        if (num_classes > static_cast<int>(m_charset.size())) {
            m_charset.resize(num_classes, "<unk>");
        }
        else {
            m_charset.resize(num_classes);
        }
    }

    m_loaded = true;
    Log.info(
        "OcrPackNcnn loaded, num_classes",
        num_classes,
        "charset",
        m_charset.size(),
        "threads",
        m_cpu_threads,
        "backend",
        m_runtime.backend == detail::NcnnOcrBackend::Vulkan ? "Vulkan" : "CPU");
    return true;
}

std::optional<std::vector<asst::OcrPackNcnn::DetBox>> asst::OcrPackNcnn::detect(const cv::Mat& image) const
{
    const int src_h = image.rows;
    const int src_w = image.cols;

    float ratio = 1.f;
    if (std::max(src_h, src_w) > kDetLimitSideLen) {
        ratio = static_cast<float>(kDetLimitSideLen) / std::max(src_h, src_w);
    }
    // 与 fastdeploy 的 OcrDetectorGetInfo 对齐
    // 之前用 ceil 对齐：小 ROI 上 ceil 会过度横向拉伸（如 106→128 比 round 的 106→96 多拉 ~21%），
    // 改变文字长宽比，使 DBNet 概率脊偏扁、检测框竖向过紧而切掉小字首笔，导致 rec 误识
    // （实测「聚羽之地」被读成「袭羽之地」；改 round 后框 16px→20px，两个字段均正确且置信度更高）。
    auto align32 = [](const int v) {
        return std::max(32, static_cast<int>(std::round(v / 32.f)) * 32);
    };
    const int resize_h = align32(static_cast<int>(src_h * ratio));
    const int resize_w = align32(static_cast<int>(src_w * ratio));
    const float ratio_h = static_cast<float>(resize_h) / src_h;
    const float ratio_w = static_cast<float>(resize_w) / src_w;

    // 用 PIXEL_BGR 不转 RGB，kDetMean/kDetNorm 按 BGR 通道施加，对齐 fastdeploy
    // fastdeploy/vision/common/processors/normalize_and_permute.h (swap_rb 默认 false
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(image.data, ncnn::Mat::PIXEL_BGR, src_w, src_h, resize_w, resize_h);
    in.substract_mean_normalize(kDetMean, kDetNorm);

    ncnn::Mat out;
    {
        ncnn::Extractor ex = m_det->create_extractor();
        if (ex.input("in0", in) != 0) {
            Log.error("OcrPackNcnn det input failed");
            return std::nullopt;
        }
        if (ex.extract("out0", out) != 0 || out.empty() || out.c <= 0) {
            Log.error("OcrPackNcnn det extract failed");
            return std::nullopt;
        }
    }
    ncnn::Mat plane = out.channel(0);
    cv::Mat prob(out.h, out.w, CV_32FC1, plane.data);

    cv::Mat bitmap = prob > kDetThresh;

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

    std::vector<DetBox> results;
    const int limit = std::min(static_cast<int>(contours.size()), kDetMaxCandidates);
    for (int ci = 0; ci < limit; ++ci) {
        const auto& contour = contours[ci];
        if (contour.size() <= 2) {
            continue;
        }
        cv::RotatedRect rr = cv::minAreaRect(contour);
        if (std::max(rr.size.width, rr.size.height) < kDetMinSize) {
            continue;
        }
        cv::Point2f raw[4];
        rr.points(raw);
        cv::Point2f box[4];
        order_box(raw, box);

        const float score = poly_score(prob, contour);
        if (score < kDetBoxThresh) {
            continue;
        }

        const float area = static_cast<float>(std::abs(cv::contourArea(std::vector<cv::Point2f>(box, box + 4))));
        const float length = poly_len(box);
        if (length <= 0.f) {
            continue;
        }
        const float dist = area * kDetUnclipRatio / length;

        // 对齐 fastdeploy PostProcessor::UnClip：对 box 多边形做 jtRound 偏移再取 minAreaRect。
        // 之前的近似（RotatedRect 宽高各 +2*dist）在小字上框偏紧约 0.5~1px，叠加 det 的各向异性回缩后
        // 会切掉笔画导致 rec 误识（实测 1080P 缩放后「丰饶灌木林」被读成「丰烧濯木林」，破坏 ocrReplace 锚点；
        // 改用 Clipper 后框高 14→15px、rec 置信度 0.83→0.94，与桌面 fastdeploy / 720P 表现一致）。
        ClipperLib::Path poly;
        for (int pi = 0; pi < 4; ++pi) {
            poly << ClipperLib::IntPoint(static_cast<int>(box[pi].x), static_cast<int>(box[pi].y));
        }
        ClipperLib::ClipperOffset offset;
        offset.AddPath(poly, ClipperLib::jtRound, ClipperLib::etClosedPolygon);
        ClipperLib::Paths solution;
        offset.Execute(solution, dist);
        if (solution.empty()) {
            continue;
        }
        std::vector<cv::Point2f> expanded;
        for (const auto& sub : solution) {
            for (const auto& pt : sub) {
                expanded.emplace_back(static_cast<float>(pt.X), static_cast<float>(pt.Y));
            }
        }
        if (expanded.empty()) {
            continue;
        }
        cv::RotatedRect rr2 = cv::minAreaRect(expanded);
        if (std::max(rr2.size.width, rr2.size.height) < kDetMinSize + 2) {
            continue;
        }
        cv::Point2f raw2[4];
        rr2.points(raw2);
        DetBox db;
        order_box(raw2, db.pts);
        db.score = score;
        for (auto& p : db.pts) {
            p.x = std::clamp(p.x / ratio_w, 0.f, static_cast<float>(src_w));
            p.y = std::clamp(p.y / ratio_h, 0.f, static_cast<float>(src_h));
        }
        // 对齐 fastdeploy FilterTagDetRes：丢弃过细框
        const int rect_w = static_cast<int>(cv::norm(db.pts[0] - db.pts[1]));
        const int rect_h = static_cast<int>(cv::norm(db.pts[0] - db.pts[3]));
        if (rect_w <= 4 || rect_h <= 4) {
            continue;
        }
        results.push_back(db);
    }

    std::ranges::sort(results, [](const DetBox& a, const DetBox& b) {
        auto top = [](const DetBox& d) {
            return std::min({ d.pts[0].y, d.pts[1].y, d.pts[2].y, d.pts[3].y });
        };
        auto left = [](const DetBox& d) {
            return std::min({ d.pts[0].x, d.pts[1].x, d.pts[2].x, d.pts[3].x });
        };
        const int ra = static_cast<int>(std::lround(top(a) / 10.f));
        const int rb = static_cast<int>(std::lround(top(b) / 10.f));
        if (ra != rb) {
            return ra < rb;
        }
        return left(a) < left(b);
    });
    return results;
}

cv::Mat asst::OcrPackNcnn::crop_rotated(const cv::Mat& image, const DetBox& box)
{
    const int w =
        std::max(1, static_cast<int>(std::max(cv::norm(box.pts[0] - box.pts[1]), cv::norm(box.pts[2] - box.pts[3]))));
    const int h =
        std::max(1, static_cast<int>(std::max(cv::norm(box.pts[0] - box.pts[3]), cv::norm(box.pts[1] - box.pts[2]))));

    const cv::Point2f src[4] = { box.pts[0], box.pts[1], box.pts[2], box.pts[3] };
    const cv::Point2f dst[4] = { { 0, 0 },
                                 { static_cast<float>(w), 0 },
                                 { static_cast<float>(w), static_cast<float>(h) },
                                 { 0, static_cast<float>(h) } };
    const cv::Mat m = cv::getPerspectiveTransform(src, dst);
    cv::Mat crop;
    // 对齐 fastdeploy GetRotateCropImage BORDER_REPLICATE + h>=1.5w 旋转
    cv::warpPerspective(image, crop, m, cv::Size(w, h), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    if (static_cast<float>(h) >= static_cast<float>(w) * 1.5f) {
        cv::rotate(crop, crop, cv::ROTATE_90_COUNTERCLOCKWISE);
    }
    return crop;
}

std::optional<std::pair<std::string, float>> asst::OcrPackNcnn::recognize_line(const cv::Mat& line_in) const
{
    cv::Mat line = ensure_continuous_bgr(line_in);
    if (line.empty()) {
        return std::pair { std::string(), 0.f };
    }

    const float r = line.rows > 0 ? static_cast<float>(line.cols) / line.rows : 1.f;
    int resized_w = std::clamp(static_cast<int>(std::ceil(kRecImgH * r)), 1, kRecImgW);
    cv::Mat resized;
    cv::resize(line, resized, cv::Size(resized_w, kRecImgH));

    // 对齐 fastdeploy
    cv::Mat canvas(kRecImgH, kRecImgW, CV_8UC3, cv::Scalar(127, 127, 127));
    resized.copyTo(canvas(cv::Rect(0, 0, resized_w, kRecImgH)));

    // 同 det，用 PIXEL_BGR 不转 RGB，对齐 fastdeploy rec_preprocessor.cc
    ncnn::Mat in = ncnn::Mat::from_pixels(canvas.data, ncnn::Mat::PIXEL_BGR, kRecImgW, kRecImgH);
    in.substract_mean_normalize(kRecMean, kRecNorm);

    ncnn::Mat out;
    {
        ncnn::Extractor ex = m_rec->create_extractor();
        if (ex.input("in0", in) != 0) {
            Log.error("OcrPackNcnn rec input failed");
            return std::nullopt;
        }
        if (ex.extract("out0", out) != 0 || out.empty() || out.w <= 0 || out.h <= 0) {
            Log.error("OcrPackNcnn rec extract failed");
            return std::nullopt;
        }
    }
    const int T = out.h;
    const int nc = out.w;

    std::string text;
    float conf_sum = 0.f;
    int conf_cnt = 0;
    int prev = -1;
    for (int t = 0; t < T; ++t) {
        const float* row = out.row(t);
        int best = 0;
        float best_v = row[0];
        for (int c = 1; c < nc; ++c) {
            if (row[c] > best_v) {
                best_v = row[c];
                best = c;
            }
        }
        float conf;
        if (m_rec_output_is_prob) {
            conf = best_v;
        }
        else {
            float denom = 0.f;
            for (int c = 0; c < nc; ++c) {
                denom += std::exp(row[c] - best_v);
            }
            conf = denom > 0.f ? 1.f / denom : 0.f;
        }
        if (best != 0 && best != prev) {
            if (best < static_cast<int>(m_charset.size())) {
                text += m_charset[best];
                conf_sum += conf;
                ++conf_cnt;
            }
        }
        prev = best;
    }

    const float mean_conf = conf_cnt > 0 ? conf_sum / conf_cnt : 0.f;
    return std::pair { std::move(text), mean_conf };
}

std::optional<asst::OcrPackNcnn::ResultsVec> asst::OcrPackNcnn::recognize(const cv::Mat& image_in, bool without_det)
{
    if (!m_loaded) {
        return std::nullopt;
    }

    std::lock_guard inference_lock(m_inference_mutex);

    cv::Mat image = ensure_continuous_bgr(image_in);
    if (image.empty()) {
        return ResultsVec {};
    }

    ResultsVec results;

    if (without_det) {
        auto line = recognize_line(image);
        if (!line) {
            return std::nullopt;
        }
        auto [text, score] = std::move(*line);
        results.emplace_back(Rect(0, 0, image.cols, image.rows), score, std::move(text));
        return results;
    }

    auto boxes = detect(image);
    if (!boxes) {
        return std::nullopt;
    }
    if (boxes->empty()) {
        // 对齐 fastdeploy PPOCRv2/v3 det 无框时整图当一行送 rec 小 roi 不处理直接就爆了
        auto line = recognize_line(image);
        if (!line) {
            return std::nullopt;
        }
        auto [text, score] = std::move(*line);
        results.emplace_back(Rect(0, 0, image.cols, image.rows), score, std::move(text));
        return results;
    }
    for (const auto& box : *boxes) {
        cv::Mat crop = crop_rotated(image, box);
        if (crop.empty()) {
            continue;
        }
        auto line = recognize_line(crop);
        if (!line) {
            return std::nullopt;
        }
        auto [text, score] = std::move(*line);
        const float xs[4] = { box.pts[0].x, box.pts[1].x, box.pts[2].x, box.pts[3].x };
        const float ys[4] = { box.pts[0].y, box.pts[1].y, box.pts[2].y, box.pts[3].y };
        const int left = static_cast<int>(*std::min_element(xs, xs + 4));
        const int right = static_cast<int>(*std::max_element(xs, xs + 4));
        const int top = static_cast<int>(*std::min_element(ys, ys + 4));
        const int bottom = static_cast<int>(*std::max_element(ys, ys + 4));
        results.emplace_back(Rect(left, top, right - left, bottom - top), score, std::move(text));
    }
    return results;
}

#endif // ASST_WITH_NCNN
