#include "ApplicationLogs.h"
#include "../../../src/windows/ipc/ServiceProtocol.h"
#include <windows.h>
#include <cstdio>
#include <stdexcept>
using namespace stone::windows;
namespace fs = std::filesystem;

namespace {
void Check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

template <class Work> void Reject(Work work)
{
    bool rejected = false;
    try {
        work();
    }
    catch (const std::exception&) {
        rejected = true;
    }
    Check(rejected, "Invalid log request was accepted");
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        Check(argc == 2, "Missing application log test directory");
        auto root =
            fs::absolute(argv[1]) / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        auto read = [&](const fs::path& data, const std::wstring& package) {
            return ApplicationLogRequest(data, package, {L"dump"});
        };
        Check(read(root, L"org.example.first").empty(), "New journal is not empty");
        const std::wstring first = L"11-20 12:00:00.001  123  456 I Tag: actual text 中文 😀 \\\"\n";
        ApplicationLogRequest(root, L"org.example.first", {L"append", first});
        Check(read(root, L"org.example.first") == first, "Unicode log did not survive a fresh disk read");
        Check(read(root, L"org.example.second").empty() && read(root / L"other", L"org.example.first").empty(),
              "Journal escaped application/data identity");
        Check(read(root.parent_path() / (root.filename().wstring() + L"-sibling"), L"org.example.first").empty(),
              "Shared Logs directory mixed sibling application data roots");
        for (int i = 0; i < 70; ++i)
            ApplicationLogRequest(root, L"org.example.first", {L"append", std::wstring(7999, L'a') + L"\n"});
        auto bounded = read(root, L"org.example.first");
        Check(bounded.size() == 512000 && bounded.find(first) == bounded.npos,
              "Journal did not rotate complete lines at its limit");
        Check(ipc::Pack({root.wstring(), L"org.example.first", L"append", std::wstring(7999, L'"') + L"\n"}).size() <=
                  32767,
              "Worst-case batch exceeds protocol limit");
        Reject([&] {
            ApplicationLogRequest(root, L"../other", {L"dump"});
        });
        Reject([&] {
            ApplicationLogRequest(root, L"org.example.first", {L"append", L"unfinished"});
        });
        Reject([&] {
            ApplicationLogRequest(root, L"org.example.first", {L"append", std::wstring(16001, L'\n')});
        });
        Reject([&] {
            ApplicationLogRequest(root, L"org.example.first", {L"clear"});
        });
        ApplicationLogRequest(root, L"org.example.second", {L"append", first});
        RemoveApplicationLogs(root, L"org.example.first");
        Check(read(root, L"org.example.first").empty() && read(root, L"org.example.second") == first,
              "Uninstall log cleanup crossed package boundary");
        std::puts(
            "PASS application log persistence, Unicode, bounded rotation, identity, malformed requests and removal");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL application logs: %s\n", error.what());
        return 1;
    }
}
