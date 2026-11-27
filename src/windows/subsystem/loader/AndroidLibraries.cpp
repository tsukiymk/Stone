#include "loader/AndroidSymbolResolver.h"
#include "AndroidLibraryState.h"
#include "filesystem/AndroidPrivateLibraries.h"
#include "abi/TypedAndroidImport.h"
#include "abi/Arm64HostJni.h"

namespace stone::windows {
using namespace android_libraries;
bool ArtNativeLoaderAvailable();

namespace android_libraries {
struct LibraryNamespace;

State& Process()
{
    static auto* state = new State;
    return *state;
}

const ElfImage* NativeSystemImageForCode(uintptr_t address, size_t size)
{
    for (auto* module = Process().nativeSystemImages.load(std::memory_order_acquire); module; module = module->next) {
        if (module->image->ContainsCode(address, size))
            return module->image.get();
    }
    return nullptr;
}

thread_local LibraryNamespace* activeNamespace = nullptr;

LibraryNamespace& CurrentNamespace()
{
    if (activeNamespace)
        return *activeNamespace;
    // Native host calls made after ART bootstrap share its system scope.
    if (Process().runtime.artRuntime)
        return Process().runtime;
    return Process().application;
}

LibraryNamespace* NamespaceForCode(uintptr_t address)
{
    if (Process().translate) {
        const std::lock_guard lock(Process().mutex);
        for (auto* scope : {&Process().application, &Process().runtime, &Process().webView}) {
            for (const auto& [name, library] : scope->images)
                if (library->image.ContainsCode(address, 4))
                    return scope;
            if (scope->imageUnwind && scope->imageUnwind->ContainsCode(address, 4))
                return scope;
        }
        return nullptr;
    }

    // Native-system ELF and host thunks intentionally have no loader scope.
    // Cache those misses too: SQLite comparators repeatedly call this path.
    // Publishing another DSO invalidates every thread's old lookup results.
    struct Entry {
        uintptr_t address = 0;
        uint64_t generation = 0;
        LibraryNamespace* scope = nullptr;
    };

    thread_local std::array<Entry, 4> recent;
    thread_local size_t next = 0;
    const auto generation = Process().codeGeneration.load(std::memory_order_acquire);
    for (const auto& entry : recent)
        if (entry.address == address && entry.generation == generation)
            return entry.scope;
    LibraryNamespace* scope = nullptr;
    for (const auto& slot : Process().executableImages) {
        const auto* library = slot.load(std::memory_order_acquire);
        if (!library)
            break;
        if (library->image.ContainsCode(address, 1)) {
            scope = library->scope;
            break;
        }
    }
    recent[next] = {address, generation, scope};
    next = (next + 1) % recent.size();
    return scope;
}

} // namespace android_libraries

bool IsArm64ElfFunction(uintptr_t address)
{
    if (address % 4)
        return false;
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    for (const auto* scope : {&state.application, &state.runtime, &state.webView})
        for (const auto& [name, library] : scope->images)
            if (library->image.Architecture() == ElfImage::Machine::Arm64 && library->image.ContainsCode(address, 4))
                return true;
    return false;
}

JavaVM* AndroidHostVm()
{
    return hostVm;
}

bool InitializeAndroidNativeBackend()
{
    return InstallAndroidExceptionHandler();
}

bool IsArm64TranslationEnabled()
{
    return Process().translate;
}

std::recursive_mutex& AndroidLoaderMutex()
{
    return Process().mutex;
}

int AndroidTargetSdk()
{
    return Process().applicationTargetSdk.load(std::memory_order_relaxed);
}

void SetAndroidTargetSdk(int sdk)
{
    Process().applicationTargetSdk.store(sdk, std::memory_order_relaxed);
    ConfigureAndroidFdSan(sdk);
}

void* ResolveAndroidTls(const ElfImage* module, uint64_t offset)
{
    if (!module)
        std::abort();
    try {
        auto& state = Process();
        if (const auto* image = state.executableWebViewUnwinder.load(std::memory_order_acquire); image == module) {
            if (void* address = image->ThreadLocalAddress(offset))
                return address;
        }
        for (const auto& slot : state.executableImages) {
            const auto* library = slot.load(std::memory_order_acquire);
            if (!library)
                break;
            if (&library->image != module)
                continue;
            if (void* address = library->image.ThreadLocalAddress(offset))
                return address;
            break;
        }
    }
    catch (const std::bad_alloc&) {
    }
    std::fprintf(stderr, "[android-native] could not resolve ELF thread-local storage\n");
    std::abort();
}

namespace android_libraries {
void InstallNativeImageZlib(const AndroidElfReader& read)
{
    const auto selected = read("libz.so");
    constexpr uint8_t x64Elf[]{0x7f, 'E', 'L', 'F', 2, 1};
    if (selected.size() < 64 || !std::equal(std::begin(x64Elf), std::end(x64Elf), selected.begin()) ||
        selected[18] != 0x3e || selected[19] != 0)
        return;
    // Only a system-image library may run natively on behalf of an ARM64 guest.
    if (selected != read("/system/lib64/libz.so"))
        return;
    LoadNativeSystemElf("libz.so", selected);
    std::fputs("[android-jit] bridged x86_64 system-image libz.so for ARM64\n", stderr);
}

std::unique_ptr<AndroidLoadedImage> PrepareImageUnwind(std::span<const uint8_t> bytes, size_t& staticTlsUsed,
                                                       bool inspection)
{
    auto& state = Process();
    auto image = std::make_unique<AndroidLoadedImage>(bytes, state.machine, true);
    constexpr std::string_view required[]{
        "_Unwind_DeleteException",
        "_Unwind_GetDataRelBase",
        "_Unwind_GetIP",
        "_Unwind_GetIPInfo",
        "_Unwind_GetLanguageSpecificData",
        "_Unwind_GetRegionStart",
        "_Unwind_GetTextRelBase",
        "_Unwind_RaiseException",
        "_Unwind_Resume",
        "_Unwind_SetGR",
        "_Unwind_SetIP",
    };
    for (const auto name : required) {
        if (!image->Export(name, true))
            throw std::runtime_error("missing Bionic unwind export: " + std::string(name));
    }
    auto tls = staticTlsUsed;
    image->AssignStaticTls(tls);
    const auto record = [&](std::string_view name, uintptr_t address) {
        if (address && state.translate)
            state.arm64ImportNames.try_emplace(address, name);
        return address;
    };
    image->Relocate(
        [&](std::string_view name) -> uintptr_t {
            if (name == "dl_iterate_phdr")
                return record(name, Import(
                                        "Bionic unwind dl_iterate_phdr", +[](uintptr_t callback, void* data) {
                                            return DlIterateProgramHeaders(callback, data, true);
                                        }));
            if (const auto address = Libdl(name))
                return record(name, address);
            // The image is an unwind provider. It has no Android linker bootstrap
            // or Bionic process initialization. Fail explicitly if that code is reached.
            if (name.starts_with("__loader_"))
                return record(
                    name, Import(
                              name, +[] {
                                  std::fputs("[android-native] Bionic unwind entered Android linker bootstrap code\n",
                                             stderr);
                                  std::abort();
                              }));
            return 0;
        },
        nullptr, {},
        [&](std::string_view name) {
            return LookupSystemSymbol(systemLibraries[SystemLibraryIndex("libc.so")], name);
        },
        state.translate ? Arm64TlsDescriptorResolver() : 0);
    if (image->IndirectRelocationCount()) {
        if (!inspection) {
            if (!state.translate)
                throw std::runtime_error("Bionic unwinder requires indirect resolvers outside the Windows libc bridge");
            const ElfImage* modules[]{image.get()};
            ElfImage::PublishStaticTls(modules);
            image->ResolveIndirectRelocations([&](uintptr_t resolver) {
                const auto target = RunArm64IfuncResolver(*image, resolver, Arm64ImportAt, AndroidProperty);
                if (!image->ContainsCode(target, 4) && Arm64ImportAt(target).empty())
                    throw std::runtime_error("Bionic ARM64 indirect resolver returned an unknown code address");
                return target;
            });
        }
    }
    if (!image->IndirectRelocationCount())
        image->Protect(state.translate ? ElfImage::Execution::Translated : ElfImage::Execution::Native);
    staticTlsUsed = tls;
    return image;
}

Library& Prepare(LibraryNamespace& scope, const std::string& name, const AndroidElfReader& read, Libraries& staged,
                 size_t& staticTlsUsed, unsigned depth, AndroidElfLinkReport* report, bool collectBindings,
                 bool artCompiler, bool oatCode)
{
    const auto& state = Process();
    if (const auto found = scope.images.find(name); found != scope.images.end())
        return *found->second;
    if (state.resolvingStagedElf)
        throw std::runtime_error("cannot load another ELF from a relocation resolver");
    if (const auto found = staged.find(name); found != staged.end()) {
        if (!found->second)
            throw std::runtime_error("cyclic Android ELF dependency: " + name);
        return *found->second;
    }
    if (depth > 32)
        throw std::runtime_error(name + ": Android ELF dependency depth exceeds 32");
    const auto loaded = state.application.images.size() + state.webView.images.size() + state.runtime.images.size();
    if (staged.size() + loaded >= MaxAndroidLibraries)
        throw std::runtime_error(name + ": Android ELF library count exceeds " + std::to_string(MaxAndroidLibraries));
    staged.emplace(name, nullptr);
    const auto bytes = read(name);
    if (bytes.empty())
        throw std::runtime_error(std::string(state.machine == ElfImage::Machine::Arm64
                                                 ? "missing Android arm64-v8a dependency: "
                                                 : "missing Android x86_64 dependency: ") +
                                 name);
    try {
        auto lib = std::make_unique<Library>(bytes, state.machine, !state.translate || scope.artRuntime, artCompiler);
        lib->name = name;
        if (state.translate)
            RegisterArm64ProfileImage(lib->image.Identity(), name);
        if (!state.translate)
            lib->ubsanSyscalls = PrepareUbsanRuntime(name, bytes, lib->image);
        lib->scope = &scope;
        lib->image.AssignStaticTls(staticTlsUsed);
        for (const auto& dependency : lib->image.Dependencies()) {
            if (state.translate && !scope.artRuntime && dependency == "libz.so" && !NativeImageZlibLoaded() &&
                !scope.images.contains(dependency) && !staged.contains(dependency))
                InstallNativeImageZlib(read);
            const auto system = SystemLibraryIndex(dependency);
            const bool useImage =
                (scope.imageMath && dependency == "libm.so") || (scope.imageLog && dependency == "liblog.so");
            if (system != SystemLibraryCount && !useImage) {
                // Resolve platform imports to the existing host bridge. The
                // translated leaf backend still rejects calls into those imports.
                lib->systemDependencies.set(system);
                continue;
            }
            lib->dependencies.push_back(
                &Prepare(scope, dependency, read, staged, staticTlsUsed, depth + 1, report, collectBindings));
        }
        std::set<std::string> missingImports;
        Library* imageMath = nullptr;
        lib->image.Relocate(
            [&](std::string_view symbol) {
                if (artCompiler && symbol == "_exit") {
                    // dex2oat deliberately ends its dedicated compiler process.
                    // Do not enable process termination for arbitrary ARM64 code.
                    const auto stop = +[](int status) {
                        ExitProcess(static_cast<UINT>(status));
                    };
                    const auto target =
                        ImportAndroidScalars<decltype(stop)>("dex2oat process exit", reinterpret_cast<uintptr_t>(stop));
                    return ElfImage::LinkSymbol{
                        state.translate ? PrepareArm64HostJniTarget("dex2oat process exit", "(I)V", target) : target};
                }
                if (artCompiler && symbol == "__libc_init")
                    return ElfImage::LinkSymbol{Import(
                        "dex2oat unsupported process entry", +[] {
                            std::fputs("dex2oat must be invoked through its main function\n", stderr);
                            ExitProcess(3);
                        })};
                if (scope.artRuntime && state.machine == ElfImage::Machine::X64 && name == "libart.so") {
                    if (const auto result = AndroidArtHeapImport(symbol)) {
                        if (collectBindings && report)
                            report->bindings.push_back({name, std::string(symbol), "libc.so", true});
                        return ElfImage::LinkSymbol{result};
                    }
                }
                if (const auto result = GlobalSymbol(scope, symbol))
                    return result;
                AndroidElfLinkReport::Binding binding{{}, {}, {}, false};
                if (report && scope.artRuntime && IsArtAssetImport(symbol)) {
                    // Registration loads this provider before Bitmap/asset JNI is
                    // used. Model that dependency without executing registration.
                    auto& assets = Prepare(scope, "libandroid.so", read, staged, staticTlsUsed, depth + 1, report,
                                           collectBindings);
                    if (const auto result = assets.image.FindLinkSymbol(symbol)) {
                        if (collectBindings)
                            report->bindings.push_back({name, std::string(symbol), assets.name, false});
                        return result;
                    }
                }
                if (const auto result =
                        DependencySymbol(*lib, symbol, scope.imageUnwind.get(), collectBindings ? &binding : nullptr)) {
                    if (collectBindings && report) {
                        binding.consumer = name;
                        binding.symbol = symbol;
                        report->bindings.push_back(std::move(binding));
                    }
                    return result;
                }
                // Windows long double has 64 bits, while these Android ARM64 functions use binary128.
                if (state.translate && lib->systemDependencies.test(SystemLibraryIndex("libm.so")) &&
                    (symbol == "sincosl" || symbol == "floorl")) {
                    auto& math = Prepare(scope, "/apex/com.android.runtime/lib64/bionic/libm.so", read, staged,
                                         staticTlsUsed, depth + 1, report);
                    if (const auto result = math.image.FindLinkSymbol(symbol)) {
                        imageMath = &math;
                        return result;
                    }
                }
                return ElfImage::LinkSymbol{};
            },
            &missingImports,
            [&](std::string_view symbol) {
                for (const auto* global : scope.globalImages) {
                    if (auto tls = global->image.FindTlsExport(symbol); tls.module)
                        return tls;
                }
                return FindDependency(*lib, [&](const Library& dependency) {
                    return dependency.image.FindTlsExport(symbol);
                });
            },
            {}, scope.artRuntime && state.translate ? Arm64TlsDescriptorResolver() : 0);
        if (imageMath)
            lib->dependencies.push_back(imageMath);
        if (report) {
            ++report->libraries;
            report->indirectRelocations += lib->image.IndirectRelocationCount();
            for (const auto& symbol : missingImports)
                report->missingImports.push_back(name + ": " + symbol);
            auto& slot = staged.at(name);
            slot = std::move(lib);
            return *slot;
        }
        if (!missingImports.empty()) {
            std::string message = state.translate ? "unresolved Android JIT imports: " : "unresolved Android imports: ";
            size_t count = 0;
            for (const auto& symbol : missingImports) {
                if (count == 64) {
                    message += ", and " + std::to_string(missingImports.size() - count) + " more";
                    break;
                }
                if (count++)
                    message += ", ";
                message += symbol;
            }
            throw std::runtime_error(message);
        }
        if (lib->image.IndirectRelocationCount()) {
            if (state.translate)
                throw std::runtime_error("Android ELF indirect relocations require a JIT resolver");
            auto& flag = Process().resolvingStagedElf;
            flag = true;
            try {
                lib->image.ResolveIndirectRelocations([&scope](uintptr_t resolver) {
                    return CallElfResolver(resolver, scope);
                });
            }
            catch (...) {
                flag = false;
                throw;
            }
            flag = false;
        }
        const bool artThreadCode = name == "libart.so" || name == "libartd.so" || name == "libopenjdkjvm.so" ||
                                   name == "libopenjdkjvmti.so" || name == "libadbconnection.so" ||
                                   name == "libperfetto_hprof.so" || artCompiler || oatCode;
        if (oatCode) {
            lib->debugSymbols = ElfDebugSymbols(bytes);
            if (std::none_of(lib->debugSymbols->begin(), lib->debugSymbols->end(), [](const auto& entry) {
                    return entry.second.code;
                }))
                throw std::runtime_error("Stone AOT requires dex2oat --generate-debug-info");
        }
        if (scope.artRuntime && !state.translate && artThreadCode) {
            lib->artGs = std::make_unique<ArtGsCode>(lib->image, bytes, oatCode);
            if (lib->artGs->Size())
                std::fprintf(stderr, "[art] rewrote %zu GS instructions in %s\n", lib->artGs->Size(), name.c_str());
        }
        lib->initializers = lib->image.Initializers();
        lib->image.Protect(state.translate ? ElfImage::Execution::Translated : ElfImage::Execution::Native);
        auto& slot = staged.at(name);
        slot = std::move(lib);
        return *slot;
    }
    catch (const std::exception& error) {
        throw std::runtime_error(name + ": " + error.what());
    }
}

void Initialize(Library& lib)
{
    if (lib.initializerFailure)
        std::rethrow_exception(lib.initializerFailure);
    if (lib.initialized)
        return;
    const bool translate = Process().translate;
    const ScopedNamespace context(*lib.scope);
    // Mark before traversing dependencies: ELF cycles and constructor dlopen
    // must not recursively run the same constructors on either architecture.
    lib.initialized = true;
    try {
        for (auto* dependency : lib.dependencies)
            Initialize(*dependency);
        if (lib.scope->read)
            std::fprintf(stderr, "[art] initializing %s at 0x%llx\n", lib.name.c_str(),
                         static_cast<unsigned long long>(lib.image.LoadBias()));
        for (void* initializer : lib.initializers) {
            if (translate)
                RunArm64Lifecycle(lib.image, reinterpret_cast<uintptr_t>(initializer), false);
            else {
                Call call(reinterpret_cast<uintptr_t>(initializer), "()V", false);
                call.scope = lib.scope;
                CallAndroid(&call, nullptr, nullptr);
            }
        }
        if (!translate) {
            if (const auto sqliteRegister = lib.image.Export("sqlite3_vfs_register", true);
                sqliteRegister && lib.image.Export("sqlite3_open_v2", true)) {
                Call call(sqliteRegister, "(LI)I", false, true);
                call.scope = lib.scope;
                if (CallAndroid(&call, WindowsSqliteVfs(&lib.sqliteVfs, lib.scope->artRuntime),
                                reinterpret_cast<void*>(1)) != SQLITE_OK)
                    throw std::runtime_error("could not register the Windows SQLite VFS for " + lib.name);
            }
        }
    }
    catch (...) {
        lib.initializerFailure = std::current_exception();
        throw;
    }
    lib.initialized = true;
}

void PublishImages(State& state, LibraryNamespace& scope, Libraries& staged, size_t staticTlsUsed,
                   const ElfImage* newUnwinder)
{
    if (staged.empty())
        return;
    scope.images.reserve(scope.images.size() + staged.size());
    if (state.staticTlsUsed != staticTlsUsed) {
        std::array<const ElfImage*, MaxAndroidLibraries + 1> images;
        size_t count = 0;
        if (newUnwinder)
            images[count++] = newUnwinder;
        for (const auto& [name, library] : staged)
            images[count++] = &library->image;
        ElfImage::PublishStaticTls(std::span(images).first(count));
    }
    if (!state.translate) {
        for (const auto& [name, library] : staged) {
            state.executableImages[state.executableCount++].store(library.get(), std::memory_order_release);
        }
        if (newUnwinder)
            state.executableWebViewUnwinder.store(newUnwinder, std::memory_order_release);
    }
    state.staticTlsUsed = staticTlsUsed;
    scope.images.merge(staged);
    state.codeGeneration.fetch_add(1, std::memory_order_release);
}

thread_local std::array<char, 512> dlError{};
thread_local bool dlErrorPending = false;
char defaultHandle;
}

void SetDlError(const char* message) noexcept
{
    std::snprintf(dlError.data(), dlError.size(), "%s", message);
    dlErrorPending = true;
}

char* DlError() noexcept
{
    if (!dlErrorPending)
        return nullptr;
    dlErrorPending = false;
    return dlError.data();
}

namespace android_libraries {
void* OpenLibrary(const char* filename, int flags, bool forceLoad) noexcept
{
    constexpr int NoLoad = 0x4, Global = 0x100;
    constexpr int KnownFlags = 0x1 | 0x2 | NoLoad | Global | 0x1000;
    JNIEnv* env = nullptr;
    try {
        if (flags & ~KnownFlags)
            throw std::runtime_error("unsupported Android dlopen flags");
        if (!filename)
            return &defaultHandle;
        std::string name(filename);
        auto& state = Process();
        auto& scope = CurrentNamespace();
        const std::lock_guard lock(state.mutex);
        const bool oatCode =
            scope.artRuntime &&
            (name.starts_with("/data/") || name.starts_with("/system/") || name.starts_with("/apex/")) &&
            (name.ends_with(".oat") || name.ends_with(".odex") ||
             (name.starts_with(state.translate ? "/data/dalvik-cache/arm64/" : "/data/dalvik-cache/x86_64/") &&
              name.ends_with("@classes.dex")));
        const auto oatPath = name;
        if (oatCode && forceLoad && scope.images.contains(name)) {
            size_t generation = 1;
            while (scope.images.contains(name + '#' + std::to_string(generation)))
                ++generation;
            name += '#' + std::to_string(generation);
        }
        if (const auto system = SystemLibraryPathIndex(name); system != SystemLibraryCount) {
            name = systemLibraries[system].name;
        }
        else if (!oatCode && name.find_first_of("/\\:") != name.npos) {
            if (scope.artRuntime && !scope.applicationLibraryPrefix.empty() &&
                name.starts_with(scope.applicationLibraryPrefix) &&
                name.substr(scope.applicationLibraryPrefix.size()).find('/') == name.npos) {
                name = name.substr(scope.applicationLibraryPrefix.size());
            }
            else if (scope.artRuntime && !scope.applicationDataPrefix.empty() &&
                     name.starts_with(scope.applicationDataPrefix)) {
                // Keep the full private path as the ELF identity. Two core
                // versions with the same basename must not alias each other.
            }
            else if (scope.artRuntime && !scope.applicationDataPrefix.empty() && name.starts_with("/data/")) {
                throw std::runtime_error("native library path is outside the current application");
            }
            else if (scope.read) {
                name = name.substr(name.find_last_of("/\\") + 1);
            }
            else {
                const AttachedEnvironment thread;
                auto* attached = thread.env;
                if (!scope.loader || !state.resolveLibraryPath || !attached || attached->ExceptionCheck())
                    throw std::runtime_error("dlopen needs a usable JNI environment to resolve an APK path");
                const auto path = attached->NewStringUTF(filename);
                const auto resolved =
                    path
                        ? static_cast<jstring>(attached->CallObjectMethod(scope.loader, state.resolveLibraryPath, path))
                        : nullptr;
                if (path)
                    attached->DeleteLocalRef(path);
                if (attached->ExceptionCheck())
                    attached->ExceptionClear();
                if (!resolved)
                    throw std::runtime_error("dlopen expects a library name or a path to a library in the APK");
                name = Utf(attached, resolved);
                attached->DeleteLocalRef(resolved);
            }
        }
        if (!oatCode && !name.ends_with(".so")) {
            // Some engines look up a library by the name they passed to System.loadLibrary.
            // Resolve that alias only when the ELF already belongs to this namespace.
            auto mapped = "lib" + name + ".so";
            if (!scope.images.contains(mapped))
                throw std::runtime_error("dlopen expects a library name from the APK: \"" + name + '"');
            name = std::move(mapped);
        }
        if (const auto system = SystemLibraryIndex(name); system != SystemLibraryCount &&
                                                          !(scope.imageMath && name == "libm.so") &&
                                                          !(scope.imageLog && name == "liblog.so")) {
            auto& handles = state.systemHandles[system];
            ++handles;
            return &handles;
        }
        Library* lib;
        if (const auto found = scope.images.find(name); found != scope.images.end()) {
            lib = found->second.get();
        }
        else {
            if (flags & NoLoad)
                throw std::runtime_error("Android library is not loaded");
            if (!scope.read &&
                (!scope.loader || !hostVm || hostVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK))
                throw std::runtime_error("dlopen needs a thread attached to the JVM");
            if (env && env->ExceptionCheck()) {
                env = nullptr;
                throw std::runtime_error("dlopen called while a JNI exception is pending");
            }
            Libraries staged;
            auto staticTlsUsed = state.staticTlsUsed;
            const AndroidElfReader libraries =
                scope.read ? scope.read : AndroidElfReader{[&](const std::string& dependency) {
                    return ReadLibrary(env, scope.loader, dependency);
                }};
            const AndroidElfReader read = [&](const std::string& dependency) {
                if (!oatCode || dependency != name)
                    return libraries(dependency);
                const auto path = ResolveAndroidSystemFile(
                    std::filesystem::path(
                        std::u8string_view(reinterpret_cast<const char8_t*>(oatPath.data() + 1), oatPath.size() - 1))
                        .wstring());
                const auto size = std::filesystem::file_size(path);
                if (size > 256 * 1024 * 1024)
                    throw std::runtime_error("AOT file is too large");
                std::vector<uint8_t> bytes(static_cast<size_t>(size));
                std::ifstream input(path, std::ios::binary);
                if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
                    throw std::runtime_error("could not read AOT file");
                return bytes;
            };
            lib = &Prepare(scope, name, read, staged, staticTlsUsed, 0, nullptr, false, false, oatCode);
            PublishImages(state, scope, staged, staticTlsUsed);
            if (oatCode)
                std::fprintf(stderr, "[art-aot] loaded %s\n", name.c_str());
        }
        if (flags & Global) {
            auto& global = scope.globalImages;
            if (std::find(global.begin(), global.end(), lib) == global.end())
                global.push_back(lib);
        }
        Initialize(*lib);
        ++lib->openHandles;
        return lib;
    }
    catch (const std::exception& error) {
        if (env && env->ExceptionCheck())
            env->ExceptionClear();
        wchar_t diagnostics[2]{};
        if (GetEnvironmentVariableW(L"STONE_ART_DIAGNOSTICS", diagnostics, 2) == 1 && diagnostics[0] == L'1')
            std::fprintf(stderr, "[android-linker] dlopen %s rejected: %s\n", filename ? filename : "<default>",
                         error.what());
        if (filename && std::string_view(filename).starts_with("/data/dalvik-cache/"))
            std::fprintf(stderr, "[art-aot] load refused: %s\n", error.what());
        SetDlError(error.what());
        return nullptr;
    }
    catch (const AndroidSystemPathError&) {
        SetDlError("AOT file is outside the Android image or data mount");
        return nullptr;
    }
}
}

void* DlOpen(const char* filename, int flags) noexcept
{
    return OpenLibrary(filename, flags, false);
}

void* AndroidExportedNamespace(const char* name) noexcept
{
    // The native ART bootstrap has one system scope backed by the selected image.
    if (name && std::string_view(name) == "system" && Process().runtime.read)
        return &Process().runtime;
    return nullptr;
}

bool ArtNativeLoaderAvailable()
{
    return CurrentNamespace().artRuntime;
}

void* AndroidDlOpenExt(const char* filename, int flags, const uint64_t* extensionFlags) noexcept
{
    // The extension structure begins with a 64-bit flags field. A null or
    // empty extension has the same meaning as an ordinary dlopen request.
    if (extensionFlags &&
        (*extensionFlags == 0x200 || *extensionFlags == 0x240)) { // USE_NAMESPACE, optional FORCE_LOAD.
        if ((*extensionFlags & 0x40) &&
            (!filename ||
             !(std::string_view(filename).ends_with(".oat") || std::string_view(filename).ends_with(".odex") ||
               (std::string_view(filename).starts_with(Process().translate ? "/data/dalvik-cache/arm64/"
                                                                           : "/data/dalvik-cache/x86_64/") &&
                std::string_view(filename).ends_with("@classes.dex"))))) {
            SetDlError("FORCE_LOAD is currently supported only for ART OAT files");
            return nullptr;
        }
        const auto target = reinterpret_cast<const uintptr_t*>(extensionFlags)[5];
        if (target != reinterpret_cast<uintptr_t>(&Process().runtime) || !Process().runtime.read) {
            SetDlError("android_dlopen_ext received an unknown linker namespace");
            return nullptr;
        }
        auto* previous = std::exchange(activeNamespace, &Process().runtime);
        auto* result = OpenLibrary(filename, flags, (*extensionFlags & 0x40) != 0);
        activeNamespace = previous;
        return result;
    }
    if (extensionFlags && *extensionFlags) {
        SetDlError("android_dlopen_ext extension flags are unsupported by the Stone linker");
        return nullptr;
    }
    return DlOpen(filename, flags);
}

void* DlSym(void* handle, const char* symbol) noexcept
{
    try {
        if (!symbol || !*symbol)
            throw std::runtime_error("empty Android symbol name");
        auto& state = Process();
        auto& scope = CurrentNamespace();
        const std::lock_guard lock(state.mutex);
        ElfImage::LinkSymbol result;
        if (!handle || handle == &defaultHandle) {
            result = GlobalSymbol(scope, symbol);
            if (!result && scope.artRuntime) {
                // Honor the explicit image-provider policy for dlsym as well
                // as relocations. A blanket dependency search would expose
                // libsigchain's wrappers ahead of the required Windows signal
                // bridge and break its "next" handler lookup during startup.
                for (const auto name : {"libm.so", "liblog.so"}) {
                    if ((name == std::string_view("libm.so") && !scope.imageMath) ||
                        (name == std::string_view("liblog.so") && !scope.imageLog))
                        continue;
                    const auto image = scope.images.find(name);
                    if (image == scope.images.end())
                        continue;
                    if (name == std::string_view("liblog.so"))
                        result = AndroidLogImport(symbol);
                    if (!result)
                        result = image->second->image.FindLinkSymbol(symbol);
                    if (result)
                        break;
                }
            }
            for (const auto& system : systemLibraries) {
                if (result)
                    break;
                result = LookupSystemSymbol(system, symbol);
            }
        }
        else {
            for (size_t i = 0; i < state.systemHandles.size(); ++i) {
                if (handle == &state.systemHandles[i] && state.systemHandles[i]) {
                    result = LookupSystemSymbol(systemLibraries[i], symbol);
                    break;
                }
            }
            for (const auto& [name, lib] : scope.images) {
                if (lib.get() != handle || !lib->openHandles)
                    continue;
                result = lib->image.FindLinkSymbol(symbol);
                if (!result)
                    result = DependencySymbol(*lib, symbol, scope.imageUnwind.get());
                break;
            }
        }
        if (result) {
            if (result.indirect && state.translate)
                throw std::runtime_error("ARM64 resolver symbols are unsupported");
            return reinterpret_cast<void*>(result.indirect ? CallElfResolver(result.address, scope) : result.address);
        }
        SetDlError(("Android symbol not found: " + std::string(symbol)).c_str());
    }
    catch (const std::exception& error) {
        SetDlError(error.what());
    }
    return nullptr;
}

int DlClose(void* handle) noexcept
{
    try {
        auto& state = Process();
        const std::lock_guard lock(state.mutex);
        if (handle == &defaultHandle)
            return 0;
        for (auto& handles : state.systemHandles) {
            if (handle == &handles && handles) {
                --handles;
                return 0;
            }
        }
        for (const auto& [name, lib] : CurrentNamespace().images) {
            if (lib.get() == handle && lib->openHandles) {
                --lib->openHandles;
                return 0;
            }
        }
        SetDlError("invalid Android library handle");
    }
    catch (const std::exception& error) {
        SetDlError(error.what());
    }
    return -1;
}

int DlAddr(const void* address, AndroidDlInfo* info) noexcept
{
    if (!address || !info)
        return 0;
    try {
        auto& state = Process();
        const std::lock_guard lock(state.mutex);
        AndroidDlInfo found{};
        for (const auto* scope : {&state.application, &state.webView, &state.runtime}) {
            for (const auto& [name, lib] : scope->images) {
                if (lib->image.AddressInfo(reinterpret_cast<uintptr_t>(address), found.base, found.symbol,
                                           found.address)) {
                    found.file = name.c_str();
                    *info = found;
                    return 1;
                }
            }
        }
    }
    catch (const std::exception&) {
    }
    return 0;
}

bool ProtectAndroidElfData(uintptr_t address, size_t length, bool writable)
{
    // Published images outlive all native guest calls.
    for (const auto& slot : Process().executableImages) {
        const auto* library = slot.load(std::memory_order_acquire);
        if (!library)
            break;
        if (library->image.ProtectData(address, length, writable))
            return true;
    }
    return false;
}

void DlCfiSlowpath(uint64_t typeId, void* target, void* diagnostic) noexcept
{
    auto& state = Process();
    const auto address = reinterpret_cast<uintptr_t>(target);
    const ElfImage* owner = nullptr;
    // Cross-DSO CFI also checks vtables in data segments. The published ELF
    // images are permanent, so this lookup needs no loader lock.
    for (const auto& slot : state.executableImages) {
        const auto* library = slot.load(std::memory_order_acquire);
        if (!library)
            break;
        if (library->image.DataAt(address, 1, false)) {
            owner = &library->image;
            break;
        }
    }
    if (!owner) {
        const auto* image = state.executableWebViewUnwinder.load(std::memory_order_acquire);
        if (image && image->DataAt(address, 1, false))
            owner = image;
    }
    if (!owner) {
        for (auto* module = state.nativeSystemImages.load(std::memory_order_acquire); module; module = module->next) {
            if (module->ready.load(std::memory_order_acquire) && module->image->DataAt(address, 1, false)) {
                owner = module->image.get();
                break;
            }
        }
    }
    if (owner) {
        try {
            if (const auto check = owner->Export("__cfi_check", true)) {
                CallAndroidCallback(check, reinterpret_cast<void*>(typeId), target, diagnostic);
            }
            return; // Bionic allows targets in loaded, uninstrumented libraries.
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "[android-native] CFI check failed: %s\n", error.what());
        }
    }
    std::fprintf(stderr, "[android-native] invalid CFI target %p for type 0x%llx\n", target,
                 static_cast<unsigned long long>(typeId));
    std::abort();
}

