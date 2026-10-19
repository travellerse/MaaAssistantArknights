#include "SpaPod.h"

#include <cstring>

namespace asst::waydroid
{
namespace
{
uint32_t read_u32(const uint8_t* data)
{
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

void write_u32(uint8_t*& cursor, uint32_t value)
{
    std::memcpy(cursor, &value, sizeof(value));
    cursor += sizeof(value);
}

size_t align8(size_t value)
{
    return (value + 7U) & ~size_t { 7U };
}
} // namespace

// Builds the EnumFormat spa_pod without depending on PipeWire headers.
// NOLINTNEXTLINE(readability-function-size) — byte-level SPA pod layout
size_t build_format_pod(uint8_t* output, size_t capacity, int width, int height)
{
    constexpr size_t kPodSize = 168;
    if (capacity < kPodSize) {
        return 0;
    }

    uint8_t* const start = output;
    uint8_t* cursor = start + 8;
    write_u32(cursor, pwabi::kObjectFormat);
    write_u32(cursor, pwabi::kParamEnumFormat);

    write_u32(cursor, pwabi::kFormatMediaType);
    write_u32(cursor, 0);
    write_u32(cursor, 4);
    write_u32(cursor, pwabi::kTypeId);
    write_u32(cursor, pwabi::kMediaTypeVideo);
    cursor = start + align8(static_cast<size_t>(cursor - start));

    write_u32(cursor, pwabi::kFormatMediaSubtype);
    write_u32(cursor, 0);
    write_u32(cursor, 4);
    write_u32(cursor, pwabi::kTypeId);
    write_u32(cursor, pwabi::kMediaSubtypeRaw);
    cursor = start + align8(static_cast<size_t>(cursor - start));

    write_u32(cursor, pwabi::kFormatVideoFormat);
    write_u32(cursor, 0);
    write_u32(cursor, 32);
    write_u32(cursor, pwabi::kTypeChoice);
    write_u32(cursor, pwabi::kChoiceEnum);
    write_u32(cursor, 0);
    write_u32(cursor, 4);
    write_u32(cursor, pwabi::kTypeId);
    write_u32(cursor, pwabi::kVideoFormatBGRx);
    write_u32(cursor, pwabi::kVideoFormatBGRA);
    write_u32(cursor, pwabi::kVideoFormatRGBx);
    write_u32(cursor, pwabi::kVideoFormatRGBA);
    cursor = start + align8(static_cast<size_t>(cursor - start));

    const uint32_t default_width = width > 0 ? static_cast<uint32_t>(width) : 1280;
    const uint32_t default_height = height > 0 ? static_cast<uint32_t>(height) : 720;
    write_u32(cursor, pwabi::kFormatVideoSize);
    write_u32(cursor, 0);
    write_u32(cursor, 40);
    write_u32(cursor, pwabi::kTypeChoice);
    write_u32(cursor, pwabi::kChoiceRange);
    write_u32(cursor, 0);
    write_u32(cursor, 8);
    write_u32(cursor, pwabi::kTypeRectangle);
    write_u32(cursor, default_width);
    write_u32(cursor, default_height);
    write_u32(cursor, 1);
    write_u32(cursor, 1);
    write_u32(cursor, 8192);
    write_u32(cursor, 8192);
    cursor = start + align8(static_cast<size_t>(cursor - start));

    const auto total_size = static_cast<size_t>(cursor - start);
    const auto pod_size = static_cast<uint32_t>(total_size - 8);
    std::memcpy(start, &pod_size, sizeof(pod_size));
    constexpr auto kPodTypeObject = uint32_t { 15 };
    std::memcpy(start + 4, &kPodTypeObject, sizeof(kPodTypeObject));
    return total_size;
}

// Asks for CPU-mappable buffers (MemFd or MemPtr). Without this some
// compositors hand out dmabuf, which this backend cannot copy from.
size_t build_buffers_pod(uint8_t* output, size_t capacity)
{
    constexpr size_t kPodSize = 104;
    if (capacity < kPodSize) {
        return 0;
    }

    uint8_t* const start = output;
    uint8_t* cursor = start + 8;
    write_u32(cursor, pwabi::kObjectParamBuffers);
    write_u32(cursor, pwabi::kParamBuffers);

    write_u32(cursor, pwabi::kParamBuffersBuffers);
    write_u32(cursor, 0);
    write_u32(cursor, 28);
    write_u32(cursor, pwabi::kTypeChoice);
    write_u32(cursor, pwabi::kChoiceRange);
    write_u32(cursor, 0);
    write_u32(cursor, 4);
    write_u32(cursor, pwabi::kTypeInt);
    write_u32(cursor, 8);
    write_u32(cursor, 2);
    write_u32(cursor, 32);
    cursor = start + align8(static_cast<size_t>(cursor - start));

    write_u32(cursor, pwabi::kParamBuffersDataType);
    write_u32(cursor, 0);
    write_u32(cursor, 20);
    write_u32(cursor, pwabi::kTypeChoice);
    write_u32(cursor, pwabi::kChoiceFlags);
    write_u32(cursor, 0);
    write_u32(cursor, 4);
    write_u32(cursor, pwabi::kTypeInt);
    write_u32(cursor, (1U << pwabi::kDataMemPtr) | (1U << pwabi::kDataMemFd));
    cursor = start + align8(static_cast<size_t>(cursor - start));

    const auto total_size = static_cast<size_t>(cursor - start);
    const auto pod_size = static_cast<uint32_t>(total_size - 8);
    std::memcpy(start, &pod_size, sizeof(pod_size));
    constexpr auto kPodTypeObject = uint32_t { 15 };
    std::memcpy(start + 4, &kPodTypeObject, sizeof(kPodTypeObject));
    return total_size;
}

// NOLINTNEXTLINE(readability-function-size) — byte-level SPA pod walk
std::optional<StreamFormat> parse_stream_format(const pwabi::SpaPod* param)
{
    if (param == nullptr) {
        return std::nullopt;
    }
    const auto* object = reinterpret_cast<const pwabi::SpaPodObject*>(param);
    if (object->pod.type != 15 || object->body.type != pwabi::kObjectFormat) {
        return std::nullopt;
    }

    const auto* base = reinterpret_cast<const uint8_t*>(param);
    const size_t total = 8 + object->pod.size;
    size_t offset = sizeof(pwabi::SpaPodObject);

    uint32_t video_format = 0;
    StreamFormat result;
    while (offset + 16 <= total) {
        const uint8_t* prop = base + offset;
        const uint32_t key = read_u32(prop);
        const uint32_t value_size = read_u32(prop + 8);
        const uint32_t value_type = read_u32(prop + 12);
        if (key == pwabi::kFormatVideoFormat) {
            if (value_type == pwabi::kTypeId) {
                video_format = read_u32(prop + 16);
            }
            else if (value_type == pwabi::kTypeChoice) {
                video_format = read_u32(prop + 32);
            }
        }
        else if (key == pwabi::kFormatVideoSize) {
            if (value_type == pwabi::kTypeRectangle) {
                result.width = static_cast<int>(read_u32(prop + 16));
                result.height = static_cast<int>(read_u32(prop + 20));
            }
            else if (value_type == pwabi::kTypeChoice) {
                result.width = static_cast<int>(read_u32(prop + 32));
                result.height = static_cast<int>(read_u32(prop + 36));
            }
        }
        offset += 8 + align8(8 + value_size);
    }

    switch (video_format) {
    case pwabi::kVideoFormatBGRx:
        result.format = PixelFormat::BGRX;
        break;
    case pwabi::kVideoFormatBGRA:
        result.format = PixelFormat::BGRA;
        break;
    case pwabi::kVideoFormatRGBx:
        result.format = PixelFormat::RGBX;
        break;
    case pwabi::kVideoFormatRGBA:
        result.format = PixelFormat::RGBA;
        break;
    default:
        return std::nullopt;
    }
    if (result.width <= 0 || result.height <= 0) {
        return std::nullopt;
    }
    return result;
}

uint32_t read_video_transform(const pwabi::SpaBuffer& buffer)
{
    if (buffer.metas == nullptr) {
        return 0;
    }
    for (uint32_t index = 0; index < buffer.n_metas; ++index) {
        const pwabi::SpaMeta& meta = buffer.metas[index];
        if (meta.type == pwabi::kMetaVideoTransform && meta.data != nullptr && meta.size >= sizeof(uint32_t)) {
            return *static_cast<const uint32_t*>(meta.data);
        }
    }
    return 0;
}
} // namespace asst::waydroid
