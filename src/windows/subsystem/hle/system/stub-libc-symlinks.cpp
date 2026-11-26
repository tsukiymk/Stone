#include "stub-libc-symlinks.h"
#include "filesystem/AndroidSystemFiles.h"
#include <windows.h>
#include <winioctl.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <vector>

namespace stone::windows {
namespace {
struct Handle {
    HANDLE value;

    ~Handle()
    {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

int FileError(DWORD code)
{
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return EACCES;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_FILENAME_EXCED_RANGE:
        return 36;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        return 95;
    default:
        return EIO;
    }
}

bool ValidTarget(std::string_view target)
{
    return !target.empty() && target.size() < 4096 && target.find('\0') == target.npos &&
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target.data(), static_cast<int>(target.size()), nullptr,
                               0) > 0;
}

std::filesystem::path Resolve(const std::filesystem::path& input, bool writable, bool followFinal,
                              AndroidPathMapper mapper, unsigned& followed)
{
    namespace fs = std::filesystem;
    if (input.empty())
        return input;
    auto text = input.generic_u8string();
    if (text.size() >= 3 && text[0] == u8'/' && text[2] == u8':' && (text.size() == 3 || text[3] == u8'/'))
        text.erase(text.begin());
    auto absolute = fs::path(text);
    if (!absolute.has_root_directory())
        absolute = fs::absolute(absolute);
    fs::path current = absolute.root_path();
    std::vector<fs::path> parts;
    for (const auto& part : absolute.relative_path())
        parts.push_back(part);
    for (size_t at = 0; at < parts.size(); ++at) {
        const auto& part = parts[at];
        if (part == L".")
            continue;
        if (part == L"..") {
            current = current.parent_path();
            continue;
        }
        current /= part;
        if (at + 1 == parts.size() && !followFinal)
            continue;
        const auto name = current.generic_u8string();
        fs::path mapped;
        // Ancestors may be read-only while containing a distinct writable /data mount.
        // Enforce write access after resolving the complete destination.
        try {
            mapped = mapper(reinterpret_cast<const char*>(name.c_str()), false);
        }
        catch (const AndroidSystemPathError& error) {
            if (error.code == ENOENT && at + 1 < parts.size())
                continue;
            throw;
        }
        const auto attributes = GetFileAttributesW(mapped.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES && at + 1 < parts.size())
            throw AndroidSystemPathError{FileError(GetLastError())};
        if (at + 1 < parts.size() && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw AndroidSystemPathError{ENOTDIR};
        const auto target = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                                ? ReadAndroidSymbolicLink(mapped)
                                : std::optional<std::string>{};
        if (!target)
            continue;
        if (++followed > 40)
            throw AndroidSystemPathError{40};
        auto destination =
            fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(target->data()), target->size()));
        if (!destination.has_root_directory() && !destination.has_root_name())
            destination = current.parent_path() / destination;
        else if (!destination.has_root_directory())
            destination = fs::absolute(destination);
        for (size_t rest = at + 1; rest < parts.size(); ++rest)
            destination /= parts[rest];
        return Resolve(destination, writable, followFinal, mapper, followed);
    }
    const auto name = current.generic_u8string();
    return mapper(reinterpret_cast<const char*>(name.c_str()), writable);
}
}

bool IsAndroidSymbolicLink(const std::filesystem::path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                            nullptr)};
    FILE_ATTRIBUTE_TAG_INFO info{};
    return file.value != INVALID_HANDLE_VALUE &&
           GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &info, sizeof(info)) &&
           info.ReparseTag == AndroidSymlinkTag;
}

std::optional<std::string> ReadAndroidSymbolicLink(uintptr_t handle)
{
    const auto file = reinterpret_cast<HANDLE>(handle);
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)))
        throw AndroidSystemPathError{FileError(GetLastError())};
    if (!(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || info.ReparseTag != AndroidSymlinkTag)
        return {};
    alignas(8) std::array<uint8_t, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> bytes{};
    DWORD returned = 0;
    if (!DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, nullptr, 0, bytes.data(), static_cast<DWORD>(bytes.size()),
                         &returned, nullptr))
        throw AndroidSystemPathError{FileError(GetLastError())};
    uint32_t tag = 0, version = 0;
    uint16_t length = 0;
    if (returned < 12)
        throw AndroidSystemPathError{EIO};
    std::memcpy(&tag, bytes.data(), 4);
    std::memcpy(&length, bytes.data() + 4, 2);
    std::memcpy(&version, bytes.data() + 8, 4);
    if (tag != AndroidSymlinkTag || length < 5 || size_t(length) + 8 != returned)
        throw AndroidSystemPathError{EIO};
    if (version != 2)
        throw AndroidSystemPathError{95};
    std::string target(reinterpret_cast<const char*>(bytes.data() + 12), length - 4);
    if (!ValidTarget(target))
        throw AndroidSystemPathError{EIO};
    return target;
}

std::optional<std::string> ReadAndroidSymbolicLink(const std::filesystem::path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return {};
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                            nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        throw AndroidSystemPathError{FileError(GetLastError())};
    return ReadAndroidSymbolicLink(reinterpret_cast<uintptr_t>(file.value));
}

int CreateAndroidSymbolicLink(std::string_view target, const std::filesystem::path& link)
{
    if (!ValidTarget(target)) {
        errno = target.empty() ? ENOENT : target.size() >= 4096 ? 36 : 84;
        return -1;
    }
    try {
        std::vector<uint8_t> bytes(12 + target.size());
        const uint16_t length = static_cast<uint16_t>(4 + target.size());
        const uint32_t version = 2;
        std::memcpy(bytes.data(), &AndroidSymlinkTag, 4);
        std::memcpy(bytes.data() + 4, &length, 2);
        std::memcpy(bytes.data() + 8, &version, 4);
        std::memcpy(bytes.data() + 12, target.data(), target.size());
        Handle file{CreateFileW(link.c_str(), GENERIC_WRITE | DELETE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            errno = FileError(GetLastError());
            return -1;
        }
        DWORD returned = 0;
        if (DeviceIoControl(file.value, FSCTL_SET_REPARSE_POINT, bytes.data(), static_cast<DWORD>(bytes.size()),
                            nullptr, 0, &returned, nullptr))
            return 0;
        const int error = FileError(GetLastError());
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
        errno = error;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

std::filesystem::path ResolveAndroidSymbolicLinks(const std::filesystem::path& path, bool writable, bool followFinal,
                                                  AndroidPathMapper mapper)
{
    unsigned followed = 0;
    return Resolve(path, writable, followFinal, mapper, followed);
}
}