struct DlProgramHeaderInfo {
    uintptr_t loadBias;
    const char* file;
    const elf::Elf64Phdr* headers;
    uint16_t headerCount;
    uint64_t loads;
    uint64_t unloads;
    uintptr_t tlsModule;
    void* tlsData;
};

static_assert(sizeof(DlProgramHeaderInfo) == 64 && offsetof(DlProgramHeaderInfo, loads) == 32);

int DlIterateProgramHeaders(uintptr_t callback, void* data, bool lockForUnwind)
{
    if (!callback)
        return 0;
    auto& state = Process();
    std::array<std::pair<const std::string*, const ElfImage*>, MaxAndroidLibraries> images{};
    size_t count = 0;
    const NativeSystemImage* nativeSystemImages;
    uint64_t nativeCount = 0;
    {
        const std::lock_guard lock(state.mutex);
        if (!state.translate) {
            for (const auto* scope : {&state.application, &state.webView, &state.runtime}) {
                for (const auto& [name, library] : scope->images)
                    images[count++] = {&name, &library->image};
            }
        }
        nativeSystemImages = state.nativeSystemImages.load(std::memory_order_acquire);
        for (auto* module = nativeSystemImages; module; module = module->next)
            if (module->ready.load(std::memory_order_acquire))
                ++nativeCount;
    }
    const uint64_t loads = count + nativeCount;
    // Published images and names are permanent. Let callbacks invoke loader
    // APIs without holding the loader mutex across guest calls.
    // The Android linker serializes unwinder callbacks that mutate their
    // shared program-header cache. Acquire this lock after taking the snapshot.
    static std::recursive_mutex unwindHeadersMutex;
    std::unique_lock unwindLock(unwindHeadersMutex, std::defer_lock);
    if (lockForUnwind)
        unwindLock.lock();
    const auto visit = [&](const std::string& name, const ElfImage* image) {
        const auto headers = image->ProgramHeaders();
        DlProgramHeaderInfo info{image->LoadBias(),
                                 name.c_str(),
                                 headers.data(),
                                 static_cast<uint16_t>(headers.size()),
                                 loads,
                                 0,
                                 image->TlsModule(),
                                 image->ThreadLocalAddress(0, false)};
        return static_cast<int>(CallAndroidCallback(callback, &info, reinterpret_cast<void*>(sizeof(info)), data));
    };
    for (size_t i = 0; i < count; ++i) {
        if (const auto result = visit(*images[i].first, images[i].second))
            return result;
    }
    for (auto* module = nativeSystemImages; module; module = module->next) {
        if (module->ready.load(std::memory_order_acquire))
            if (const auto result = visit(module->name, module->image.get()))
                return result;
    }
    return 0;
}

