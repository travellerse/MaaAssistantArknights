#include "PortalBackend.h"

#include "DynLib.h"
#include "Utils/Logger.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>

namespace asst::waydroid
{
namespace
{
constexpr const char* kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr const char* kSessionInterface = "org.freedesktop.portal.Session";
constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";

constexpr int kCreateSessionTimeoutMs = 10'000;
constexpr int kSelectSourcesTimeoutMs = 60'000;
constexpr int kStartTimeoutMs = 60'000;
constexpr int kMethodCallTimeoutMs = 10'000;
constexpr int kFirstFrameTimeoutMs = 5000;
constexpr int kFrameWaitTimeoutMs = 100;

constexpr int kResponseSuccess = 0;
constexpr int kResponseCancelled = 1;

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

void append_option_string(dbus::MessageIter& options, const char* key, std::string_view value)
{
    dbus::MessageIter entry = dbus_util::open_container(options, dbus::kTypeDictEntry, nullptr);
    dbus_util::append_string(entry, key);
    dbus::MessageIter variant = dbus_util::open_container(entry, dbus::kTypeVariant, "s");
    dbus_util::append_string(variant, value);
    dbus_util::close_container(entry, variant);
    dbus_util::close_container(options, entry);
}

void append_option_uint32(dbus::MessageIter& options, const char* key, uint32_t value)
{
    dbus::MessageIter entry = dbus_util::open_container(options, dbus::kTypeDictEntry, nullptr);
    dbus_util::append_string(entry, key);
    dbus::MessageIter variant = dbus_util::open_container(entry, dbus::kTypeVariant, "u");
    dbus_util::append_uint32(variant, value);
    dbus_util::close_container(entry, variant);
    dbus_util::close_container(options, entry);
}

void append_option_boolean(dbus::MessageIter& options, const char* key, bool value)
{
    dbus::MessageIter entry = dbus_util::open_container(options, dbus::kTypeDictEntry, nullptr);
    dbus_util::append_string(entry, key);
    dbus::MessageIter variant = dbus_util::open_container(entry, dbus::kTypeVariant, "b");
    dbus_util::append_boolean(variant, value);
    dbus_util::close_container(entry, variant);
    dbus_util::close_container(options, entry);
}

std::string state_dir()
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — read once while starting capture
    if (const char* dir = std::getenv("XDG_STATE_HOME"); dir != nullptr && *dir != '\0') {
        return dir;
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — read once while starting capture
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::string(home) + "/.local/state";
    }
    return "/tmp";
}

std::string request_sender_token(std::string_view unique_name)
{
    std::string token;
    token.reserve(unique_name.size());
    for (const char ch : unique_name) {
        if (ch == ':') {
            continue;
        }
        token.push_back(ch == '.' ? '_' : ch);
    }
    return token;
}

struct StreamFormat
{
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::BGRX;
};

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
        LogWarn << "Waydroid portal: unsupported video format" << VAR(video_format);
        return std::nullopt;
    }
    if (result.width <= 0 || result.height <= 0) {
        return std::nullopt;
    }
    return result;
}
} // namespace

struct PortalBackend::PipeWire
{
    DynLib lib { { "libpipewire-0.3.so.0", "libpipewire-0.3.so" } };

    void (*init)(int*, char***) = nullptr;
    void (*deinit)() = nullptr;
    void* (*thread_loop_new)(const char*, const void*) = nullptr;
    int (*thread_loop_start)(void*) = nullptr;
    void (*thread_loop_stop)(void*) = nullptr;
    void (*thread_loop_destroy)(void*) = nullptr;
    void (*thread_loop_lock)(void*) = nullptr;
    void (*thread_loop_unlock)(void*) = nullptr;
    void* (*thread_loop_get_loop)(void*) = nullptr;
    void* (*context_new)(void*, void*, size_t) = nullptr;
    void (*context_destroy)(void*) = nullptr;
    void* (*context_connect_fd)(void*, int, void*, size_t) = nullptr;
    int (*core_disconnect)(void*) = nullptr;
    void* (*properties_new)(const char*, ...) = nullptr;
    void* (*stream_new)(void*, const char*, void*) = nullptr;
    void (*stream_add_listener)(void*, void*, const pwabi::PwStreamEvents*, void*) = nullptr;
    int (*stream_connect)(void*, int, uint32_t, uint32_t, const pwabi::SpaPod**, uint32_t) = nullptr;
    int (*stream_disconnect)(void*) = nullptr;
    void (*stream_destroy)(void*) = nullptr;
    pwabi::PwBuffer* (*stream_dequeue_buffer)(void*) = nullptr;
    int (*stream_queue_buffer)(void*, pwabi::PwBuffer*) = nullptr;

