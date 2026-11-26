#include "cpu/arm64/Arm64ExecutionSession.h"
#include "ArtRuntime.h"
#include "ArtAot.h"
#include "../../../host/WindowsPath.h"
#include "../../loader/AndroidSystemElf.h"
#include "../../filesystem/AndroidSystemFiles.h"
#include "../../loader/ElfImage.h"
#include "../../cpu/arm64/Arm64Initializer.h"
#include "../../cpu/arm64/Arm64BlockJit.h"
#include "../../cpu/arm64/cache/Arm64BlockCache.h"
#include "../../cpu/arm64/Arm64ExecutionProfile.h"
#include "hle/system/stub-libc-properties.h"

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cstring>
#include <windows.h>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace stone::windows {
namespace {
void Environment(const wchar_t* name, const wchar_t* value)
{
    if (_wputenv_s(name, value))
        throw std::runtime_error("could not configure the ART environment");
}

ElfImage::Machine ImageMachine(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open the image ART architecture header");
    std::array<uint8_t, 20> header{};
    constexpr uint8_t ident[]{0x7f, 'E', 'L', 'F', 2, 1, 1};
    if (!input.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        std::memcmp(header.data(), ident, sizeof(ident)) || header[16] != 3 || header[17] != 0)
        throw std::runtime_error("ART requires a little-endian ELF64 shared library");
    const auto machine = static_cast<ElfImage::Machine>(header[18] | (uint16_t{header[19]} << 8));
    if (machine != ElfImage::Machine::X64 && machine != ElfImage::Machine::Arm64)
        throw std::runtime_error("ART image architecture is unsupported");
    return machine;
}

AndroidSystemPropertyTable ReadProperties(const std::filesystem::path& file, ElfImage::Machine machine)
{
    if (std::filesystem::file_size(file) > 8 * 1024 * 1024)
        throw std::runtime_error("the Android system property file is too large");
    std::ifstream input(file);
    if (!input)
        throw std::runtime_error("could not read Android system properties");
    AndroidSystemPropertyTable values;
    const auto trim = [](std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == text.npos)
            return std::string_view{};
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    };
    for (std::string line; std::getline(input, line);) {
        const auto text = trim(line);
        if (text.empty() || text.front() == '#')
            continue;
        const auto separator = text.find('=');
        if (separator == text.npos)
            continue;
        const auto name = trim(text.substr(0, separator));
        const auto value = trim(text.substr(separator + 1));
        if (name.empty() || name.find('\0') != name.npos || value.find('\0') != value.npos)
            throw std::runtime_error("invalid Android system property");
        values.insert_or_assign(std::string(name), std::string(value));
    }
    if (!input.eof())
        throw std::runtime_error("could not finish reading Android system properties");
    // Android init normally derives these public aliases from partition properties.
    for (const auto* field : {"sdk", "release", "codename", "incremental", "security_patch"}) {
        const auto key = std::string("ro.build.version.") + field;
        const auto source = values.find(std::string("ro.system.build.version.") + field);
        if (!values.contains(key) && source != values.end())
            values.emplace(key, source->second);
    }
    for (const auto* field : {"brand", "device", "manufacturer", "model", "name"}) {
        const auto key = std::string("ro.product.") + field;
        const auto source = values.find(std::string("ro.product.system.") + field);
        if (!values.contains(key) && source != values.end())
            values.emplace(key, source->second);
    }
    // Advertise only the architecture selected from the real ART ELF header.
    const auto abi = machine == ElfImage::Machine::Arm64 ? "arm64-v8a" : "x86_64";
    values.insert_or_assign("ro.product.cpu.abi", abi);
    values.insert_or_assign("ro.product.cpu.abilist", abi);
    values.insert_or_assign("ro.product.cpu.abilist64", abi);
    values.insert_or_assign("ro.product.cpu.abilist32", "");
    // The Windows memory-file backend provides memfd and sealing. There is no
    // Linux ashmem device; direct framework startup must select the memfd path.
    values.insert_or_assign("sys.use_memfd", "true");
    // libbinder can run without a system service-manager process. Stone supplies
    // its own Java services and has no kernel Binder driver to wait on.
    values.insert_or_assign("servicemanager.installed", "false");
    // Software frames use AOSP's Handler-based Choreographer clock. There is
    // no SurfaceFlinger display-event connection in the Windows host yet.
    values.insert_or_assign("debug.choreographer.vsync", "false");
    return values;
}