extern "C" uintptr_t* StoneGuestThreadPointer()
{
    if (!Process().translate)
        RegisterNativeSignalThread();
    return ElfImage::GuestThreadPointer();
}

extern "C" uintptr_t* StoneCurrentGuestThreadPointer() noexcept
{
    return ElfImage::CurrentGuestThreadPointer();
}

uintptr_t Arm64VulkanSymbol(std::string_view name)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    const auto address = Libvulkan(name);
    if (address)
        state.arm64ImportNames.try_emplace(address, name);
    return address;
}

uintptr_t Arm64OpenSlSymbol(std::string_view name)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    const auto address = Libopensles(name);
    if (address)
        state.arm64ImportNames.try_emplace(address, name);
    return address;
}

bool NativeImageZlibLoaded()
{
    return NativeSystemElfExport("libz.so", "inflate") != 0;
}

uintptr_t NativeImageZlibExport(std::string_view name)
{
    return NativeSystemElfExport("libz.so", name);
}

int CallNativeImageZlib(std::string_view name, const std::array<uintptr_t, 4>& arguments)
{
    const auto address = NativeImageZlibExport(name);
    if (!address)
        throw std::runtime_error("missing Android libz export: " + std::string(name));
    const Call call(address, "(LLLL)I", false, true);
    return static_cast<int>(CallAndroid(&call, reinterpret_cast<void*>(arguments[0]),
                                        reinterpret_cast<void*>(arguments[1]), reinterpret_cast<void*>(arguments[2]),
                                        arguments[3]));
}