    void* loop = nullptr;
    void* context = nullptr;
    void* core = nullptr;
    void* stream = nullptr;

    bool load()
    {
        if (!lib) {
            return false;
        }
        init = lib.load<decltype(init)>("pw_init");
        deinit = lib.load<decltype(deinit)>("pw_deinit");
        thread_loop_new = lib.load<decltype(thread_loop_new)>("pw_thread_loop_new");
        thread_loop_start = lib.load<decltype(thread_loop_start)>("pw_thread_loop_start");
        thread_loop_stop = lib.load<decltype(thread_loop_stop)>("pw_thread_loop_stop");
        thread_loop_destroy = lib.load<decltype(thread_loop_destroy)>("pw_thread_loop_destroy");
        thread_loop_lock = lib.load<decltype(thread_loop_lock)>("pw_thread_loop_lock");
        thread_loop_unlock = lib.load<decltype(thread_loop_unlock)>("pw_thread_loop_unlock");
        thread_loop_get_loop = lib.load<decltype(thread_loop_get_loop)>("pw_thread_loop_get_loop");
        context_new = lib.load<decltype(context_new)>("pw_context_new");
        context_destroy = lib.load<decltype(context_destroy)>("pw_context_destroy");
        context_connect_fd = lib.load<decltype(context_connect_fd)>("pw_context_connect_fd");
        core_disconnect = lib.load<decltype(core_disconnect)>("pw_core_disconnect");
        properties_new = lib.load<decltype(properties_new)>("pw_properties_new");
        stream_new = lib.load<decltype(stream_new)>("pw_stream_new");
        stream_add_listener = lib.load<decltype(stream_add_listener)>("pw_stream_add_listener");
        stream_connect = lib.load<decltype(stream_connect)>("pw_stream_connect");
        stream_disconnect = lib.load<decltype(stream_disconnect)>("pw_stream_disconnect");
        stream_destroy = lib.load<decltype(stream_destroy)>("pw_stream_destroy");
        stream_dequeue_buffer = lib.load<decltype(stream_dequeue_buffer)>("pw_stream_dequeue_buffer");
        stream_queue_buffer = lib.load<decltype(stream_queue_buffer)>("pw_stream_queue_buffer");
        return init != nullptr && deinit != nullptr && thread_loop_new != nullptr && thread_loop_start != nullptr &&
               thread_loop_stop != nullptr && thread_loop_destroy != nullptr && thread_loop_lock != nullptr &&
               thread_loop_unlock != nullptr && thread_loop_get_loop != nullptr && context_new != nullptr &&
               context_destroy != nullptr && context_connect_fd != nullptr && core_disconnect != nullptr &&
               properties_new != nullptr && stream_new != nullptr && stream_add_listener != nullptr &&
               stream_connect != nullptr && stream_disconnect != nullptr && stream_destroy != nullptr &&
               stream_dequeue_buffer != nullptr && stream_queue_buffer != nullptr;
    }
};

PortalBackend::PortalBackend() = default;

PortalBackend::~PortalBackend()
{
    stop();
}

std::string PortalBackend::token_path()
{
    return (std::filesystem::path(state_dir()) / "maa" / "waydroid-portal.token").string();
}

std::string PortalBackend::load_restore_token()
{
    std::ifstream file(token_path(), std::ios::binary);
    if (!file) {
        return {};
    }
    std::string token;
    std::getline(file, token);
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) {
        token.pop_back();
    }
    return token;
}

void PortalBackend::save_restore_token(const std::string& token)
{
    if (token.empty()) {
        return;
    }
    const auto path = std::filesystem::path(token_path());
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        LogWarn << "Waydroid portal: cannot save restore token" << VAR(path.string());
        return;
    }
    file << token;
    file.close();
    chmod(path.c_str(), 0600);
}

std::string PortalBackend::next_token()
{
    return "maa_" + std::to_string(static_cast<long>(getpid())) + "_" + std::to_string(m_request_counter++);
}

