#include "KWinBackend.h"

#include "Utils/Logger.hpp"
#include "WindowMatcher.h"

#include <meojson/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace asst::waydroid
{
namespace
{
constexpr const char* kKWinService = "org.kde.KWin";
constexpr const char* kScriptingPath = "/Scripting";
constexpr const char* kScriptingInterface = "org.kde.kwin.Scripting";
constexpr const char* kScreenShotPath = "/org/kde/KWin/ScreenShot2";
constexpr const char* kScreenShotInterface = "org.kde.KWin.ScreenShot2";
constexpr int kCaptureTimeoutMs = 3000;
constexpr int kReadPollIntervalMs = 100;
constexpr size_t kReadBufferSize = size_t { 64 } * 1024;

// Drains the capture pipe until EOF. KWin may keep a duplicate of the write
// end after the D-Bus reply, so a plain blocking read can hang forever; the
// poll interval lets the reader stop once the caller has closed its end.
void read_capture_pipe(int read_fd, std::vector<uint8_t>& data, const std::atomic<bool>& caller_closed)
{
    std::array<uint8_t, kReadBufferSize> buffer {};
    while (true) {
        pollfd descriptor { read_fd, POLLIN, 0 };
        const int ready = ::poll(&descriptor, 1, kReadPollIntervalMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            if (caller_closed.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
            break;
        }
        const ssize_t count = ::read(read_fd, buffer.data(), buffer.size());
        if (count > 0) {
            data.insert(data.end(), buffer.begin(), buffer.begin() + count);
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
    }
}

std::string runtime_dir()
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — read once while starting capture
    if (const char* dir = std::getenv("XDG_RUNTIME_DIR"); dir != nullptr && *dir != '\0') {
        return dir;
    }
    return "/tmp";
}

std::string make_script(const std::string& service, const std::string& object_path)
{
    std::string script = R"JS(
const windows = workspace.windowList().map(w => ({
    caption: w.caption,
    resourceClass: w.resourceClass,
    internalId: String(w.internalId),
    pid: w.pid,
    minimized: w.minimized,
    width: w.frameGeometry.width,
    height: w.frameGeometry.height
}));
callDBus(")JS";
    script += service;
    script += R"JS(", ")JS";
    script += object_path;
    script += R"JS(", ")JS";
    script += service;
    script += R"JS(", "Report", JSON.stringify(windows));
)JS";
    return script;
}

struct CaptureGeometry
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
};

CaptureGeometry parse_capture_geometry(dbus::Message* reply)
{
    CaptureGeometry geometry;
    dbus::MessageIter reply_iter {};
    if (!dbus_util::init_read(reply, reply_iter)) {
        return geometry;
    }
    dbus::MessageIter array = dbus_util::recurse(reply_iter);
    while (dbus_util::arg_type(array) == dbus::kTypeDictEntry) {
        std::string key;
        dbus_util::Variant value;
        if (dbus_util::read_dict_entry_sv(array, key, value)) {
            if (key == "width") {
                geometry.width = value.uint32_value;
            }
            else if (key == "height") {
                geometry.height = value.uint32_value;
            }
            else if (key == "stride") {
                geometry.stride = value.uint32_value;
            }
            else if (key == "format") {
                geometry.format = value.type == dbus::kTypeString
                                      ? static_cast<uint32_t>(std::strtoul(value.string_value.c_str(), nullptr, 10))
                                      : value.uint32_value;
            }
        }
        if (!dbus_util::next(array)) {
            break;
        }
    }
    return geometry;
}
} // namespace

KWinBackend::KWinBackend()
{
    static uint64_t counter = 0;
    const std::string suffix = "p" + std::to_string(static_cast<long>(getpid())) + "_" + std::to_string(counter++);
    m_service_name = "org.maa.WaydroidCapture." + suffix;
    m_object_path = "/org/maa/WaydroidCapture/" + suffix;
}

KWinBackend::~KWinBackend()
{
    stop();
}