AndroidElfLinkReport InspectAndroidElf(std::string_view soname, std::string_view entry, const AndroidElfReader& read,
                                       bool collectBindings, ElfImage::Machine machine)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    if (state.translate || (machine != state.machine && state.runtime.artRuntime))
        throw std::runtime_error("ELF link inspection cannot change a running Android backend");

    struct Restore {
        State& state;
        ElfImage::Machine machine;
        bool translate;

        ~Restore()
        {
            state.machine = machine;
            state.translate = translate;
        }
    } restore{state, state.machine, state.translate};

    state.machine = machine;
    state.translate = machine == ElfImage::Machine::Arm64;
    LibraryNamespace scope;
    scope.imageMath = true;
    scope.imageLog = true;
    scope.artRuntime = true;
    const ScopedNamespace context(scope);
    Libraries staged;
    size_t staticTlsUsed = 0;
    AndroidElfLinkReport report{};
    const auto bionic = read("libc.so");
    if (!bionic.empty()) {
        scope.imageUnwind = PrepareImageUnwind(bionic, staticTlsUsed, true);
        report.indirectRelocations += scope.imageUnwind->IndirectRelocationCount();
    }
    const auto& library = Prepare(scope, std::string(soname), read, staged, staticTlsUsed, 0, &report, collectBindings);
    if (!entry.empty() && !library.image.Export(entry, true)) {
        throw std::runtime_error(std::string(soname) + ": missing Android ELF export " + std::string(entry));
    }
    return report;
}

