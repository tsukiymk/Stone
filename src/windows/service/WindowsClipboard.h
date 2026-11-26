#pragma once

#include <memory>
#include <string>
#include <vector>

namespace stone::windows {
// A worker owns the Windows clipboard window. No Android objects or runtime.
class WindowsClipboard {
    struct State;
    std::shared_ptr<State> state_;

public:
    WindowsClipboard();
    ~WindowsClipboard();
    WindowsClipboard(const WindowsClipboard&) = delete;
    WindowsClipboard& operator=(const WindowsClipboard&) = delete;
    // sequence; describe; get; clear; set,label,text,opaque-description-extras.
    // Replies: sequence,present,has-text,label,text,extras. Describe omits text.
    std::vector<std::wstring> Exchange(const std::vector<std::wstring>& fields);
    bool Stop(unsigned timeoutMillis = 500) noexcept;
};
}
