#include "../../../src/windows/host/ProcessLog.h"
#include "../../../src/windows/host/CommandLine.h"
#include "../../../src/windows/ipc/ServiceProtocol.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string_view>

using namespace stone::windows;
using namespace stone::windows::ipc;
namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 3) {
            const std::wstring_view mode(argv[1]);
            const bool package = mode.starts_with(L"--package");
            Check(StartProcessLog(mode == L"--child" ? L""
                                  : package          ? L"launch-apk"
                                                     : L"startup"),
                  "Cannot create user process log");
            Check(StartProcessLog(), "Repeated log start failed");
            const auto initialPath = ProcessLogPath();
            const auto processDirectory = fs::absolute(argv[2]) / fs::path(argv[0]).stem();
            Check(!initialPath.empty() &&
                      initialPath.parent_path() ==
                          (mode == L"--child" ? processDirectory : processDirectory / L"Operations"),
                  "Reported log path is outside the selected log root");
            for (const auto& entry : fs::recursive_directory_iterator(argv[2]))
                Check(!entry.is_regular_file(), "Logger eagerly created an empty file");
            if (mode == L"--silent" || mode == L"--blank") {
                if (mode == L"--blank")
                    std::fputs("\n\r\n", stdout);
                StopProcessLog();
                return 0;
            }
            if (mode == L"--package-early")
                Check(SetProcessLogPackage(L"com.trebledot.stone.test"), "Cannot set early package identity");
            if (mode == L"--operation")
                Check(SetProcessLogOperation(L"package-list"), "Cannot name package listing log");
            if (mode == L"--collision") {
                std::ofstream existing(initialPath, std::ios::binary);
                existing << "existing log";
                Check(existing.good(), "Cannot prepare filename collision");
            }
            std::puts("process stdout record");
            if (mode == L"--package" || mode == L"--operation-late") {
                // Ensure startup output has reached disk before attaching identity.
                bool written = false;
                for (int attempt = 0; attempt < 200 && !written; ++attempt) {
                    for (const auto& entry : fs::recursive_directory_iterator(argv[2]))
                        if (entry.is_regular_file() && fs::file_size(entry.path()) != 0)
                            written = true;
                    if (!written)
                        Sleep(10);
                }
                Check(written, "Startup output did not reach the log");
                Check(!SetProcessLogPackage(L"../escape"), "Unsafe package name accepted");
                Check(!SetProcessLogOperation(L"../escape"), "Unsafe operation name accepted");
                if (mode == L"--package") {
                    Check(SetProcessLogPackage(L"com.trebledot.stone.test"), "Cannot attach package identity");
                    Check(SetProcessLogPackage(L"com.trebledot.stone.test"), "Cannot reattach package identity");
                    Check(ProcessLogPath() != initialPath &&
                              ProcessLogPath().filename().wstring().starts_with(L"com.trebledot.stone.test-"),
                          "Reported log path did not follow package rename");
                }
                else {
                    Check(SetProcessLogOperation(L"package-install"), "Cannot rename operation log");
                    Check(SetProcessLogOperation(L"package-install"), "Cannot reattach operation name");
                }
            }
            std::fputs("process stderr record\n", stderr);
            std::fputs("partial record", stdout);
            StopProcessLog();
            StopProcessLog();
            return 0;
        }
        Check(argc == 2, "Missing isolated log test directory");
        auto root = fs::absolute(argv[1]) / std::to_wstring(GetCurrentProcessId());
        Check(SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()), "Cannot isolate log user data");
        Check(SetEnvironmentVariableW(L"STONE_LOG_DIRECTORY", nullptr), "Cannot select default user log directory");
        std::wstring executable(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        Check(count && count < executable.size(), "Cannot locate log test executable");
        executable.resize(count);
        for (const auto mode : {L"--child", L"--package", L"--package-early", L"--operation", L"--operation-late",
                                L"--collision", L"--silent", L"--blank"}) {
            const auto isolated = root / mode;
            Check(SetEnvironmentVariableW(L"LOCALAPPDATA", isolated.c_str()), "Cannot isolate log case");
            const auto logRoot = isolated / L"Trebledot Productions/Stone/Logs";
            // Package case also verifies that the explicit override is a root, with
            // the executable directory still appended beneath it.
            const auto overrideRoot = isolated / L"override";
            Check(SetEnvironmentVariableW(L"STONE_LOG_DIRECTORY",
                                          std::wstring_view(mode) == L"--package" ? overrideRoot.c_str() : nullptr),
                  "Cannot set log override");
            const auto selectedRoot = std::wstring_view(mode) == L"--package" ? overrideRoot : logRoot;
            fs::create_directories(selectedRoot);
            auto command =
                stone::session::Quote(executable) + L" " + mode + L" " + stone::session::Quote(selectedRoot.wstring());
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &process),
                  "Cannot start log writer");
            CloseHandle(process.hThread);
            Handle child(process.hProcess);
            Check(WaitForSingleObject(child.value, 15000) == WAIT_OBJECT_0, "Log writer timed out");
            DWORD result = 1;
            Check(GetExitCodeProcess(child.value, &result) && !result, "Log writer failed");
            const auto logs = selectedRoot / fs::path(executable).stem();
            size_t files = 0;
            size_t existingFiles = 0;
            for (const auto& entry : fs::recursive_directory_iterator(logs)) {
                if (!entry.is_regular_file())
                    continue;
                std::ifstream input(entry.path(), std::ios::binary);
                const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                if (text == "existing log") {
                    ++existingFiles;
                    continue;
                }
                Check(text.find("process stdout record") != text.npos &&
                          text.find("process stderr record") != text.npos && text.find("partial record") != text.npos,
                      "User log did not retain both output streams");
                const auto filename = entry.path().filename().wstring();
                Check(text.find("[p-" + std::to_string(process.dwProcessId) + "]") != text.npos,
                      "Process log records do not identify their writer");
                const bool package = std::wstring_view(mode).starts_with(L"--package");
                const std::wstring prefix = package                                     ? L"com.trebledot.stone.test-"
                                            : std::wstring_view(mode) == L"--operation" ? L"package-list-"
                                            : std::wstring_view(mode) == L"--operation-late" ? L"package-install-"
                                            : std::wstring_view(mode) == L"--collision"      ? L"startup-"
                                                                                             : L"";
                const auto category = package ? L"Applications" : prefix.empty() ? L"" : L"Operations";
                const auto expectedDirectory = prefix.empty() ? logs : logs / category;
                Check(entry.path().parent_path() == expectedDirectory, "Incorrect process log category directory");
                Check(filename.starts_with(prefix) &&
                          std::regex_match(filename.substr(prefix.size()), std::wregex(LR"(\d{8}-\d{6}-\d{7}\.log)")),
                      "Unexpected process log filename");
                ++files;
            }
            const bool silent = std::wstring_view(mode) == L"--silent" || std::wstring_view(mode) == L"--blank";
            Check(files == (silent ? 0 : 1), "Unexpected process log count");
            Check(existingFiles == (std::wstring_view(mode) == L"--collision" ? 1 : 0),
                  "Filename collision overwrote an existing log");
        }
        std::puts(
            "PASS default user Logs directory, parent creation, stdout/stderr capture, flush and process identity");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL process log: %s\n", error.what());
        return 1;
    }
}
