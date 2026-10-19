#pragma once

// Minimal ABI subset of PipeWire/SPA. The client libraries are loaded with
// dlopen so that MaaCore keeps building against MaaDeps without adding a
// PipeWire dependency. These layouts are part of the stable SPA ABI; the
// values are cross-checked against the real headers by
// unit_test/MaaCore/WaydroidAbiTest.cpp when they are installed.

#include <array>
#include <cstddef>
#include <cstdint>

namespace asst::waydroid::pwabi
{
struct SpaPod
{
    uint32_t size;
    uint32_t type;
};

struct SpaPodProp
{
    uint32_t key;
    uint32_t flags;
    SpaPod value;
};

struct SpaPodObjectBody
{
    uint32_t type;
    uint32_t id;
};

struct SpaPodObject
{
    SpaPod pod;
    SpaPodObjectBody body;
};

struct SpaPodChoiceBody
{
    uint32_t type;
    uint32_t flags;
    SpaPod child;
};

struct SpaPodChoice
{
    SpaPod pod;
    SpaPodChoiceBody body;
};

struct SpaPodId
{
    SpaPod pod;
    uint32_t value;
};

struct SpaPodRectangle
{
    SpaPod pod;
    int32_t width;
    int32_t height;
};

struct SpaChunk
{
    uint32_t offset;
    uint32_t size;
    int32_t stride;
    int32_t flags;
};

struct SpaData
{
    uint32_t type;
    uint32_t flags;
    int64_t fd;
    uint32_t mapoffset;
    uint32_t maxsize;
    void* data;
    SpaChunk* chunk;
};

struct SpaMeta
{
    uint32_t type;
    uint32_t size;
    void* data;
};

struct SpaBuffer
{
    uint32_t n_metas;
    uint32_t n_datas;
    SpaMeta* metas;
    SpaData* datas;
};

struct PwBuffer
{
    SpaBuffer* buffer;
};

struct PwStreamEvents
{
    uint32_t version;
    void (*destroy)(void*);
    void (*state_changed)(void*, int, int, const char*);
    void (*control_info)(void*, uint32_t, const void*);
    void (*io_changed)(void*, uint32_t, void*, uint32_t);
    void (*param_changed)(void*, uint32_t, const SpaPod*);
    void (*add_buffer)(void*, void*);
    void (*remove_buffer)(void*, void*);
    void (*process)(void*);
    void (*drained)(void*);
    void (*command)(void*, const void*);
    void (*trigger_done)(void*);
};

// spa/utils/type.h
inline constexpr uint32_t kTypeId = 3;
inline constexpr uint32_t kTypeInt = 4;
inline constexpr uint32_t kTypeRectangle = 10;
inline constexpr uint32_t kTypeChoice = 19;
inline constexpr uint32_t kObjectFormat = 0x40003;
inline constexpr uint32_t kObjectParamBuffers = 0x40004;

// spa/param/param.h
inline constexpr uint32_t kParamEnumFormat = 3;
inline constexpr uint32_t kParamFormat = 4;
inline constexpr uint32_t kParamBuffers = 5;

// spa/param/buffers.h
inline constexpr uint32_t kParamBuffersBuffers = 1;
inline constexpr uint32_t kParamBuffersDataType = 6;

// spa/param/format.h
inline constexpr uint32_t kFormatMediaType = 1;
inline constexpr uint32_t kFormatMediaSubtype = 2;
inline constexpr uint32_t kFormatVideoFormat = 0x20001;
inline constexpr uint32_t kFormatVideoSize = 0x20003;

// spa/param/format.h, spa/param/video/raw.h
inline constexpr uint32_t kMediaTypeVideo = 2;
inline constexpr uint32_t kMediaSubtypeRaw = 1;
inline constexpr uint32_t kVideoFormatRGBx = 7;
inline constexpr uint32_t kVideoFormatBGRx = 8;
inline constexpr uint32_t kVideoFormatRGBA = 11;
inline constexpr uint32_t kVideoFormatBGRA = 12;

// spa/pod/pod.h
inline constexpr uint32_t kChoiceRange = 1;
inline constexpr uint32_t kChoiceEnum = 3;
inline constexpr uint32_t kChoiceFlags = 4;

// spa/buffer/buffer.h
inline constexpr uint32_t kDataMemPtr = 1;
inline constexpr uint32_t kDataMemFd = 2;
inline constexpr uint32_t kDataDmaBuf = 3;

// spa/buffer/meta.h
inline constexpr uint32_t kMetaVideoTransform = 8;

// pipewire/stream.h
inline constexpr uint32_t kVersionStreamEvents = 2;
inline constexpr uint32_t kStreamFlagAutoConnect = 1U << 0;
inline constexpr uint32_t kStreamFlagMapBuffers = 1U << 2;
inline constexpr int kDirectionInput = 0;

// A PipeWire stream hook is only used as opaque storage. It is larger than
// struct spa_hook and has the same alignment.
struct SpaHookStorage
{
    std::array<void*, 8> reserved {};
};

static_assert(sizeof(SpaHookStorage) >= 48);
static_assert(alignof(SpaHookStorage) >= alignof(void*));
} // namespace asst::waydroid::pwabi
