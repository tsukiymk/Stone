#include "Sharing.h"
#include <shobjidl_core.h>
#include <bcrypt.h>
#include <winternl.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace stone::windows {
struct ShareFile {
    std::filesystem::path path;
    HANDLE lease = INVALID_HANDLE_VALUE;
    winrt::Windows::Storage::StorageFile item{nullptr};
    std::atomic_bool published = false;

    ~ShareFile()
    {
        item = nullptr;
        if (lease != INVALID_HANDLE_VALUE)
            CloseHandle(lease);
        std::error_code ignored;
        // A recipient may use its StorageFile after the source window exits.
        // Published snapshots stay in the bounded cache for at least a day.
        if (!published && !path.empty()) {
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(path.parent_path(), ignored);
        }
    }
};

namespace {
using namespace winrt::Windows::ApplicationModel::DataTransfer;

void Validate(const ShareContent& content)
{
    if (content.title.empty() || content.title.size() > 1024 || (!content.file && content.text.empty()) ||
        content.text.size() > 8192 || (content.file && (!content.text.empty() || !content.file->item)) ||
        content.title.find(L'\0') != content.title.npos || content.text.find(L'\0') != content.text.npos)
        throw std::invalid_argument("Invalid Windows share text or title");
}

struct FileHandle {
    HANDLE value;

