#include "WindowsIsolatedProcess.h"
#include "WindowsBinderFiles.h"

#include <windows.h>
#include <aclapi.h>
#include <userenv.h>

#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
using Sid = std::unique_ptr<void, decltype(&FreeSid)>;

[[noreturn]] void Fail(const char* step, DWORD error = GetLastError())
{
    throw std::runtime_error(std::string(step) + " (Win32 " + std::to_string(error) + ")");
}

std::wstring ReadString(JNIEnv* env, jstring value)
{
    const auto length = env->GetStringLength(value);
    std::wstring result(length, L'\0');
    env->GetStringRegion(value, 0, length, reinterpret_cast<jchar*>(result.data()));
    return result;
}

bool IsAppContainer(HANDLE process)
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return false;
    Handle token(raw, &CloseHandle);
    DWORD isolated = 0, written = 0;
    return GetTokenInformation(token.get(), TokenIsAppContainer, &isolated, sizeof(isolated), &written) &&
           isolated != 0;
}

class Attributes {
public:
    explicit Attributes(DWORD count)
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, count, 0, &size);
        if (!size)
            Fail("Size isolated process attributes");
        data_.resize(size);
        if (!InitializeProcThreadAttributeList(get(), count, 0, &size))
            Fail("Initialize isolated process attributes");
    }

    ~Attributes()
    {
        DeleteProcThreadAttributeList(get());
    }

    void set(DWORD_PTR name, void* value, SIZE_T size)
    {
        if (!UpdateProcThreadAttribute(get(), 0, name, value, size, nullptr, nullptr)) {
            Fail("Set isolated process attribute");
        }
    }

    LPPROC_THREAD_ATTRIBUTE_LIST get()
    {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(data_.data());
    }

private:
    std::vector<std::byte> data_;
};

struct Pipe {
    Handle reader{nullptr, &CloseHandle};
    Handle writer{nullptr, &CloseHandle};

    explicit Pipe(bool childReads)
    {
        SECURITY_ATTRIBUTES inheritance{sizeof(inheritance), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inheritance, 0))
            Fail("Create isolated process pipe");
        reader.reset(read);
        writer.reset(write);
        if (!SetHandleInformation(childReads ? write : read, HANDLE_FLAG_INHERIT, 0)) {
            Fail("Restrict pipe inheritance");
        }
    }
};

std::wstring WorkerEnvironment(const std::wstring& javaHome)
{
    std::array<wchar_t, MAX_PATH> windows{};
    const auto length = GetSystemWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (!length || length >= windows.size())
        Fail("Resolve Windows directory");
    std::wstring result;
    const auto add = [&result](const wchar_t* name, const std::wstring& value) {
        result += name;
        result += L'=';
        result += value;
        result.push_back(L'\0');
    };
    const auto addRedirected = [&add](const wchar_t* name) {
        const auto size = GetEnvironmentVariableW(name, nullptr, 0);
        if (!size)
            return;
        std::wstring value(size, L'\0');
        const auto written = GetEnvironmentVariableW(name, value.data(), size);
        if (written && written < size) {
            value.resize(written);
            add(name, value);
        }
    };
    add(L"JAVA_HOME", javaHome);
    // Windows redirects these entries to the registered AppContainer's directories.
    addRedirected(L"LOCALAPPDATA");
    add(L"SystemRoot", std::wstring(windows.data(), length));
    addRedirected(L"TEMP");
    addRedirected(L"TMP");
    result.push_back(L'\0');
    return result;
}

void PermitWorkerRead(std::wstring& path, PSID sid)
{
    PACL previous = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto status = GetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                        &previous, nullptr, &descriptor);
    std::unique_ptr<void, decltype(&LocalFree)> security(descriptor, &LocalFree);
    if (status != ERROR_SUCCESS)
        Fail("Read isolated worker file permissions", status);
    // A null DACL already grants access to the file.
    if (!previous)
        return;
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    BuildTrusteeWithSidW(&access.Trustee, sid);
    PACL updated = nullptr;
    status = SetEntriesInAclW(1, &access, previous, &updated);
    std::unique_ptr<void, decltype(&LocalFree)> acl(updated, &LocalFree);
    if (status == ERROR_SUCCESS) {
        status = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                       updated, nullptr);
    }
    if (status != ERROR_SUCCESS)
        Fail("Grant isolated worker read access", status);
}
} // namespace

