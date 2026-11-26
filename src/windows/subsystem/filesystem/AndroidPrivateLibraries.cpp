#include "AndroidPrivateLibraries.h"
#include "../../host/WindowsPath.h"
#include <windows.h>
#include <memory>
#include <stdexcept>

namespace stone::windows {
std::vector<uint8_t> ReadAndroidPrivateLibrary(const std::filesystem::path& directory, std::string_view prefix,
                                               std::string_view name)
{
    if (!name.starts_with(prefix) || name.size() <= prefix.size() || !name.ends_with(".so"))
        throw std::runtime_error("native library is outside the current application's private directory");
    const auto relative = name.substr(prefix.size());
    if (relative.find_first_of("\\:\r\n") != relative.npos || relative.find('\0') != relative.npos)
        throw std::runtime_error("invalid private native library path");
    for (size_t at = 0; at < relative.size();) {
        const auto end = relative.find('/', at);
        const auto part = relative.substr(at, end == relative.npos ? relative.size() - at : end - at);
        if (part.empty() || part == "." || part == "..")
            throw std::runtime_error("invalid private native library component");
        if (end == relative.npos)
            break;
        at = end + 1;
    }
    auto root = ExtendedWindowsPath(std::filesystem::canonical(directory)).wstring();
    if (!root.ends_with(L'\\'))
        root += L'\\';
    const auto path = ExtendedWindowsPath(
        directory /
        std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(relative.data()), relative.size())));
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open private native library");
    std::unique_ptr<void, decltype(&CloseHandle)> file(handle, CloseHandle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("private native library must be a regular file");
    std::wstring finalPath(32768, L'\0');
    const auto count = GetFinalPathNameByHandleW(handle, finalPath.data(), static_cast<DWORD>(finalPath.size()),
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count || count >= finalPath.size() || count <= root.size() ||
        CompareStringOrdinal(finalPath.data(), static_cast<int>(root.size()), root.data(),
                             static_cast<int>(root.size()), TRUE) != CSTR_EQUAL)
        throw std::runtime_error("private native library resolves outside the application's directory");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 64 || size.QuadPart > 256 * 1024 * 1024)
        throw std::runtime_error("invalid private native library size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
        throw std::runtime_error("cannot read private native library");
    return bytes;
}
}
