#include "Sharing.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <shobjidl_core.h>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace stone::windows;
using namespace winrt::Windows::ApplicationModel::DataTransfer;

void Require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

DataPackage BuildSharePackage(const ShareContent& content)
{
    DataPackage data;
    PopulateSharePackage(data, content);
    return data;
}

void FileSnapshot()
{
    namespace fs = std::filesystem;
    using namespace std::string_literals;
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    wchar_t suffix[40]{};
    StringFromGUID2(id, suffix, 40);
    const auto root = fs::temp_directory_path() / (std::wstring(L"StoneShareTest-") + suffix);
    fs::create_directory(root);
    const auto source = root / L"source.apk", cache = root / L"cache";
    const std::string original = "Stone APK snapshot bytes\0tail"s;
    {
        std::ofstream output(source, std::ios::binary);
        output.write(original.data(), original.size());
    }
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    Require(input != INVALID_HANDLE_VALUE, "Cannot create source APK fixture");

    struct Handle {
        HANDLE value;

        ~Handle()
        {
            if (value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    } handle{input};

    LARGE_INTEGER offset{};
    offset.QuadPart = 7;
    winrt::check_bool(SetFilePointerEx(input, offset, nullptr, FILE_BEGIN));
    auto content = SnapshotShareFile(L"APK fixture", L"example.apk", input, cache);
    LARGE_INTEGER current{};
    LARGE_INTEGER zero{};
    winrt::check_bool(SetFilePointerEx(input, zero, &current, FILE_CURRENT));
    Require(current.QuadPart == 7, "Snapshot changed the provider descriptor offset");
    auto packet = BuildSharePackage(content);
    auto view = packet.GetView();
    content.file.reset();
    Require(!view.Contains(StandardDataFormats::Text()) && view.Contains(StandardDataFormats::StorageItems()),
            "APK payload became text");
    auto items = view.GetStorageItemsAsync().get();
    Require(items.Size() == 1, "APK share item count differs");
    auto file = items.GetAt(0).as<winrt::Windows::Storage::StorageFile>();
    const fs::path snapshot(file.Path().c_str());
    Require(file.Name() == L"example.apk" && fs::equivalent(snapshot.parent_path().parent_path(), cache),
            "Source path or unsafe filename escaped into share package");
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "caller changed";
    }
    auto stream = file.OpenReadAsync().get();
    using namespace winrt::Windows::Storage::Streams;
    Buffer buffer(static_cast<uint32_t>(original.size()));
    auto bytes = stream.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::None).get();
    std::vector<uint8_t> actual(bytes.Length());
    DataReader::FromBuffer(bytes).ReadBytes(actual);
    Require(std::string(actual.begin(), actual.end()) == original, "Shared APK differs from the immutable snapshot");
    stream.Close();
    stream = nullptr;
    HANDLE writer = CreateFileW(snapshot.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
            "Published snapshot was writable");
    file = nullptr;
    items = nullptr;
    view = nullptr;
    packet = nullptr;
    Require(fs::exists(snapshot), "Closing source package deleted a recipient's file");
    // Advance only this fixture's timestamp, then trigger normal cache pruning.
    fs::last_write_time(snapshot, fs::file_time_type::clock::now() - std::chrono::hours(25));
    bool expired = false;
    for (int attempt = 0; attempt < 100 && !expired; ++attempt) {
        {
            auto next = SnapshotShareFile(L"next", L"next.apk", input, cache);
        }
        expired = !fs::exists(snapshot);
        if (!expired)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Require(expired && fs::exists(source), "Expired share snapshot not reclaimed or source removed");
    for (const auto& invalid : {L"../bad.apk", L"bad:stream.apk", L"CON.apk", L"bad.exe"}) {
        bool rejected = false;
        try {
            (void)SnapshotShareFile(L"title", invalid, input, cache);
        }
        catch (...) {
            rejected = true;
        }
        Require(rejected, "Unsafe APK filename accepted");
    }
    Handle writeOnly{CreateFileW(source.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, 0, nullptr)};
    Require(writeOnly.value != INVALID_HANDLE_VALUE, "Cannot open write-only descriptor fixture");
    bool rejected = false;
    try {
        (void)SnapshotShareFile(L"title", L"write-only.apk", writeOnly.value, cache);
    }
    catch (const std::invalid_argument&) {
        rejected = true;
    }
    Require(rejected, "Snapshot upgraded a provider's write-only descriptor to read access");
    CloseHandle(writeOnly.value);
    writeOnly.value = INVALID_HANDLE_VALUE;
    CloseHandle(handle.value);
    handle.value = INVALID_HANDLE_VALUE;
    fs::remove(source);
    fs::remove(cache / L".lock");
    fs::remove(cache);
    fs::remove(root);
    std::puts(
        "PASS real Windows StorageItems APK snapshot bytes, provider offset isolation, read-only lease, recipient lifetime and expired-cache cleanup");
}

int WindowControl()
{
    // Explicit interactive SDK control: fixed plain text, no ART/Stone window
    // code, no clipboard, no automatic target selection. Close after 300 s.
    winrt::check_hresult(SetCurrentProcessExplicitAppUserModelID(L"Trebledot.Stone.ShareControl"));
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    struct Apartment {
        ~Apartment()
        {
            winrt::uninit_apartment();
        }
    } apartment;

    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpszClassName = L"Stone.Share.Control";
    type.lpfnWndProc = +[](HWND window, UINT message, WPARAM first, LPARAM second) -> LRESULT {
        if (message == WM_TIMER) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        if (message == WM_COMMAND && LOWORD(first) == 101) {
            std::printf("SDK control button command received; foreground=%d\n", GetForegroundWindow() == window);
            std::fflush(stdout);
            SetForegroundWindow(window);
            try {
                auto interop = winrt::get_activation_factory<DataTransferManager, IDataTransferManagerInterop>();
                const auto status = interop->ShowShareUIForWindow(window);
                std::printf("SDK control ShowShareUIForWindow=0x%08X\n", static_cast<unsigned>(status));
                std::fflush(stdout);
                winrt::check_hresult(status);
            }
            catch (const winrt::hresult_error& error) {
                std::fprintf(stderr, "SDK control activation HRESULT 0x%08X\n",
                             static_cast<unsigned>(error.code().value));
            }
            return 0;
        }
        return DefWindowProcW(window, message, first, second);
    };
    Require(RegisterClassW(&type) != 0, "Cannot register SDK share control");
    auto window = CreateWindowExW(0, type.lpszClassName, L"Stone plain Windows share control", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 560, 320, nullptr, nullptr, instance, nullptr);
    Require(window != nullptr, "Cannot create SDK share control");
    const auto button =
        CreateWindowExW(0, L"BUTTON", L"Share fixed test text", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 24,
                        24, 240, 40, window, reinterpret_cast<HMENU>(101), instance, nullptr);
    Require(button != nullptr, "Cannot create SDK share button");
    // A hidden test launcher can override the first ShowWindow call.
    ShowWindow(window, SW_SHOW);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    auto interop = winrt::get_activation_factory<DataTransferManager, IDataTransferManagerInterop>();
    DataTransferManager manager{nullptr};
    winrt::check_hresult(interop->GetForWindow(window, winrt::guid_of<DataTransferManager>(), winrt::put_abi(manager)));
    unsigned requests = 0;
    auto token = manager.DataRequested([&](const auto&, const DataRequestedEventArgs& args) {
        auto data = args.Request().Data();
        data.Properties().Title(L"Stone native SDK control");
        data.SetText(L"Stone plain text share control");
        ++requests;
        std::puts("SDK control DataRequested: plain text populated without Stone runtime");
        std::fflush(stdout);
    });
    SetTimer(window, 1, 300000, nullptr);
    std::puts("SDK share control waiting for explicit button click");
    std::fflush(stdout);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    manager.DataRequested(token);
    Require(requests > 0, "SDK control did not receive a share request");
    std::puts("SDK share control window closed");
    return 0;
}

int Run(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--window-control")
            return WindowControl();
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        struct Apartment {
            ~Apartment()
            {
                winrt::uninit_apartment();
            }
        } apartment;

        FileSnapshot();
        ShareContent content{L"Stone \u5206\u4eab", L"Unicode \u4e2d\u6587 \U0001f680\n\"quoted\" text"};
        const auto expected = content.text;
        auto packet = BuildSharePackage(content);
        auto view = packet.GetView();
        content.text = L"caller changed";
        content.title = L"caller changed";
        Require(view.GetTextAsync().get() == expected, "Share text changed or lost Unicode/newlines");
        Require(view.Properties().Title() == L"Stone \u5206\u4eab", "Share title was not copied");
        Require(!view.Contains(StandardDataFormats::WebLink()), "Mixed share text became an unrelated URL");
        Require(packet.RequestedOperation() == DataPackageOperation::Copy, "Share requested a destructive operation");
        auto url = BuildSharePackage({L"Stone sample", L"https://example.com/packages/sample/"}).GetView();
        Require(url.Contains(StandardDataFormats::Text()) && url.Contains(StandardDataFormats::WebLink()) &&
                    url.GetWebLinkAsync().get().AbsoluteUri() == url.GetTextAsync().get(),
                "Share URL formats disagree");
        auto fileText = BuildSharePackage({L"Literal", L"file:///C:/Windows/notepad.exe"}).GetView();
        Require(fileText.GetTextAsync().get() == L"file:///C:/Windows/notepad.exe" &&
                    !fileText.Contains(StandardDataFormats::StorageItems()) &&
                    !fileText.Contains(StandardDataFormats::ApplicationLink()),
                "Plain text was promoted to file/protocol access");
        for (const auto& invalid :
             {ShareContent{L"", L"body"}, ShareContent{L"title", L""}, ShareContent{std::wstring(1025, L'x'), L"body"},
              ShareContent{L"title", std::wstring(8193, L'x')}, ShareContent{L"title", std::wstring(L"a\0b", 3)}}) {
            bool rejected = false;
            try {
                (void)BuildSharePackage(invalid);
            }
            catch (const std::invalid_argument&) {
                rejected = true;
            }
            Require(rejected, "Oversized or invalid share payload accepted");
        }
        bool rejected = false;
        try {
            QueueWindowShare(nullptr, {L"title", L"body"});
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        Require(rejected, "Sharing accepted a missing application window");
        std::puts(
            "PASS real Windows share DataPackage text/URL roundtrip, Unicode/title isolation, payload limits and missing-window rejection; no clipboard or target accessed");
        return 0;
    }
    catch (const winrt::hresult_error& error) {
        std::fprintf(stderr, "FAIL sharing HRESULT 0x%08X\n", static_cast<unsigned>(error.code().value));
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL sharing: %s\n", error.what());
    }
    return 1;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command, int)
{
    char name[] = "stone-sharing-tests", control[] = "--window-control";
    char* arguments[]{name, control};
    return Run(command && std::wstring_view(command) == L"--window-control" ? 2 : 1, arguments);
}
