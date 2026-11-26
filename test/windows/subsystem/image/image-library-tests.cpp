#include "../support/test-suite.h"
#include "runtime/art/ArtRuntime.h"

#include "hle/system/stub-libsqlite.h"
#include "hle/system/stub-libz.h"
#include <sqlite3.h>
#include <windows.h>

#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace stone::windows::test {
namespace {
std::wstring Quote(std::wstring_view argument)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto value : argument) {
        if (value == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (value == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (value == L'"')
            result += L'\\';
        result += value;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

void RunTool(const std::vector<std::wstring>& arguments)
{
    wchar_t executable[32768]{};
    const auto size = GetEnvironmentVariableW(L"STONE_IMAGE_TOOLS", executable, 32768);
    STONE_CHECK(size && size < 32768);
    std::wstring command = Quote(executable);
    for (const auto& argument : arguments)
        command += L' ' + Quote(argument);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    STONE_CHECK(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                               &startup, &process));
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, 300000);
    if (wait != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
    DWORD code = 1;
    const auto exited = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    STONE_CHECK(wait == WAIT_OBJECT_0 && exited && code == 0 && !GetModuleHandleW(L"jvm.dll"));
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    STONE_CHECK(input);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

std::vector<uint8_t> Extract(const std::filesystem::path& image, std::wstring_view directory, std::wstring_view name)
{
    const auto target = std::filesystem::current_path() / L"image-extraction" / name;
    RunTool({L"--extract-file", image.wstring(), std::wstring(directory), std::wstring(name), target.wstring()});
    return ReadFile(target);
}

void CheckSqlite(std::span<const uint8_t> bytes)
{
    LoadImageSqlite(bytes);
    const auto registerVfs = reinterpret_cast<int (*)(sqlite3_vfs*, int)>(SqliteExport("sqlite3_vfs_register", "(LI)"));
    const auto open = reinterpret_cast<int (*)(const char*, sqlite3**)>(SqliteExport("sqlite3_open", "(LL)"));
    const auto close = reinterpret_cast<int (*)(sqlite3*)>(SqliteExport("sqlite3_close", "(L)"));
    const auto prepare = reinterpret_cast<int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**)>(
        SqliteExport("sqlite3_prepare_v2", "(LLILL)"));
    const auto step = reinterpret_cast<int (*)(sqlite3_stmt*)>(SqliteExport("sqlite3_step", "(L)"));
    const auto value = reinterpret_cast<int (*)(sqlite3_stmt*, int)>(SqliteExport("sqlite3_column_int", "(LI)"));
    const auto finalize = reinterpret_cast<int (*)(sqlite3_stmt*)>(SqliteExport("sqlite3_finalize", "(L)"));
    STONE_CHECK(registerVfs(WindowsSqliteVfs(), 1) == SQLITE_OK);
    sqlite3* database = nullptr;
    STONE_CHECK(open(":memory:", &database) == SQLITE_OK && database);
    sqlite3_stmt* statement = nullptr;
    const int prepared = prepare(database, "select 40+2", -1, &statement, nullptr);
    const int result = prepared == SQLITE_OK ? step(statement) : prepared;
    const int actual = result == SQLITE_ROW ? value(statement, 0) : -1;
    if (statement)
        finalize(statement);
    STONE_CHECK(close(database) == SQLITE_OK);
    STONE_CHECK(result == SQLITE_ROW && actual == 42);
}
}

void TestImage(Suite& suite, int api, const std::filesystem::path& image)
{
    suite.Run("system-image-api-metadata", [&] {
        const auto props = Extract(image, L"", L"build.prop");
        std::istringstream lines(std::string(props.begin(), props.end()));
        for (std::string line; std::getline(lines, line);) {
            if (line.ends_with('\r'))
                line.pop_back();
            if (line.starts_with("ro.build.version.sdk=") || line.starts_with("ro.system.build.version.sdk=")) {
                STONE_CHECK(line.substr(line.find('=') + 1) == std::to_string(api));
                return;
            }
        }
        throw std::runtime_error("system image SDK property is missing");
    });
    suite.Run("libsqlite-image-query", [&] {
        CheckSqlite(Extract(image, L"lib64", L"libsqlite.so"));
    });
    suite.Run("libz-image-crc32", [&] {
        const auto bytes = Extract(image, L"lib64", L"libz.so");
        LoadNativeSystemElf("libz.so", bytes);
        constexpr char text[] = "123456789";
        std::array<uint64_t, 8> args{0, reinterpret_cast<uintptr_t>(text), sizeof(text) - 1};
        const auto memory = [&](uintptr_t address, unsigned size, bool write) -> uint8_t* {
            STONE_CHECK(address == args[1] && size == args[2] && !write);
            return reinterpret_cast<uint8_t*>(address);
        };
        STONE_CHECK(CallArm64Zlib("crc32", args, memory, {}) == 0xcbf43926);
    });
}

void InspectPreparedArm64Art(const std::filesystem::path& directory, bool framework)
{
    const auto reader = ArtRuntimeLibraries(directory);
    STONE_CHECK(!GetModuleHandleW(L"jvm.dll"));
    size_t missing = 0;
    std::vector<const char*> roots{"libart.so", "libartpalette-system.so", "libjavacore.so", "libopenjdkjvm.so",
                                   "libopenjdk.so"};
    if (framework)
        roots.insert(roots.end(), {"libandroid_runtime.so", "libhwui.so", "libjavacrypto.so", "libjnigraphics.so",
                                   "libandroid.so", "libsqlite.so"});
    for (const auto* root : roots) {
        const auto entry = std::string_view(root) == "libart.so" ? "JNI_CreateJavaVM" : "";
        const auto report = InspectAndroidElf(root, entry, reader, false, ElfImage::Machine::Arm64);
        std::cout << root << ": inspected " << report.libraries << " ARM64 ELF libraries, "
                  << report.indirectRelocations << " deferred indirect relocations.\n";
        for (const auto& symbol : report.missingImports)
            std::cout << "Unresolved " << symbol << '\n';
        missing += report.missingImports.size();
    }
    STONE_CHECK(!GetModuleHandleW(L"jvm.dll"));
    STONE_CHECK(missing == 0);
}

void InspectArtImage(const std::filesystem::path& image, const std::filesystem::path& cache)
{
    std::filesystem::create_directories(cache);
    const auto index = std::filesystem::absolute(cache / L"root-path.txt");
    RunTool({L"--output-file", index.wstring(), image.wstring(), cache.wstring()});
    const auto bytes = ReadFile(index);
    const auto staged = std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
    std::wcout << L"ART native probe files staged at " << staged.c_str() << L'\n';
    const auto reader = ArtRuntimeLibraries(staged);
    for (const auto* root :
         {"libart.so", "libartpalette-system.so", "libjavacore.so", "libopenjdkjvm.so", "libopenjdk.so"}) {
        const auto entry = std::string_view(root) == "libart.so" ? "JNI_CreateJavaVM" : "";
        const auto report = InspectAndroidElf(root, entry, reader);
        std::cout << root << ": inspected " << report.libraries << " ELF libraries, " << report.indirectRelocations
                  << " deferred indirect relocations.\n";
        for (const auto& symbol : report.missingImports)
            std::cout << "Unresolved " << symbol << '\n';
        if (!report.missingImports.empty()) {
            throw std::runtime_error(std::string(root) + " has " + std::to_string(report.missingImports.size()) +
                                     " unresolved ELF imports");
        }
    }
}
}
