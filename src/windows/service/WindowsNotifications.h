#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace stone::windows {
struct WindowsNotification {
    std::wstring key, title, text;
    std::vector<std::wstring> actions;
    int progress = 0, maximum = 0;
    bool indeterminate = false, silent = false, suppressPopup = false;
    std::wstring route;
    uint32_t recipient = 0;
};

// Windows-only host; it has no ART, Android filesystem or Java dependency.
class WindowsNotifications {
    struct State;
    std::shared_ptr<State> state_;

public:
    WindowsNotifications(const std::filesystem::path& settings, const std::filesystem::path& androidData,
                         const std::wstring& package, const std::filesystem::path& diagnostic = {},
                         uint32_t recipient = 0);
    ~WindowsNotifications();
    WindowsNotifications(const WindowsNotifications&) = delete;
    WindowsNotifications& operator=(const WindowsNotifications&) = delete;
    bool Allowed();
    uint64_t Post(const WindowsNotification& notification);
    void Remove(const std::wstring& key);
    void RemoveAll();
    std::vector<int64_t> Poll();
    std::vector<std::wstring> PollRoutes();
    void SynchronizeRoutes(const std::vector<std::wstring>& routes);
    std::vector<std::wstring> History();
};
}
