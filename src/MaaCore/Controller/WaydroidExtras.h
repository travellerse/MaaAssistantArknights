#pragma once

#if ASST_WITH_WAYDROID_EXTRAS

#include "MaaUtils/NoWarningCVMat.hpp"
#include "Waydroid/CaptureBackend.h"

#include <meojson/json.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace asst
{
// In-core Waydroid screencap enhancement. The compositor backends live in
// Controller/Waydroid and talk to the host session directly; no helper
// process or IPC endpoint is involved.
class WaydroidExtras
{
public:
    enum class Mode : uint8_t
    {
        Off,
        Auto,
        KWin,
        Portal,
    };

    WaydroidExtras() = default;
    ~WaydroidExtras();

    WaydroidExtras(const WaydroidExtras&) = delete;
    WaydroidExtras& operator=(const WaydroidExtras&) = delete;
    WaydroidExtras(WaydroidExtras&&) = delete;
    WaydroidExtras& operator=(WaydroidExtras&&) = delete;

    // Parses extras without touching the compositor. The backend is started
    // lazily by the first screencap() so that connection setup stays cheap.
    bool init(const json::object& extras, int width, int height);
    void uninit() noexcept;
    // Turns the enhancement off; used when a reconnect selects a
    // non-Waydroid config so the previous mode does not leak into it.
    void disable() noexcept;
    bool reload(int width, int height);

    [[nodiscard]] bool enabled() const noexcept { return m_mode != Mode::Off; }

    [[nodiscard]] bool forced() const noexcept { return m_mode == Mode::KWin || m_mode == Mode::Portal; }

    // Starts the selected backend without taking a frame. Called before the
    // AdbController probe timer so one-time compositor/portal setup is not
    // mistaken for the steady-state screencap cost.
    bool prepare();

    // Retries KWin when the first probe ran before the Waydroid window
    // existed. Rate-limited and never falls through to the portal, so a
    // screencap never blocks on a window picker.
    bool try_upgrade();

    std::optional<cv::Mat> screencap();

private:
    bool start_backend(int width, int height, bool allow_portal);
    std::optional<cv::Mat> convert(const waydroid::Frame& frame);

    Mode m_mode = Mode::Off;
    std::string m_selector;
    std::unique_ptr<waydroid::CaptureBackend> m_backend;

    int m_width = 0;
    int m_height = 0;
    bool m_portal_rejected = false;
    bool m_kwin_available = false;
    bool m_size_warned = false;
    std::chrono::steady_clock::time_point m_next_upgrade {};
};
} // namespace asst

#endif // ASST_WITH_WAYDROID_EXTRAS
