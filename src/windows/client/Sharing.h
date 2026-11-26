#pragma once

#include <windows.h>
#include <string>
#include <filesystem>
#include <memory>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

namespace stone::windows {
inline constexpr UINT WindowsShareMessage = WM_APP + 10;
struct ShareFile;

struct ShareContent {
    std::wstring title, text;
    std::shared_ptr<ShareFile> file;
};

// Copies the borrowed disk handle on a worker thread; never exposes the source path.
ShareContent SnapshotShareFile(std::wstring title, const std::wstring& name, HANDLE source,
                               const std::filesystem::path& cache);
void PopulateSharePackage(const winrt::Windows::ApplicationModel::DataTransfer::DataPackage& data,
                          const ShareContent& content);
void AttachWindowSharing(HWND owner);
void QueueWindowShare(HWND owner, ShareContent content);
void ShowWindowShare(HWND owner) noexcept;
void CloseWindowSharing(HWND owner) noexcept;
}
