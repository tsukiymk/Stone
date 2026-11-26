#include "platform/windows/GuestPaths.h"
#include "AndroidSystemFiles.h"
#include "../../host/WindowsPath.h"
#include "hle/system/stub-libc-symlinks.h"

#include <windows.h>
#include <cerrno>
#include <string_view>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace stone::windows {
namespace {
std::filesystem::path cacheRoot, dataRoot;
std::filesystem::path storageRoot;
std::wstring storagePackage;
std::mutex storageMutex;
std::atomic_bool storageAllFiles{false};
bool nativeDirectory = false;

std::optional<std::wstring> StoragePath(const std::wstring& path)
{
    for (const auto prefix : {L"sdcard", L"storage/emulated/0", L"storage/self/primary", L"data/media/0"}) {
        const std::wstring_view base(prefix);
        if (path == base)
            return L"";
        if (path.starts_with(base) && path.size() > base.size() && path[base.size()] == L'/')
            return path.substr(base.size() + 1);
    }
    return {};
}

std::pair<std::filesystem::path, std::filesystem::path> NativePath(const std::wstring& path)
{
    if (!storageRoot.empty())
        if (auto relative = StoragePath(path))
            return {storageRoot, *relative};
    if (path == L"data")
        return {dataRoot, {}};
    if (path.starts_with(L"data/"))
        return {dataRoot, path.substr(5)};
    return {cacheRoot, path};
}

}

void SetAndroidSystemDirectory(const std::filesystem::path& directory, const std::filesystem::path& dataDirectory)
{
    cacheRoot = ExtendedWindowsPath(std::filesystem::canonical(directory));
    dataRoot =
        ExtendedWindowsPath(std::filesystem::canonical(dataDirectory.empty() ? cacheRoot / L"data" : dataDirectory));
    storageRoot = (dataRoot.parent_path() / L"storage/emulated/0").make_preferred();
    const auto withinImage = RelativeWindowsGuestPath(dataRoot, cacheRoot);
    if ((withinImage && *withinImage != L"data") || RelativeWindowsGuestPath(cacheRoot, dataRoot))
        throw std::runtime_error("the Android data directory must not overlap the system image cache");
    nativeDirectory = true;
}

std::wstring AndroidSystemVisiblePath(const std::wstring& path)
{
    return L"/" + path;
}

std::optional<std::wstring> AndroidSystemGuestPath(const std::filesystem::path& path)
{
    if (!path.has_root_directory() && !path.has_root_name())
        return std::nullopt;
    const auto name = path.generic_wstring();
    if (name.starts_with(L"//?/") || name.starts_with(L"//./"))
        return std::nullopt;
    if (name.empty() || name.front() != L'/')
        return std::nullopt;
    const auto start = name.find_first_not_of(L'/');
    if (start == name.npos)
        return nativeDirectory ? std::optional(std::wstring{}) : std::nullopt;
    const auto remaining = std::filesystem::path(L"/" + name.substr(start)).lexically_normal().generic_wstring();
    return nativeDirectory ? std::optional(remaining.substr(1)) : std::nullopt;
}

bool IsAndroidSystemGuestReadOnly(const std::wstring& path)
{
    if (nativeDirectory && StoragePath(path))
        return false;
    return !nativeDirectory || (path != L"data" && !path.starts_with(L"data/"));
}

std::optional<std::wstring> AndroidSystemCachedPath(const std::filesystem::path& path)
{
    if (nativeDirectory) {
        if (!storageRoot.empty())
            if (const auto storage = RelativeWindowsGuestPath(path, storageRoot))
                return storage->empty() ? L"storage/emulated/0" : L"storage/emulated/0/" + *storage;
        if (const auto data = RelativeWindowsGuestPath(path, dataRoot))
            return data->empty() ? L"data" : L"data/" + *data;
    }
    return RelativeWindowsGuestPath(path, cacheRoot);
}

bool IsAndroidSystemCachePath(const std::filesystem::path& path)
{
    if (nativeDirectory) {
        if (!storageRoot.empty() && RelativeWindowsGuestPath(path, storageRoot))
            return false;
        if (RelativeWindowsGuestPath(path, dataRoot))
            return false;
        const auto relative = RelativeWindowsGuestPath(path, cacheRoot);
        if (!relative)
            return false;
        return true; // The only writable mount was handled above.
    }
    return false;
}