std::vector<AndroidStubExports> InspectAndroidStubExports(const AndroidElfReader& read)
{
    const std::lock_guard lock(Process().mutex);
    if (Process().translate)
        throw std::runtime_error("stub export inspection requires native x64");
    LibraryNamespace scope;
    scope.artRuntime = true;
    const ScopedNamespace context(scope);
    std::vector<AndroidStubExports> report;
    for (const auto& system : systemLibraries) {
        AndroidStubExports row{std::string(system.name), system.name == "libm.so" || system.name == "liblog.so", {}};
        const auto bytes = read(std::string(system.name));
        row.imageAvailable = !bytes.empty();
        if (row.imageAvailable) {
            ElfImage image(bytes, ElfImage::Machine::X64, true);
            for (const auto& name : image.ExportNames())
                if (LookupSystemSymbol(system, name))
                    row.overlappingExports.push_back(name);
        }
        report.push_back(std::move(row));
    }
    return report;
}

void LoadArtImageLibrary(std::string_view soname)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    auto& scope = state.runtime;
    if (!scope.artRuntime || !scope.read || soname.empty() || soname.find_first_of("/\\:") != soname.npos ||
        !soname.ends_with(".so"))
        throw std::runtime_error("image library loading requires ART and a library SONAME");
    Libraries staged;
    auto tls = state.staticTlsUsed;
    auto& library = Prepare(scope, std::string(soname), scope.read, staged, tls, 0);
    PublishImages(state, scope, staged, tls);
    Initialize(library);
}