bool KWinBackend::connect_bus()
{
    std::string error;
    auto bus = DBusConnection::open_session(error);
    if (!bus) {
        LogWarn << "KWin capture: cannot connect to the session bus" << VAR(error);
        return false;
    }
    m_bus = std::move(*bus);

    // on_report is private; defining the vtable inside a member function keeps
    // the callback out of the public interface.
    static const dbus::ObjectPathVTable kWindowVTable {
        .message_function = &KWinBackend::on_report,
    };
    if (!m_bus.register_object_path(m_object_path, &kWindowVTable, this)) {
        LogWarn << "KWin capture: cannot register window report object" << VAR(m_object_path);
        return false;
    }
    if (!m_bus.request_name(m_service_name, error)) {
        LogWarn << "KWin capture: cannot own service name" << VAR(m_service_name) << VAR(error);
        return false;
    }
    return true;
}

bool KWinBackend::start(int width, int height, std::string_view selector)
{
    m_expected_width = width;
    m_expected_height = height;
    m_selector = std::string(selector);
    if (m_bus.valid()) {
        return true;
    }
    if (!connect_bus()) {
        release_bus();
        return false;
    }
    if (!discover_window()) {
        release_bus();
        return false;
    }
    return true;
}

void KWinBackend::stop() noexcept
{
    release_bus();
}

void KWinBackend::release_bus() noexcept
{
    m_bus = DBusConnection {};
    m_handle.clear();
    m_caption.clear();
    m_resource_class.clear();
}

int KWinBackend::on_report(dbus::Connection* connection, dbus::Message* message, void* user_data)
{
    auto* self = static_cast<KWinBackend*>(user_data);
    if (auto report = dbus_util::read_string_argument(message)) {
        self->m_report = std::move(*report);
        self->m_reported = true;
    }
    dbus::Message* reply = dbus_util::new_method_return(message);
    if (reply != nullptr) {
        dbus_util::send_reply(connection, reply);
        dbus_util::message_unref(reply);
    }
    return dbus::kHandlerResultHandled;
}

bool KWinBackend::run_script(const std::string& path, const std::string& plugin_name)
{
    std::string error;
    dbus::Message* message = m_bus.new_method_call(kKWinService, kScriptingPath, kScriptingInterface, "loadScript");
    if (message == nullptr) {
        return false;
    }
    dbus::MessageIter args {};
    if (dbus_util::init_append(message, args)) {
        dbus_util::append_string(args, path);
        dbus_util::append_string(args, plugin_name);
    }
    dbus::Message* reply = m_bus.call(message, kCaptureTimeoutMs, error);
    const bool load_ok = reply != nullptr;
    m_kwin_reachable = load_ok;
    dbus_util::message_unref(reply);
    dbus_util::message_unref(message);
    if (!load_ok) {
        LogWarn << "KWin capture: loadScript failed" << VAR(error);
        return false;
    }

    m_reported = false;
    m_report.clear();
    message = m_bus.new_method_call(kKWinService, kScriptingPath, kScriptingInterface, "start");
    if (message == nullptr) {
        return false;
    }
    reply = m_bus.call(message, kCaptureTimeoutMs, error);
    const bool start_ok = reply != nullptr;
    dbus_util::message_unref(reply);
    dbus_util::message_unref(message);
    if (!start_ok) {
        LogWarn << "KWin capture: Scripting.start failed" << VAR(error);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!m_reported && std::chrono::steady_clock::now() < deadline) {
        if (!m_bus.read_write_dispatch(10)) {
            break;
        }
    }

    message = m_bus.new_method_call(kKWinService, kScriptingPath, kScriptingInterface, "unloadScript");
    if (message != nullptr) {
        dbus::MessageIter unload_args {};
        if (dbus_util::init_append(message, unload_args)) {
            dbus_util::append_string(unload_args, plugin_name);
        }
        reply = m_bus.call(message, kCaptureTimeoutMs, error);
        dbus_util::message_unref(reply);
        dbus_util::message_unref(message);
    }

    return m_reported;
}

bool KWinBackend::discover_window()
{
    m_last_discovery = std::chrono::steady_clock::now();
    static uint64_t counter = 0;
    const auto path =
        std::filesystem::path(runtime_dir()) /
        ("maa-waydroid-kwin-" + std::to_string(static_cast<long>(getpid())) + "-" + std::to_string(counter++) + ".js");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            LogWarn << "KWin capture: cannot write script" << VAR(path.string());
            return false;
        }
        file << make_script(m_service_name, m_object_path);
    }
    chmod(path.c_str(), 0600);

    const std::string plugin =
        "maa-waydroid-" + std::to_string(static_cast<long>(getpid())) + "-" + std::to_string(counter);
    const bool ok = run_script(path.string(), plugin);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (!ok) {
        LogWarn << "KWin capture: no window report received";
        return false;
    }
    return select_window(m_report);
}