class Arm64BootArguments {
public:
    std::array<uintptr_t, 2>* output;
    JavaVMInitArgs* input;

    explicit Arm64BootArguments(const JavaVMInitArgs& original)
    {
        try {
            output = static_cast<std::array<uintptr_t, 2>*>(Allocate(sizeof(*output)));
            auto* options = static_cast<JavaVMOption*>(Allocate(original.nOptions * sizeof(JavaVMOption)));
            for (jint index = 0; index < original.nOptions; ++index) {
                const auto* value = original.options[index].optionString;
                const auto length = std::strlen(value) + 1;
                auto* text = static_cast<char*>(Allocate(length));
                std::memcpy(text, value, length);
                options[index] = {text, nullptr};
            }
            input = static_cast<JavaVMInitArgs*>(Allocate(sizeof(*input)));
            *input = original;
            input->options = options;
        }
        catch (...) {
            Clear();
            throw;
        }
    }

    ~Arm64BootArguments()
    {
        Clear();
    }

private:
    std::vector<uintptr_t> blocks;

    void* Allocate(size_t size)
    {
        const auto address = AllocateArm64GuestMemory(size, true);
        if (!address)
            throw std::bad_alloc{};
        try {
            blocks.push_back(address);
        }
        catch (...) {
            FreeArm64GuestMemory(address);
            throw;
        }
        auto* memory = reinterpret_cast<void*>(address);
        std::memset(memory, 0, size);
        return memory;
    }

    void Clear()
    {
        for (const auto address : blocks)
            FreeArm64GuestMemory(address);
        blocks.clear();
    }
};
}

ElfImage::Machine ArtImageArchitecture(const std::filesystem::path& directory)
{
    return ImageMachine(ExtendedWindowsPath(directory / L"lib64/libart.so").make_preferred());
}

AndroidElfReader ArtRuntimeLibraries(const std::filesystem::path& directory)
{
    namespace fs = std::filesystem;
    const auto root = ExtendedWindowsPath(fs::canonical(directory));
    return [root](const std::string& name) {
        if (name.empty() || name.find_first_of("/\\:") != name.npos)
            throw std::runtime_error("invalid ART library name");
        const auto path = root / (name == "dex2oat64" ? "bin" : "lib64") / name;
        if (!fs::exists(path))
            return std::vector<uint8_t>{};
        const auto size = fs::file_size(path);
        if (size > 256 * 1024 * 1024)
            throw std::runtime_error("ART library is too large: " + name);
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error("could not read ART library: " + name);
        return bytes;
    };
}

int RunArtDex2Oat(const std::filesystem::path& systemDirectory, const std::filesystem::path& dataDirectory,
                  std::vector<std::string> arguments)
{
    namespace fs = std::filesystem;
    const auto root = ExtendedWindowsPath(fs::canonical(systemDirectory));
    const auto machine = ArtImageArchitecture(root);
    const auto data = fs::absolute(dataDirectory);
    fs::create_directories(data);
    SetAndroidSystemDirectory(root, data);
    SetAndroidSystemProperties(ReadProperties((root / "system/build.prop").make_preferred(), machine));
    fs::current_path(data);
    Environment(L"ANDROID_ROOT", L"/system");
    Environment(L"ANDROID_ART_ROOT", L"/apex/com.android.art");
    Environment(L"ANDROID_I18N_ROOT", L"/apex/com.android.i18n");
    Environment(L"ANDROID_CONSCRYPT_ROOT", L"/apex/com.android.conscrypt");
    Environment(L"ANDROID_TZDATA_ROOT", L"/apex/com.android.tzdata");
    Environment(L"ANDROID_DATA", L"/data");
    LoadAndroidRuntime(ArtRuntimeLibraries(root), machine == ElfImage::Machine::Arm64, machine);
    const auto main = reinterpret_cast<int (*)(int, char**)>(LoadArtDex2Oat());
    arguments.insert(arguments.begin(), "/apex/com.android.art/bin/dex2oat64");
    if (machine == ElfImage::Machine::Arm64) {
        // argv and every pointed-to string must be mapped guest memory. A host
        // vector's backing storage is not readable by translated instructions.
        struct GuestArguments {
            std::vector<uintptr_t> blocks;

            ~GuestArguments()
            {
                for (auto block : blocks)
                    FreeArm64GuestMemory(block);
            }

            void* Allocate(size_t bytes)
            {
                const auto block = AllocateArm64GuestMemory(bytes, true);
                if (!block)
                    throw std::bad_alloc{};
                try {
                    blocks.push_back(block);
                }
                catch (...) {
                    FreeArm64GuestMemory(block);
                    throw;
                }
                return reinterpret_cast<void*>(block);
            }
        } guest;

        auto** argv = static_cast<char**>(guest.Allocate((arguments.size() + 1) * sizeof(char*)));
        for (size_t i = 0; i < arguments.size(); ++i) {
            argv[i] = static_cast<char*>(guest.Allocate(arguments[i].size() + 1));
            std::memcpy(argv[i], arguments[i].c_str(), arguments[i].size() + 1);
        }
        argv[arguments.size()] = nullptr;
        return main(static_cast<int>(arguments.size()), argv);
    }
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    return main(static_cast<int>(arguments.size()), argv.data());
}

