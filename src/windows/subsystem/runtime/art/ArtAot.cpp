#include "ArtAot.h"
#include "ArtRuntime.h"
#include "../../filesystem/AndroidSystemFiles.h"
#include "../../../host/CommandLine.h"
#include "../../../host/WindowsPath.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;

struct Handle {
    HANDLE value;

    explicit Handle(HANDLE handle) : value(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
    {
    }

    ~Handle()
    {
        if (value)
            CloseHandle(value);
    }
};

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

fs::path compilerPath, systemPath, dataPath;
std::string bootPath;
std::string instructionSet;
std::mutex aotMutex;

fs::path GuestFile(std::string_view path)
{
    Check(!path.empty() && path.front() == '/', "AOT requires an absolute Android code path");
    return ResolveAndroidSystemFile(fs::path(std::u8string(path.begin() + 1, path.end())).generic_wstring());
}

std::wstring Wide(std::string_view text)
{
    return fs::path(std::u8string(text.begin(), text.end())).wstring();
}

std::string HashFiles(const std::vector<fs::path>& files, std::string_view prefix = {})
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
          "Could not create AOT digest");
    const auto closeAlgorithm = [](void* value) {
        BCryptCloseAlgorithmProvider(value, 0);
    };
    const std::unique_ptr<void, decltype(closeAlgorithm)> algorithmOwner(algorithm, closeAlgorithm);
    Check(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, "Could not create AOT hash");
    const auto closeHash = [](void* value) {
        BCryptDestroyHash(value);
    };
    const std::unique_ptr<void, decltype(closeHash)> hashOwner(hash, closeHash);
    const auto add = [&](const char* bytes, size_t size) {
        Check(size <= ULONG_MAX && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes)),
                                                  static_cast<ULONG>(size), 0) >= 0,
              "Could not hash AOT input");
    };
    add(prefix.data(), prefix.size());
    std::array<char, 65536> bytes;
    for (const auto& file : files) {
        // Include boundaries so different boot JAR concatenations cannot alias.
        const auto length = fs::file_size(file);
        add(reinterpret_cast<const char*>(&length), sizeof(length));
        std::ifstream input(file, std::ios::binary);
        Check(bool(input), "Could not open AOT input");
        while (input.read(bytes.data(), bytes.size()) || input.gcount())
            add(bytes.data(), static_cast<size_t>(input.gcount()));
        Check(input.eof(), "Could not finish reading AOT input");
    }
    std::array<uint8_t, 32> digest{};
    Check(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
          "Could not finish AOT digest");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

void Compile(const std::vector<std::wstring>& arguments, const fs::path& log)
{
    std::wstring command = stone::session::Quote(compilerPath.wstring());
    for (const auto& argument : arguments)
        command += L' ' + stone::session::Quote(argument);
    Check(command.size() < 32767, "AOT compiler command is too long");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output(CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle input(
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    Check(output.value && input.value, "Could not create AOT compiler log");
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<uint8_t> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    Check(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes), "Could not configure AOT child handles");
    const auto release = [](LPPROC_THREAD_ATTRIBUTE_LIST value) {
        DeleteProcThreadAttributeList(value);
    };
    const std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(release)> owner(attributes, release);
    HANDLE inherited[]{output.value, input.value};
    Check(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                    nullptr, nullptr),
          "Could not isolate AOT child handles");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.value;
    startup.lpAttributeList = attributes;
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Check(job.value && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
          "Could not bind AOT child lifetime");
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(compilerPath.c_str(), command.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, dataPath.c_str(),
                         &startup.StartupInfo, &process),
          "Could not start image AOT compiler");
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.value, child.value)) {
        TerminateProcess(child.value, ERROR_CANCELLED);
        throw std::runtime_error("Could not isolate AOT child lifetime");
    }
    Check(ResumeThread(thread.value) != DWORD(-1), "Could not resume AOT compiler");
    DWORD code = 1;
    Check(WaitForSingleObject(child.value, 10 * 60 * 1000) == WAIT_OBJECT_0 && GetExitCodeProcess(child.value, &code) &&
              code == 0,
          "Image AOT compilation failed or timed out; see the application's dalvik-cache compiler log");
}
}

void ConfigureArtAot(const fs::path& compiler, const fs::path& system, const fs::path& data, std::string bootClassPath)
{
    Check(compilerPath.empty(), "AOT compilation was already configured");
    compilerPath = ExtendedWindowsPath(fs::canonical(compiler));
    systemPath = system;
    dataPath = data;
    bootPath = std::move(bootClassPath);
    instructionSet = ArtImageArchitecture(systemPath) == ElfImage::Machine::Arm64 ? "arm64" : "x86_64";
    fs::create_directories((dataPath / "dalvik-cache" / instructionSet).make_preferred());
}