bool PortalBackend::start(int width, int height, std::string_view /*selector*/)
{
    m_expected_width = width;
    m_expected_height = height;

    std::string error;
    if (!open_portal(error)) {
        LogWarn << "Waydroid portal: cannot start capture" << VAR(error);
        stop();
        return false;
    }

    {
        std::scoped_lock lock(m_mutex);
        m_want_frame = true;
    }
    if (!open_pipewire(error) || !wait_for_first_frame(error)) {
        LogWarn << "Waydroid portal: PipeWire stream unavailable" << VAR(error);
        stop();
        return false;
    }

    m_started = true;
    LogInfo << "Waydroid portal capture started" << VAR(m_stream_width) << VAR(m_stream_height);
    return true;
}

bool PortalBackend::open_portal(std::string& error)
{
    auto bus = DBusConnection::open_session(error);
    if (!bus) {
        return false;
    }
    m_bus = std::move(*bus);

    if (!m_bus.add_filter(&PortalBackend::on_bus_message, this)) {
        error = "cannot install portal response filter";
        return false;
    }
    if (!m_bus.add_match("type='signal',interface='org.freedesktop.portal.Request',member='Response'", error)) {
        return false;
    }
    LogDebug << "Waydroid portal: creating session";
    if (!create_session(error)) {
        LogWarn << "Waydroid portal: CreateSession failed" << VAR(error);
        return false;
    }
    LogDebug << "Waydroid portal: selecting sources";
    if (!select_sources(error)) {
        LogWarn << "Waydroid portal: SelectSources failed" << VAR(error);
        return false;
    }
    LogDebug << "Waydroid portal: starting session";
    if (!start_session(error)) {
        LogWarn << "Waydroid portal: Start failed" << VAR(error);
        return false;
    }
    if (m_pending_streams.empty()) {
        error = "portal returned no stream";
        return false;
    }
    return true;
}

bool PortalBackend::create_session(std::string& error)
{
    const std::string token = next_token();
    const bool ok = request(
        "CreateSession",
        token,
        [&](dbus::MessageIter& args) {
            dbus::MessageIter options = dbus_util::open_container(args, dbus::kTypeArray, "{sv}");
            append_option_string(options, "handle_token", token);
            append_option_string(options, "session_handle_token", token + "_session");
            dbus_util::close_container(args, options);
        },
        kCreateSessionTimeoutMs,
        error);
    if (!ok || m_pending_session_handle.empty()) {
        if (error.empty()) {
            error = "portal did not return a session handle";
        }
        return false;
    }
    m_session_handle = m_pending_session_handle;
    return true;
}

bool PortalBackend::select_sources(std::string& error)
{
    uint32_t source_types = 2; // window
    std::string property_error;
    if (!query_source_types(source_types, property_error)) {
        LogInfo << "Waydroid portal: cannot query source types, assuming window" << VAR(property_error);
        source_types = 2;
    }
    if ((source_types & 2U) != 0) {
        source_types = 2;
    }
    else if ((source_types & 1U) != 0) {
        source_types = 1;
        LogWarn << "Waydroid portal: window capture is not supported, using monitor capture";
    }
    else {
        error = "portal does not support window or monitor capture";
        return false;
    }

    const std::string token = next_token();
    const std::string restore_token = m_restore_token.empty() ? load_restore_token() : m_restore_token;
    if (restore_token.empty()) {
        LogInfo << "Waydroid portal: select the Waydroid window in the system dialog";
    }
    return request(
        "SelectSources",
        token,
        [&](dbus::MessageIter& args) {
            dbus_util::append_object_path(args, m_session_handle);
            dbus::MessageIter options = dbus_util::open_container(args, dbus::kTypeArray, "{sv}");
            append_option_string(options, "handle_token", token);
            append_option_uint32(options, "types", source_types);
            append_option_boolean(options, "multiple", false);
            append_option_uint32(options, "cursor_mode", 1);
            append_option_uint32(options, "persist_mode", 2);
            if (!restore_token.empty()) {
                append_option_string(options, "restore_token", restore_token);
            }
            dbus_util::close_container(args, options);
        },
        kSelectSourcesTimeoutMs,
        error);
}

