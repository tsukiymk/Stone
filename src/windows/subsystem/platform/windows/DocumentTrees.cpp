#include "DocumentTrees.h"
#include <wincrypt.h>
#include <combaseapi.h>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <cstring>
#include <cwctype>
#include <memory>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;

struct Grant {
    std::wstring id, name;
    fs::path path;
    DWORD volume, high, low;
    uint64_t persisted = 0;
};

std::mutex gate;
std::map<std::wstring, Grant> grants;
fs::path storage;

void Check(bool value)
{
    if (!value)
        throw std::runtime_error("Cannot access the selected document tree");
}

winrt::handle FileHandle(HANDLE handle)
{
    Check(handle != INVALID_HANDLE_VALUE && handle != nullptr);
    return winrt::handle(handle);
}

std::wstring FinalPath(HANDLE file)
{
    std::wstring result(32768, L'\0');
    DWORD size =
        GetFinalPathNameByHandleW(file, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    Check(size && size < result.size());
    result.resize(size);
    return result;
}

std::vector<unsigned char> Protect(const std::vector<unsigned char>& input, bool encrypt)
{
    Check(!input.empty() && input.size() <= 131072);
    auto entropyText = storage.wstring();
    DATA_BLOB source{static_cast<DWORD>(input.size()), const_cast<BYTE*>(input.data())};
    DATA_BLOB entropy{static_cast<DWORD>(entropyText.size() * sizeof(wchar_t)),
                      reinterpret_cast<BYTE*>(entropyText.data())},
        output{};
    Check((encrypt ? CryptProtectData(&source, L"Stone selected directory", &entropy, nullptr, nullptr,
                                      CRYPTPROTECT_UI_FORBIDDEN, &output)
                   : CryptUnprotectData(&source, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                                        &output)) != FALSE);
    std::unique_ptr<void, decltype(&LocalFree)> owner(output.pbData, LocalFree);
    Check(output.cbData <= 131072);
    return {output.pbData, output.pbData + output.cbData};
}

bool RootId(const std::wstring& id)
{
    if (id.size() != 36)
        return false;
    for (size_t index = 0; index < id.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (id[index] != L'-')
                return false;
        }
        else if (!iswxdigit(id[index]))
            return false;
    }
    return true;
}

std::pair<Grant*, std::wstring> Parse(const std::wstring& id)
{
    const auto separator = id.find(L'/');
    auto root = id.substr(0, separator);
    Check(RootId(root));
    const auto found = grants.find(root);
    Check(found != grants.end());
    std::wstring relative = separator == id.npos ? L"" : id.substr(separator + 1);
    Check(id.size() <= 8192 && relative.find_first_of(L"\\:\r\n") == relative.npos &&
          relative.find(L'\0') == relative.npos);
    if (separator != id.npos)
        Check(!relative.empty());
    if (!relative.empty())
        for (const auto& component : fs::path(relative))
            Check(!component.empty() && component != L"." && component != L".." && !component.has_root_path());
    return {&found->second, relative};
}