bool KWinBackend::select_window(const std::string& report)
{
    auto parsed = json::parse(report);
    if (!parsed || !parsed->is_array()) {
        LogWarn << "KWin capture: invalid window report";
        return false;
    }

    struct Candidate
    {
        std::string handle;
        std::string caption;
        std::string resource_class;
        int pid = 0;
        double width = 0;
        double height = 0;
        bool minimized = false;
    };

    std::vector<Candidate> candidates;
    for (const auto& item : parsed->as_array()) {
        if (!item.is_object()) {
            continue;
        }
        const auto& object = item.as_object();
        Candidate candidate;
        candidate.handle = object.get("internalId", "");
        candidate.caption = object.get("caption", "");
        candidate.resource_class = object.get("resourceClass", "");
        candidate.pid = object.get("pid", 0);
        candidate.width = object.get("width", 0.0);
        candidate.height = object.get("height", 0.0);
        candidate.minimized = object.get("minimized", false);
        if (!candidate.handle.empty() && candidate.width > 0 && candidate.height > 0) {
            candidates.emplace_back(std::move(candidate));
        }
    }

    // Prefer a visible app window over the full UI shell, then the largest
    // one. Keep the current binding when it is still present unless an app
    // window can replace the shell, so focus changes do not switch captures.
    const Candidate* chosen = nullptr;
    const Candidate* current = nullptr;
    int chosen_score = 0;
    for (const auto& candidate : candidates) {
        if (!is_waydroid_window(candidate.resource_class, candidate.caption, candidate.pid, m_selector)) {
            continue;
        }
        if (!m_handle.empty() && candidate.handle == m_handle) {
            current = &candidate;
        }
        const int score = window_score(candidate.minimized, candidate.resource_class);
        const bool larger = chosen != nullptr && candidate.width * candidate.height > chosen->width * chosen->height;
        if (chosen == nullptr || score > chosen_score || (score == chosen_score && larger)) {
            chosen = &candidate;
            chosen_score = score;
        }
    }
    if (chosen == nullptr) {
        LogWarn << "KWin capture: no Waydroid window found" << VAR(m_selector);
        for (const auto& candidate : candidates) {
            LogDebug << "KWin window candidate" << VAR(candidate.caption) << VAR(candidate.resource_class)
                     << VAR(candidate.pid) << VAR(candidate.width) << VAR(candidate.height) << VAR(candidate.minimized);
        }
        return false;
    }
    if (current != nullptr && chosen != current) {
        const int current_score = window_score(current->minimized, current->resource_class);
        const bool upgrade = is_app_window_class(chosen->resource_class) &&
                             !is_app_window_class(current->resource_class) && chosen_score >= current_score;
        if (!upgrade) {
            chosen = current;
        }
    }

    const bool changed = m_handle != chosen->handle;
    m_handle = chosen->handle;
    m_caption = chosen->caption;
    m_resource_class = chosen->resource_class;
    if (changed) {
        LogInfo << "KWin capture window" << VAR(m_caption) << VAR(m_resource_class) << VAR(chosen->width)
                << VAR(chosen->height) << VAR(m_expected_width) << VAR(m_expected_height);
    }
    return true;
}

std::optional<Frame> KWinBackend::capture()
{
    if (!m_bus.valid()) {
        return std::nullopt;
    }
    // When bound to the full UI shell, Waydroid may open the app in its own
    // window later (multi-window mode). Poll occasionally so that window can
    // replace the shell; app windows themselves are kept until they vanish.
    constexpr auto kUpgradeInterval = std::chrono::seconds(30);
    const bool upgrade_due = !m_handle.empty() && !is_app_window_class(m_resource_class) &&
                             std::chrono::steady_clock::now() - m_last_discovery >= kUpgradeInterval;
    if ((m_handle.empty() || upgrade_due) && !discover_window() && m_handle.empty()) {
        return std::nullopt;
    }

    auto frame = capture_window();
    if (frame) {
        return frame;
    }
    // The window may have been recreated by a Waydroid restart.
    if (discover_window()) {
        return capture_window();
    }
    return std::nullopt;
}

