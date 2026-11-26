#include "Documents.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl.h>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace stone::windows {
namespace {
using Microsoft::WRL::ComPtr;

struct Request {
    HWND owner;
    std::wstring name, mime;
    std::atomic_bool cancelled = false;
    bool started = false;
    bool tree = false;
};

std::mutex gate;
std::map<uint64_t, std::shared_ptr<Request>> requests;
std::map<uint64_t, SelectedDocument> grants;
uint64_t nextId = 0;
std::optional<std::filesystem::path> testTarget;

std::shared_ptr<Request> Find(uint64_t id)
{
    const std::lock_guard lock(gate);
    auto found = requests.find(id);
    if (found == requests.end())
        throw std::invalid_argument("Unknown document request");
    return found->second;
}

struct DialogTimer {
    IFileDialog* dialog;
    const std::shared_ptr<Request>& request;
    const DocumentDialogObserver& observer;
    bool observed = false;
    std::exception_ptr error;
};

thread_local DialogTimer* currentTimer = nullptr;

void CALLBACK PollDialog(HWND, UINT, UINT_PTR, DWORD) noexcept
{
    auto* state = currentTimer;
    if (!state)
        return;
    try {
        if (state->request->cancelled || !IsWindow(state->request->owner)) {
            state->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            return;
        }
        if (state->observer && !state->observed) {
            ComPtr<IOleWindow> window;
            HWND handle = nullptr;
            if (SUCCEEDED(state->dialog->QueryInterface(IID_PPV_ARGS(&window))) &&
                SUCCEEDED(window->GetWindow(&handle)) && IsWindowVisible(handle)) {
                ComPtr<IFileSaveDialog> save;
                winrt::check_hresult(state->dialog->QueryInterface(IID_PPV_ARGS(&save)));
                state->observed = true;
                state->observer(save.Get(), handle);
            }
        }
    }
    catch (...) {
        state->error = std::current_exception();
        state->dialog->Close(E_ABORT);
    }
}

struct Apartment {
    Apartment()
    {
        winrt::check_hresult(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    }

    ~Apartment()
    {
        CoUninitialize();
    }
};
}

uint64_t CreateWindowDocument(HWND owner, std::wstring name, std::wstring mime)
{
    DWORD process = 0;
    if (!IsWindow(owner) || !GetWindowThreadProcessId(owner, &process) || process != GetCurrentProcessId())
        throw std::invalid_argument("A document requires this Client's live window");
    if (name.empty() || name.size() > 255 || name.find_first_of(L"<>:\"/\\|?*") != name.npos ||
        name.find_first_of(std::wstring(1, L'\0')) != name.npos || mime.empty() || mime.size() > 255)
        throw std::invalid_argument("Invalid document name or MIME type");
    for (wchar_t value : name)
        if (value < 0x20)
            throw std::invalid_argument("Invalid document name");
    auto request = std::make_shared<Request>();
    request->owner = owner;
    request->name = std::move(name);
    request->mime = std::move(mime);
    const std::lock_guard lock(gate);
    if (!requests.empty())
        throw std::runtime_error("A document selection is already pending");
    if (!++nextId)
        throw std::overflow_error("Document request identifier exhausted");
    requests.emplace(nextId, std::move(request));
    return nextId;
}

std::optional<SelectedDocument> PickWindowDocument(uint64_t id, const DocumentDialogObserver& observer)
{
    auto request = Find(id);
    {
        const std::lock_guard lock(gate);
        if (std::exchange(request->started, true))
            throw std::invalid_argument("Document selection already started");
    }
    if (request->cancelled || !IsWindow(request->owner))
        return {};
    Apartment apartment;
    ComPtr<IFileDialog> dialog;
    winrt::check_hresult(CoCreateInstance(request->tree ? CLSID_FileOpenDialog : CLSID_FileSaveDialog, nullptr,
                                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)));
    FILEOPENDIALOGOPTIONS options{};
    winrt::check_hresult(dialog->GetOptions(&options));
    winrt::check_hresult(
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
                           (request->tree ? FOS_PICKFOLDERS : FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN)));
    ComPtr<IShellItem> documents;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&documents))))
        winrt::check_hresult(dialog->SetDefaultFolder(documents.Get()));
    COMDLG_FILTERSPEC filters[]{{L"Text document", L"*.txt"}, {L"All files", L"*.*"}};
    if (request->mime == L"text/plain") {
        winrt::check_hresult(dialog->SetFileTypes(2, filters));
        winrt::check_hresult(dialog->SetDefaultExtension(L"txt"));
    }
    if (!request->tree)
        winrt::check_hresult(dialog->SetFileName(request->name.c_str()));
    DialogTimer state{dialog.Get(), request, observer};
    if (currentTimer)
        throw std::runtime_error("Nested document dialogs are unsupported");
    auto timer = SetTimer(nullptr, 0, 50, PollDialog);
    if (!timer)
        winrt::throw_last_error();
    currentTimer = &state;
    HRESULT result = dialog->Show(request->owner);
    KillTimer(nullptr, timer);
    currentTimer = nullptr;
    if (state.error)
        std::rethrow_exception(state.error);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || request->cancelled || !IsWindow(request->owner))
        return {};
    winrt::check_hresult(result);
    ComPtr<IShellItem> selected;
    winrt::check_hresult(dialog->GetResult(&selected));
    PWSTR path = nullptr;
    winrt::check_hresult(selected->GetDisplayName(SIGDN_FILESYSPATH, &path));
    std::filesystem::path target;
    try {
        target = path;
    }
    catch (...) {
        CoTaskMemFree(path);
        throw;
    }
    CoTaskMemFree(path);
    {
        const std::lock_guard lock(gate);
        if (testTarget && _wcsicmp(std::filesystem::absolute(target).lexically_normal().c_str(), testTarget->c_str()))
            throw std::runtime_error("Document test rejected an unexpected selected path: " + target.string());
    }
    HANDLE file = CreateFileW(
        target.c_str(), request->tree ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, request->tree ? OPEN_EXISTING : OPEN_ALWAYS,
        request->tree ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        winrt::throw_last_error();
    SelectedDocument selection{winrt::handle(file), target.filename().wstring()};
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != request->tree)
        throw std::runtime_error("Selected document has an unexpected file type");
    return selection;
}

