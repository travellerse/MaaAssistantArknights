#pragma once

#include <string>
#include <string_view>

namespace asst::waydroid
{
std::string to_lower(std::string_view text);

// Reads /proc/<pid>/cgroup; used when the window class does not reveal that
// the client runs inside the Waydroid container.
bool is_waydroid_pid(int pid);

bool is_app_window_class(const std::string& resource_class);
int window_score(bool minimized, const std::string& resource_class);

bool is_waydroid_window(
    const std::string& resource_class,
    const std::string& caption,
    int pid,
    const std::string& selector);
} // namespace asst::waydroid