void* LoadArtDex2Oat()
{
    auto& state = Process();
    {
        const std::lock_guard lock(state.mutex);
        auto& scope = state.runtime;
        if (!scope.artRuntime || !scope.read || hostVm)
            throw std::runtime_error("dex2oat requires a fresh ART compiler process");
        Libraries staged;
        auto tls = state.staticTlsUsed;
        auto& library = Prepare(scope, "dex2oat64", scope.read, staged, tls, 0, nullptr, false, true);
        PublishImages(state, scope, staged, tls);
        Initialize(library);
    }
    const auto main = AndroidRuntimeFunction("dex2oat64", "main");
    return MakeNativeAndroidThunk(main, "(IL)I");
}

void SetArtPrivateLibraryRoot(const std::filesystem::path& dataDirectory, std::string_view guestData)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    auto& scope = state.runtime;
    if (!scope.artRuntime || !scope.read || state.translate)
        throw std::runtime_error("private native libraries currently require x64 image ART");
    const auto prefix = std::string(guestData) + "/";
    if (!scope.applicationDataPrefix.empty()) {
        if (scope.applicationDataPrefix != prefix)
            throw std::runtime_error("private native library data identity changed");
        return;
    }
    const auto imageReader = scope.read;
    const auto privateRoot = std::filesystem::canonical(dataDirectory);
    scope.read = [imageReader, privateRoot, prefix](const std::string& name) {
        return name.starts_with(prefix) ? ReadAndroidPrivateLibrary(privateRoot, prefix, name) : imageReader(name);
    };
    scope.applicationDataPrefix = prefix;
}

