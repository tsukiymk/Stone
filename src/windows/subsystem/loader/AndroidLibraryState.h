#pragma once

#include "platform/windows/ExecutableMemory.h"
#include "loader/AndroidLoadedImage.h"
// Private coordination state. One owner and one recursive loader lock across all adapters.
#include "hle/SystemLibraryRegistry.h"
#include "ElfImage.h"
#include "abi/AndroidLibraryImports.h"
#include "hle/system/stub-libmedia_jni.h"
#include "AndroidSystemElf.h"
#include "filesystem/AndroidSystemFiles.h"
#include "hle/external/LibraryJniStubs.h"
#include "hle/external/stub-libclang_rt.ubsan_standalone.h"
#include "hle/system/stub-libandroid-fonts.h"
#include "hle/system/stub-libandroid-hardware-buffer.h"
#include "hle/system/stub-libc.h"
#include "hle/system/stub-libnativeloader.h"
#include "hle/system/stub-libmediandk-format.h"
#include "hle/system/stub-libsqlite.h"
#include "hle/system/stub-libvulkan.h"
#include "hle/system/stub-libz.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "abi/Arm64HostJni.h"
#include "cpu/arm64/Arm64ToX64.h"
#include "cpu/arm64/Arm64ExecutionProfile.h"
#include "runtime/art/ArtGs.h"
#include "runtime/art/ArtJit.h"
#include "framework/ArtFramework.h"
#include "signals/AndroidSignalContext.h"

#include <jni.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sqlite3.h>

#include <algorithm>
#include <optional>
#include <array>
#include <atomic>
#include <bitset>
#include <bit>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace stone::windows::android_libraries {
struct LibraryNamespace;

struct Call {
    uintptr_t target;
    uint32_t count = 0;
    uint32_t floats = 0;
    std::array<uint8_t, 14> sources{};
    bool adaptEnv = false;
    bool useTls = false;
    std::array<uint8_t, 25> stackSources{};
    uint32_t stackCount = 0;
    LibraryNamespace* scope = nullptr;

    explicit Call(uintptr_t address, std::string_view signature, bool jni, bool raw = false)
        : target(address), adaptEnv(jni), useTls(IsProcessorFeaturePresent(PF_RDWRFSGSBASE_AVAILABLE) != FALSE)
    {
        sources.fill(31);
        unsigned integers = 0, vectors = 0;
        const auto add = [&](bool fp) {
            if (count == 31 || stackCount == stackSources.size())
                throw std::runtime_error("too many JNI arguments for the Windows bridge");
            if (fp)
                floats |= 1u << count;
            if (fp && vectors < 8)
                sources[6 + vectors++] = static_cast<uint8_t>(count);
            else if (!fp && integers < 6)
                sources[integers++] = static_cast<uint8_t>(count);
            else
                stackSources[stackCount++] = static_cast<uint8_t>(count);
            ++count;
        };
        if (signature.empty() || signature.front() != '(')
            throw std::runtime_error("invalid JNI method signature");
        if (!raw) {
            add(false);
            add(false);
        }
        for (size_t i = 1; i < signature.size() && signature[i] != ')'; ++i) {
            bool array = false;
            while (i < signature.size() && signature[i] == '[') {
                array = true;
                ++i;
            }
            if (i == signature.size())
                throw std::runtime_error("invalid JNI array signature");
            const char type = signature[i];
            if (raw && type == 'L') {
                add(false);
                continue;
            }
            if (type == 'L') {
                i = signature.find(';', i);
                if (i == signature.npos)
                    throw std::runtime_error("invalid JNI reference signature");
            }
            else if (std::string_view("ZBCSIJFD").find(type) == signature.npos) {
                throw std::runtime_error("invalid JNI parameter signature");
            }
            add(!array && (type == 'F' || type == 'D'));
        }
    }
};

static_assert(offsetof(Call, count) == 8 && offsetof(Call, floats) == 12 && offsetof(Call, sources) == 16 &&
              offsetof(Call, adaptEnv) == 30 && offsetof(Call, useTls) == 31 && offsetof(Call, stackSources) == 32 &&
              offsetof(Call, stackCount) == 60 && offsetof(Call, scope) == 64);

extern "C" void WindowsToAndroid();
extern "C" void AndroidToWindowsCodecCallbacks();
extern "C" uintptr_t CallAndroidFive(const void* descriptor, void*, void*, uintptr_t, uintptr_t, const void*);
extern "C" void AndroidJniVarargs();
extern "C" uintptr_t CallAndroid(const Call*, void*, void*, void* = nullptr, uintptr_t = 0);

struct Library {
    Library(std::span<const uint8_t> bytes, ElfImage::Machine machine, bool dynamicTls, bool artCompiler = false)
        : image(bytes, machine, dynamicTls, artCompiler)
    {
    }

    AndroidLoadedImage image;
    std::string name;
    std::unique_ptr<ArtGsCode> artGs;
    std::optional<std::map<std::string, ElfDebugSymbol, std::less<>>> debugSymbols;
    std::vector<Library*> dependencies;
    std::vector<void*> initializers;
    LibraryNamespace* scope = nullptr;
    std::bitset<SystemLibraryCount> systemDependencies;
    size_t openHandles = 0;
    sqlite3_vfs sqliteVfs{};
    bool initialized = false;
    bool ubsanSyscalls = false;
    std::exception_ptr initializerFailure;
    std::exception_ptr jniFailure;
    enum class JniState {
        Pending,
        Running,
        Loaded,
        Failed
    } jni = JniState::Pending;
};

struct Arm64NativeCall {
    const ElfImage& image;
    uintptr_t address;
    std::string name, signature;
    std::vector<char> parameters;
    char result = 'V';
    unsigned filePathArguments = 0;
    unsigned filePathArrayArguments = 0;
    bool raw = false;
    LibraryNamespace* scope = nullptr;
};

