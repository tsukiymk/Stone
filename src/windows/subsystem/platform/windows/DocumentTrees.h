#pragma once

#include "../../../client/Documents.h"
#include <vector>

namespace stone::windows {
void ConfigureDocumentTrees(const std::filesystem::path& storage);
// Flat records: root id, display name, persisted time; document id, name, directory, size, modified time.
std::vector<std::wstring> RestoreDocumentTrees();
std::vector<std::wstring> KeepDocumentTree(SelectedDocument selection);
void PersistDocumentTree(const std::wstring& id, uint64_t time);
void ReleaseDocumentTree(const std::wstring& id);
std::vector<std::wstring> StatTreeDocument(const std::wstring& id);
std::vector<std::wstring> ListTreeDocuments(const std::wstring& id);
winrt::handle OpenTreeDocument(const std::wstring& id);
}