bool PortalBackend::start_session(std::string& error)
{
    const std::string token = next_token();
    const bool ok = request(
        "Start",
        token,
        [&](dbus::MessageIter& args) {
            dbus_util::append_object_path(args, m_session_handle);
            dbus_util::append_string(args, "");
            dbus::MessageIter options = dbus_util::open_container(args, dbus::kTypeArray, "{sv}");
            append_option_string(options, "handle_token", token);
            dbus_util::close_container(args, options);
        },
        kStartTimeoutMs,
        error);
    if (!ok) {
        return false;
    }
    if (!m_pending_restore_token.empty()) {
        m_restore_token = m_pending_restore_token;
        save_restore_token(m_restore_token);
    }
    return true;
}

bool PortalBackend::query_source_types(uint32_t& types, std::string& error)
{
    dbus::Message* message =
        m_bus.new_method_call(kPortalService, kPortalPath, "org.freedesktop.DBus.Properties", "Get");
    if (message == nullptr) {
        error = "cannot create property call";
        return false;
    }
    dbus::MessageIter args {};
    if (dbus_util::init_append(message, args)) {
        dbus_util::append_string(args, kScreenCastInterface);
        dbus_util::append_string(args, "AvailableSourceTypes");
    }
    dbus::Message* reply = m_bus.call(message, kMethodCallTimeoutMs, error);
    dbus_util::message_unref(message);
    if (reply == nullptr) {
        return false;
    }

    bool ok = false;
    dbus::MessageIter reply_iter {};
    if (dbus_util::init_read(reply, reply_iter) && dbus_util::arg_type(reply_iter) == dbus::kTypeVariant) {
        dbus::MessageIter value = dbus_util::recurse(reply_iter);
        ok = dbus_util::get_uint32(value, types);
    }
    dbus_util::message_unref(reply);
    if (!ok) {
        error = "invalid AvailableSourceTypes reply";
    }
    return ok;
}

bool PortalBackend::request(
    const char* member,
    std::string_view handle_token,
    const std::function<void(dbus::MessageIter&)>& append_args,
    int timeout_ms,
    std::string& error)
{
    if (!send_request(member, handle_token, append_args, error)) {
        return false;
    }
    if (!wait_response(timeout_ms, error)) {
        return false;
    }
    if (m_response != kResponseSuccess) {
        m_rejected = m_response == kResponseCancelled;
        error = m_response == kResponseCancelled ? "cancelled by user" : "portal request failed";
        return false;
    }
    return true;
}

bool PortalBackend::send_request(
    const char* member,
    std::string_view handle_token,
    const std::function<void(dbus::MessageIter&)>& append_args,
    std::string& error)
{
    const char* unique_name = m_bus.unique_name();
    if (unique_name == nullptr) {
        error = "no unique bus name";
        return false;
    }

    m_pending_session_handle.clear();
    m_pending_restore_token.clear();
    m_pending_streams.clear();
    m_response = 2;
    m_pending_done = false;
    m_pending_path = "/org/freedesktop/portal/desktop/request/" + request_sender_token(unique_name) + "/" +
                     std::string(handle_token);

    dbus::Message* message = m_bus.new_method_call(kPortalService, kPortalPath, kScreenCastInterface, member);
    if (message == nullptr) {
        error = "cannot create portal request";
        return false;
    }
    dbus::MessageIter args {};
    if (!dbus_util::init_append(message, args)) {
        dbus_util::message_unref(message);
        error = "cannot build portal request";
        return false;
    }
    append_args(args);

    std::string call_error;
    LogDebug << "Waydroid portal: calling" << VAR(member) << VAR(m_pending_path);
    dbus::Message* reply = m_bus.call(message, kMethodCallTimeoutMs, call_error);
    dbus_util::message_unref(message);
    if (reply == nullptr) {
        error = "portal call failed: " + call_error;
        return false;
    }

    std::string request_path;
    dbus::MessageIter reply_iter {};
    if (dbus_util::init_read(reply, reply_iter)) {
        dbus_util::get_string(reply_iter, request_path);
    }
    dbus_util::message_unref(reply);
    if (request_path.empty()) {
        error = "portal returned no request handle";
        return false;
    }
    if (request_path != m_pending_path) {
        LogWarn << "Waydroid portal: request path differs from handle token" << VAR(request_path)
                << VAR(m_pending_path);
        m_pending_path = std::move(request_path);
    }
    LogDebug << "Waydroid portal: request accepted" << VAR(m_pending_path);
    return true;
}