std::optional<std::filesystem::path> UnresolvedAndroidDataFile(const std::filesystem::path& path)
{
    if (!nativeDirectory)
        return {};
    const auto guest = AndroidSystemGuestPath(path);
    if (!guest || IsAndroidSystemGuestReadOnly(*guest))
        return {};
    const auto [mount, relative] = NativePath(*guest);
    const auto candidate = (mount / relative).lexically_normal().make_preferred();
    if (!RelativeWindowsGuestPath(candidate, mount))
        throw AndroidSystemPathError{ENOENT};
    return ExtendedWindowsPath(candidate);
}

std::filesystem::path ResolveAndroidSystemFile(const std::wstring& path)
{
    if (cacheRoot.empty())
        throw AndroidSystemPathError{ENOENT};
    if (nativeDirectory) {
        const auto [mount, relative] = NativePath(path);
        const auto candidate = (mount / relative).lexically_normal().make_preferred();
        if (!RelativeWindowsGuestPath(candidate, mount))
            throw AndroidSystemPathError{ENOENT};
        std::error_code error;
        const auto resolved = IsAndroidSystemGuestReadOnly(path) ? std::filesystem::canonical(candidate, error)
                                                                 : std::filesystem::weakly_canonical(candidate, error);
        if (error || !RelativeWindowsGuestPath(resolved, mount))
            throw AndroidSystemPathError{ENOENT};
        CheckAndroidSharedStorageAccess(resolved);
        return ExtendedWindowsPath(resolved);
    }
    throw AndroidSystemPathError{ENOENT};
}

std::vector<std::pair<unsigned, std::wstring>> ListAndroidSystemDirectory(const std::wstring& path)
{
    if (nativeDirectory) {
        std::vector<std::pair<unsigned, std::wstring>> entries{{4, L"."}, {4, L".."}};
        for (const auto& entry : std::filesystem::directory_iterator(ResolveAndroidSystemFile(path))) {
            const auto type = IsAndroidSymbolicLink(entry.path()) || entry.is_symlink() ? 10u
                              : entry.is_directory()                                    ? 4u
                                                                                        : 8u;
            entries.emplace_back(type, entry.path().filename().wstring());
        }
        return entries;
    }
    throw AndroidSystemPathError{ENOENT};
}

std::wstring ReadAndroidSystemLink(const std::wstring& path)
{
    if (cacheRoot.empty())
        throw AndroidSystemPathError{ENOENT};
    if (nativeDirectory) {
        const auto [mount, relative] = NativePath(path);
        const auto candidate = (mount / relative).lexically_normal().make_preferred();
        if (!RelativeWindowsGuestPath(candidate, mount))
            throw AndroidSystemPathError{ENOENT};
        std::error_code error;
        const auto parent = std::filesystem::canonical(candidate.parent_path(), error);
        if (error || !RelativeWindowsGuestPath(parent, mount))
            throw AndroidSystemPathError{ENOENT};
        // Resolve the parent, preserving the link itself (including broken links).
        const auto link = std::filesystem::read_symlink(ExtendedWindowsPath(parent / candidate.filename()), error);
        if (error)
            throw AndroidSystemPathError{error == std::errc::no_such_file_or_directory ? ENOENT : EINVAL};
        return link.wstring();
    }
    throw AndroidSystemPathError{ENOENT};
}

