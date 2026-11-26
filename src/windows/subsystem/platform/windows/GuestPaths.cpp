#include "platform/windows/GuestPaths.h"
#include <windows.h>

namespace stone::windows {
std::optional<std::wstring> RelativeWindowsGuestPath(const std::filesystem::path& path,
                                                     const std::filesystem::path& parent)
{
    if (parent.empty() || !path.is_absolute())
        return std::nullopt;
    auto name = path.lexically_normal().generic_wstring();
    if (name.starts_with(L"//?/UNC/"))
        name.replace(0, 8, L"//");
    else if (name.starts_with(L"//?/"))
        name.erase(0, 4);
    auto root = parent.generic_wstring();
    if (root.starts_with(L"//?/UNC/"))
        root.replace(0, 8, L"//");
    else if (root.starts_with(L"//?/"))
        root.erase(0, 4);
    if (name.size() < root.size() || (name.size() > root.size() && name[root.size()] != L'/') ||
        CompareStringOrdinal(name.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()),
                             TRUE) != CSTR_EQUAL)
        return std::nullopt;
    return name.size() == root.size() ? std::wstring{} : name.substr(root.size() + 1);
}

}