bool PortalBackend::wait_response(int timeout_ms, std::string& error)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!m_pending_done) {
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "portal request timed out";
            return false;
        }
        if (!m_bus.read_write_dispatch(50)) {
            error = "session bus disconnected";
            return false;
        }
    }
    return true;
}

int PortalBackend::on_bus_message(dbus::Connection* /*connection*/, dbus::Message* message, void* user_data)
{
    auto* self = static_cast<PortalBackend*>(user_data);
    if (!dbus_util::is_signal(message, kRequestInterface, "Response")) {
        return dbus::kHandlerResultNotYetHandled;
    }
    const char* path = dbus_util::message_path(message);
    if (path == nullptr || self->m_pending_path != path) {
        return dbus::kHandlerResultNotYetHandled;
    }
    self->handle_response(message);
    return dbus::kHandlerResultHandled;
}

void PortalBackend::handle_response(dbus::Message* message)
{
    dbus::MessageIter iter {};
    if (!dbus_util::init_read(message, iter)) {
        m_pending_done = true;
        return;
    }
    uint32_t response = 2;
    if (!dbus_util::get_uint32(iter, response)) {
        m_pending_done = true;
        return;
    }
    m_response = response;
    LogDebug << "Waydroid portal: response received" << VAR(response);
    if (dbus_util::next(iter) && dbus_util::arg_type(iter) == dbus::kTypeArray) {
        dbus::MessageIter array = dbus_util::recurse(iter);
        parse_response_results(array);
    }
    m_pending_done = true;
}

void PortalBackend::parse_response_results(dbus::MessageIter& array)
{
    while (dbus_util::arg_type(array) == dbus::kTypeDictEntry) {
        dbus::MessageIter fields = dbus_util::recurse(array);
        std::string key;
        if (!dbus_util::get_string(fields, key) || !dbus_util::next(fields)) {
            break;
        }
        dbus::MessageIter variant = dbus_util::recurse(fields);
        const int type = dbus_util::arg_type(variant);
        LogDebug << "Waydroid portal: response key" << VAR(key) << VAR(type);
        if (key == "session_handle" && (type == dbus::kTypeObjectPath || type == dbus::kTypeString)) {
            dbus_util::get_string(variant, m_pending_session_handle);
        }
        else if (key == "restore_token" && type == dbus::kTypeString) {
            dbus_util::get_string(variant, m_pending_restore_token);
        }
        else if (key == "streams" && type == dbus::kTypeArray) {
            dbus::MessageIter streams = dbus_util::recurse(variant);
            parse_streams(streams);
        }
        if (!dbus_util::next(array)) {
            break;
        }
    }
}

void PortalBackend::parse_streams(dbus::MessageIter& array)
{
    while (dbus_util::arg_type(array) == dbus::kTypeStruct) {
        dbus::MessageIter fields = dbus_util::recurse(array);
        PortalStream stream;
        if (!dbus_util::get_uint32(fields, stream.node_id) || !dbus_util::next(fields)) {
            break;
        }
        if (dbus_util::arg_type(fields) == dbus::kTypeArray) {
            dbus::MessageIter properties = dbus_util::recurse(fields);
            parse_stream_properties(properties, stream);
        }
        if (stream.node_id != 0) {
            m_pending_streams.push_back(stream);
        }
        if (!dbus_util::next(array)) {
            break;
        }
    }
}

void PortalBackend::parse_stream_properties(dbus::MessageIter& array, PortalStream& stream)
{
    while (dbus_util::arg_type(array) == dbus::kTypeDictEntry) {
        dbus::MessageIter fields = dbus_util::recurse(array);
        std::string key;
        if (!dbus_util::get_string(fields, key) || !dbus_util::next(fields)) {
            break;
        }
        dbus::MessageIter variant = dbus_util::recurse(fields);
        const int type = dbus_util::arg_type(variant);
        if (key == "size" && type == dbus::kTypeStruct) {
            dbus::MessageIter size = dbus_util::recurse(variant);
            int32_t width = 0;
            int32_t height = 0;
            if (dbus_util::get_int32(size, width) && dbus_util::next(size)) {
                dbus_util::get_int32(size, height);
                stream.width = width;
                stream.height = height;
            }
        }
        else if (key == "source_type") {
            dbus_util::get_uint32(variant, stream.source_type);
        }
        if (!dbus_util::next(array)) {
            break;
        }
    }
}

