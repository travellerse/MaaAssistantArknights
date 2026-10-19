#include "WaydroidExtras.h"

#if ASST_WITH_WAYDROID_EXTRAS

#include "MaaUtils/NoWarningCV.hpp"
#include "Utils/Logger.hpp"
#include "Waydroid/KWinBackend.h"
#include "Waydroid/PortalBackend.h"

#include <meojson/json.hpp>

#include <cstdint>

namespace asst
{
namespace
{
bool same_aspect(int width, int height, int expected_width, int expected_height)
{
    return static_cast<int64_t>(width) * expected_height == static_cast<int64_t>(height) * expected_width;
}
} // namespace

WaydroidExtras::~WaydroidExtras()
{
    uninit();
}

bool WaydroidExtras::init(const json::object& extras, int width, int height)
{
    m_width = width;
    m_height = height;
    m_mode = Mode::Auto;
    m_selector.clear();
    m_portal_rejected = false;
    m_kwin_available = false;
    m_next_upgrade = {};

    if (auto object = extras.find<json::object>("waydroid_capture")) {
        const std::string mode = object->get("mode", std::string("auto"));
        if (mode == "off" || mode == "none") {
            m_mode = Mode::Off;
        }
        else if (mode == "kwin") {
            m_mode = Mode::KWin;
        }
        else if (mode == "portal") {
            m_mode = Mode::Portal;
        }
        else {
            m_mode = Mode::Auto;
        }
        m_selector = object->get("window", "");
    }
    else if (auto value = extras.find<json::value>("waydroid_capture")) {
        if (value->is_boolean()) {
            m_mode = value->as_boolean() ? Mode::Auto : Mode::Off;
        }
        else if (value->is_string()) {
            const std::string mode = value->as_string();
            if (mode == "off" || mode == "none") {
                m_mode = Mode::Off;
            }
            else if (mode == "kwin") {
                m_mode = Mode::KWin;
            }
            else if (mode == "portal") {
                m_mode = Mode::Portal;
            }
        }
    }

    const int mode_value = static_cast<int>(m_mode);
    LogInfo << "Waydroid capture configured" << VAR(mode_value) << VAR(m_selector);
    return true;
}

void WaydroidExtras::uninit() noexcept
{
    if (m_backend != nullptr) {
        m_backend->stop();
        m_backend.reset();
    }
    m_size_warned = false;
}

bool WaydroidExtras::reload(int width, int height)
{
    uninit();
    m_width = width;
    m_height = height;
    return enabled();
}

bool WaydroidExtras::start_backend(int width, int height, bool allow_portal)
{
    if (m_backend != nullptr) {
        return true;
    }
    if (m_mode == Mode::Off) {
        return false;
    }

    bool kwin_reachable = false;
    if (m_mode == Mode::Auto || m_mode == Mode::KWin) {
        auto backend = std::make_unique<waydroid::KWinBackend>();
        if (backend->start(width, height, m_selector)) {
            m_kwin_available = true;
            LogInfo << "Waydroid capture backend started" << VAR(backend->name());
            m_backend = std::move(backend);
            return true;
        }
        kwin_reachable = backend->reachable();
        m_kwin_available = kwin_reachable;
        backend->stop();
    }

    // On KDE the window may simply not exist yet. Do not raise a portal
    // picker in that case; try_upgrade() retries KWin when the window shows.
    const bool portal_allowed = allow_portal && (m_mode == Mode::Auto || m_mode == Mode::Portal) &&
                                !m_portal_rejected && !(m_mode == Mode::Auto && kwin_reachable);
    if (portal_allowed) {
        auto backend = std::make_unique<waydroid::PortalBackend>();
        if (backend->start(width, height, m_selector)) {
            LogInfo << "Waydroid capture backend started" << VAR(backend->name());
            m_backend = std::move(backend);
            return true;
        }
        m_portal_rejected = backend->rejected();
        backend->stop();
    }
    return false;
}

bool WaydroidExtras::prepare()
{
    return start_backend(m_width, m_height, true);
}

bool WaydroidExtras::try_upgrade()
{
    if (m_backend != nullptr) {
        return true;
    }
    if (m_mode == Mode::Off || m_mode == Mode::Portal || !m_kwin_available) {
        return false;
    }
    constexpr auto kRetryInterval = std::chrono::seconds(10);
    const auto now = std::chrono::steady_clock::now();
    if (now < m_next_upgrade) {
        return false;
    }
    m_next_upgrade = now + kRetryInterval;
    return start_backend(m_width, m_height, false);
}

std::optional<cv::Mat> WaydroidExtras::screencap()
{
    if (m_backend == nullptr && !start_backend(m_width, m_height, false)) {
        return std::nullopt;
    }

    auto frame = m_backend->capture();
    if (!frame) {
        LogWarn << "Waydroid capture failed, dropping backend" << VAR(m_backend->name());
        m_backend->stop();
        m_backend.reset();
        return std::nullopt;
    }
    return convert(*frame);
}

std::optional<cv::Mat> WaydroidExtras::convert(const waydroid::Frame& frame)
{
    const int width = frame.width;
    const int height = frame.height;
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    if (frame.stride < width * 4 || frame.data.size() < static_cast<size_t>(frame.stride) * height) {
        LogWarn << "Waydroid capture: invalid frame layout" << VAR(width) << VAR(height) << VAR(frame.stride)
                << VAR(frame.data.size());
        return std::nullopt;
    }

    const cv::Mat
        raw(height, width, CV_8UC4, const_cast<uint8_t*>(frame.data.data()), static_cast<size_t>(frame.stride));
    cv::Mat bgr;
    switch (frame.format) {
    case waydroid::PixelFormat::RGBA:
    case waydroid::PixelFormat::RGBX:
        cv::cvtColor(raw, bgr, cv::COLOR_RGBA2BGR);
        break;
    case waydroid::PixelFormat::BGRA:
    case waydroid::PixelFormat::BGRX:
        cv::cvtColor(raw, bgr, cv::COLOR_BGRA2BGR);
        break;
    default:
        return std::nullopt;
    }

    if (width == m_width && height == m_height) {
        return bgr;
    }
    if (width == m_height && height == m_width) {
        LogWarn << "Waydroid capture: rotated frame, falling back" << VAR(width) << VAR(height) << VAR(m_width)
                << VAR(m_height);
        return std::nullopt;
    }
    if (!same_aspect(width, height, m_width, m_height)) {
        LogWarn << "Waydroid capture: frame aspect mismatch" << VAR(width) << VAR(height) << VAR(m_width)
                << VAR(m_height);
        return std::nullopt;
    }

    if (!m_size_warned) {
        LogInfo << "Waydroid capture: scaling frame to the ADB resolution" << VAR(width) << VAR(height) << VAR(m_width)
                << VAR(m_height);
        m_size_warned = true;
    }
    const int interpolation = width > m_width ? cv::INTER_AREA : cv::INTER_LINEAR;
    cv::Mat scaled;
    cv::resize(bgr, scaled, cv::Size(m_width, m_height), 0.0, 0.0, interpolation);
    return scaled;
}
} // namespace asst

#endif // ASST_WITH_WAYDROID_EXTRAS
