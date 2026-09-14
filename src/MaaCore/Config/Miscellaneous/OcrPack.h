#pragma once

#include "Common/AsstTypes.h"
#include "Config/AbstractResource.h"
#include "Config/GpuDeviceSelector.h"
#include "Config/Miscellaneous/OcrPackNcnnRuntime.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace cv
{
class Mat;
}

namespace asst
{
class OcrPackNcnn;

class OcrPack : public AbstractResource
{
public:
    using Result = TextRect;
    using ResultsVec = std::vector<Result>;

public:
    virtual ~OcrPack() override;

    virtual bool load(const std::filesystem::path& path) override;

    void use_cpu();

    // 记录 GPU 请求。实际后端在加载 OCR session 时解析，可用性在推理时验证：
    // 请求成功不代表 Vulkan OCR 一定能跑起来。Vulkan 失败只会把当前 pack 降级到 CPU。
    void use_gpu(GpuDeviceSelector selector);

    // 当前实际是否使用 GPU 后端（仅供诊断/基准测试读取）。
    bool is_using_gpu();

    ResultsVec recognize(const cv::Mat& image, bool without_det = false, const std::optional<Rect>& base_roi = {});

protected:
    OcrPack();

    // 两个后端各实现其中一个，且每个目标只会编译其中一个实现
    // （见 src/MaaCore/CMakeLists.txt 对 OcrPack.cpp / OcrPackNcnn.cpp 的过滤）。
    // 两者都在这里声明，避免头文件依赖私有编译宏、以及不同 TU 看到不同的类定义。
    //   ncnn       -> ensure_session()：返回可用的 OCR session，加载失败返回 nullptr
    //   fastdeploy -> check_and_load()：按需创建 PPOCR pipeline
    std::shared_ptr<OcrPackNcnn> ensure_session(const detail::NcnnOcrRuntimeOptions& runtime);
    bool check_and_load();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::optional<GpuDeviceSelector> m_gpu_selector = std::nullopt;
    bool m_gpu_active = false;
};

class WordOcr final : public MAA_NS::SingletonHolder<WordOcr>, public OcrPack
{
    friend class MAA_NS::SingletonHolder<WordOcr>;
};

class CharOcr final : public MAA_NS::SingletonHolder<CharOcr>, public OcrPack
{
    friend class MAA_NS::SingletonHolder<CharOcr>;
};
}
