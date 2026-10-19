#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace asst::waydroid
{
enum class PixelFormat : uint8_t
{
    RGBA,
    BGRA,
    RGBX,
    BGRX,
};

struct Frame
{
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::RGBA;
    std::vector<uint8_t> data;
    uint64_t id = 0;
};

class CaptureBackend
{
public:
    CaptureBackend() = default;
    virtual ~CaptureBackend() = default;

    CaptureBackend(const CaptureBackend&) = delete;
    CaptureBackend& operator=(const CaptureBackend&) = delete;
    CaptureBackend(CaptureBackend&&) = delete;
    CaptureBackend& operator=(CaptureBackend&&) = delete;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    virtual bool start(int width, int height, std::string_view selector) = 0;
    virtual std::optional<Frame> capture() = 0;
    virtual void stop() noexcept = 0;
};
} // namespace asst::waydroid
