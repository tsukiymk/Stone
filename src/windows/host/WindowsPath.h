#pragma once

#include <filesystem>

namespace stone::windows {
// canonical()/weakly_canonical() strip the Win32 extended prefix in MSVC's STL.
// Restore it before file access so long image paths work without registry changes.
inline std::filesystem::path ExtendedWindowsPath(std::filesystem::path path)
{
    auto value = std::filesystem::absolute(path).make_preferred().wstring();
    if (value.starts_with(L"\\\\?\\"))
        return value;
    if (value.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
}