void ConfigureAndroidSharedStorage(const std::wstring& packageName, bool allFiles)
{
    if (!nativeDirectory || packageName.empty() ||
        packageName.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.") !=
            packageName.npos)
        throw std::runtime_error("Invalid shared-storage application");
    const std::lock_guard lock(storageMutex);
    if (!storagePackage.empty() && storagePackage != packageName)
        throw std::runtime_error("Shared storage belongs to another application");
    if (!storagePackage.empty()) {
        storageAllFiles.store(allFiles);
        return;
    }
    // Preserve the previous emulated volume when upgrading an existing profile.
    // Both locations are inside the selected profile; never overwrite a new root.
    const auto profile = dataRoot.parent_path();
    if (!RelativeWindowsGuestPath(std::filesystem::weakly_canonical(storageRoot), profile))
        throw std::runtime_error("Shared storage escapes the selected profile");
    std::filesystem::create_directories(storageRoot.parent_path());
    for (const auto legacy : {profile / L"sdcard", (dataRoot / L"media/0").make_preferred()}) {
        if (!std::filesystem::exists(storageRoot) && std::filesystem::is_directory(legacy)) {
            const auto resolved = RelativeWindowsGuestPath(std::filesystem::canonical(legacy), legacy);
            if (!resolved || !resolved->empty() || std::filesystem::is_symlink(legacy) || IsAndroidSymbolicLink(legacy))
                throw std::runtime_error("Invalid legacy storage directory");
            std::error_code error;
            std::filesystem::rename(legacy, storageRoot, error);
            if (error && (!std::filesystem::is_directory(storageRoot) || std::filesystem::exists(legacy)))
                throw std::filesystem::filesystem_error("Cannot migrate shared storage", legacy, storageRoot, error);
        }
    }
    std::filesystem::create_directories(storageRoot);
    const auto resolved = RelativeWindowsGuestPath(std::filesystem::canonical(storageRoot), storageRoot);
    if (!resolved || !resolved->empty() || std::filesystem::is_symlink(storageRoot) ||
        IsAndroidSymbolicLink(storageRoot))
        throw std::runtime_error("Shared storage must be a directory in the selected profile");
    const auto createDirectory = [&](const std::filesystem::path& relative) {
        const auto directory = (storageRoot / relative).make_preferred();
        const auto destination = RelativeWindowsGuestPath(std::filesystem::weakly_canonical(directory), storageRoot);
        if (!destination)
            throw std::runtime_error("Shared-storage directory escapes the selected profile");
        std::filesystem::create_directories(directory);
    };
    for (const auto directory :
         {L"Android", L"Android/data", L"Android/obb", L"Android/media", L"Alarms", L"Audiobooks", L"DCIM",
          L"Documents", L"Download", L"Movies", L"Music", L"Notifications", L"Pictures", L"Pictures/Screenshots",
          L"Podcasts", L"Recordings", L"Ringtones"})
        createDirectory(directory);
    storagePackage = packageName;
    storageAllFiles.store(allFiles);
}

void CheckAndroidSharedStorageAccess(const std::filesystem::path& path)
{
    if (storageRoot.empty())
        return;
    const auto relative = RelativeWindowsGuestPath(path, storageRoot);
    if (!relative)
        return;
    std::error_code error;
    const auto resolved = IsAndroidSymbolicLink(path)
                              ? std::filesystem::weakly_canonical(path.parent_path(), error) / path.filename()
                              : std::filesystem::weakly_canonical(path, error);
    if (error || !RelativeWindowsGuestPath(resolved, storageRoot))
        throw AndroidSystemPathError{EACCES};
    const std::lock_guard lock(storageMutex);
    if (storagePackage.empty())
        return; // Image preparation and native component tests.
    const auto inside = [](std::wstring_view name, std::wstring_view base) {
        return name.size() >= base.size() && (name.size() == base.size() || name[base.size()] == L'/') &&
               CompareStringOrdinal(name.data(), static_cast<int>(base.size()), base.data(),
                                    static_cast<int>(base.size()), TRUE) == CSTR_EQUAL;
    };
    for (const auto prefix : {L"Android/data", L"Android/obb"}) {
        const std::wstring_view base(prefix);
        if (inside(*relative, base)) {
            const auto own = std::wstring(base) + L"/" + storagePackage;
            if (inside(*relative, own))
                return;
            throw AndroidSystemPathError{EACCES};
        }
    }
    if (!storageAllFiles.load())
        throw AndroidSystemPathError{EACCES};
}

void CheckAndroidSharedStorageTarget(const std::filesystem::path& source, const std::filesystem::path& target)
{
    const auto name = source.generic_wstring();
    const bool shared = RelativeWindowsGuestPath(source, storageRoot).has_value() ||
                        (!name.empty() && name.front() == L'/' && StoragePath(name.substr(1)).has_value());
    if (shared && !RelativeWindowsGuestPath(target, storageRoot))
        throw AndroidSystemPathError{EACCES};
    CheckAndroidSharedStorageAccess(target);
}
}
