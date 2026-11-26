#include "stub-libsqlite.h"
#include "abi/AndroidLibraryImports.h"
#include "loader/AndroidSystemElf.h"

#include <cerrno>

namespace stone::windows {
namespace {
uintptr_t UnixVfsImport(std::string_view name)
{
    // SQLite uses WindowsSqliteVfs by default. Its compiled-in Unix VFS
    // still needs imports, so explicitly selecting it fails with ENOSYS.
    if (name == "mmap" || name == "mremap")
        return Import(
            "SQLite Unix VFS map", +[]() -> void* {
                errno = 38;
                return reinterpret_cast<void*>(-1);
            });
    if (name == "getcwd")
        return Import(
            "SQLite Unix VFS getcwd", +[]() -> char* {
                errno = 38;
                return nullptr;
            });
    constexpr std::string_view operations[]{"fdatasync", "android_fdsan_close_with_tag",
                                            "open",      "utimes",
                                            "close",     "access",
                                            "stat",      "fstat",
                                            "ftruncate", "fcntl",
                                            "read",      "pread",
                                            "write",     "pwrite",
                                            "fchmod",    "unlink",
                                            "mkdir",     "rmdir",
                                            "fchown",    "munmap",
                                            "readlink",  "lstat",
                                            "ioctl"};
    for (const auto operation : operations) {
        if (name == operation)
            return Import(
                "SQLite Unix VFS I/O", +[]() -> intptr_t {
                    errno = 38;
                    return -1;
                });
    }
    return 0;
}
}

void LoadImageSqlite(std::span<const uint8_t> bytes)
{
    LoadNativeSystemElf("libsqlite.so", bytes, UnixVfsImport);
}

void* SqliteExport(std::string_view name, std::string_view signature)
{
    return NativeSystemElfThunk("libsqlite.so", name, signature);
}
}
