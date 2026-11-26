#include "platform/windows/DocumentTrees.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>

using namespace stone::windows;
namespace fs = std::filesystem;

void Require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

void Denied(const std::function<void()>& action)
{
    bool denied = false;
    try {
        action();
    }
    catch (const std::exception&) {
        denied = true;
    }
    Require(denied, "Unexpected directory access");
}

std::string Read(HANDLE file)
{
    char bytes[64]{};
    DWORD size = 0;
    Require(ReadFile(file, bytes, sizeof(bytes), &size, nullptr) != FALSE, "Read document");
    return {bytes, size};
}

void RestoreChild(const fs::path& storage, const std::wstring& id, int expected)
{
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --restore \"" + storage.wstring() + L"\" " + id +
                           L" " + std::to_wstring(expected);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           storage.parent_path().c_str(), &startup, &process) != FALSE,
            "Restore child launch");
    winrt::handle thread(process.hThread), handle(process.hProcess);
    Require(WaitForSingleObject(handle.get(), 10000) == WAIT_OBJECT_0, "Restore child timeout");
    DWORD exit = 1;
    Require(GetExitCodeProcess(handle.get(), &exit) && exit == 0, "Restore child failure");
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 5 && std::wstring_view(argv[1]) == L"--restore") {
            ConfigureDocumentTrees(argv[2]);
            auto restored = RestoreDocumentTrees();
            Require(restored.size() == static_cast<size_t>(_wtoi(argv[4])) * 3, "Restored grant count");
            if (!restored.empty()) {
                Require(restored[0] == argv[3], "Restored grant identity");
                auto file = OpenTreeDocument(restored[0] + L"/棋谱.gba");
                Require(Read(file.get()) == "fixture ROM bytes", "Restored file content");
            }
            return 0;
        }
        Require(argc == 2, "Pass a Debug test directory");
        fs::path root =
            fs::absolute(argv[1]) / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        auto folder = root / L"roms", storage = root / L"grants";
        fs::create_directories(folder);
        {
            std::ofstream file(folder / L"棋谱.gba", std::ios::binary);
            file << "fixture ROM bytes";
        }
        fs::create_directory(folder / L"nested");
        ConfigureDocumentTrees(storage);
        HANDLE raw = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        Require(raw != INVALID_HANDLE_VALUE, "Open test root");
        auto selected = KeepDocumentTree({winrt::handle(raw), L"ROM 目录"});
        auto id = selected[0];
        Require(RestoreDocumentTrees().empty(), "Unpersisted grant restored");
        auto children = ListTreeDocuments(id);
        Require(children.size() == 10, "Directory listing");
        auto metadata = StatTreeDocument(id + L"/棋谱.gba");
        Require(metadata[1] == L"棋谱.gba" && metadata[3] == L"17", "Unicode document metadata");
        auto first = OpenTreeDocument(id + L"/棋谱.gba"), second = OpenTreeDocument(id + L"/棋谱.gba");
        Require(Read(first.get()) == "fixture ROM bytes" && Read(second.get()) == "fixture ROM bytes",
                "Independent read positions");
        Denied([&] {
            OpenTreeDocument(id + L"/../outside");
        });
        Denied([&] {
            OpenTreeDocument(id + L"/棋谱.gba:stream");
        });
        Denied([&] {
            OpenTreeDocument(L"00000000-0000-0000-0000-000000000000/x");
        });
        Denied([&] {
            OpenTreeDocument(id);
        });
        PersistDocumentTree(id, 123456);
        RestoreChild(storage, id, 1);
        auto record = storage / (id + L".grant");
        std::vector<char> original;
        {
            std::ifstream file(record, std::ios::binary);
            original.assign(std::istreambuf_iterator<char>(file), {});
        }
        auto corrupt = original;
        corrupt.back() ^= 1;
        {
            std::ofstream file(record, std::ios::binary | std::ios::trunc);
            file.write(corrupt.data(), corrupt.size());
        }
        RestoreChild(storage, id, 0);
        {
            std::ofstream file(record, std::ios::binary | std::ios::trunc);
            file.write(original.data(), original.size());
        }
        auto copied = root / L"other-profile";
        fs::create_directory(copied);
        fs::copy_file(record, copied / record.filename());
        RestoreChild(copied, id, 0);
        auto moved = root / L"moved-roms";
        first.close();
        second.close();
        Require(folder.parent_path() == root && moved.parent_path() == root, "Fixture move boundary");
        Require(MoveFileW(folder.c_str(), moved.c_str()) != FALSE, "Move fixture root");
        fs::create_directory(folder);
        Denied([&] {
            ListTreeDocuments(id);
        });
        RestoreChild(storage, id, 0);
        ReleaseDocumentTree(id);
        Require(!fs::exists(record), "Grant revocation persistence");
        Denied([&] {
            StatTreeDocument(id);
        });
        std::cout
            << "PASS document trees: Unicode metadata, independent reads, traversal/stream/unknown-root denial, cold restore, DPAPI tamper/profile isolation, root identity and revocation.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