uint64_t CreateWindowDocumentTree(HWND owner)
{
    DWORD process = 0;
    if (!IsWindow(owner) || !GetWindowThreadProcessId(owner, &process) || process != GetCurrentProcessId())
        throw std::invalid_argument("A directory selection requires this Client's live window");
    auto request = std::make_shared<Request>();
    request->owner = owner;
    request->tree = true;
    const std::lock_guard lock(gate);
    if (!requests.empty())
        throw std::runtime_error("A document selection is already pending");
    if (!++nextId)
        throw std::overflow_error("Document request identifier exhausted");
    requests.emplace(nextId, std::move(request));
    return nextId;
}

void CancelWindowDocument(uint64_t id)
{
    Find(id)->cancelled = true;
}

void ReleaseWindowDocument(uint64_t id)
{
    const std::lock_guard lock(gate);
    auto found = requests.find(id);
    if (found == requests.end())
        return;
    found->second->cancelled = true;
    requests.erase(found);
}

void CloseWindowDocuments(HWND owner)
{
    const std::lock_guard lock(gate);
    for (const auto& [id, request] : requests)
        if (request->owner == owner)
            request->cancelled = true;
}

uint64_t KeepWindowDocument(SelectedDocument selection)
{
    const std::lock_guard lock(gate);
    if (grants.size() >= 64)
        throw std::runtime_error("Too many open document grants");
    if (!++nextId)
        throw std::overflow_error("Document grant identifier exhausted");
    grants.emplace(nextId, std::move(selection));
    return nextId;
}

winrt::handle OpenWindowDocument(uint64_t id, bool read, bool write, bool truncate, bool append)
{
    if ((!read && !write) || (!write && (truncate || append)) || (truncate && append))
        throw std::invalid_argument("Invalid document access mode");
    const std::lock_guard lock(gate);
    auto found = grants.find(id);
    if (found == grants.end())
        throw std::invalid_argument("Unknown document grant");
    DWORD access = read ? GENERIC_READ : 0;
    if (write)
        access |= append ? FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE : GENERIC_WRITE;
    HANDLE raw =
        ReOpenFile(found->second.file.get(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (raw == INVALID_HANDLE_VALUE)
        winrt::throw_last_error();
    winrt::handle file(raw);
    if (truncate && !SetEndOfFile(raw))
        winrt::throw_last_error();
    return file;
}

uint64_t WindowDocumentSize(uint64_t id)
{
    const std::lock_guard lock(gate);
    auto found = grants.find(id);
    if (found == grants.end())
        throw std::invalid_argument("Unknown document grant");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(found->second.file.get(), &size))
        winrt::throw_last_error();
    return static_cast<uint64_t>(size.QuadPart);
}

void ReleaseWindowDocumentGrant(uint64_t id)
{
    const std::lock_guard lock(gate);
    grants.erase(id);
}

void ConstrainWindowDocumentTestTarget(std::filesystem::path target)
{
    const std::lock_guard lock(gate);
    testTarget = std::filesystem::absolute(target).lexically_normal();
}
}
