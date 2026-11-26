#pragma once

#include <jni.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace stone::windows {
class ElfImage;
struct AndroidDlInfo;
struct Arm64AssetBridge;
struct Arm64WindowBridge;

struct GuestEnv {
    const void* const* table;
    JNIEnv* host;
};

extern "C" void AndroidToWindows();
extern "C" void AndroidToWindowsFloatFirst();
extern "C" void AndroidToWindowsIntFirst();
extern "C" void AndroidToWindowsWide();
extern "C" void AndroidToWindowsWideEleven();
extern "C" void AndroidVarargsPrint();

uintptr_t Import(std::string_view name, uintptr_t address, void (*bridge)() = AndroidToWindows);
bool AndroidImportMatches(uintptr_t thunk, uintptr_t address, void (*bridge)());
using AndroidHostCallback = uintptr_t (*)(uintptr_t, void*, void*, void*);
// Explicit four-slot callbacks; ordinary imports are not eligible for direct host dispatch.
uintptr_t ImportAndroidCallback(std::string_view name, AndroidHostCallback callback);
uintptr_t MakeAndroidCallbackThunk(uintptr_t address);

template <typename T> uintptr_t Import(std::string_view name, T fn, void (*bridge)() = AndroidToWindows)
{
    return Import(name, reinterpret_cast<uintptr_t>(fn), bridge);
}

JavaVM* AndroidHostVm();
bool InitializeAndroidNativeBackend();
std::string Utf(JNIEnv* env, jstring value);
bool IsArm64TranslationEnabled();
std::recursive_mutex& AndroidLoaderMutex();
int AndroidTargetSdk();
void SetAndroidTargetSdk(int sdk);
void* ResolveAndroidTls(const ElfImage* module, uint64_t offset);
bool ProtectAndroidElfData(uintptr_t address, size_t length, bool writable);

uintptr_t Libc(std::string_view name);
uintptr_t Libandroidicu(std::string_view name);
uintptr_t Libdl(std::string_view name);
uintptr_t LibdlAndroid(std::string_view name);
uintptr_t Libm(std::string_view name);
uintptr_t Libandroid(std::string_view name);
uintptr_t Libegl(std::string_view name, bool hostAbi = false);
uintptr_t Libgles(std::string_view name, bool hostAbi = false);
uintptr_t Libstdcxx(std::string_view name);
uintptr_t Libopensles(std::string_view name);
uintptr_t Arm64OpenSlSymbol(std::string_view name);
uintptr_t Arm64VulkanSymbol(std::string_view name);
uintptr_t Arm64ConfigurationImport(std::string_view symbol);
uintptr_t Arm64FenvImport(std::string_view symbol);
uintptr_t Arm64LogImport(std::string_view symbol);
int AndroidProperty(const char* name, char* value);

void* DlOpen(const char* filename, int flags) noexcept;
void* AndroidDlOpenExt(const char* filename, int flags, const uint64_t* extensionFlags) noexcept;
void* AndroidExportedNamespace(const char* name) noexcept;
void* DlSym(void* handle, const char* symbol) noexcept;
int DlClose(void* handle) noexcept;
char* DlError() noexcept;
void SetDlError(const char* message) noexcept;
int DlAddr(const void* address, AndroidDlInfo* info) noexcept;
int DlIterateProgramHeaders(uintptr_t callback, void* data, bool lockForUnwind);
void DlCfiSlowpath(uint64_t typeId, void* target, void* diagnostic) noexcept;

Arm64WindowBridge MakeArm64WindowBridge();

class AttachedEnvironment {
public:
    AttachedEnvironment()
    {
        auto* vm = AndroidHostVm();
        if (!vm)
            return;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            attached = vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) == JNI_OK;
        }
    }

    ~AttachedEnvironment()
    {
        if (attached)
            AndroidHostVm()->DetachCurrentThread();
    }

    AttachedEnvironment(const AttachedEnvironment&) = delete;
    AttachedEnvironment& operator=(const AttachedEnvironment&) = delete;

    JNIEnv* env = nullptr;

private:
    bool attached = false;
};
}