uint32_t stone::windows::AppContainerWorkerParent(uintptr_t input, uintptr_t output)
{
    if (!IsAppContainer(GetCurrentProcess()))
        return 0;
    ULONG inputParent = 0, outputParent = 0, errorParent = 0;
    if (!GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(input), &inputParent) ||
        !GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(output), &outputParent) ||
        !GetNamedPipeServerProcessId(GetStdHandle(STD_ERROR_HANDLE), &errorParent) || inputParent != outputParent ||
        outputParent != errorParent)
        return 0;
    return outputParent;
}

extern "C" JNIEXPORT jlongArray JNICALL Java_com_trebledot_stone_loader_WindowsIsolatedProcess_launch(
    JNIEnv* env, jclass, jstring executable, jstring javaHome, jstring identity, jint parentUid, jint workerUid,
    jobjectArray descriptors)
{
    try {
        const auto image = ReadString(env, executable);
        const auto home = ReadString(env, javaHome);
        const auto moniker = ReadString(env, identity);
        if (env->ExceptionCheck())
            return nullptr;
        PSID derived = nullptr;
        const auto result = DeriveAppContainerSidFromAppContainerName(moniker.c_str(), &derived);
        if (FAILED(result))
            Fail("Derive isolated process identity", static_cast<DWORD>(result));
        Sid sid(derived, &FreeSid);

        Pipe input(true), output(false), error(false);
        HANDLE inherited[] = {input.reader.get(), output.writer.get(), error.writer.get()};
        SECURITY_CAPABILITIES security{sid.get(), nullptr, 0, 0};

        Handle job(CreateJobObjectW(nullptr, nullptr), &CloseHandle);
        if (!job)
            Fail("Create isolated process job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            Fail("Set isolated process job limits");
        }
        Attributes attributes(3);
        attributes.set(PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &security, sizeof(security));
        attributes.set(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited));
        HANDLE jobs[] = {job.get()};
        attributes.set(PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof(jobs));

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        // Keep standard output available for JVM and guest diagnostics.
        startup.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
        startup.StartupInfo.hStdOutput = error.writer.get();
        startup.StartupInfo.hStdError = error.writer.get();
        startup.lpAttributeList = attributes.get();
        auto command = L"\"" + image + L"\" --stone-isolated-worker " + std::to_wstring(parentUid) + L' ' +
                       std::to_wstring(workerUid) + L' ' +
                       std::to_wstring(reinterpret_cast<uintptr_t>(input.reader.get())) + L' ' +
                       std::to_wstring(reinterpret_cast<uintptr_t>(output.writer.get()));
        auto environment = WorkerEnvironment(home);
        const auto directory = std::filesystem::path(image).parent_path();
        PROCESS_INFORMATION created{};
        if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, TRUE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW |
                                CREATE_UNICODE_ENVIRONMENT,
                            environment.data(), directory.c_str(), &startup.StartupInfo, &created)) {
            Fail("Start AppContainer worker");
        }
        Handle process(created.hProcess, &CloseHandle), thread(created.hThread, &CloseHandle);
        if (!IsAppContainer(process.get()))
            Fail("Verify AppContainer worker token", ERROR_ACCESS_DENIED);
        input.reader.reset();
        output.writer.reset();
        error.writer.reset();

        Handle* streams[] = {&output.reader, &input.writer, &error.reader};
        for (jsize i = 0; i < static_cast<jsize>(std::size(streams)); ++i) {
            const auto fd = stone::windows::AdoptWindowsHandle(env, reinterpret_cast<uintptr_t>(streams[i]->release()));
            if (!fd)
                return nullptr;
            env->SetObjectArrayElement(descriptors, i, fd);
            env->DeleteLocalRef(fd);
            if (env->ExceptionCheck())
                return nullptr;
        }
        const auto info = env->NewLongArray(3);
        if (!info)
            return nullptr;
        const jlong values[] = {reinterpret_cast<jlong>(process.get()), reinterpret_cast<jlong>(job.get()),
                                created.dwProcessId};
        env->SetLongArrayRegion(info, 0, 3, values);
        if (env->ExceptionCheck())
            return nullptr;
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
            Fail("Resume AppContainer worker");
        process.release();
        job.release();
        return info;
    }
    catch (const std::exception& error) {
        if (!env->ExceptionCheck()) {
            const auto type = env->FindClass("java/io/IOException");
            if (type)
                env->ThrowNew(type, error.what());
        }
        return nullptr;
    }
}

