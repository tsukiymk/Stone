#pragma once

#include <windows.h>
#include <winrt/base.h>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>
struct IFileSaveDialog;

namespace stone::windows {
struct SelectedDocument {
    winrt::handle file;
    std::wstring name;
};

using DocumentDialogObserver = std::function<void(IFileSaveDialog*, HWND)>;
uint64_t CreateWindowDocument(HWND owner, std::wstring name, std::wstring mime);
uint64_t CreateWindowDocumentTree(HWND owner);
// Runs on a worker thread. The optional observer is used by the native UI test only.
std::optional<SelectedDocument> PickWindowDocument(uint64_t id, const DocumentDialogObserver& observer = {});
void CancelWindowDocument(uint64_t id);
void ReleaseWindowDocument(uint64_t id);
void CloseWindowDocuments(HWND owner);
uint64_t KeepWindowDocument(SelectedDocument selection);
winrt::handle OpenWindowDocument(uint64_t grant, bool read, bool write, bool truncate, bool append);
uint64_t WindowDocumentSize(uint64_t grant);
void ReleaseWindowDocumentGrant(uint64_t grant);
// The probe alone installs this guard before loading an APK. Production never reads its test environment.
void ConstrainWindowDocumentTestTarget(std::filesystem::path target);
}
