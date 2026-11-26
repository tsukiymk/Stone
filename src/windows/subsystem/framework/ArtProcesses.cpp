#include "ArtProcesses.h"
#include "../runtime/art/ArtJni.h"
#include "../abi/TypedAndroidImport.h"
#include "hle/system/stub-libc.h"
#include "../../client/ClientApplicationLogs.h"
#include "../../ipc/ServiceProtocol.h"
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace stone::windows {
namespace {
std::mutex gate;
std::set<jint> completed;
jint nextProcess = 0x60000000;

void Throw(JNIEnv* env, const char* message)
{
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/io/IOException");
    if (type) {
        ArtJni<&JNINativeInterface_::ThrowNew>(env, type, message);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    }
}

std::string Bytes(JNIEnv* env, jbyteArray array)
{
    if (!array)
        throw std::runtime_error("Missing process argument bytes");
    const auto size = ArtJni<&JNINativeInterface_::GetArrayLength>(env, array);
    if (size < 0 || size > 32768)
        throw std::runtime_error("Process arguments exceed their limit");
    std::string bytes(size, '\0');
    ArtJni<&JNINativeInterface_::GetByteArrayRegion>(env, array, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
        throw std::runtime_error("Cannot read process arguments");
    return bytes;
}

std::vector<std::string> Arguments(const std::string& block, jint count)
{
    if (count < 0 || count > 16)
        throw std::runtime_error("Unsupported process argument count");
    std::vector<std::string> result;
    size_t at = 0;
    for (int i = 0; i < count; ++i) {
        auto end = block.find('\0', at);
        if (end == block.npos)
            throw std::runtime_error("Unterminated process argument");
        result.emplace_back(block.substr(at, end - at));
        at = end + 1;
    }
    if (at != block.size())
        throw std::runtime_error("Trailing process argument bytes");
    return result;
}

int Snapshot(const std::wstring& text)
{
    std::string bytes;
    if (!text.empty()) {
        auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
        if (!count)
            throw std::runtime_error("Cannot encode application log snapshot");
        bytes.resize(count);
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), bytes.data(),
                                count, nullptr, nullptr) != count)
            throw std::runtime_error("Cannot encode application log snapshot");
    }
    for (int attempt = 0; attempt < 32; ++attempt) {
        uint64_t random;
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&random), sizeof(random),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Cannot create application log snapshot identity");
        auto path = std::filesystem::temp_directory_path() / (L"Stone-logcat-" + std::to_wstring(random) + L".tmp");
        ipc::Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        if (!file.value) {
            if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
                continue;
            throw std::runtime_error("Cannot create temporary application log snapshot");
        }
        DWORD written = 0;
        if (!bytes.empty() &&
            (!WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
             written != bytes.size()))
            throw std::runtime_error("Cannot write application log snapshot");
        auto reader = ReOpenFile(file.value, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
        if (reader == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open readonly application log snapshot");
        auto descriptor = ImportAndroidSocketFile({reinterpret_cast<uintptr_t>(reader)}); // consumes the handle
        if (descriptor < 0)
            throw std::runtime_error("Cannot import application log snapshot descriptor");
        return descriptor;
    }
    throw std::runtime_error("Cannot allocate application log snapshot");
}

jint Execute(JNIEnv* env, jobject, jbyteArray program, jbyteArray block, jint count, jbyteArray environment,
             jint environmentCount, jbyteArray directory, jintArray descriptors, jboolean)
{
    int output = -1;
    try {
        const auto names = Arguments(Bytes(env, program), 1);
        if (names[0] != "logcat" && names[0] != "/system/bin/logcat")
            throw std::runtime_error("Executable is not supported by the Windows ART host");
        const auto arguments = Arguments(Bytes(env, block), count);
        const auto uid = "--uid=" + std::to_string(AndroidApplicationUid());
        if (arguments.size() != 3 ||
            std::set<std::string>(arguments.begin(), arguments.end()) != std::set<std::string>{"-d", uid, "*:V"})
            throw std::runtime_error("Only logcat -d --uid=<current app UID> *:V is supported");
        if (environment || environmentCount || directory || !descriptors ||
            ArtJni<&JNINativeInterface_::GetArrayLength>(env, descriptors) != 3)
            throw std::runtime_error("Logcat environment, working directory or redirection is unsupported");
        jint fds[3];
        ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, descriptors, 0, 3, fds);
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            return -1;
        if (fds[0] != -1 || fds[1] != -1 || fds[2] != -1)
            throw std::runtime_error("Logcat descriptor redirection is unsupported");
        output = Snapshot(ReadClientApplicationLogs());
        const std::lock_guard lock(gate);
        if (completed.size() >= 64 || nextProcess == INT_MAX)
            throw std::runtime_error("Too many pending logcat results");
        const jint process = ++nextProcess;
        completed.insert(process);
        const jint result[]{-1, output, -1};
        ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, descriptors, 0, 3, result);
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env)) {
            completed.erase(process);
            CloseAndroidFd(output);
            return -1;
        }
        return process;
    }
    catch (const std::exception& error) {
        if (output >= 0)
            CloseAndroidFd(output);
        if (!ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            Throw(env, error.what());
        return -1;
    }
}

jint Wait(JNIEnv*, jobject, jint process)
{
    const std::lock_guard lock(gate);
    return completed.erase(process) ? 0 : 127;
}

void Destroy(JNIEnv*, jobject, jint)
{
    // These are already completed, in-process commands, never Windows PIDs.
    // Reaping still owns the completion entry; destroy must not remove it.
}

template <class Function> JNINativeMethod Method(const char* name, const char* signature, Function function)
{
    return {const_cast<char*>(name), const_cast<char*>(signature),
            reinterpret_cast<void*>(ImportAndroidScalars<Function>(std::string("ART UNIXProcess ") + name + signature,
                                                                   reinterpret_cast<uintptr_t>(function)))};
}
}

bool RegisterArtProcesses(JNIEnv* env)
{
    const JNINativeMethod methods[]{
        Method("forkAndExec", "([B[BI[BI[B[IZ)I", &Execute),
        Method("waitForProcessExit", "(I)I", &Wait),
        Method("destroyProcess", "(I)V", &Destroy),
    };
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/UNIXProcess");
    if (!type)
        return false;
    const bool result = ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, methods, 3) == JNI_OK;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return result;
}
}
