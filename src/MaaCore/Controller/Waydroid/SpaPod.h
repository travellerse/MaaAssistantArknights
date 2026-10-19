#pragma once

#include "CaptureBackend.h"
#include "PipeWireABI.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace asst::waydroid
{
struct StreamFormat
{
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::BGRX;
};

// Byte-level SPA pod builders. They do not depend on the PipeWire runtime so
// the layout can be unit-tested without libpipewire.
size_t build_format_pod(uint8_t* output, size_t capacity, int width, int height);
size_t build_buffers_pod(uint8_t* output, size_t capacity);

std::optional<StreamFormat> parse_stream_format(const pwabi::SpaPod* param);
uint32_t read_video_transform(const pwabi::SpaBuffer& buffer);
} // namespace asst::waydroid
