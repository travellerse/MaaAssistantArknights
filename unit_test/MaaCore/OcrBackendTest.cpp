#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "Config/Miscellaneous/OcrPackNcnnRuntime.h"

TEST_CASE("NCNN OCR backend selects requested compute device")
{
    using asst::detail::NcnnOcrBackend;
    using asst::detail::select_ncnn_ocr_runtime;

    const auto cpu = select_ncnn_ocr_runtime(std::nullopt, 0, true);
    REQUIRE(cpu.backend == NcnnOcrBackend::Cpu);
    REQUIRE(cpu.device_id == 0);
    REQUIRE(cpu.cpu_threads == 1);

    const auto gpu = select_ncnn_ocr_runtime(2, 4, true);
    REQUIRE(gpu.backend == NcnnOcrBackend::Vulkan);
    REQUIRE(gpu.device_id == 2);
    REQUIRE(gpu.cpu_threads == 4);

    const auto cpu_only = select_ncnn_ocr_runtime(2, 4, false);
    REQUIRE(cpu_only.backend == NcnnOcrBackend::Cpu);
    REQUIRE(cpu_only.device_id == 0);
    REQUIRE(cpu_only.cpu_threads == 4);
}

TEST_CASE("NCNN OCR validates Vulkan device indices")
{
    using asst::detail::is_valid_ncnn_vulkan_device;

    REQUIRE(is_valid_ncnn_vulkan_device(0, 1));
    REQUIRE(is_valid_ncnn_vulkan_device(2, 3));
    REQUIRE_FALSE(is_valid_ncnn_vulkan_device(-1, 1));
    REQUIRE_FALSE(is_valid_ncnn_vulkan_device(1, 1));
    REQUIRE_FALSE(is_valid_ncnn_vulkan_device(9999, 1));
    REQUIRE_FALSE(is_valid_ncnn_vulkan_device(0, 0));
}

TEST_CASE("NCNN OCR rejects an implicit Vulkan to CPU downgrade")
{
    using asst::detail::is_ncnn_ocr_runtime_applied;
    using asst::detail::select_ncnn_ocr_runtime;

    const auto gpu = select_ncnn_ocr_runtime(0, 2, true);
    REQUIRE(is_ncnn_ocr_runtime_applied(gpu, true));
    REQUIRE_FALSE(is_ncnn_ocr_runtime_applied(gpu, false));

    const auto cpu = select_ncnn_ocr_runtime(std::nullopt, 2, true);
    REQUIRE(is_ncnn_ocr_runtime_applied(cpu, false));
}

TEST_CASE("NCNN OCR CPU fallback is derived from the failed session request")
{
    using asst::detail::ncnn_ocr_cpu_fallback;
    using asst::detail::NcnnOcrBackend;
    using asst::detail::select_ncnn_ocr_runtime;

    const auto failed = select_ncnn_ocr_runtime(3, 5, true);
    const auto fallback = ncnn_ocr_cpu_fallback(failed);
    REQUIRE(fallback.backend == NcnnOcrBackend::Cpu);
    REQUIRE(fallback.device_id == 0);
    REQUIRE(fallback.cpu_threads == 5);

    const auto cpu = select_ncnn_ocr_runtime(std::nullopt, 5, true);
    REQUIRE(ncnn_ocr_cpu_fallback(cpu) == cpu);
}
