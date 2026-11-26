#pragma once

#include "ElfImage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace stone::windows {
using NativeSystemImport = uintptr_t (*)(std::string_view);
void LoadNativeSystemElf(std::string_view soname, std::span<const uint8_t> bytes,
                         NativeSystemImport interpose = nullptr);
uintptr_t NativeSystemElfExport(std::string_view soname, std::string_view symbol);
void* NativeSystemElfThunk(std::string_view soname, std::string_view symbol, std::string_view signature);
void* MakeNativeAndroidThunk(uintptr_t address, std::string_view signature);
// ELF owners and cached entries live for the process. This is a scalar,
// fixed-signature bridge; pointer arguments still obey guest memory bounds.
void* MakeArm64FunctionThunk(const ElfImage& image, uintptr_t address, std::string_view signature);

using AndroidElfReader = std::function<std::vector<uint8_t>(const std::string&)>;

struct AndroidElfLinkReport {
    size_t libraries;
    size_t indirectRelocations;
    std::vector<std::string> missingImports;

    struct Binding {
        std::string consumer, symbol, provider;
        bool hostBridge;
    };

    std::vector<Binding> bindings;
};

struct AndroidStubExports {
    std::string library;
    bool imagePreferred;
    std::vector<std::string> overlappingExports;
    bool imageAvailable = true;
};

// Same-named exports are candidates for review, not proof that the image can
// replace a Windows bridge. No constructors, IFUNC resolvers or JNI are run.
std::vector<AndroidStubExports> InspectAndroidStubExports(const AndroidElfReader& read);

// Resolve relocations for the selected ELF machine without publishing the
// images or calling guest code. A running backend cannot change architecture.
AndroidElfLinkReport InspectAndroidElf(std::string_view soname, std::string_view entry, const AndroidElfReader& read,
                                       bool collectBindings = false,
                                       ElfImage::Machine machine = ElfImage::Machine::X64);
void* LoadAndroidRuntime(const AndroidElfReader& read, bool useJit = false,
                         ElfImage::Machine machine = ElfImage::Machine::X64);
// Dedicated compiler process only; calls the selected image's dex2oat main.
void* LoadArtDex2Oat();
// Load an image library even when its SONAME also has host-backed NDK imports.
// The library stays in ART's system scope; its native handles must not be mixed
// with Stone-owned handles from unrelated host bridges.
void LoadArtImageLibrary(std::string_view soname);
void SetArtApplicationLibraries(const std::filesystem::path& directory, const std::filesystem::path& dataDirectory,
                                std::string_view guestData);
uintptr_t ArtImageLibraryExport(std::string_view soname, std::string_view symbol);
uintptr_t AndroidRuntimeFunction(std::string_view soname, std::string_view symbol, bool allowUniqueSuffix = false);
}