bool PortalBackend::open_pipewire(std::string& error)
{
    if (m_pending_streams.empty()) {
        error = "no portal stream";
        return false;
    }
    const uint32_t node_id = m_pending_streams.front().node_id;
    const int fd = open_pipewire_remote(error);
    if (fd < 0) {
        return false;
    }
    return start_pipewire(fd, node_id, error);
}

int PortalBackend::open_pipewire_remote(std::string& error)
{
    dbus::Message* message =
        m_bus.new_method_call(kPortalService, kPortalPath, kScreenCastInterface, "OpenPipeWireRemote");
    if (message == nullptr) {
        error = "cannot create OpenPipeWireRemote call";
        return -1;
    }
    dbus::MessageIter args {};
    if (dbus_util::init_append(message, args)) {
        dbus_util::append_object_path(args, m_session_handle);
        dbus::MessageIter options = dbus_util::open_container(args, dbus::kTypeArray, "{sv}");
        dbus_util::close_container(args, options);
    }
    dbus::Message* reply = m_bus.call(message, kMethodCallTimeoutMs, error);
    dbus_util::message_unref(message);
    if (reply == nullptr) {
        return -1;
    }
    int fd = -1;
    dbus::MessageIter reply_iter {};
    const bool have_fd = dbus_util::init_read(reply, reply_iter) && dbus_util::get_fd(reply_iter, fd);
    dbus_util::message_unref(reply);
    if (!have_fd || fd < 0) {
        error = "portal did not return a PipeWire fd";
        return -1;
    }
    return fd;
}

bool PortalBackend::start_pipewire(int fd, uint32_t node_id, std::string& error)
{
    return init_pipewire(fd, error) && connect_pipewire_stream(node_id, error);
}

bool PortalBackend::init_pipewire(int fd, std::string& error)
{
    m_pipewire = std::make_unique<PipeWire>();
    auto& pw = *m_pipewire;
    if (!pw.load()) {
        ::close(fd);
        error = "libpipewire-0.3 is not available";
        return false;
    }

    pw.init(nullptr, nullptr);
    pw.loop = pw.thread_loop_new("maa-waydroid-portal", nullptr);
    if (pw.loop == nullptr) {
        ::close(fd);
        error = "cannot create PipeWire thread loop";
        return false;
    }
    pw.context = pw.context_new(pw.thread_loop_get_loop(pw.loop), nullptr, 0);
    if (pw.context == nullptr) {
        ::close(fd);
        error = "cannot create PipeWire context";
        return false;
    }
    if (pw.thread_loop_start(pw.loop) < 0) {
        ::close(fd);
        error = "cannot start PipeWire thread loop";
        return false;
    }

    pw.thread_loop_lock(pw.loop);
    pw.core = pw.context_connect_fd(pw.context, fd, nullptr, 0);
    pw.thread_loop_unlock(pw.loop);
    if (pw.core == nullptr) {
        error = "cannot connect to the portal PipeWire remote";
        return false;
    }
    return true;
}

bool PortalBackend::connect_pipewire_stream(uint32_t node_id, std::string& error)
{
    auto& pw = *m_pipewire;
    pw.thread_loop_lock(pw.loop);
    pw.stream = pw.stream_new(
        pw.core,
        "maa-waydroid-capture",
        pw.properties_new(
            "media.type",
            "Video",
            "media.category",
            "Capture",
            "media.role",
            "Screen",
            static_cast<const char*>(nullptr)));
    if (pw.stream == nullptr) {
        pw.thread_loop_unlock(pw.loop);
        error = "cannot create PipeWire stream";
        return false;
    }

    m_events.version = pwabi::kVersionStreamEvents;
    m_events.process = &PortalBackend::on_process_cb;
    m_events.param_changed = &PortalBackend::on_param_changed_cb;
    m_events.state_changed = &PortalBackend::on_state_changed_cb;
    pw.stream_add_listener(pw.stream, &m_hook, &m_events, this);

    std::array<uint8_t, 256> pod_buffer {};
    const size_t pod_size = build_format_pod(pod_buffer.data(), pod_buffer.size(), m_expected_width, m_expected_height);
    std::array<const pwabi::SpaPod*, 1> params { reinterpret_cast<const pwabi::SpaPod*>(pod_buffer.data()) };
    const int ret = pw.stream_connect(
        pw.stream,
        pwabi::kDirectionInput,
        node_id,
        pwabi::kStreamFlagAutoConnect | pwabi::kStreamFlagMapBuffers,
        params.data(),
        pod_size > 0 ? 1 : 0);
    pw.thread_loop_unlock(pw.loop);
    if (ret < 0) {
        error = "PipeWire stream connect failed";
        return false;
    }
    return true;
}