void SetArtApplicationLibraries(const std::filesystem::path& directory, const std::filesystem::path& dataDirectory,
                                std::string_view guestData)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    auto& scope = state.runtime;
    if (!scope.artRuntime || !scope.read || state.translate)
        throw std::runtime_error("APK native libraries currently require x64 image ART");
    const auto imageReader = scope.read;
    scope.applicationDataPrefix = std::string(guestData) + "/";
    const auto prefix = scope.applicationDataPrefix;
    const auto privateRoot = std::filesystem::canonical(dataDirectory);
    const auto relative = std::filesystem::canonical(directory).lexically_relative(privateRoot);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        throw std::runtime_error("APK native directory is outside application data");
    scope.applicationLibraryPrefix = std::string(guestData) + "/" + relative.generic_string() + "/";
    scope.read = [imageReader, directory, privateRoot, prefix](const std::string& name) {
        if (name.starts_with(prefix))
            return ReadAndroidPrivateLibrary(privateRoot, prefix, name);
        // Platform providers retain their image identity; APK libraries cannot
        // replace a library already selected by the ART bootstrap.
        auto bytes = imageReader(name);
        if (!bytes.empty())
            return bytes;
        if (name.empty() || name.find_first_of("/\\:") != name.npos || !name.ends_with(".so"))
            throw std::runtime_error("invalid APK native library name");
        const auto file = directory / name;
        if (!std::filesystem::is_regular_file(file))
            return bytes;
        const auto size = std::filesystem::file_size(file);
        if (size > 256 * 1024 * 1024)
            throw std::runtime_error("APK native library is too large");
        std::ifstream input(file, std::ios::binary);
        bytes.resize(static_cast<size_t>(size));
        if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error("cannot read APK native library: " + name);
        return bytes;
    };
}

uintptr_t ArtImageLibraryExport(std::string_view soname, std::string_view symbol)
{
    const std::lock_guard lock(Process().mutex);
    const auto& images = Process().runtime.images;
    const auto found = images.find(std::string(soname));
    return found == images.end() ? 0 : found->second->image.Export(symbol, true);
}

uintptr_t AndroidRuntimeFunction(std::string_view soname, std::string_view symbol, bool allowUniqueSuffix)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    auto& scope = state.runtime;
    const auto loaded = scope.images.find(std::string(soname));
    if (!scope.read || loaded == scope.images.end())
        throw std::runtime_error("ART library is not loaded");
    auto& library = *loaded->second;
    if (const auto address = library.image.Export(symbol, true))
        return address;
    if (!library.debugSymbols)
        library.debugSymbols = ElfDebugSymbols(scope.read(library.name));
    auto found = library.debugSymbols->find(symbol);
    if (found == library.debugSymbols->end() && allowUniqueSuffix) {
        const auto prefix = std::string(symbol) + ".__uniq.";
        const auto candidate = library.debugSymbols->lower_bound(prefix);
        if (candidate != library.debugSymbols->end() && candidate->first.starts_with(prefix)) {
            const auto next = std::next(candidate);
            if (next == library.debugSymbols->end() || !next->first.starts_with(prefix))
                found = candidate;
        }
    }
    if (found == library.debugSymbols->end() || !found->second.code)
        throw std::runtime_error("missing ART image function: " + std::string(symbol));
    if (found->second.address > UINTPTR_MAX - library.image.LoadBias())
        throw std::runtime_error("invalid ART image function address");
    const auto address = library.image.LoadBias() + found->second.address;
    if (!library.image.ContainsCode(address, static_cast<size_t>(found->second.size)))
        throw std::runtime_error("ART image function is outside executable segments");
    return address;
}

