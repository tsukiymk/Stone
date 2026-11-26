#include "FolderPicker.h"
#include <shobjidl.h>
#include <wrl.h>
#include <memory>

namespace stone::windows {
namespace {
using Microsoft::WRL::ComPtr;
IFileOpenDialog* active = nullptr; // Win32 UI thread only.
} // namespace

void SetInitialFolder(IFileDialog* dialog, const std::wstring& path)
{
    ComPtr<IShellItem> folder;
    if (!path.empty() && SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder.Get());
    }
}

HRESULT PickWindowsFolder(HWND owner, const std::wstring& initialDirectory, const FolderValidation& validate)
{
    ComPtr<IFileOpenDialog> dialog;
    HRESULT status = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(status))
        return status;
    FILEOPENDIALOGOPTIONS options;
    status = dialog->GetOptions(&options);
    if (FAILED(status))
        return status;
    status = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST |
                                FOS_DONTADDTORECENT);
    if (FAILED(status))
        return status;
    SetInitialFolder(dialog.Get(), initialDirectory);

    while (IsWindow(owner)) {
        active = dialog.Get();
        status = dialog->Show(owner);
        active = nullptr;
        if (FAILED(status))
            return status;
        ComPtr<IShellItem> selected;
        status = dialog->GetResult(&selected);
        if (FAILED(status))
            return status;
        PWSTR path = nullptr;
        status = selected->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (FAILED(status))
            return status;
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> ownerPath(path, CoTaskMemFree);
        std::wstring reason;
        status = validate(path, reason);
        if (status != S_FALSE)
            return status;
        auto message = L"Choose another folder. The selected folder is " + reason + L".";
        MessageBoxW(owner, message.c_str(), L"Select Folder", MB_OK | MB_ICONWARNING);
        dialog->SetFolder(selected.Get());
    }
    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
}

bool WindowsFolderPickerActive()
{
    return active != nullptr;
}

void CancelWindowsFolderPicker()
{
    if (active)
        active->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}
} // namespace stone::windows
