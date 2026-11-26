#pragma once

#include <windows.h>
#include <cstdint>

namespace stone::windows {
bool IsWindowsRegularFile(HANDLE file);
HANDLE DuplicateWindowsBinderHandle(HANDLE source, HANDLE target, bool socketGrant);
void RevokeWindowsBinderHandle(HANDLE target, HANDLE remote);
int64_t WindowsHandleSize(HANDLE file);
} // namespace stone::windows
