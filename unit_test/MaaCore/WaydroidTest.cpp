#include <catch2/catch_test_macros.hpp>

#include "Controller/Waydroid/PipeWireABI.h"
#include "Controller/Waydroid/SpaPod.h"
#include "Controller/Waydroid/WindowMatcher.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{
uint32_t read_u32(const uint8_t* data)
{
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

template <size_t N>
uint32_t pod_u32(const std::array<uint8_t, N>& buffer, size_t offset)
{
    return read_u32(buffer.data() + offset);
}
} // namespace

TEST_CASE("waydroid::build_format_pod emits a valid EnumFormat object", "[waydroid]")
{
    std::array<uint8_t, 256> buffer {};
    const size_t size = asst::waydroid::build_format_pod(buffer.data(), buffer.size(), 1280, 720);

    REQUIRE(size == 168);
    REQUIRE(pod_u32(buffer, 0) == 160);
    REQUIRE(pod_u32(buffer, 4) == 15); // SPA_TYPE_Object
    REQUIRE(pod_u32(buffer, 8) == asst::waydroid::pwabi::kObjectFormat);
    REQUIRE(pod_u32(buffer, 12) == asst::waydroid::pwabi::kParamEnumFormat);

    // mediaType = video
    REQUIRE(pod_u32(buffer, 16) == asst::waydroid::pwabi::kFormatMediaType);
    REQUIRE(pod_u32(buffer, 28) == asst::waydroid::pwabi::kTypeId);
    REQUIRE(pod_u32(buffer, 32) == asst::waydroid::pwabi::kMediaTypeVideo);

    // mediaSubtype = raw
    REQUIRE(pod_u32(buffer, 40) == asst::waydroid::pwabi::kFormatMediaSubtype);
    REQUIRE(pod_u32(buffer, 56) == asst::waydroid::pwabi::kMediaSubtypeRaw);

    // videoFormat = enum { BGRx, BGRA, RGBx, RGBA }
    REQUIRE(pod_u32(buffer, 64) == asst::waydroid::pwabi::kFormatVideoFormat);
    REQUIRE(pod_u32(buffer, 76) == asst::waydroid::pwabi::kTypeChoice);
    REQUIRE(pod_u32(buffer, 80) == asst::waydroid::pwabi::kChoiceEnum);
    REQUIRE(pod_u32(buffer, 96) == asst::waydroid::pwabi::kVideoFormatBGRx);
    REQUIRE(pod_u32(buffer, 100) == asst::waydroid::pwabi::kVideoFormatBGRA);
    REQUIRE(pod_u32(buffer, 104) == asst::waydroid::pwabi::kVideoFormatRGBx);
    REQUIRE(pod_u32(buffer, 108) == asst::waydroid::pwabi::kVideoFormatRGBA);

    // videoSize = range(default 1280x720, 1x1 .. 8192x8192)
    REQUIRE(pod_u32(buffer, 112) == asst::waydroid::pwabi::kFormatVideoSize);
    REQUIRE(pod_u32(buffer, 124) == asst::waydroid::pwabi::kTypeChoice);
    REQUIRE(pod_u32(buffer, 128) == asst::waydroid::pwabi::kChoiceRange);
    REQUIRE(pod_u32(buffer, 144) == 1280);
    REQUIRE(pod_u32(buffer, 148) == 720);
    REQUIRE(pod_u32(buffer, 152) == 1);
    REQUIRE(pod_u32(buffer, 156) == 1);
    REQUIRE(pod_u32(buffer, 160) == 8192);
    REQUIRE(pod_u32(buffer, 164) == 8192);
}

TEST_CASE("waydroid::build_format_pod rejects short buffers", "[waydroid]")
{
    std::array<uint8_t, 64> buffer {};
    REQUIRE(asst::waydroid::build_format_pod(buffer.data(), buffer.size(), 1280, 720) == 0);
}

TEST_CASE("waydroid::build_buffers_pod requests CPU-mappable memory", "[waydroid]")
{
    std::array<uint8_t, 128> buffer {};
    const size_t size = asst::waydroid::build_buffers_pod(buffer.data(), buffer.size());

    REQUIRE(size == 104);
    REQUIRE(pod_u32(buffer, 0) == 96);
    REQUIRE(pod_u32(buffer, 4) == 15);
    REQUIRE(pod_u32(buffer, 8) == asst::waydroid::pwabi::kObjectParamBuffers);
    REQUIRE(pod_u32(buffer, 12) == asst::waydroid::pwabi::kParamBuffers);

    // buffers = range(default 8, 2 .. 32)
    REQUIRE(pod_u32(buffer, 16) == asst::waydroid::pwabi::kParamBuffersBuffers);
    REQUIRE(pod_u32(buffer, 32) == asst::waydroid::pwabi::kChoiceRange);
    REQUIRE(pod_u32(buffer, 48) == 8);
    REQUIRE(pod_u32(buffer, 52) == 2);
    REQUIRE(pod_u32(buffer, 56) == 32);

    // dataType = flags { MemPtr, MemFd }
    REQUIRE(pod_u32(buffer, 64) == asst::waydroid::pwabi::kParamBuffersDataType);
    REQUIRE(pod_u32(buffer, 80) == asst::waydroid::pwabi::kChoiceFlags);
    const uint32_t expected = (1U << asst::waydroid::pwabi::kDataMemPtr) | (1U << asst::waydroid::pwabi::kDataMemFd);
    REQUIRE(pod_u32(buffer, 96) == expected);
}

