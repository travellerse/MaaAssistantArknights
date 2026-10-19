#include "WindowMatcher.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace asst::waydroid
{
std::string to_lower(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool is_waydroid_pid(int pid)
{
    if (pid <= 0) {
        return false;
    }
    std::ifstream file("/proc/" + std::to_string(pid) + "/cgroup");
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("waydroid") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool is_app_window_class(const std::string& resource_class)
{
    // App windows in Waydroid multi-window mode use waydroid.<package>; the
    // full UI shell uses the bare Waydroid class.
    return to_lower(resource_class).starts_with("waydroid.");
}

int window_score(bool minimized, const std::string& resource_class)
{
    return (minimized ? 0 : 2) + (is_app_window_class(resource_class) ? 1 : 0);
}

bool is_waydroid_window(
    const std::string& resource_class,
    const std::string& caption,
    int pid,
    const std::string& selector)
{
    if (!selector.empty()) {
        const std::string lower_selector = to_lower(selector);
        return to_lower(resource_class).find(lower_selector) != std::string::npos ||
               to_lower(caption).find(lower_selector) != std::string::npos;
    }
    const std::string lower_class = to_lower(resource_class);
    if (lower_class == "waydroid" || lower_class.starts_with("waydroid.")) {
        return true;
    }
    // Full-UI and some app windows do not use the waydroid.* class. The
    // window's client lives in the Waydroid LXC cgroup, which is stable.
    return is_waydroid_pid(pid);
}
} // namespace asst::waydroid
