#include "Documents.h"
#include <shobjidl.h>
#include <wrl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

using namespace stone::windows;
namespace fs = std::filesystem;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void Write(HANDLE file, std::string_view text)
{
    DWORD written = 0;
    winrt::check_bool(WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
    Require(written == text.size(), "Short document write");
}

std::string Read(HANDLE file)
{
    char bytes[64]{};
    DWORD count = 0;
    winrt::check_bool(ReadFile(file, bytes, sizeof(bytes), &count, nullptr));
    return {bytes, count};
}

void GrantChecks(const fs::path& root)
{
    auto path = root / L"descriptor-fixture.txt";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "old bytes";
    }
    HANDLE raw =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    winrt::check_bool(raw != INVALID_HANDLE_VALUE);
    auto grant = KeepWindowDocument({winrt::handle(raw), path.filename().wstring()});
    auto write = OpenWindowDocument(grant, false, true, true, false);
    Write(write.get(), "selected document");
    write.close();
    Require(WindowDocumentSize(grant) == 17, "Truncate/write length differs");
    auto first = OpenWindowDocument(grant, true, false, false, false),
         second = OpenWindowDocument(grant, true, false, false, false);
    Require(Read(first.get()) == "selected document" && Read(second.get()) == "selected document",
            "Document opens share a file offset");
    auto append = OpenWindowDocument(grant, false, true, false, true);
    Write(append.get(), " + append");
    append.close();
    auto after = OpenWindowDocument(grant, true, false, false, false);
    Require(Read(after.get()) == "selected document + append", "Document append overwrote content");
    auto truncated = OpenWindowDocument(grant, true, true, true, false);
    Require(WindowDocumentSize(grant) == 0, "Read/write truncate did not reset length");
    Write(truncated.get(), "retained handle");
    truncated.close();
    auto retained = OpenWindowDocument(grant, true, false, false, false);
    ReleaseWindowDocumentGrant(grant);
    Require(Read(retained.get()) == "retained handle", "Grant release invalidated an open stream");
    try {
        auto invalid = OpenWindowDocument(grant, true, false, false, false);
        throw std::runtime_error("Revoked grant accepted");
    }
    catch (const std::invalid_argument&) {
    }
    std::cout
        << "PASS document grants: independent positions, truncation, append, metadata, revocation and retained open streams.\n";
}

std::optional<SelectedDocument> Dialog(HWND owner, const fs::path& target, bool cancel)
{
    Require(!fs::exists(target), "Use a new dialog output path; preserve existing files");
    auto id = CreateWindowDocument(owner, target.filename().wstring(), L"text/plain");
    std::atomic_bool clicked = false;
    auto pending = std::async(std::launch::async, [&] {
        try {
            auto result = PickWindowDocument(id, [&](IFileSaveDialog* dialog, HWND window) {
                Microsoft::WRL::ComPtr<IShellItem> directory;
                winrt::check_hresult(
                    SHCreateItemFromParsingName(target.parent_path().c_str(), nullptr, IID_PPV_ARGS(&directory)));
                winrt::check_hresult(dialog->SetFolder(directory.Get()));
                winrt::check_hresult(dialog->SetFileName(target.filename().c_str()));
                clicked =
                    PostMessageW(window, WM_COMMAND, MAKEWPARAM(cancel ? IDCANCEL : IDOK, BN_CLICKED), 0) != FALSE;
            });
            ReleaseWindowDocument(id);
            return result;
        }
        catch (...) {
            ReleaseWindowDocument(id);
            throw;
        }
    });
    auto end = GetTickCount64() + 30000;
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount64() > end) {
            CancelWindowDocument(id);
            throw std::runtime_error("Document dialog did not complete in 30 seconds");
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    auto result = pending.get();
    Require(clicked, "Actual native save/cancel control was not invoked");
    return result;
}

void DialogChecks(const fs::path& root)
{
    HWND owner = CreateWindowExW(0, L"STATIC", L"Stone document integration test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    winrt::check_bool(owner != nullptr);
    try {
        ShowWindow(owner, SW_SHOWNOACTIVATE);
        GUID id{};
        winrt::check_hresult(CoCreateGuid(&id));
        wchar_t suffix[40]{};
        StringFromGUID2(id, suffix, 40);
        auto target = root / (std::wstring(L"selected-") + suffix + L".txt");
        auto selected = Dialog(owner, target, false);
        Require(selected.has_value(), "Native save was cancelled");
        Require(selected->name == target.filename(), "Actual selected filename differs");
        Write(selected->file.get(), "real Windows save dialog");
        selected.reset();
        {
            std::ifstream input(target, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(input)), {});
            Require(text == "real Windows save dialog", "Actual chosen file bytes differ");
        }
        auto cancelled = root / (std::wstring(L"cancelled-") + suffix + L".txt");
        Require(!Dialog(owner, cancelled, true) && !fs::exists(cancelled), "Native cancel created a file");
        auto before = CreateWindowDocument(owner, L"cancel-before-show.txt", L"text/plain");
        CancelWindowDocument(before);
        Require(!PickWindowDocument(before), "Cancelled request displayed a dialog");
        ReleaseWindowDocument(before);
        DestroyWindow(owner);
        std::cout
            << "PASS actual Windows save and cancel dialogs, selected file bytes and cancellation before display.\n";
    }
    catch (...) {
        CloseWindowDocuments(owner);
        DestroyWindow(owner);
        throw;
    }
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc < 2 || argc > 3)
            throw std::invalid_argument("Usage: stone-document-tests <output-directory> [--dialog]");
        auto root = fs::absolute(argv[1]);
        fs::create_directories(root);
        GrantChecks(root);
        if (argc == 3) {
            Require(std::wstring_view(argv[2]) == L"--dialog", "Unknown document test mode");
            DialogChecks(root);
        }
        return 0;
    }
    catch (const winrt::hresult_error& error) {
        std::cerr << "FAIL document HRESULT: " << winrt::to_string(error.message()) << '\n';
        return 1;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL documents: " << error.what() << '\n';
        return 1;
    }
}