TEST_CASE("waydroid::parse_stream_format reads enum and range pods", "[waydroid]")
{
    std::array<uint8_t, 256> buffer {};
    REQUIRE(asst::waydroid::build_format_pod(buffer.data(), buffer.size(), 1920, 1080) != 0);

    const auto format =
        asst::waydroid::parse_stream_format(reinterpret_cast<const asst::waydroid::pwabi::SpaPod*>(buffer.data()));
    REQUIRE(format.has_value());
    REQUIRE(format->width == 1920);
    REQUIRE(format->height == 1080);
    REQUIRE(format->format == asst::waydroid::PixelFormat::BGRX);
}

TEST_CASE("waydroid::parse_stream_format rejects unknown formats", "[waydroid]")
{
    SECTION("null pod")
    {
        REQUIRE_FALSE(asst::waydroid::parse_stream_format(nullptr).has_value());
    }

    SECTION("unknown video format")
    {
        std::array<uint8_t, 256> buffer {};
        REQUIRE(asst::waydroid::build_format_pod(buffer.data(), buffer.size(), 1280, 720) != 0);
        const uint32_t unknown = 9999;
        std::memcpy(buffer.data() + 96, &unknown, sizeof(unknown));
        REQUIRE_FALSE(
            asst::waydroid::parse_stream_format(reinterpret_cast<const asst::waydroid::pwabi::SpaPod*>(buffer.data()))
                .has_value());
    }
}

TEST_CASE("waydroid::read_video_transform finds the metadata entry", "[waydroid]")
{
    uint32_t transform = 3;
    asst::waydroid::pwabi::SpaMeta metas[2] = {
        { .type = asst::waydroid::pwabi::kMetaVideoTransform, .size = sizeof(uint32_t), .data = &transform },
        { .type = 0, .size = 0, .data = nullptr },
    };
    asst::waydroid::pwabi::SpaBuffer buffer {
        .n_metas = 2,
        .n_datas = 0,
        .metas = metas,
        .datas = nullptr,
    };
    REQUIRE(asst::waydroid::read_video_transform(buffer) == 3);

    buffer.metas = nullptr;
    buffer.n_metas = 0;
    REQUIRE(asst::waydroid::read_video_transform(buffer) == 0);
}

TEST_CASE("waydroid window matching", "[waydroid]")
{
    using asst::waydroid::is_app_window_class;
    using asst::waydroid::is_waydroid_pid;
    using asst::waydroid::is_waydroid_window;
    using asst::waydroid::to_lower;
    using asst::waydroid::window_score;

    REQUIRE(to_lower("WaYdRoId") == "waydroid");

    REQUIRE(is_waydroid_window("Waydroid", "Waydroid", 0, ""));
    REQUIRE(is_waydroid_window("waydroid", "", 0, ""));
    REQUIRE(is_waydroid_window("waydroid.com.hypergryph.arknights", "", 0, ""));
    REQUIRE_FALSE(is_waydroid_window("firefox", "Firefox", 0, ""));
    REQUIRE_FALSE(is_waydroid_pid(0));
    REQUIRE_FALSE(is_waydroid_pid(-1));

    SECTION("selector matches class or caption case-insensitively")
    {
        REQUIRE(is_waydroid_window("firefox", "Waydroid", 0, "waydroid"));
        REQUIRE(is_waydroid_window("waydroid.com.example", "Example", 0, "COM.EXAMPLE"));
        REQUIRE_FALSE(is_waydroid_window("firefox", "Firefox", 0, "waydroid"));
    }

    SECTION("app windows outrank the full UI shell")
    {
        REQUIRE(is_app_window_class("waydroid.com.example"));
        REQUIRE_FALSE(is_app_window_class("Waydroid"));
        REQUIRE(window_score(false, "waydroid.com.example") > window_score(false, "Waydroid"));
        REQUIRE(window_score(false, "Waydroid") > window_score(true, "Waydroid"));
    }
}