struct Win64JniArguments {
    std::array<uint64_t, 4> integers, floats;
    const uint64_t* stack;
};

static_assert(offsetof(Win64JniArguments, floats) == 32 && offsetof(Win64JniArguments, stack) == 64);
extern "C" void Arm64NativeEntry();

using Libraries = std::unordered_map<std::string, std::unique_ptr<Library>>;

struct LibraryNamespace {
    // Process namespaces retain published entries after dlclose. ARM64 data
    // ownership caching relies on that lifetime; unloading needs invalidation
    // and reader ownership before releasing a published Library.
    Libraries images;
    jobject loader = nullptr;
    std::vector<Library*> globalImages;
    std::unique_ptr<AndroidLoadedImage> imageUnwind;
    bool imageMath = false;
    bool imageLog = false;
    bool artRuntime = false;
    AndroidElfReader read;
    std::string applicationDataPrefix;
    std::string applicationLibraryPrefix;
};

struct NativeSystemImage {
    std::string name;
    std::unique_ptr<AndroidLoadedImage> image;
    const NativeSystemImage* next;
    std::atomic<bool> ready{false};
};

// Chromium and the Android media stack use more than 128 ELF objects on Android 37.
constexpr size_t MaxAndroidLibraries = 256;

struct State {
    std::recursive_mutex mutex;
    ElfImage::Machine machine = ElfImage::Machine::X64;
    bool translate = false;
    std::atomic<int> applicationTargetSdk{0};
    LibraryNamespace application, webView, runtime;
    size_t staticTlsUsed = 0;
    std::unordered_map<std::string, void*> nativeThunks;
    jmethodID readLibrary = nullptr;
    std::array<std::atomic<const Library*>, MaxAndroidLibraries> executableImages{};
    std::atomic<uint64_t> codeGeneration{0};
    std::atomic<const ElfImage*> executableWebViewUnwinder{};
    std::atomic<const NativeSystemImage*> nativeSystemImages{};
    size_t executableCount = 0;
    std::array<size_t, SystemLibraryCount> systemHandles{};
    bool resolvingStagedElf = false;

    struct ImportedFunction {
        void* thunk;
        uintptr_t target;
        void (*bridge)();
        bool callback = false;
    };

    std::unordered_map<std::string, ImportedFunction> imports;
    std::unordered_map<uintptr_t, std::string> arm64ImportNames;
    std::vector<std::unique_ptr<Call>> calls;
    std::vector<std::unique_ptr<Arm64NativeCall>> arm64Calls;

    struct Registration {
        jclass type;
        std::string name;
        std::string signature;
    };

    std::vector<Registration> registrations;
    std::mutex signaturesMutex;
    std::unordered_map<jmethodID, std::string> signatures;
    std::unordered_set<jmethodID> webViewDirectoryMethods;
    jmethodID loadClass = nullptr;
    jmethodID resolveLibraryPath = nullptr;
};

extern thread_local LibraryNamespace* activeNamespace;

class ScopedNamespace {
public:
    explicit ScopedNamespace(LibraryNamespace& scope) : previous_(std::exchange(activeNamespace, &scope))
    {
    }

    ~ScopedNamespace()
    {
        activeNamespace = previous_;
    }

private:
    LibraryNamespace* previous_;
};

State& Process();
LibraryNamespace& CurrentNamespace();
LibraryNamespace* NamespaceForCode(uintptr_t address);
const ElfImage* NativeSystemImageForCode(uintptr_t address, size_t size);
extern thread_local LibraryNamespace* activeNamespace;
void* MakeThunk(uintptr_t data, void (*bridge)());
uintptr_t CallElfResolver(uintptr_t resolver, LibraryNamespace& scope);
void ThrowLinkError(JNIEnv* env, const char* message);
jclass FindGuestClassFromHost(JNIEnv* env, const char* name);
jint RegisterNativeTable(JNIEnv* env, jclass type, std::span<const JNINativeMethod> methods);
void* NativeThunk(uintptr_t address, std::string_view signature);
std::string Mangle(std::string_view modifiedUtf8);
void BindTables();
bool InstallAndroidExceptionHandler();
std::string_view Arm64ImportAt(uintptr_t target);
void* TranslatedThunk(const ElfImage&, uintptr_t, std::string_view, unsigned hidden = 2);
void* Arm64NativeThunk(const ElfImage&, uintptr_t, std::string_view, std::string_view);
const ElfImage* FindArm64Code(uintptr_t address);
uint8_t* FindArm64Data(uintptr_t address, unsigned bytes, bool write);
Arm64Jni Arm64JniBridge();
uint64_t RunArm64Lifecycle(const ElfImage&, uintptr_t address, bool onLoad);

struct GuestVm {
    const void* const* table;
    JavaVM* host;
};

extern GuestVm guestVm;
extern thread_local GuestEnv guestEnv;
extern JavaVM* hostVm;

std::vector<uint8_t> ReadLibrary(JNIEnv*, jobject, const std::string& name);
std::unique_ptr<AndroidLoadedImage> PrepareImageUnwind(std::span<const uint8_t>, size_t& staticTlsUsed,
                                                       bool inspection = false);
Library& Prepare(LibraryNamespace&, const std::string&, const AndroidElfReader&, Libraries&, size_t&, unsigned depth,
                 AndroidElfLinkReport* report = nullptr, bool collectBindings = false, bool artCompiler = false,
                 bool oatCode = false);
void Initialize(Library& library);
void PublishImages(State&, LibraryNamespace&, Libraries&, size_t staticTlsUsed, const ElfImage* newUnwinder = nullptr);
}
