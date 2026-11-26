#include "BinderHandles.h"

namespace stone::windows {
bool IsWindowsRegularFile(HANDLE file)
{
    BY_HANDLE_FILE_INFORMATION info{};
    return file != nullptr && file != INVALID_HANDLE_VALUE && GetFileType(file) == FILE_TYPE_DISK &&
           GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

HANDLE DuplicateWindowsBinderHandle(HANDLE source, HANDLE target, bool socketGrant)
{
    HANDLE remote = nullptr;
    if ((socketGrant && GetFileType(source) == FILE_TYPE_PIPE) || IsWindowsRegularFile(source))
        DuplicateHandle(GetCurrentProcess(), source, target, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return remote;
}

void RevokeWindowsBinderHandle(HANDLE target, HANDLE remote)
{
    DuplicateHandle(target, remote, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
}

int64_t WindowsHandleSize(HANDLE file)
{
    LARGE_INTEGER size{};
    return GetFileSizeEx(file, &size) ? size.QuadPart : -1;
}
} // namespace stone::windows
