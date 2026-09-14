#pragma once

#include <optional>

namespace asst::detail
{

enum class NcnnOcrBackend
{
    Cpu,
    Vulkan,
};

struct NcnnOcrRuntimeOptions
{
    NcnnOcrBackend backend = NcnnOcrBackend::Cpu;
    int device_id = 0;
    int cpu_threads = 1;

    bool operator==(const NcnnOcrRuntimeOptions&) const = default;
};

// 把"请求的 GPU 设备"解析成实际使用的运行时配置。
// 没有 Vulkan OCR 路径的构建（Android）接受请求但一律解析为 CPU，
// 与该平台此前的行为一致：GPU 选项不会让 OCR 失败。
constexpr NcnnOcrRuntimeOptions
    select_ncnn_ocr_runtime(std::optional<int> requested_gpu, int cpu_threads, bool vulkan_enabled)
{
    const auto backend = vulkan_enabled && requested_gpu ? NcnnOcrBackend::Vulkan : NcnnOcrBackend::Cpu;
    return NcnnOcrRuntimeOptions {
        .backend = backend,
        .device_id = backend == NcnnOcrBackend::Vulkan ? requested_gpu.value_or(0) : 0,
        .cpu_threads = cpu_threads > 0 ? cpu_threads : 1,
    };
}

constexpr bool is_valid_ncnn_vulkan_device(int device_id, int gpu_count)
{
    return device_id >= 0 && device_id < gpu_count;
}

constexpr bool is_ncnn_ocr_runtime_applied(const NcnnOcrRuntimeOptions& runtime, bool use_vulkan_compute)
{
    return runtime.backend != NcnnOcrBackend::Vulkan || use_vulkan_compute;
}

// Fallback is selected by an OCR session after its own Vulkan request fails.
constexpr NcnnOcrRuntimeOptions ncnn_ocr_cpu_fallback(const NcnnOcrRuntimeOptions& failed_runtime)
{
    if (failed_runtime.backend != NcnnOcrBackend::Vulkan) {
        return failed_runtime;
    }
    return NcnnOcrRuntimeOptions {
        .backend = NcnnOcrBackend::Cpu,
        .device_id = 0,
        .cpu_threads = failed_runtime.cpu_threads,
    };
}
}
