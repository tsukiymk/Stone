#include "cpu/arm64/Arm64ExecutionSession.h"
#include "ArtLaunch.h"
#include "LaunchScreen.h"
#include "../host/ProcessLog.h"
#include "../host/UserPaths.h"
#include "../host/RuntimeFeatures.h"
#include "Window.h"
#include "BrokerClient.h"
#include "ClientApplicationLogs.h"
#include "../ipc/LaunchProfile.h"
#include "../host/WindowsPath.h"
#include "../subsystem/runtime/art/ArtRuntime.h"
#include "../subsystem/framework/ArtFramework.h"
#include "../subsystem/framework/ArtNotifications.h"
#include "../subsystem/framework/ArtApplicationLauncher.h"
#include "../subsystem/platform/windows/ArtSettingsConfirmation.h"
#include "../subsystem/runtime/art/ArtJni.h"
#include "../subsystem/runtime/art/ArtJit.h"
#include "../subsystem/cpu/arm64/Arm64ExecutionProfile.h"

#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <array>
#include <climits>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;

struct Handle {
    HANDLE value;

    explicit Handle(HANDLE handle = nullptr) : value(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
    {
    }

    ~Handle()
    {
        if (value)
            CloseHandle(value);
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::string Utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

fs::path Utf8Path(std::string_view text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

class Digest {
public:
    Digest()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Could not initialize the ART cache digest");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            throw std::runtime_error("Could not initialize the ART cache hash");
        }
    }

    ~Digest()
    {
        BCryptDestroyHash(hash_);
        BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    void Add(std::span<const char> bytes)
    {
        if (bytes.size() > ULONG_MAX || BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                                       static_cast<ULONG>(bytes.size()), 0) < 0)
            throw std::runtime_error("Could not hash ART launch input");
    }

    std::string Finish()
    {
        std::array<unsigned char, 32> bytes{};
        if (BCryptFinishHash(hash_, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0)
            throw std::runtime_error("Could not finish the ART cache digest");
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        for (const auto byte : bytes) {
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        return result;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::string FileDigest(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not open ART launch code: " + Utf8(path));
    Digest hash;
    std::array<char, 65536> buffer;
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
        hash.Add({buffer.data(), static_cast<size_t>(input.gcount())});
    if (!input.eof())
        throw std::runtime_error("Could not read ART launch code: " + Utf8(path));
    return hash.Finish();
}

std::string Metadata(const fs::path& path)
{
    return Utf8(path) + '\n' + std::to_string(fs::file_size(path)) + '\n' +
           std::to_string(fs::last_write_time(path).time_since_epoch().count()) + '\n';
}

std::wstring Quote(std::wstring_view value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (character == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (character == L'\"')
            result += L'\\';
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

bool Within(const fs::path& path, const fs::path& root)
{
    const auto value = path.generic_wstring(), base = root.generic_wstring();
    return value.size() >= base.size() && (value.size() == base.size() || value[base.size()] == L'/') &&
           CompareStringOrdinal(value.data(), static_cast<int>(base.size()), base.data(), static_cast<int>(base.size()),
                                TRUE) == CSTR_EQUAL;
}

std::string ReadSmallFile(const fs::path& path)
{
    if (fs::file_size(path) > 32768)
        throw std::runtime_error("Invalid ART preparation result");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not read ART preparation result");
    std::string result((std::istreambuf_iterator<char>(input)), {});
    if (input.bad())
        throw std::runtime_error("Could not finish reading ART preparation result");
    return result;
}

void PrepareWithNativeTool(const fs::path& tools, const fs::path& image, const fs::path& cache, const fs::path& result)
{
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle log(CreateFileW((cache / L"prepare.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!log.value || !input.value)
        throw std::runtime_error("Could not create the image preparation log");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
        throw std::runtime_error("Could not initialize image tool process attributes");

    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST value;

        ~Attributes()
        {
            DeleteProcThreadAttributeList(value);
        }
    } cleanup{attributes};

    HANDLE inherited[]{log.value, input.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                   nullptr, nullptr))
        throw std::runtime_error("Could not isolate image tool handles");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log.value;
    startup.lpAttributeList = attributes;
    Handle preparationJob(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!preparationJob.value ||
        !SetInformationJobObject(preparationJob.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("Could not configure image preparation lifetime");
    std::wstring command = Quote(tools.wstring()) + L" --output-file " + Quote(result.wstring()) + L' ' +
                           Quote(image.wstring()) + L' ' + Quote(cache.wstring()) +
                           L" libandroid_runtime.so libhwui.so";
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tools.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &startup.StartupInfo, &process))
        throw std::runtime_error("Could not start the standalone image preparation tool");
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(preparationJob.value, child.value)) {
        TerminateProcess(child.value, ERROR_CANCELLED);
        throw std::runtime_error("Could not bind image preparation lifetime");
    }
    if (ResumeThread(thread.value) == DWORD(-1))
        throw std::runtime_error("Could not resume image preparation");
    DWORD code = 1;
    if (WaitForSingleObject(child.value, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.value, &code))
        throw std::runtime_error("Could not wait for system image preparation");
    if (code)
        throw std::runtime_error("System image preparation failed; see " + Utf8(cache / L"prepare.log"));
}

fs::path PrepareImage(const fs::path& image, const fs::path& tools, const fs::path& data)
{
    Digest digest;
    const auto metadata = "stone-art-image-v3-native\n" + Metadata(image) + FileDigest(tools);
    digest.Add(metadata);
    const auto cache = ExtendedWindowsPath(data / L"cache/art" / digest.Finish());
    fs::create_directories(cache);
    const auto canonicalCache = fs::canonical(cache);
    Handle lock(CreateFileW((cache / L".index.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    OVERLAPPED range{};
    if (!lock.value || !LockFileEx(lock.value, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &range))
        throw std::runtime_error("Could not lock the ART image cache");
    const auto index = cache / L"root-path.txt";
    const auto resolve = [&]() -> fs::path {
        if (!fs::is_regular_file(index))
            return {};
        const auto root = fs::weakly_canonical(Utf8Path(ReadSmallFile(index)));
        if (!Within(root, canonicalCache) || root == canonicalCache)
            throw std::runtime_error("Invalid ART cache location");
        for (const auto* file :
             {L"lib64/libart.so", L"lib64/libandroid_runtime.so", L"lib64/libhwui.so",
              L"system/etc/stone-bootclasspath.list", L"system/build.prop", L"system/usr/keychars/Virtual.kcm"})
            if (!fs::is_regular_file(ExtendedWindowsPath(root / file)))
                return {};
        return root;
    };
    if (const auto root = resolve(); !root.empty())
        return root;
    std::cerr << "Stone: preparing the selected image for ART.\n";
    PrepareWithNativeTool(tools, image, cache, index);
    const auto root = resolve();
    if (root.empty())
        throw std::runtime_error("The prepared ART image is incomplete");
    return root;
}

void JavaCheck(JNIEnv* env, bool success, const char* message)
{
    const bool exception = ArtJni<&JNINativeInterface_::ExceptionCheck>(env);
    if (exception)
        ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
    if (!success || exception)
        throw std::runtime_error(message);
}

void InitializeArt(ArtRuntime& runtime, bool graphics)
{
    auto* env = runtime.GuestEnvironment();
    JavaCheck(env, RegisterArtFramework(env), "Could not initialize the ART framework bridge");
    const auto looper = ArtJni<&JNINativeInterface_::FindClass>(env, "android/os/Looper");
    const auto prepare =
        looper ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, looper, "prepareMainLooper", "()V") : nullptr;
    JavaCheck(env, prepare != nullptr, "Could not find the ART main Looper");
    ArtJni<&JNINativeInterface_::CallStaticVoidMethodA>(env, looper, prepare, nullptr);
    JavaCheck(env, true, "Could not prepare the ART main Looper");
    JavaCheck(env, RegisterArtResources(env), "Could not register image resource natives");
    if (graphics)
        JavaCheck(env, RegisterArtGraphics(env), "Could not register image graphics natives");
    JavaCheck(env, RegisterArtHost(env), "Could not register the ART Windows host");
}

fs::path DataDirectory(fs::path data)
{
    if (data.empty()) {
        data = DefaultAndroidDataDirectory();
    }
    fs::create_directories(data);
    return fs::canonical(data);
}

fs::path SystemImage(fs::path image, const fs::path& data, const fs::path& runtimeDirectory)
{
    if (image.empty() && fs::is_regular_file(data / L"system-image.path"))
        image = Utf8Path(ReadSmallFile(data / L"system-image.path"));
    if (image.empty())
        image = runtimeDirectory / L"system.img";
    if (!fs::is_regular_file(image))
        throw std::runtime_error("Select an Android system image with --system-image");
    return fs::canonical(image);
}

void RememberImage(const fs::path& image, const fs::path& data)
{
    const auto name = data / (L"system-image." + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    try {
        {
            std::ofstream output(name, std::ios::binary | std::ios::trunc);
            output << Utf8(image);
            output.close();
            if (!output)
                throw std::runtime_error("Could not write the selected system image path");
        }
        if (!MoveFileExW(name.c_str(), (data / L"system-image.path").c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not remember the selected system image");
    }
    catch (...) {
        std::error_code ignored;
        fs::remove(name, ignored);
        throw;
    }
}

int RunApplication(ArtRuntime& runtime, const std::string& app, bool installedPackage, bool background)
{
    InitializeArt(runtime, true);
    auto* env = runtime.GuestEnvironment();
    const auto boot = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/runtime/ArtBoot");
    const auto entry =
        boot ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(
                   env, boot, installedPackage ? "runInstalledSession" : "runSession", "(Ljava/lang/String;Z)I")
             : nullptr;
    const auto path = ArtJni<&JNINativeInterface_::NewStringUTF>(env, app.c_str());
    JavaCheck(env, entry && path, "Could not load the ART application entry point");
    const jvalue values[]{jvalue{.l = path}, jvalue{.z = static_cast<jboolean>(background)}};
    std::cerr << "Stone: starting the original APK on image ART, without a Windows JVM.\n";
    const int result = ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, boot, entry, values);
    JavaCheck(env, true, "ART application startup or execution failed");
    if (GetModuleHandleW(L"jvm.dll"))
        throw std::runtime_error("A Windows JVM was loaded during ART execution");
    return result;
}
}

int LaunchArtApplication(const std::vector<std::wstring>& arguments, const fs::path& runtimeDirectory, bool enableJit,
                         bool background, bool enableAot)
{

    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
        throw std::runtime_error("Could not configure the ART DLL search path");
    fs::path apk, data, image, prepared, origin;
    std::wstring package, expectedPackage, imageStamp, wakeToken, processName;
    for (size_t i = 0; i + 1 < arguments.size(); i += 2) {
        if (arguments[i] == L"--apk")
            apk = ExtendedWindowsPath(fs::canonical(ExtendedWindowsPath(arguments[i + 1])));
        else if (arguments[i] == L"--package")
            package = arguments[i + 1];
        else if (arguments[i] == L"--data-dir")
            data = arguments[i + 1];
        else if (arguments[i] == L"--system-image")
            image = fs::canonical(arguments[i + 1]);
        else if (arguments[i] == L"--prepared-system")
            prepared = fs::canonical(arguments[i + 1]);
        else if (arguments[i] == L"--origin-apk")
            origin = arguments[i + 1];
        else if (arguments[i] == L"--expected-package")
            expectedPackage = arguments[i + 1];
        else if (arguments[i] == L"--image-stamp")
            imageStamp = arguments[i + 1];
        else if (arguments[i] == L"--wake-token")
            wakeToken = arguments[i + 1];
        else if (arguments[i] == L"--application-process")
            processName = arguments[i + 1];
    }
    data = DataDirectory(data);
    // Resolve preferences in the Client so direct, Service and scheduled launches
    // all use the current data-root policy, without persisting explicit CLI flags.
    const auto features = RuntimeFeatures::Load(data);
    enableJit = enableJit || features.jit;
    enableAot = enableAot || features.aot;
    if (package.empty() && !fs::is_regular_file(apk))
        throw std::runtime_error("The APK must be a regular file");
    Digest identity;
    identity.Add(Utf8(data));
    identity.Add(package.empty() ? FileDigest(apk) : Utf8(fs::path(package)));
    if (!processName.empty())
        identity.Add(Utf8(fs::path(processName)));
    const auto lockName = L"Local\\Stone.APK." + fs::path(identity.Finish()).wstring();
    Handle application(CreateMutexW(nullptr, FALSE, lockName.c_str()));
    if (!application.value)
        throw std::runtime_error("Could not lock the APK launch");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("APK already running");
    image = SystemImage(image, data, runtimeDirectory);
    const auto root = ExtendedWindowsPath(
        prepared.empty() ? PrepareImage(image, fs::canonical(runtimeDirectory / L"StoneImageTool.exe"), data)
                         : prepared);
    for (const auto* file :
         {L"lib64/libart.so", L"lib64/libandroid_runtime.so", L"lib64/libhwui.so",
          L"system/etc/stone-bootclasspath.list", L"system/build.prop", L"system/usr/keychars/Virtual.kcm"})
        if (!fs::is_regular_file(ExtendedWindowsPath(root / file)))
            throw std::runtime_error("The scheduled ART system cache is incomplete");
    RememberImage(image, data);
    // Installed code must keep its registered /data path so ART can bind its UID
    // and data identity. Explicit APKs outside the store still use immutable staging.
    const auto managed = data / L"data/app/stone";
    // Compare directory identity, including data-directory junctions and Win32
    // extended path spellings; lexical prefixes can lose the registered UID.
    const bool installed = apk.filename() == L"base.apk" && fs::is_directory(managed) &&
                           fs::equivalent(apk.parent_path().parent_path(), managed);
    const auto app = !package.empty() ? Utf8(fs::path(package))
                     : installed      ? "/data/app/stone/" + Utf8(apk.parent_path().filename()) + "/base.apk"
                                      : StageArtLaunchCode(apk, root, ".apk");
    const auto host = StageArtLaunchCode(fs::canonical(runtimeDirectory / L"stone-art-runtime.jar"), root, ".jar");
    if (GetModuleHandleW(L"jvm.dll"))
        throw std::runtime_error("The ART application process must not contain a Windows JVM");
    // ArtBoot selects current package code before creating its application
    // loader. Keep an older explicit APK out of the system loader entirely.
    ArtRuntimeOptions options{root, host, {}, {host}};
    options.useImageBootClassPath = true;
    options.dataDirectory = data / L"data";
    options.useJit = enableJit;
    if (!processName.empty())
        options.vmOptions.push_back("-Dstone.application.process=" + Utf8(fs::path(processName)));
    if (enableAot) {
        options.aotCompiler = runtimeDirectory / L"StoneArtCompiler.exe";
        options.vmOptions.push_back("-verbose:oat");
    }
    ConfigureArtSettings(fs::absolute(runtimeDirectory / L"settings/StoneSettings.exe"));
    ConfigureArtNotifications(runtimeDirectory / L"settings/StoneSettings.exe");
    ConfigureArtApplicationLauncher(runtimeDirectory / L"StoneService.exe", data, image);
    ConfigureClientShell(runtimeDirectory / L"StoneService.exe", data, image, true);
    InitializeClientSession();
    ConfigureClientLaunchProfile(
        root, package.empty() && !installed ? ExtendedWindowsPath(root / Utf8Path(app.substr(1))) : fs::path{},
        origin.empty() ? apk : origin, expectedPackage.empty() ? package : expectedPackage, imageStamp, wakeToken,
        processName);
    ArtRuntime runtime(options);
    if (runtime.JitEnabled())
        std::cerr << "Stone: experimental " << (runtime.Architecture() == ElfImage::Machine::Arm64 ? "ARM64" : "x64")
                  << " ART Java JIT enabled for this launch\n";
    int result = 1;
    std::string launchError;
    try {
        result = RunApplication(runtime, app, !package.empty(), background);
    }
    catch (const std::exception& error) {
        launchError = error.what();
        std::cerr << "Stone: " << error.what() << '\n';
    }
    if (result != 0) {
        if (launchError.empty()) {
            launchError = "Android application startup or execution failed (exit code " + std::to_string(result) + ").";
            std::cerr << "Stone: " << launchError << '\n';
        }
        FailLaunchScreen(launchError);
    }
    FinishLaunchScreen();
    // The standalone Android process ends after its Activity lifecycle completes.
    if (runtime.JitEnabled()) {
        const auto statistics = GetArtJitStatistics();
        std::cerr << "Stone: ART JIT publications=" << statistics.methods << " GS=" << statistics.instructions
                  << " TLS-recoveries=" << statistics.tlsRecoveries << " breakpoints=" << statistics.breakpoints
                  << '\n';
    }
    // ExitProcess deliberately bypasses DestroyJavaVM and its profiling dump.
    // Retain opt-in ARM64 observations on normal and failed application exits.
    if (runtime.Architecture() == ElfImage::Machine::Arm64)
        FinishArm64ExecutionSession();
    // DestroyJavaVM would wait indefinitely for APK-owned non-daemon executors.
    StopClientApplicationLogs();
    StopProcessLog();
    ExitProcess(static_cast<UINT>(result));
}

int LaunchArtPackageManagement(const std::vector<std::wstring>& arguments, const fs::path& runtimeDirectory,
                               std::wstring diagnosticArguments)
{
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
        throw std::runtime_error("Could not configure the ART DLL search path");
    fs::path data, image;
    std::wstring operation, target, confirmationPipe, confirmationParent;
    for (size_t i = 0; i + 1 < arguments.size(); i += 2) {
        if (arguments[i] == L"--manage-package")
            operation = arguments[i + 1];
        else if (arguments[i] == L"--target")
            target = arguments[i + 1];
        else if (arguments[i] == L"--data-dir")
            data = arguments[i + 1];
        else if (arguments[i] == L"--system-image")
            image = arguments[i + 1];
        else if (arguments[i] == L"--confirmation-host-pipe")
            confirmationPipe = arguments[i + 1];
        else if (arguments[i] == L"--confirmation-host-parent")
            confirmationParent = arguments[i + 1];
    }
    if ((operation != L"list" && operation != L"install" && operation != L"uninstall") ||
        (operation == L"list" ? !target.empty() : target.empty()))
        throw std::runtime_error("Invalid package-management operation");
    SetProcessLogOperation(L"package-" + operation);
    data = DataDirectory(data);
    const auto settings = fs::absolute(runtimeDirectory / L"settings/StoneSettings.exe");
    if (!confirmationPipe.empty()) {
        size_t consumed = 0;
        const auto parent = std::stoull(confirmationParent, &consumed);
        if (operation != L"install" || consumed != confirmationParent.size() || !parent || parent > UINT32_MAX)
            throw std::runtime_error("Invalid settings confirmation parent");
        ConfigureArtSettingsParent(settings, confirmationPipe, static_cast<uint32_t>(parent));
    }
    else {
        if (!confirmationParent.empty())
            throw std::runtime_error("Missing settings confirmation pipe");
        ConfigureArtSettings(settings, std::move(diagnosticArguments));
    }
    image = SystemImage(image, data, runtimeDirectory);
    ConfigureClientShell(runtimeDirectory / L"StoneService.exe", data, image);
    const auto root =
        ExtendedWindowsPath(PrepareImage(image, fs::canonical(runtimeDirectory / L"StoneImageTool.exe"), data));
    const auto host = StageArtLaunchCode(fs::canonical(runtimeDirectory / L"stone-art-runtime.jar"), root, ".jar");
    if (operation == L"install")
        target = fs::path(StageArtLaunchCode(fs::canonical(target), root, ".apk")).wstring();
    ArtRuntimeOptions options{root, "", {}, {host}};
    options.useImageBootClassPath = true;
    options.dataDirectory = data / L"data";
    ArtRuntime runtime(options);
    int result = 1;
    try {
        // Listing and removal also rasterize application icons through image libhwui.
        InitializeArt(runtime, true);
        auto* env = runtime.GuestEnvironment();
        const auto type =
            ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/runtime/ArtPackageManagement");
        const auto method = type ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(
                                       env, type, "run", "(Ljava/lang/String;Ljava/lang/String;)I")
                                 : nullptr;
        const auto action = ArtJni<&JNINativeInterface_::NewString>(
            env, reinterpret_cast<const jchar*>(operation.data()), static_cast<jsize>(operation.size()));
        const auto value = ArtJni<&JNINativeInterface_::NewString>(env, reinterpret_cast<const jchar*>(target.data()),
                                                                   static_cast<jsize>(target.size()));
        JavaCheck(env, method && action && value, "Could not load the ART package-management entry point");
        const jvalue values[]{jvalue{.l = action}, jvalue{.l = value}};
        result = ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, method, values);
        JavaCheck(env, true, "ART package management failed");
        if (GetModuleHandleW(L"jvm.dll"))
            throw std::runtime_error("Package management loaded a Windows JVM");
        if (result == 0 || result == 3)
            try {
                RememberImage(image, data);
            }
            catch (const std::exception& error) {
                std::cerr << "Stone: " << error.what() << '\n';
            }
    }
    catch (const std::exception& error) {
        result = 1;
        std::cerr << "Stone: " << error.what() << '\n';
    }
    StopProcessLog();
    ExitProcess(static_cast<UINT>(result));
}
}
