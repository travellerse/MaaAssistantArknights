#pragma once

#include "CaptureBackend.h"
#include "DBus.h"
#include "PipeWireABI.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asst::waydroid
{
class PortalBackend final : public CaptureBackend
{
public:
    PortalBackend();
    ~PortalBackend() override;

    PortalBackend(const PortalBackend&) = delete;
    PortalBackend& operator=(const PortalBackend&) = delete;
    PortalBackend(PortalBackend&&) = delete;
    PortalBackend& operator=(PortalBackend&&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override { return "portal"; }

    bool start(int width, int height, std::string_view selector) override;
    std::optional<Frame> capture() override;
    void stop() noexcept override;

    // True when the user cancelled or the portal rejected the request. The
    // capture manager uses it to avoid prompting again in the same session.
    [[nodiscard]] bool rejected() const noexcept { return m_rejected; }

private:
    struct PipeWire;

    struct PortalStream
    {
        uint32_t node_id = 0;
        int width = 0;
        int height = 0;
        uint32_t source_type = 0;
    };

    bool open_portal(std::string& error);
    bool query_source_types(uint32_t& types, std::string& error);
    bool create_session(std::string& error);
    bool select_sources(std::string& error);
    bool start_session(std::string& error);
    bool open_pipewire(std::string& error);
    int open_pipewire_remote(std::string& error);
    bool start_pipewire(int fd, uint32_t node_id, std::string& error);
    bool init_pipewire(int fd, std::string& error);
    bool connect_pipewire_stream(uint32_t node_id, std::string& error);
    void stop_pipewire() noexcept;
    void close_portal_session() noexcept;
    bool wait_for_first_frame(std::string& error);

    bool send_request(
        const char* member,
        std::string_view handle_token,
        const std::function<void(dbus::MessageIter&)>& append_args,
        std::string& error);
    bool request(
        const char* member,
        std::string_view handle_token,
        const std::function<void(dbus::MessageIter&)>& append_args,
        int timeout_ms,
        std::string& error);
    bool wait_response(int timeout_ms, std::string& error);
    void handle_response(dbus::Message* message);
    void parse_response_results(dbus::MessageIter& array);
    void parse_streams(dbus::MessageIter& array);
    static void parse_stream_properties(dbus::MessageIter& array, PortalStream& stream);

    static int on_bus_message(dbus::Connection* connection, dbus::Message* message, void* user_data);

    void on_process();
    void on_param_changed(uint32_t id, const pwabi::SpaPod* param);
    void on_state_changed(int state, const char* error);
    static void on_process_cb(void* user_data);
    static void on_param_changed_cb(void* user_data, uint32_t id, const pwabi::SpaPod* param);
    static void on_state_changed_cb(void* user_data, int /*old*/, int state, const char* error);

    std::string next_token();
    static std::string load_restore_token();
    static void save_restore_token(const std::string& token);
    static std::string token_path();

    DBusConnection m_bus;
    std::string m_session_handle;
    std::string m_restore_token;

    std::string m_pending_path;
    bool m_pending_done = false;
    uint32_t m_response = 2;
    std::string m_pending_session_handle;
    std::string m_pending_restore_token;
    std::vector<PortalStream> m_pending_streams;

    uint64_t m_request_counter = 0;

    std::unique_ptr<PipeWire> m_pipewire;
    pwabi::PwStreamEvents m_events {};
    pwabi::SpaHookStorage m_hook {};

    int m_expected_width = 0;
    int m_expected_height = 0;
    int m_stream_width = 0;
    int m_stream_height = 0;
    PixelFormat m_stream_format = PixelFormat::BGRX;

    mutable std::mutex m_mutex;
    std::condition_variable m_frame_ready;
    Frame m_latest;
    uint64_t m_frame_seq = 0;
    uint64_t m_consumed_seq = 0;
    bool m_has_frame = false;
    bool m_want_frame = false;
    bool m_stream_error = false;
    bool m_started = false;
    bool m_rejected = false;
};
} // namespace asst::waydroid