bool PortalBackend::wait_for_first_frame(std::string& error)
{
    std::unique_lock lock(m_mutex);
    const bool ready = m_frame_ready.wait_for(lock, std::chrono::milliseconds(kFirstFrameTimeoutMs), [this] {
        return m_stream_error || m_frame_seq > 0;
    });
    if (m_stream_error) {
        error = "PipeWire stream reported an error";
        return false;
    }
    if (!ready) {
        error = "PipeWire stream produced no frames";
        return false;
    }
    return true;
}

std::optional<Frame> PortalBackend::capture()
{
    std::unique_lock lock(m_mutex);
    if (!m_started || m_stream_error) {
        return std::nullopt;
    }
    if (m_frame_seq > m_consumed_seq && m_has_frame) {
        m_consumed_seq = m_frame_seq;
        return m_latest;
    }

    m_want_frame = true;
    const uint64_t wanted = m_consumed_seq;
    const bool ready = m_frame_ready.wait_for(lock, std::chrono::milliseconds(kFrameWaitTimeoutMs), [this, wanted] {
        return m_stream_error || m_frame_seq > wanted;
    });
    if (m_stream_error) {
        return std::nullopt;
    }
    if (!ready || !m_has_frame) {
        // A static screen may not produce damage frames. Reusing the last
        // frame is correct here; returning nullopt would needlessly fall back
        // to ADB.
        if (m_has_frame) {
            m_consumed_seq = m_frame_seq;
            return m_latest;
        }
        return std::nullopt;
    }
    m_consumed_seq = m_frame_seq;
    return m_latest;
}

void PortalBackend::on_state_changed_cb(void* user_data, int /*old*/, int state, const char* error)
{
    static_cast<PortalBackend*>(user_data)->on_state_changed(state, error);
}

void PortalBackend::on_state_changed(int state, const char* error)
{
    constexpr int kStateError = -1;
    if (state == kStateError) {
        LogWarn << "Waydroid portal: PipeWire stream error" << VAR(error == nullptr ? "" : error);
        std::scoped_lock lock(m_mutex);
        m_stream_error = true;
        m_frame_ready.notify_all();
    }
}

void PortalBackend::on_param_changed_cb(void* user_data, uint32_t id, const pwabi::SpaPod* param)
{
    static_cast<PortalBackend*>(user_data)->on_param_changed(id, param);
}

void PortalBackend::on_param_changed(uint32_t id, const pwabi::SpaPod* param)
{
    if (id != pwabi::kParamFormat) {
        return;
    }
    const auto format = parse_stream_format(param);
    if (!format) {
        return;
    }
    m_stream_width = format->width;
    m_stream_height = format->height;
    m_stream_format = format->format;
    const int format_value = static_cast<int>(m_stream_format);
    LogInfo << "Waydroid portal stream format" << VAR(m_stream_width) << VAR(m_stream_height) << VAR(format_value);
}

void PortalBackend::on_process_cb(void* user_data)
{
    static_cast<PortalBackend*>(user_data)->on_process();
}

