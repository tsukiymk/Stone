#pragma once

#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace stone::windows {
inline constexpr wchar_t UserStateSubdirectory[] = L"Trebledot Productions\\Stone";

inline std::filesystem::path UserStateDirectory()
{
    std::wstring local(32768, L'\0');
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!size || size >= local.size())
        throw std::runtime_error("Cannot locate the Stone user data directory: LOCALAPPDATA is unavailable");
    return std::filesystem::path(local.substr(0, size)) / UserStateSubdirectory;
}

inline std::filesystem::path DefaultAndroidDataDirectory()
{
    return UserStateDirectory() / L"User Data";
}

inline std::filesystem::path UserLogDirectory()
{
    // Explicit override keeps diagnostic/integration runs inside their test tree.
    std::wstring overridePath(32768, L'\0');
    const auto size =
        GetEnvironmentVariableW(L"STONE_LOG_DIRECTORY", overridePath.data(), static_cast<DWORD>(overridePath.size()));
    if (size >= overridePath.size())
        throw std::runtime_error("Stone log directory is too long");
    if (size) {
        const std::filesystem::path directory(overridePath.substr(0, size));
        if (!directory.is_absolute())
            throw std::runtime_error("STONE_LOG_DIRECTORY must be an absolute path");
        return directory;
    }
    return UserStateDirectory() / L"Logs";
}
}
