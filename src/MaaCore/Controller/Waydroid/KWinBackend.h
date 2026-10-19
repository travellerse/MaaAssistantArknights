#pragma once

#include "CaptureBackend.h"
#include "DBus.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asst::waydroid
{
class KWinBackend final : public CaptureBackend
{
public:
    KWinBackend();
    ~KWinBackend() override;

    KWinBackend(const KWinBackend&) = delete;
    KWinBackend& operator=(const KWinBackend&) = delete;
    KWinBackend(KWinBackend&&) = delete;
    KWinBackend& operator=(KWinBackend&&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override { return "kwin"; }

    bool start(int width, int height, std::string_view selector) override;
    std::optional<Frame> capture() override;
    void stop() noexcept override;

    // True when KWin's scripting interface answered during the last attempt.
    // The capture manager uses this to retry later when no window exists yet.
    [[nodiscard]] bool reachable() const noexcept { return m_kwin_reachable; }

private:
    bool connect_bus();
    bool discover_window();
    bool run_script(const std::string& path, const std::string& plugin_name);
    bool select_window(const std::string& report);
    dbus::Message* new_capture_message(int pipe_fd);
    bool
        capture_into(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height, uint32_t& stride, uint32_t& format);
    std::optional<Frame> capture_window();
    void release_bus() noexcept;

    static int on_report(dbus::Connection* connection, dbus::Message* message, void* user_data);

    DBusConnection m_bus;
    std::string m_service_name;
    std::string m_object_path;
    std::string m_selector;
    std::string m_handle;
    std::string m_caption;
    std::string m_resource_class;

    int m_expected_width = 0;
    int m_expected_height = 0;

    bool m_reported = false;
    bool m_kwin_reachable = false;
    std::string m_report;

    std::chrono::steady_clock::time_point m_last_discovery {};

    uint64_t m_frame_id = 0;
};
} // namespace asst::waydroid