void PrepareArtAot(std::string_view apk)
{
    const std::lock_guard guard(aotMutex);
    if (compilerPath.empty())
        return;
    Check(apk.ends_with(".apk") && apk.find_first_of("\\:\r\n") == apk.npos, "Invalid APK path for AOT");
    const auto source = GuestFile(apk);
    const auto attributes = GetFileAttributesW(source.c_str());
    Check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY),
          "AOT requires immutable APK code");
    std::vector<fs::path> inputs{compilerPath, (systemPath / "bin/dex2oat64").make_preferred(),
                                 (systemPath / "lib64/libart.so").make_preferred(), source};
    for (size_t first = 0; first < bootPath.size();) {
        const auto end = bootPath.find(':', first);
        inputs.push_back(GuestFile(std::string_view(bootPath).substr(first, end == bootPath.npos ? end : end - first)));
        if (end == bootPath.npos)
            break;
        first = end + 1;
    }
    const auto cacheIdentity =
        "stone-aot-v2\n" + instructionSet + "\nspeed\ncopy-dex=false\nPCL[]\n" + bootPath + '\n' + std::string(apk);
    const auto identity = HashFiles(inputs, cacheIdentity);
    std::string name(apk.substr(1));
    for (auto& ch : name)
        if (ch == '/')
            ch = '@';
    name += "@classes.dex";
    const auto directory = ExtendedWindowsPath(dataPath / "dalvik-cache" / instructionSet);
    fs::create_directories(directory);
    const auto oat = directory / Wide(name);
    auto vdex = oat;
    vdex.replace_extension(".vdex");
    const auto marker = fs::path(oat.wstring() + L".stone-aot");
    Handle lock(CreateFileW((oat.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(lock.value != nullptr, "Another process is preparing this AOT cache");
    if (fs::is_regular_file(oat) && fs::is_regular_file(vdex) && fs::is_regular_file(marker)) {
        std::ifstream saved(marker);
        std::string previous, artifacts;
        std::getline(saved, previous);
        std::getline(saved, artifacts);
        if (previous == identity && artifacts == HashFiles({oat, vdex})) {
            std::cerr << "[art-aot] reused " << apk << '\n';
            return;
        }
    }
    const auto stage = directory / (L".stone-aot-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                    std::to_wstring(GetTickCount64()));
    Check(fs::create_directory(stage), "Could not create AOT staging directory");

    struct Staging {
        fs::path path;

        ~Staging()
        {
            std::error_code ignored;
            for (const auto* name : {L"code.odex", L"code.vdex", L"identity"})
                fs::remove(path / name, ignored);
            fs::remove(path, ignored);
        }
    } staging{stage};

    const auto stageOat = stage / L"code.odex", stageVdex = stage / L"code.vdex";
    const auto log = fs::path(oat.wstring() + L".compile.log");
    std::cerr << "[art-aot] compiling " << apk << '\n';
    Compile({L"--system-directory", systemPath.wstring(), L"--data-dir", dataPath.wstring(), L"--",
             Wide("--dex-file=" + std::string(apk)),
             Wide("--oat-file=/data/dalvik-cache/" + instructionSet + '/') + stage.filename().wstring() + L"/code.odex",
             Wide("--instruction-set=" + instructionSet), L"--compiler-filter=speed", L"--copy-dex-files=false",
             L"--generate-debug-info", L"--class-loader-context=PCL[]", L"--boot-image=/system/nonexistent.art",
             L"--runtime-arg", L"-Xnoimage-dex2oat", L"--runtime-arg", Wide("-Xbootclasspath:" + bootPath), L"-j2"},
            log);
    Check(HashFiles(inputs, cacheIdentity) == identity, "AOT inputs changed during compilation");
    const auto artifacts = HashFiles({stageOat, stageVdex});
    fs::remove(marker);
    Check(MoveFileExW(stageVdex.c_str(), vdex.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) &&
              MoveFileExW(stageOat.c_str(), oat.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
          "Could not publish AOT artifacts");
    const auto pending = stage / L"identity";
    {
        std::ofstream output(pending);
        output << identity << '\n' << artifacts << '\n';
        output.close();
        Check(bool(output), "Could not write AOT cache identity");
    }
    Check(MoveFileExW(pending.c_str(), marker.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
          "Could not publish AOT cache identity");
    std::cerr << "[art-aot] compiled " << apk << '\n';
}
}