std::optional<Frame> KWinBackend::capture_window()
{
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
    if (!capture_into(data, width, height, stride, format)) {
        return std::nullopt;
    }

    if (stride == 0) {
        stride = width * 4;
    }
    const size_t expected_size = static_cast<size_t>(stride) * static_cast<size_t>(height);
    if (data.size() < expected_size) {
        LogWarn << "KWin capture: short frame" << VAR(data.size()) << VAR(expected_size);
        return std::nullopt;
    }
    data.resize(expected_size);

    Frame frame;
    frame.width = static_cast<int>(width);
    frame.height = static_cast<int>(height);
    frame.stride = static_cast<int>(stride);
    frame.id = ++m_frame_id;
    switch (format) {
    case 16: // QImage::Format_RGBX8888
    case 17: // QImage::Format_RGBA8888
    case 18: // QImage::Format_RGBA8888_Premultiplied
        frame.format = PixelFormat::RGBA;
        break;
    case 4: // QImage::Format_RGB32
    case 5: // QImage::Format_ARGB32
    case 6: // QImage::Format_ARGB32_Premultiplied
        frame.format = PixelFormat::BGRA;
        break;
    default:
        LogWarn << "KWin capture: unsupported QImage format" << VAR(format);
        return std::nullopt;
    }
    frame.data = std::move(data);
    return frame;
}

dbus::Message* KWinBackend::new_capture_message(int pipe_fd)
{
    dbus::Message* message =
        m_bus.new_method_call(kKWinService, kScreenShotPath, kScreenShotInterface, "CaptureWindow");
    if (message == nullptr) {
        return nullptr;
    }
    dbus::MessageIter args {};
    if (!dbus_util::init_append(message, args)) {
        dbus_util::message_unref(message);
        return nullptr;
    }
    dbus_util::append_string(args, m_handle);
    dbus::MessageIter options = dbus_util::open_container(args, dbus::kTypeArray, "{sv}");
    dbus::MessageIter entry = dbus_util::open_container(options, dbus::kTypeDictEntry, nullptr);
    dbus_util::append_string(entry, "native-resolution");
    dbus::MessageIter variant = dbus_util::open_container(entry, dbus::kTypeVariant, "b");
    dbus_util::append_boolean(variant, true);
    dbus_util::close_container(entry, variant);
    dbus_util::close_container(options, entry);
    dbus_util::close_container(args, options);
    dbus_util::append_fd(args, pipe_fd);
    return message;
}

bool KWinBackend::capture_into(
    std::vector<uint8_t>& data,
    uint32_t& width,
    uint32_t& height,
    uint32_t& stride,
    uint32_t& format)
{
    std::array<int, 2> pipe_fds { -1, -1 };
    if (pipe2(pipe_fds.data(), O_CLOEXEC) < 0) {
        LogWarn << "KWin capture: pipe2 failed";
        return false;
    }

    std::atomic<bool> caller_closed { false };
    std::thread reader(
        [&data, &caller_closed, read_fd = pipe_fds.at(0)] { read_capture_pipe(read_fd, data, caller_closed); });

    dbus::Message* message = new_capture_message(pipe_fds.at(1));
    if (message == nullptr) {
        ::close(pipe_fds.at(1));
        caller_closed.store(true, std::memory_order_release);
        reader.join();
        ::close(pipe_fds.at(0));
        return false;
    }

    std::string error;
    dbus::Message* reply = m_bus.call(message, kCaptureTimeoutMs, error);
    dbus_util::message_unref(message);
    ::close(pipe_fds.at(1));
    caller_closed.store(true, std::memory_order_release);
    reader.join();
    ::close(pipe_fds.at(0));

    if (reply == nullptr) {
        LogWarn << "KWin capture: CaptureWindow failed" << VAR(error);
        return false;
    }

    const CaptureGeometry geometry = parse_capture_geometry(reply);
    dbus_util::message_unref(reply);
    if (geometry.width == 0 || geometry.height == 0) {
        LogWarn << "KWin capture: CaptureWindow returned no geometry";
        return false;
    }
    width = geometry.width;
    height = geometry.height;
    stride = geometry.stride;
    format = geometry.format;
    return true;
}
} // namespace asst::waydroid
