#pragma once

#include <windows.h>
#include <functional>
#include <string>

namespace stone::windows {
using FolderValidation = std::function<HRESULT(const wchar_t*, std::wstring&)>;
bool WindowsFolderPickerActive();
HRESULT PickWindowsFolder(HWND owner, const std::wstring& initialDirectory, const FolderValidation& validate);
void CancelWindowsFolderPicker();
} // namespace stone::windows