winrt::handle Open(const Grant& grant, const std::wstring& relative, bool content = false)
{
    auto root = FileHandle(CreateFileW(grant.path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    Check(bool(root));
    BY_HANDLE_FILE_INFORMATION rootInfo{};
    Check(GetFileInformationByHandle(root.get(), &rootInfo) && rootInfo.dwVolumeSerialNumber == grant.volume &&
          rootInfo.nFileIndexHigh == grant.high && rootInfo.nFileIndexLow == grant.low);
    if (relative.empty()) {
        Check(!content);
        return root;
    }
    fs::path path = grant.path;
    for (const auto& component : fs::path(relative)) {
        path /= component;
        const auto attributes = GetFileAttributesW(path.c_str());
        Check(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
    }
    auto file = FileHandle(CreateFileW(path.c_str(), content ? GENERIC_READ : FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Check(bool(file));
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(file.get(), &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
          (!content || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)));
    auto resolved = FinalPath(file.get()), prefix = FinalPath(root.get());
    if (!prefix.ends_with(L"\\"))
        prefix += L'\\';
    Check(resolved.size() > prefix.size() &&
          CompareStringOrdinal(resolved.data(), static_cast<int>(prefix.size()), prefix.data(),
                               static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL);
    return file;
}

std::vector<std::wstring> Stat(const Grant& grant, const std::wstring& relative)
{
    auto file = Open(grant, relative);
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(file.get(), &info) != FALSE);
    const uint64_t size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const uint64_t ticks = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    return {grant.id + (relative.empty() ? L"" : L"/" + relative),
            relative.empty() ? grant.name : fs::path(relative).filename().wstring(),
            info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? L"1" : L"0", std::to_wstring(size),
            std::to_wstring(ticks >= 116444736000000000ULL ? (ticks - 116444736000000000ULL) / 10000 : 0)};
}

void Save(const Grant& grant)
{
    Check(!storage.empty());
    std::wostringstream text;
    text << L"1\n"
         << grant.id << L'\n' << grant.path.wstring() << L'\n' << grant.name << L'\n' << grant.volume << L'\n'
         << grant.high << L'\n' << grant.low << L'\n' << grant.persisted << L'\n';
    const auto plain = text.str();
    std::vector<unsigned char> bytes(plain.size() * sizeof(wchar_t));
    std::memcpy(bytes.data(), plain.data(), bytes.size());
    const auto protectedBytes = Protect(bytes, true);
    const auto target = storage / (grant.id + L".grant"), temp = storage / (grant.id + L".tmp");
    auto output =
        FileHandle(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(bool(output));
    DWORD written = 0;
    Check(
        WriteFile(output.get(), protectedBytes.data(), static_cast<DWORD>(protectedBytes.size()), &written, nullptr) &&
        written == protectedBytes.size() && FlushFileBuffers(output.get()));
    output.close();
    Check(MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE);
}
}

void ConfigureDocumentTrees(const fs::path& directory)
{
    const std::lock_guard lock(gate);
    Check(storage.empty());
    fs::create_directories(directory);
    storage = fs::canonical(directory);
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(storage)) {
        if (entry.path().extension() != L".grant" || ++count > 128)
            continue;
        try {
            Check(entry.file_size() > 0 && entry.file_size() <= 131072);
            std::ifstream input(entry.path(), std::ios::binary);
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
            auto plain = Protect(bytes, false);
            Check(plain.size() % sizeof(wchar_t) == 0);
            std::wstring decoded(plain.size() / sizeof(wchar_t), L'\0');
            std::memcpy(decoded.data(), plain.data(), plain.size());
            std::wistringstream text(decoded);
            std::wstring version, path;
            Grant grant;
            std::getline(text, version);
            std::getline(text, grant.id);
            std::getline(text, path);
            std::getline(text, grant.name);
            text >> grant.volume >> grant.high >> grant.low >> grant.persisted;
            Check(bool(text) && version == L"1" && RootId(grant.id) && grant.id == entry.path().stem().wstring() &&
                  grant.persisted > 0);
            grant.path = path;
            auto root = Open(grant, L"");
            grants.emplace(grant.id, std::move(grant));
        }
        catch (const std::exception&) { /* Invalid/stale grants never restore access. */
        }
    }
}

std::vector<std::wstring> RestoreDocumentTrees()
{
    const std::lock_guard lock(gate);
    std::vector<std::wstring> result;
    for (const auto& [id, grant] : grants)
        if (grant.persisted)
            result.insert(result.end(), {id, grant.name, std::to_wstring(grant.persisted)});
    return result;
}

std::vector<std::wstring> KeepDocumentTree(SelectedDocument selection)
{
    const std::lock_guard lock(gate);
    Check(!storage.empty() && grants.size() < 128);
    GUID guid{};
    winrt::check_hresult(CoCreateGuid(&guid));
    wchar_t id[40]{};
    Check(StringFromGUID2(guid, id, 40) > 0);
    Grant grant;
    grant.id = std::wstring(id + 1, 36);
    grant.path = FinalPath(selection.file.get());
    grant.name = selection.name;
    Check(grant.path.wstring().find_first_of(L"\r\n") == std::wstring::npos &&
          grant.name.find_first_of(L"\r\n") == grant.name.npos);
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(selection.file.get(), &info) &&
          (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    grant.volume = info.dwVolumeSerialNumber;
    grant.high = info.nFileIndexHigh;
    grant.low = info.nFileIndexLow;
    auto result = std::vector<std::wstring>{grant.id, grant.name};
    grants.emplace(grant.id, std::move(grant));
    return result;
}

void PersistDocumentTree(const std::wstring& id, uint64_t time)
{
    const std::lock_guard lock(gate);
    auto [grant, relative] = Parse(id);
    Check(relative.empty());
    auto root = Open(*grant, L"");
    auto next = *grant;
    next.persisted = time;
    if (time)
        Save(next);
    else
        fs::remove(storage / (grant->id + L".grant"));
    *grant = std::move(next);
}

void ReleaseDocumentTree(const std::wstring& id)
{
    const std::lock_guard lock(gate);
    auto found = grants.find(id);
    if (found == grants.end())
        return;
    fs::remove(storage / (id + L".grant"));
    grants.erase(found);
}

std::vector<std::wstring> StatTreeDocument(const std::wstring& id)
{
    const std::lock_guard lock(gate);
    auto [grant, relative] = Parse(id);
    return Stat(*grant, relative);
}

std::vector<std::wstring> ListTreeDocuments(const std::wstring& id)
{
    const std::lock_guard lock(gate);
    auto [grant, relative] = Parse(id);
    auto parent = Open(*grant, relative);
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(parent.get(), &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    WIN32_FIND_DATAW entry{};
    auto pattern = fs::path(FinalPath(parent.get())) / L"*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
    std::vector<std::wstring> result;
    if (find == INVALID_HANDLE_VALUE) {
        Check(GetLastError() == ERROR_FILE_NOT_FOUND);
        return result;
    }

    struct Close {
        HANDLE handle;

        ~Close()
        {
            FindClose(handle);
        }
    } close{find};

    do {
        if (!std::wcscmp(entry.cFileName, L".") || !std::wcscmp(entry.cFileName, L"..") ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        Check(result.size() < 5 * 4096);
        auto child = relative.empty() ? std::wstring(entry.cFileName) : relative + L"/" + entry.cFileName;
        auto row = Stat(*grant, child);
        result.insert(result.end(), row.begin(), row.end());
    } while (FindNextFileW(find, &entry));
    Check(GetLastError() == ERROR_NO_MORE_FILES);
    return result;
}

winrt::handle OpenTreeDocument(const std::wstring& id)
{
    const std::lock_guard lock(gate);
    auto [grant, relative] = Parse(id);
    return Open(*grant, relative, true);
}
}