void PortalBackend::on_process()
{
    auto& pw = *m_pipewire;
    if (pw.stream == nullptr || m_stream_width <= 0 || m_stream_height <= 0) {
        return;
    }
    pwabi::PwBuffer* buffer = pw.stream_dequeue_buffer(pw.stream);
    if (buffer == nullptr) {
        return;
    }

    {
        std::scoped_lock lock(m_mutex);
        const bool wanted = m_want_frame || !m_has_frame;
        if (wanted) {
            const pwabi::SpaBuffer* spa_buffer = buffer->buffer;
            if (spa_buffer == nullptr || spa_buffer->n_datas == 0) {
                pw.stream_queue_buffer(pw.stream, buffer);
                return;
            }
            const pwabi::SpaData& data = spa_buffer->datas[0];
            uint32_t transform = 0;
            if (spa_buffer->metas != nullptr) {
                for (uint32_t meta_index = 0; meta_index < spa_buffer->n_metas; ++meta_index) {
                    const pwabi::SpaMeta& meta = spa_buffer->metas[meta_index];
                    if (meta.type == pwabi::kMetaVideoTransform && meta.data != nullptr &&
                        meta.size >= sizeof(uint32_t)) {
                        transform = *static_cast<const uint32_t*>(meta.data);
                        break;
                    }
                }
            }
            if (transform != 0) {
                static bool transform_warned = false;
                if (!transform_warned) {
                    transform_warned = true;
                    LogWarn << "Waydroid portal: unsupported video transform" << VAR(transform);
                }
                m_stream_error = true;
                m_frame_ready.notify_all();
                pw.stream_queue_buffer(pw.stream, buffer);
                return;
            }
            const int width = m_stream_width;
            const int height = m_stream_height;
            const int row_bytes = width * 4;
            int stride = data.chunk != nullptr ? data.chunk->stride : row_bytes;
            stride = std::max(stride, row_bytes);
            const size_t required = (static_cast<size_t>(stride) * static_cast<size_t>(height - 1)) + row_bytes;
            if (data.data != nullptr && data.chunk != nullptr && data.chunk->size >= required) {
                const size_t max_size = data.maxsize;
                const size_t offset = max_size > 0 ? data.chunk->offset % max_size : data.chunk->offset;
                const auto* source = static_cast<const uint8_t*>(data.data) + offset;
                m_latest.width = width;
                m_latest.height = height;
                m_latest.stride = row_bytes;
                m_latest.format = m_stream_format;
                m_latest.data.resize(static_cast<size_t>(row_bytes) * static_cast<size_t>(height));
                for (int row = 0; row < height; ++row) {
                    std::memcpy(
                        m_latest.data.data() + (static_cast<size_t>(row) * row_bytes),
                        source + (static_cast<size_t>(row) * stride),
                        row_bytes);
                }
                ++m_frame_seq;
                m_has_frame = true;
                m_want_frame = false;
                m_frame_ready.notify_all();
            }
        }
    }

    pw.stream_queue_buffer(pw.stream, buffer);
}

void PortalBackend::stop() noexcept
{
    m_started = false;
    {
        std::scoped_lock lock(m_mutex);
        m_frame_ready.notify_all();
    }

    stop_pipewire();
    close_portal_session();
    m_bus = DBusConnection {};

    m_session_handle.clear();
    m_pending_path.clear();
    m_pending_streams.clear();
    m_pending_session_handle.clear();
    m_pending_restore_token.clear();
    m_pending_done = false;
    m_response = 2;

    std::scoped_lock lock(m_mutex);
    m_has_frame = false;
    m_want_frame = false;
    m_frame_seq = 0;
    m_consumed_seq = 0;
    m_stream_width = 0;
    m_stream_height = 0;
    m_stream_error = false;
    m_latest = Frame {};
}

void PortalBackend::stop_pipewire() noexcept
{
    if (m_pipewire == nullptr) {
        return;
    }
    auto& pw = *m_pipewire;
    if (pw.loop != nullptr) {
        pw.thread_loop_lock(pw.loop);
        if (pw.stream != nullptr) {
            pw.stream_disconnect(pw.stream);
            pw.stream_destroy(pw.stream);
            pw.stream = nullptr;
        }
        if (pw.core != nullptr) {
            pw.core_disconnect(pw.core);
            pw.core = nullptr;
        }
        pw.thread_loop_unlock(pw.loop);
        pw.thread_loop_stop(pw.loop);
    }
    // The context was created on the thread loop's pw_loop, so it must be
    // destroyed before that loop.
    if (pw.context != nullptr) {
        pw.context_destroy(pw.context);
        pw.context = nullptr;
    }
    if (pw.loop != nullptr) {
        pw.thread_loop_destroy(pw.loop);
        pw.loop = nullptr;
    }
    if (pw.lib) {
        pw.deinit();
    }
    m_pipewire.reset();
}

void PortalBackend::close_portal_session() noexcept
{
    if (!m_bus.valid() || m_session_handle.empty()) {
        return;
    }
    dbus::Message* message =
        m_bus.new_method_call(kPortalService, m_session_handle.c_str(), kSessionInterface, "Close");
    if (message != nullptr) {
        std::string error;
        dbus::Message* reply = m_bus.call(message, 1000, error);
        dbus_util::message_unref(reply);
        dbus_util::message_unref(message);
    }
}
} // namespace asst::waydroid