    ~FileHandle()
    {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

uintmax_t PruneSnapshots(const std::filesystem::path& cache, size_t& count)
{
    uintmax_t bytes = 0;
    const auto oldest = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    for (const auto& directory : std::filesystem::directory_iterator(cache)) {
        const auto name = directory.path().filename().wstring();
        if (name.size() != 38 || name.front() != L'{' || name.back() != L'}' ||
            (GetFileAttributesW(directory.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) || !directory.is_directory())
            continue;
        GUID id{};
        if (FAILED(CLSIDFromString(name.c_str(), &id)))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
            if ((GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) || !entry.is_regular_file() ||
                entry.path().extension() != L".apk")
                continue;
            std::error_code error;
            if (entry.last_write_time() < oldest && std::filesystem::remove(entry.path(), error))
                continue;
            bytes += entry.file_size();
            ++count;
        }
        std::error_code ignored;
        std::filesystem::remove(directory.path(), ignored); // Empty directories only.
    }
    return bytes;
}

bool ReadableFile(HANDLE source)
{
    static const auto query =
        reinterpret_cast<decltype(&NtQueryObject)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    PUBLIC_OBJECT_BASIC_INFORMATION info{};
    return query && query(source, ObjectBasicInformation, &info, sizeof(info), nullptr) >= 0 &&
           (info.GrantedAccess & FILE_READ_DATA);
}

std::mutex gate;
HWND attached = nullptr;
std::optional<ShareContent> pending;

struct Presenter {
    bool apartment = false;
    DataTransferManager manager{nullptr};
    winrt::event_token requested{};

    void Reset() noexcept
    {
        if (manager) {
            manager.DataRequested(requested);
            manager = nullptr;
        }
    }

    void Initialize()
    {
        if (!apartment) {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            apartment = true;
        }
    }

    ~Presenter()
    {
        Reset();
        if (apartment)
            winrt::uninit_apartment();
    }
};

thread_local Presenter presenter;

std::string HashText(const winrt::hstring& value)
{
    std::array<unsigned char, 32> digest{};
    winrt::check_nt(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                               reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(value.c_str())),
                               value.size() * sizeof(wchar_t), digest.data(), static_cast<ULONG>(digest.size())));
    std::string result;
    for (auto byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}

winrt::fire_and_forget LogShareData(DataPackage data)
{
    try {
        const auto view = data.GetView();
        const auto text = co_await view.GetTextAsync();
        const auto title = view.Properties().Title();
        std::fprintf(stderr, "[share] snapshot text-length=%u text-sha256-utf16=%s title-sha256-utf16=%s web-link=%d\n",
                     text.size(), HashText(text).c_str(), HashText(title).c_str(),
                     view.Contains(StandardDataFormats::WebLink()));
    }
    catch (...) {
        std::fprintf(stderr, "[share] snapshot failed\n");
    }
}
}

ShareContent SnapshotShareFile(std::wstring title, const std::wstring& name, HANDLE source,
                               const std::filesystem::path& cache)
{
    // Only a simple APK basename reaches the filesystem. A provider's _data
    // column and source filename are never interpreted as Windows paths.
    auto stem = name.substr(0, name.find(L'.'));
    for (auto& ch : stem)
        if (ch >= L'a' && ch <= L'z')
            ch -= L'a' - L'A';
    const bool reserved = stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
                          (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
                           stem[3] >= L'1' && stem[3] <= L'9');
    if (reserved || title.empty() || title.size() > 1024 || title.find(L'\0') != title.npos || name.size() <= 4 ||
        name.size() > 128 || !name.ends_with(L".apk") || name.front() == L'.' ||
        name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != name.npos)
        throw std::invalid_argument("Invalid APK share title or filename");
    FILE_STANDARD_INFO info{};
    if (!source || source == INVALID_HANDLE_VALUE || !ReadableFile(source) || GetFileType(source) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(source, FileStandardInfo, &info, sizeof(info)) || info.Directory ||
        info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > 1024LL * 1024 * 1024)
        throw std::invalid_argument("APK sharing requires a regular file of at most 1 GiB");
    FileHandle reader{ReOpenFile(source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 FILE_FLAG_SEQUENTIAL_SCAN)};
    winrt::check_bool(reader.value != INVALID_HANDLE_VALUE);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    struct Apartment {
        ~Apartment()
        {
            winrt::uninit_apartment();
        }
    } apartment;

    std::filesystem::create_directories(cache);
    // Serialize cache accounting across Clients without blocking a window.
    FileHandle cacheLock{CreateFileW((cache / L".lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr)};
    winrt::check_bool(cacheLock.value != INVALID_HANDLE_VALUE);
    size_t cachedCount = 0;
    const auto cachedBytes = PruneSnapshots(cache, cachedCount);
    if (cachedCount >= 64 || cachedBytes + static_cast<uintmax_t>(info.EndOfFile.QuadPart) > 2ULL * 1024 * 1024 * 1024)
        throw std::runtime_error("APK share cache is full; retry after older shares expire");
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    wchar_t folder[40]{};
    StringFromGUID2(id, folder, 40);
    const auto directory = std::filesystem::canonical(cache) / folder;
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Cannot allocate APK share snapshot");
    auto file = std::make_shared<ShareFile>();
    file->path = directory / name;
    {
        FileHandle output{
            CreateFileW(file->path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        winrt::check_bool(output.value != INVALID_HANDLE_VALUE);
        std::array<unsigned char, 65536> buffer{};
        LONGLONG total = 0;
        const auto started = GetTickCount64();
        while (total < info.EndOfFile.QuadPart) {
            if (GetTickCount64() - started > 30000)
                throw std::runtime_error("APK share snapshot timed out");
            DWORD count = 0, written = 0;
            const auto wanted =
                static_cast<DWORD>((std::min)(static_cast<LONGLONG>(buffer.size()), info.EndOfFile.QuadPart - total));
            winrt::check_bool(ReadFile(reader.value, buffer.data(), wanted, &count, nullptr));
            if (!count)
                throw std::runtime_error("APK changed while preparing share snapshot");
            winrt::check_bool(WriteFile(output.value, buffer.data(), count, &written, nullptr));
            if (count != written)
                throw std::runtime_error("Incomplete APK share snapshot");
            total += count;
        }
        winrt::check_bool(FlushFileBuffers(output.value));
    }
    file->lease = CreateFileW(file->path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    winrt::check_bool(file->lease != INVALID_HANDLE_VALUE);
    file->item = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(file->path.wstring()).get();
    return {std::move(title), {}, std::move(file)};
}

void PopulateSharePackage(const DataPackage& data, const ShareContent& content)
{
    Validate(content);
    data.Properties().Title(content.title);
    data.RequestedOperation(DataPackageOperation::Copy);
    if (content.file) {
        auto items = winrt::single_threaded_vector<winrt::Windows::Storage::IStorageItem>();
        items.Append(content.file->item);
        data.SetStorageItems(items, true);
        content.file->published = true;
        // The package/view may outlive the window presenter. Its event closure
        // retains only the snapshot, never the DataPackage itself.
        data.Destroyed([file = content.file](const auto&, const auto&) {
        });
        return;
    }
    data.SetText(content.text);
    // Keep the exact text format, and additionally advertise a web link when
    // the whole text is a single URL. Mixed prose and links remain plain text.
    try {
        winrt::Windows::Foundation::Uri uri(content.text);
        if ((uri.SchemeName() == L"http" || uri.SchemeName() == L"https") && !uri.Host().empty() &&
            content.text.find_first_of(L" \t\r\n") == content.text.npos)
            data.SetWebLink(uri);
    }
    catch (const winrt::hresult_error&) {
    }
}

void AttachWindowSharing(HWND owner)
{
    std::lock_guard lock(gate);
    attached = owner;
    pending.reset();
}

void QueueWindowShare(HWND owner, ShareContent content)
{
    Validate(content);
    std::lock_guard lock(gate);
    if (!owner || owner != attached || !IsWindowVisible(owner) || pending)
        throw std::runtime_error("Share window is unavailable or busy");
    pending = std::move(content);
    if (!PostMessageW(owner, WindowsShareMessage, 0, 0)) {
        pending.reset();
        throw std::runtime_error("Cannot queue Windows share UI");
    }
}

void ShowWindowShare(HWND owner) noexcept
{
    std::optional<ShareContent> content;
    {
        std::lock_guard lock(gate);
        if (owner != attached)
            return;
        content = std::move(pending);
        pending.reset();
    }
    if (!content)
        return;
    try {
        presenter.Initialize();
        presenter.Reset();
        auto interop = winrt::get_activation_factory<DataTransferManager, IDataTransferManagerInterop>();
        winrt::check_hresult(
            interop->GetForWindow(owner, winrt::guid_of<DataTransferManager>(), winrt::put_abi(presenter.manager)));
        presenter.requested = presenter.manager.DataRequested(
            [content = std::move(*content)](const auto&, const DataRequestedEventArgs& args) {
                try {
                    auto data = args.Request().Data();
                    PopulateSharePackage(data, content);
                    data.ShareCompleted([](const auto&, const auto&) {
                        std::fprintf(stderr, "[share] Windows share completed\n");
                    });
                    data.ShareCanceled([](const auto&, const auto&) {
                        std::fprintf(stderr, "[share] Windows share canceled\n");
                    });
                    std::fprintf(stderr, "[share] Windows requested %s share data\n", content.file ? "file" : "text");
                    wchar_t diagnostics[2]{};
                    if (!content.file && GetEnvironmentVariableW(L"STONE_ART_DIAGNOSTICS", diagnostics, 2) == 1 &&
                        diagnostics[0] == L'1')
                        LogShareData(data);
                }
                catch (...) {
                    args.Request().FailWithDisplayText(L"The shared content is unavailable.");
                }
            });
        winrt::check_hresult(interop->ShowShareUIForWindow(owner));
    }
    catch (const winrt::hresult_error& error) {
        presenter.Reset();
        std::fprintf(stderr, "[share] Windows share UI failed (HRESULT 0x%08X)\n",
                     static_cast<unsigned>(error.code().value));
    }
    catch (...) {
        presenter.Reset();
        std::fprintf(stderr, "[share] Windows share UI failed\n");
    }
}

void CloseWindowSharing(HWND owner) noexcept
{
    {
        std::lock_guard lock(gate);
        if (attached != owner)
            return;
        pending.reset();
        attached = nullptr;
    }
    presenter.Reset();
}
}
