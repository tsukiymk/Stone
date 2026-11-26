#pragma once

#include <jni.h>
#include <filesystem>
#include <string>
#include <vector>
#include "../../loader/AndroidSystemElf.h"

namespace stone::windows {
ElfImage::Machine ArtImageArchitecture(const std::filesystem::path& directory);
AndroidElfReader ArtRuntimeLibraries(const std::filesystem::path& directory);
int RunArtDex2Oat(const std::filesystem::path& systemDirectory, const std::filesystem::path& dataDirectory,
                  std::vector<std::string> arguments);

struct ArtRuntimeOptions {
    std::filesystem::path systemDirectory;
    std::string classPath;
    std::vector<std::string> vmOptions;
    std::vector<std::string> bootClassPath;
    bool useImageBootClassPath = false;
    std::filesystem::path dataDirectory;
    bool useJit = false;
    std::filesystem::path aotCompiler;
};

// The returned JNI tables use the selected guest ABI. Call their functions through
// MakeNativeAndroidThunk; they must never be invoked as Win64 JNI function pointers.
class ArtRuntime {
public:
    explicit ArtRuntime(const ArtRuntimeOptions& options);
    ~ArtRuntime();
    ArtRuntime(const ArtRuntime&) = delete;
    ArtRuntime& operator=(const ArtRuntime&) = delete;

    JNIEnv* GuestEnvironment() const
    {
        return environment_;
    }

    JavaVM* GuestVm() const
    {
        return vm_;
    }

    ElfImage::Machine Architecture() const
    {
        return machine_;
    }

    bool JitEnabled() const
    {
        return jit_;
    }

    int Shutdown();

private:
    JavaVM* vm_ = nullptr;
    JNIEnv* environment_ = nullptr;
    jint (*destroy_)(JavaVM*) = nullptr;
    ElfImage::Machine machine_ = ElfImage::Machine::X64;
    bool jit_ = false;
};
}
