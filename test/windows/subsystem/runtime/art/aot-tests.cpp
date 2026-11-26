#include "runtime/art/ArtRuntime.h"
#include "runtime/art/ArtJni.h"
#include "runtime/art/ArtJit.h"
#include "cpu/arm64/Arm64ExecutionProfile.h"
#include "cpu/arm64/Arm64BlockJit.h"
#include "runtime/art/ArtAot.h"
#include "filesystem/AndroidSystemFiles.h"
#include <windows.h>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
uintptr_t target;
uint8_t original;
std::atomic<unsigned> hits;

LONG CALLBACK Observe(EXCEPTION_POINTERS* exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress) != target)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD previous;
    if (!VirtualProtect(reinterpret_cast<void*>(target), 1, PAGE_EXECUTE_READWRITE, &previous))
        return EXCEPTION_CONTINUE_SEARCH;
    *reinterpret_cast<uint8_t*>(target) = original;
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 1);
    DWORD ignored;
    VirtualProtect(reinterpret_cast<void*>(target), 1, previous, &ignored);
    exception->ContextRecord->Rip = target;
    ++hits;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

int wmain(int argc, wchar_t* argv[])
{
    using namespace stone::windows;
    const bool cache = argc == 5 && std::wstring_view(argv[3]) == L"--application-cache";
    const bool jit = argc == 4 && std::wstring_view(argv[3]) == L"--enable-jit";
    if (argc != 3 && !jit && !cache) {
        std::cerr
            << "Usage: stone-art-aot-tests <prepared-image> <compiled-data-directory> [--enable-jit | --application-cache <compiler>]\n";
        return 2;
    }
    try {
        ArtRuntimeOptions options;
        options.systemDirectory = argv[1];
        const bool arm64 = ArtImageArchitecture(options.systemDirectory) == ElfImage::Machine::Arm64;
        options.dataDirectory = argv[2];
        options.classPath = cache ? "/data/probe.apk" : "/data/probe.dex";
        if (cache) {
            SetAndroidSystemDirectory(options.systemDirectory, options.dataDirectory);
            ConfigureArtAot(
                argv[4], options.systemDirectory, options.dataDirectory,
                "/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:"
                "/apex/com.android.art/javalib/okhttp.jar:/apex/com.android.art/javalib/bouncycastle.jar:"
                "/apex/com.android.art/javalib/apache-xml.jar:/apex/com.android.i18n/javalib/core-icu4j.jar:"
                "/apex/com.android.conscrypt/javalib/conscrypt.jar");
            PrepareArtAot("/data/probe.apk");
        }
        options.useJit = jit;
        if (jit) {
            options.classPath += ":/data/jit.dex";
            options.vmOptions.push_back("-Xjitwarmupthreshold:100");
            options.vmOptions.push_back("-Xjitthreshold:1000");
        }
        options.vmOptions.push_back("-verbose:oat");
        ArtRuntime runtime(options);
        Check(runtime.JitEnabled() == (jit || arm64) && !GetModuleHandleW(L"jvm.dll"),
              "Unexpected JIT mode or Windows JVM in AOT test");
        auto* env = runtime.GuestEnvironment();
        const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/AotProbe");
        Check(type != nullptr, "Could not load AOT probe");
        const auto method = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "run", "(I)I");
        Check(method != nullptr, "Could not find AOT probe method");
        const std::string oat =
            cache   ? std::string("/data/dalvik-cache/") + (arm64 ? "arm64" : "x86_64") + "/data@probe.apk@classes.dex"
            : arm64 ? "/data/oat/arm64/probe.odex"
                    : "/data/oat/x86_64/probe.odex";
        target = AndroidRuntimeFunction(oat, "int com.trebledot.stone.probe.AotProbe.run(int)");
        auto* profile = arm64 ? CurrentArm64ExecutionProfile() : nullptr;
        if (arm64) {
            Check(profile && profile->instructionProfiling && !Arm64BlockJitEnabled(),
                  "ARM64 AOT observation requires instruction profiling and disabled native blocks");
            profile->observedAddress = target;
            profile->observedInstructions = 0;
        }
        DWORD previous;
        PVOID observer = nullptr;
        if (!arm64) {
            Check(VirtualProtect(reinterpret_cast<void*>(target), 1, PAGE_EXECUTE_READWRITE, &previous),
                  "Could not observe AOT method");
            original = *reinterpret_cast<uint8_t*>(target);
            Check(original != 0xcc, "AOT entry point already contains a breakpoint");
            observer = AddVectoredExceptionHandler(1, Observe);
            Check(observer != nullptr, "Could not register AOT observer");
            *reinterpret_cast<uint8_t*>(target) = 0xcc;
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 1);
            DWORD ignored;
            VirtualProtect(reinterpret_cast<void*>(target), 1, previous, &ignored);
        }
        jvalue argument{};
        argument.i = 6;
        const auto result = ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, method, &argument);
        if (observer)
            RemoveVectoredExceptionHandler(observer);
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
        Check(!ArtJni<&JNINativeInterface_::ExceptionCheck>(env) && result == 42, "AOT probe did not return 42");
        if (arm64) {
            Check(profile->observedInstructions > 0, "Probe did not fetch the persisted ARM64 AOT method");
            std::cout << "ARM64 persisted OAT entry fetches=" << profile->observedInstructions << '\n';
            profile->observedAddress = 0;
        }
        else
            Check(hits == 1, "Probe did not execute the persisted AOT method");
        if (jit) {
            const auto fallback =
                ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/JitFallbackProbe");
            Check(fallback != nullptr, "Could not load the DEX without AOT artifacts");
            const auto hot = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, fallback, "hot", "(I)I");
            Check(hot != nullptr, "Could not find the JIT fallback method");
            constexpr auto name = "com.trebledot.stone.probe.JitFallbackProbe.hot";
            Check(!ArtJitMethodCompiled(name), "JIT fallback method compiled before the warmup");
            const auto deadline = GetTickCount64() + 15000;
            while (!ArtJitMethodCompiled(name) && GetTickCount64() < deadline) {
                for (unsigned i = 0; i < 256; ++i) {
                    Check(ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, fallback, hot, &argument) == 42 &&
                              !ArtJni<&JNINativeInterface_::ExceptionCheck>(env),
                          "JIT fallback returned an incorrect result");
                }
                Sleep(1);
            }
            Check(ArtJitMethodCompiled(name), "The DEX without AOT artifacts did not trigger JIT compilation");
            for (unsigned i = 0; i < 256; ++i)
                Check(ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, fallback, hot, &argument) == 42 &&
                          !ArtJni<&JNINativeInterface_::ExceptionCheck>(env),
                      "Published JIT method returned an incorrect result");
            std::cout << "JIT fallback compiled: methods=" << GetArtJitStatistics().methods << '\n';
        }
        else if (!arm64)
            Check(GetArtJitStatistics().methods == 0, "JIT published code while disabled");
        Check(runtime.Shutdown() == JNI_OK, "AOT runtime shutdown failed");
        std::cout << "PASS: persisted " << (arm64 ? "ARM64" : "x64") << " AOT code executed "
                  << (jit     ? "alongside JIT"
                      : arm64 ? "with automatic Java JIT"
                              : "with JIT disabled")
                  << "; result=42, thread, GC, exception and shutdown\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