extern "C" JNIEXPORT jint JNICALL Java_com_trebledot_stone_loader_WindowsIsolatedProcess_waitForExit(JNIEnv*, jclass,
                                                                                                     jlong process,
                                                                                                     jint millis)
{
    return static_cast<jint>(WaitForSingleObject(reinterpret_cast<HANDLE>(process), static_cast<DWORD>(millis)));
}

extern "C" JNIEXPORT jint JNICALL Java_com_trebledot_stone_loader_WindowsIsolatedProcess_readExitCode(JNIEnv*, jclass,
                                                                                                      jlong process)
{
    DWORD code = 1;
    GetExitCodeProcess(reinterpret_cast<HANDLE>(process), &code);
    return static_cast<jint>(code);
}

extern "C" JNIEXPORT void JNICALL Java_com_trebledot_stone_loader_WindowsIsolatedProcess_terminate(JNIEnv*, jclass,
                                                                                                   jlong job)
{
    TerminateJobObject(reinterpret_cast<HANDLE>(job), 1);
}

extern "C" JNIEXPORT void JNICALL Java_com_trebledot_stone_loader_WindowsIsolatedProcess_release(JNIEnv*, jclass,
                                                                                                 jlong process,
                                                                                                 jlong job)
{
    CloseHandle(reinterpret_cast<HANDLE>(job));
    CloseHandle(reinterpret_cast<HANDLE>(process));
}

bool stone::windows::RegisterWindowsIsolatedProcess(JNIEnv* env)
{
    const auto type = env->FindClass("com/trebledot/stone/loader/WindowsIsolatedProcess");
    if (!type)
        return false;
    const JNINativeMethod methods[] = {
        {const_cast<char*>("prepareProfile"), const_cast<char*>("(Ljava/lang/String;[Ljava/lang/String;)V"),
         reinterpret_cast<void*>(+[](JNIEnv* env, jclass, jstring identity, jobjectArray files) {
             try {
                 const auto name = ReadString(env, identity);
                 if (env->ExceptionCheck())
                     return;
                 PSID raw = nullptr;
                 auto result = CreateAppContainerProfile(name.c_str(), L"Stone WebView", L"Stone WebView renderer",
                                                         nullptr, 0, &raw);
                 if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
                     result = DeriveAppContainerSidFromAppContainerName(name.c_str(), &raw);
                 }
                 Sid sid(raw, &FreeSid);
                 if (FAILED(result))
                     Fail("Register WebView AppContainer profile", static_cast<DWORD>(result));
                 const auto count = env->GetArrayLength(files);
                 for (jsize i = 0; i < count; ++i) {
                     const auto item = static_cast<jstring>(env->GetObjectArrayElement(files, i));
                     if (!item)
                         return;
                     auto path = ReadString(env, item);
                     env->DeleteLocalRef(item);
                     if (env->ExceptionCheck())
                         return;
                     PermitWorkerRead(path, sid.get());
                 }
             }
             catch (const std::exception& error) {
                 if (!env->ExceptionCheck()) {
                     const auto type = env->FindClass("java/io/IOException");
                     if (type)
                         env->ThrowNew(type, error.what());
                 }
             }
         })},
        {const_cast<char*>("launch"),
         const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;II[Ljava/io/FileDescriptor;)[J"),
         reinterpret_cast<void*>(&Java_com_trebledot_stone_loader_WindowsIsolatedProcess_launch)},
        {const_cast<char*>("waitForExit"), const_cast<char*>("(JI)I"),
         reinterpret_cast<void*>(&Java_com_trebledot_stone_loader_WindowsIsolatedProcess_waitForExit)},
        {const_cast<char*>("readExitCode"), const_cast<char*>("(J)I"),
         reinterpret_cast<void*>(&Java_com_trebledot_stone_loader_WindowsIsolatedProcess_readExitCode)},
        {const_cast<char*>("terminate"), const_cast<char*>("(J)V"),
         reinterpret_cast<void*>(&Java_com_trebledot_stone_loader_WindowsIsolatedProcess_terminate)},
        {const_cast<char*>("release"), const_cast<char*>("(JJ)V"),
         reinterpret_cast<void*>(&Java_com_trebledot_stone_loader_WindowsIsolatedProcess_release)},
    };
    const bool ok = env->RegisterNatives(type, methods, static_cast<jint>(std::size(methods))) == JNI_OK;
    env->DeleteLocalRef(type);
    return ok;
}