void* LoadAndroidRuntime(const AndroidElfReader& read, bool useJit, ElfImage::Machine machine)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    if (state.translate || hostVm || state.runtime.artRuntime)
        throw std::runtime_error("ART bootstrap requires a fresh process without a host JVM");
    if (machine != ElfImage::Machine::X64 && machine != ElfImage::Machine::Arm64)
        throw std::runtime_error("unsupported ART ELF architecture");
    if (machine == ElfImage::Machine::Arm64 && !useJit)
        throw std::runtime_error("ARM64 ART requires Java JIT enabled");
    if (machine == ElfImage::Machine::X64 && !IsProcessorFeaturePresent(PF_RDWRFSGSBASE_AVAILABLE))
        throw std::runtime_error("ART requires Windows support for x64 FS-base instructions");
    if (!InstallAndroidExceptionHandler())
        throw std::runtime_error("could not register Android ELF exception handler");
    state.machine = machine;
    state.translate = machine == ElfImage::Machine::Arm64;
    if (state.translate)
        std::fprintf(stderr, "[art] ARM64 image selected; translated execution with Java JIT requested\n");
    auto& scope = state.runtime;
    if (scope.read)
        throw std::runtime_error("ART bootstrap was already attempted in this process");
    // Keep Android's floating-point ABI, including x86_64's 80-bit long double.
    scope.imageMath = true;
    scope.imageLog = true;
    scope.artRuntime = true;
    Libraries staged;
    auto staticTlsUsed = state.staticTlsUsed;
    scope.imageUnwind = PrepareImageUnwind(read("libc.so"), staticTlsUsed);
    Library* library;
    try {
        library = &Prepare(scope, "libart.so", read, staged, staticTlsUsed, 0);
        PublishImages(state, scope, staged, staticTlsUsed, scope.imageUnwind.get());
    }
    catch (...) {
        scope.imageUnwind.reset();
        throw;
    }
    scope.read = read;
    scope.globalImages.push_back(library);
    Initialize(*library);
    if (useJit)
        ConfigureArtJit(library->image);
    const auto entry = library->image.Export("JNI_CreateJavaVM", true);
    if (!entry)
        throw std::runtime_error("libart.so has no JNI_CreateJavaVM export");
    if (state.translate)
        return MakeArm64FunctionThunk(library->image, entry, "(LLL)I");
    auto call = std::make_unique<Call>(entry, "(LLL)I", false, true);
    call->scope = &scope;
    auto* thunk = MakeThunk(reinterpret_cast<uintptr_t>(call.get()), WindowsToAndroid);
    state.calls.push_back(std::move(call));
    return thunk;
}

void LoadNativeSystemElf(std::string_view soname, std::span<const uint8_t> bytes, NativeSystemImport interpose)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    if (!IsProcessorFeaturePresent(PF_RDWRFSGSBASE_AVAILABLE))
        throw std::runtime_error("Android system ELF requires Windows support for x64 FS-base instructions");
    for (auto* module = state.nativeSystemImages.load(std::memory_order_acquire); module; module = module->next)
        if (module->name == soname && module->ready.load(std::memory_order_acquire))
            return;
    auto image = std::make_unique<AndroidLoadedImage>(bytes);
    for (const auto& dependency : image->Dependencies()) {
        if (dependency != "libc.so" && dependency != "libm.so" && dependency != "libdl.so" &&
            dependency != "liblog.so" && dependency != "libandroidicu.so" && dependency != "libc++.so") {
            throw std::runtime_error("unsupported Android system ELF dependency: " + dependency);
        }
    }
    image->Relocate([&](std::string_view name) {
        if (interpose)
            if (const auto address = interpose(name))
                return address;
        if (const auto address = Libc(name))
            return address;
        if (const auto address = Libm(name))
            return address;
        if (const auto address = Libdl(name))
            return address;
        if (const auto address = AndroidLogImport(name))
            return address;
        if (const auto address = Libandroidicu(name))
            return address;
        return Libstdcxx(name);
    });
    if (image->IndirectRelocationCount())
        throw std::runtime_error("unsupported Android system ELF indirect relocations");
    const auto initializers = image->Initializers();
    image->Protect();
    if (!InstallAndroidExceptionHandler())
        throw std::runtime_error("could not register Android ELF exception handler");
    // Publish the code range before constructors so the exception handler can
    // recognize Bionic stack-guard loads during initialization.
    auto* module = new NativeSystemImage{std::string(soname), std::move(image),
                                         state.nativeSystemImages.load(std::memory_order_relaxed)};
    state.nativeSystemImages.store(module, std::memory_order_release);
    for (void* initializer : initializers) {
        const Call call(reinterpret_cast<uintptr_t>(initializer), "()V", false, true);
        CallAndroid(&call, nullptr, nullptr);
    }
    module->ready.store(true, std::memory_order_release);
}

uintptr_t NativeSystemElfExport(std::string_view soname, std::string_view symbol)
{
    for (auto* module = Process().nativeSystemImages.load(std::memory_order_acquire); module; module = module->next)
        if (module->name == soname && module->ready.load(std::memory_order_acquire))
            return module->image->Export(symbol, true);
    return 0;
}

void* NativeSystemElfThunk(std::string_view soname, std::string_view symbol, std::string_view signature)
{
    const auto address = NativeSystemElfExport(soname, symbol);
    if (!address)
        throw std::runtime_error(std::string(soname) + ": missing Android system ELF export " + std::string(symbol));
    return MakeNativeAndroidThunk(address, signature);
}

void* MakeNativeAndroidThunk(uintptr_t address, std::string_view signature)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    if (state.translate)
        if (const auto* image = FindArm64Code(address))
            return MakeArm64FunctionThunk(*image, address, signature);
    const auto key = std::to_string(address) + ":raw:" + std::string(signature);
    if (const auto found = state.nativeThunks.find(key); found != state.nativeThunks.end())
        return found->second;
    auto call = std::make_unique<Call>(address, signature, false, true);
    call->scope = NamespaceForCode(address);
    auto* thunk = MakeThunk(reinterpret_cast<uintptr_t>(call.get()), WindowsToAndroid);
    state.calls.push_back(std::move(call));
    state.nativeThunks.emplace(key, thunk);
    return thunk;
}

extern "C" LibraryNamespace* StoneEnterAndroidNamespace(LibraryNamespace* scope) noexcept
{
    return std::exchange(activeNamespace, scope);
}

extern "C" void StoneLeaveAndroidNamespace(LibraryNamespace* previous) noexcept
{
    activeNamespace = previous;
}

} // namespace stone::windows