ArtRuntime::ArtRuntime(const ArtRuntimeOptions& options)
{
    namespace fs = std::filesystem;
    const auto root = ExtendedWindowsPath(fs::canonical(options.systemDirectory));
    const auto data = options.dataDirectory.empty() ? root / L"data" : fs::absolute(options.dataDirectory);
    fs::create_directories(data);
    machine_ = ArtImageArchitecture(root);
    wchar_t diskCache[2]{};
    if (machine_ == ElfImage::Machine::Arm64 && Arm64BlockJitEnabled() &&
        GetEnvironmentVariableW(L"STONE_ARM64_DISK_CACHE", diskCache, 2) == 1 && diskCache[0] == L'1') {
        if (!ConfigureArm64BlockDiskCache((data / "code_cache/stone-arm64").make_preferred()))
            std::fprintf(stderr, "[arm64-cache] persistence unavailable; continuing with in-memory translation\n");
    }
    jit_ = options.useJit || machine_ == ElfImage::Machine::Arm64;
    SetAndroidSystemDirectory(root, data);
    SetAndroidSystemProperties(ReadProperties((root / "system/build.prop").make_preferred(), machine_));
    const auto read = ArtRuntimeLibraries(root);
    fs::current_path(data);
    Environment(L"ANDROID_ROOT", L"/system");
    Environment(L"ANDROID_ART_ROOT", L"/apex/com.android.art");
    Environment(L"ANDROID_I18N_ROOT", L"/apex/com.android.i18n");
    Environment(L"ANDROID_CONSCRYPT_ROOT", L"/apex/com.android.conscrypt");
    Environment(L"ANDROID_TZDATA_ROOT", L"/apex/com.android.tzdata");
    Environment(L"ANDROID_DATA", L"/data");
    std::string boot = "-Xbootclasspath:/apex/com.android.art/javalib/core-oj.jar:"
                       "/apex/com.android.art/javalib/core-libart.jar:"
                       "/apex/com.android.art/javalib/okhttp.jar:"
                       "/apex/com.android.art/javalib/bouncycastle.jar:"
                       "/apex/com.android.art/javalib/apache-xml.jar:"
                       "/apex/com.android.i18n/javalib/core-icu4j.jar:"
                       "/apex/com.android.conscrypt/javalib/conscrypt.jar";
    if (options.useImageBootClassPath) {
        std::ifstream paths((root / "system/etc/stone-bootclasspath.list").make_preferred());
        if (!paths)
            throw std::runtime_error(
                "the prepared image has no boot class path; prepare it again with stone-image-tools");
        boot = "-Xbootclasspath";
        std::set<std::string> seen;
        for (std::string path; std::getline(paths, path);) {
            if (!path.empty() && path.back() == '\r')
                path.pop_back();
            const fs::path relative = path.empty() ? fs::path{} : fs::path(path.substr(1));
            if (seen.size() >= 256 || (!path.starts_with("/system/") && !path.starts_with("/apex/")) ||
                path.find_first_of("\\:\0", 0, 3) != path.npos || path.find("//") != path.npos ||
                !path.ends_with(".jar") || relative.lexically_normal() != relative || !seen.insert(path).second ||
                !fs::is_regular_file((root / relative).make_preferred()))
                throw std::runtime_error("invalid prepared image boot class path: " + path);
            boot += ':' + path;
        }
        if (!paths.eof() || seen.empty())
            throw std::runtime_error("could not read prepared image boot class path");
    }
    for (const auto& path : options.bootClassPath)
        boot += ':' + path;
    if (!options.aotCompiler.empty())
        ConfigureArtAot(options.aotCompiler, root, data, boot.substr(std::string_view("-Xbootclasspath:").size()));
    std::vector<std::string> flags{
        jit_ ? "-Xusejit:true" : "-Xusejit:false", "-Xnoimage-dex2oat",
        "-Ximage:/system/nonexistent.art",         std::move(boot),
        "-Djava.class.path=" + options.classPath,  "-Duser.dir=/data",
    };
    if (jit_) {
        flags.emplace_back("-Xcompiler-option");
        flags.emplace_back("--generate-mini-debug-info");
        if (machine_ == ElfImage::Machine::Arm64) {
            // The interpreter-only backend benefits from early compilation.
            // Native blocks reduce that cost; avoid flooding ART's compilation
            // queue with cold framework methods. Keep explicit caller thresholds.
            const bool blocks = Arm64BlockJitEnabled();
            for (const auto& [prefix, value] :
                 {std::pair{"-Xjitwarmupthreshold:", blocks ? "-Xjitwarmupthreshold:1000" : "-Xjitwarmupthreshold:100"},
                  std::pair{"-Xjitthreshold:", blocks ? "-Xjitthreshold:10000" : "-Xjitthreshold:1000"}}) {
                if (std::none_of(options.vmOptions.begin(), options.vmOptions.end(), [&](const std::string& option) {
                        return option.starts_with(prefix);
                    }))
                    flags.emplace_back(value);
            }
        }
    }
    flags.insert(flags.end(), options.vmOptions.begin(), options.vmOptions.end());
    wchar_t diagnostics[2]{};
    if (GetEnvironmentVariableW(L"STONE_ART_DIAGNOSTICS", diagnostics, 2) == 1 && diagnostics[0] == L'1') {
        flags.emplace_back("-Dstone.diagnostics=true");
        if (machine_ == ElfImage::Machine::Arm64)
            for (const auto& flag : flags)
                if (flag.starts_with("-Xjitwarmupthreshold:") || flag.starts_with("-Xjitthreshold:"))
                    std::fprintf(stderr, "[art] ARM64 JIT option %s\n", flag.c_str());
    }
    std::vector<JavaVMOption> arguments;
    for (auto& flag : flags)
        arguments.push_back({flag.data(), nullptr});
    JavaVMInitArgs args{JNI_VERSION_1_6, static_cast<jint>(arguments.size()), arguments.data(), JNI_FALSE};
    const auto create = reinterpret_cast<jint (*)(JavaVM**, void**, void*)>(LoadAndroidRuntime(read, jit_, machine_));
    jint result;
    if (machine_ == ElfImage::Machine::Arm64) {
        Arm64BootArguments guest(args);
        result = create(reinterpret_cast<JavaVM**>(guest.output->data()),
                        reinterpret_cast<void**>(guest.output->data() + 1), guest.input);
        if (result == JNI_OK) {
            vm_ = reinterpret_cast<JavaVM*>((*guest.output)[0]);
            environment_ = reinterpret_cast<JNIEnv*>((*guest.output)[1]);
        }
    }
    else
        result = create(&vm_, reinterpret_cast<void**>(&environment_), &args);
    if (result != JNI_OK || !vm_ || !environment_)
        throw std::runtime_error("could not start Android ART");
    destroy_ = reinterpret_cast<jint (*)(JavaVM*)>(
        MakeNativeAndroidThunk(reinterpret_cast<uintptr_t>(vm_->functions->DestroyJavaVM), "(L)I"));
    std::fprintf(stderr, "[art] JNI_CreateJavaVM ready\n");
}

ArtRuntime::~ArtRuntime()
{
    if (vm_)
        Shutdown();
}

int ArtRuntime::Shutdown()
{
    if (!vm_)
        return JNI_OK;
    const int result = destroy_(vm_);
    if (result == JNI_OK) {
        vm_ = nullptr;
        environment_ = nullptr;
        // Windows invokes main-thread FLS cleanup after static C++ objects have
        // been destroyed. Drain remaining native library TLS while the pthread
        // key registry and host adapters are still alive.
        ElfImage::RunThreadDestructors();
        if (machine_ == ElfImage::Machine::Arm64)
            FinishArm64ExecutionSession();
    }
    return result;
}
}
