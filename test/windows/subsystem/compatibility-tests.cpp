#include "support/test-suite.h"

#include "loader/ElfImage.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "cpu/arm64/Arm64ExecutionProfile.h"
#include "cpu/arm64/Arm64ToX64.h"
#include "cpu/arm64/Arm64BlockJit.h"
#include "abi/TypedAndroidImport.h"
#include "abi/Arm64HostJni.h"
#include "runtime/art/ArtJni.h"
#include "runtime/art/ArtGs.h"
#include "signals/AndroidSignalContext.h"
#include "abi/AndroidLibraryImports.h"
#include "hle/system/stub-libc.h"
#include "stub-liblog.h"
#include "hle/system/stub-libc-properties.h"
#include "hle/system/stub-libc-users.h"
#include "memory/GuestMappings.h"
#include "hle/system/stub-libc-sockets.h"
#include "hle/external/LibraryJniStubs.h"
#include "hle/external/stub-libclang_rt.ubsan_standalone.h"
#include "hle/system/stub-libnativeloader.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <io.h>
#include <iterator>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <optional>
#include <xmmintrin.h>

#include "subsystem/support/native-checks.h"

namespace stone::windows::test {
using namespace checks;

void TestExternal(Suite& suite, bool includeNetwork)
{
    suite.Run("libnativeloader-system-scope-and-error-ownership", [] {
        const auto initialize = Guest<void (*)()>(Libnativeloader("InitializeNativeLoader"), "()V");
        const auto reset = Guest<void (*)()>(Libnativeloader("ResetNativeLoader"), "()V");
        const auto open = Guest<void* (*)(JNIEnv*, int32_t, const char*, jobject, const char*, jstring, bool*, char**)>(
            Libnativeloader("OpenNativeLibrary"), "(LILLLLLL)L");
        const auto close = Guest<bool (*)(void*, bool, char**)>(Libnativeloader("CloseNativeLibrary"), "(LZL)Z");
        const auto release = Guest<void (*)(char*)>(Libnativeloader("NativeLoaderFreeErrorMessage"), "(L)V");
        initialize();
        struct Reset {
            decltype(reset) call;
            ~Reset()
            {
                call();
            }
        } cleanup{reset};
        bool bridged = true;
        char* error = nullptr;
        const auto handle = open(nullptr, 37, "libc.so", nullptr, nullptr, nullptr, &bridged, &error);
        STONE_CHECK(handle && !bridged && !error);
        STONE_CHECK(close(handle, false, &error) && !error);
        STONE_CHECK(!open(nullptr, 37, "libc.so", reinterpret_cast<jobject>(1), nullptr, nullptr, &bridged, &error));
        STONE_CHECK(error && std::strstr(error, "unregistered ART application native class loader"));
        release(error);
    });
    suite.Run("elf-required-and-weak-imports", CheckElfImports);
    suite.Run("arm64-elf-tlsdesc-thread-isolation-and-registers", CheckArm64TlsDescriptor);
    suite.Run("arm64-elf-tlsdesc-external-weak-and-module-identity", CheckArm64ExternalTls);
    suite.Run("arm64-elf-tlsdesc-rejects-invalid-descriptors", CheckArm64InvalidTls);
    suite.Run("arm64-elf-inspection-restores-native-backend", CheckArm64InspectionRestoresBackend);
    suite.Run("arm64-elf-static-tls-and-late-module-publication", CheckArm64StaticTls);
    suite.Run("arm64-elf-static-tls-requires-allocation", CheckArm64StaticTlsRejectedWithoutAllocation);
    suite.Run("arm64-elf-ifunc-and-irelative-execute-through-translator", CheckArm64IndirectRelocations);
    suite.Run("arm64-aapcs64-function-bridge-mixed-spills-and-bounds", CheckArm64FunctionBridge);
    suite.Run("arm64-hot-block-executes-real-guarded-byte-loop", [] {
        auto bytes = MakeArm64TlsElf();
        const uint32_t loop[]{0x39400023, 0x91000421, 0x0b030042, 0xf1000400, 0x54ffff81, 0x2a0203e0, 0xd65f03c0};
        std::memcpy(bytes.data() + 0x1100, loop, sizeof(loop));
        ElfImage image(bytes, ElfImage::Machine::Arm64, true);
        image.Protect(ElfImage::Execution::Translated);
        const auto memory = AllocateArm64GuestMemory(4096, true);
        STONE_CHECK(memory);
        struct Free {
            uintptr_t value;
            ~Free()
            {
                FreeArm64GuestMemory(value);
            }
        } free{memory};
        std::memset(reinterpret_cast<void*>(memory), 3, 4096);
        const Arm64Argument args[]{{'J', 4096}, {'J', memory}};
        const auto before = GetArm64BlockStatistics();
        STONE_CHECK(RunArm64Function(image, image.LoadBias() + 0x1100, 'J', {}, nullptr, nullptr, args) == 12288);
        const auto after = GetArm64BlockStatistics();
        if (Arm64BlockJitEnabled())
            STONE_CHECK(after.instructions > before.instructions && !after.failures);
        else
            STONE_CHECK(after.instructions == before.instructions);
    });
    suite.Run("arm64-block-warmup-counts-survive-batch-boundaries-exceptions-and-thread-exit", [] {
        const auto run = [](bool throwing) {
            auto bytes = MakeArm64TlsElf();
            std::vector<uint32_t> words(125, 0xd503201f);
            words.insert(words.end(), {0x91000400, 0xf100a01f, 0x54000001u | ((uint32_t(-127) & 0x7ffff) << 5)});
            if (throwing)
                words.insert(words.end(), {0xd2800030, 0xd61f0200});
            else
                words.push_back(0xd65f03c0);
            std::memcpy(bytes.data() + 0x1100, words.data(), words.size() * 4);
            ElfImage image(bytes, ElfImage::Machine::Arm64, true);
            image.Protect(ElfImage::Execution::Translated);
            bool failed = false;
            try {
                const auto value =
                    RunArm64Function(image, image.LoadBias() + 0x1100, 'J',
                                     [](uintptr_t target) -> std::string_view {
                                         return target == 1 ? "stone-warmup-test-unsupported-import" : "";
                                     },
                                     nullptr, nullptr, {});
                STONE_CHECK(value == 40);
            }
            catch (const std::runtime_error& error) {
                failed = std::string_view(error.what()).find("stone-warmup-test-unsupported-import") !=
                         std::string_view::npos;
                if (!failed)
                    throw;
            }
            STONE_CHECK(failed == throwing);
        };
        for (bool throwing : {false, true}) {
            const auto before = GetArm64BlockStatistics();
            run(throwing);
            const auto after = GetArm64BlockStatistics();
            STONE_CHECK(after.warmupSkipped - before.warmupSkipped ==
                        (Arm64BlockJitEnabled() ? (throwing ? 5122u : 5121u) : 0u));
            STONE_CHECK(after.executions == before.executions);
        }
        const auto before = GetArm64BlockStatistics();
        std::exception_ptr errors[2];
        std::thread workers[2];
        for (unsigned i = 0; i < 2; ++i)
            workers[i] = std::thread([&, i] {
                try {
                    run(false);
                }
                catch (...) {
                    errors[i] = std::current_exception();
                }
            });
        for (auto& worker : workers)
            worker.join();
        for (const auto& error : errors)
            if (error)
                std::rethrow_exception(error);
        const auto after = GetArm64BlockStatistics();
        STONE_CHECK(after.warmupSkipped - before.warmupSkipped == (Arm64BlockJitEnabled() ? 10242u : 0u));
        STONE_CHECK(after.executions == before.executions);
    });
    suite.Run("arm64-logical-immediate-aligns-real-guest-stack-pointer", [] {
        auto bytes = MakeArm64TlsElf();
        const uint32_t code[]{0x910003e9, 0xd10103ff, 0x91007fe0, 0x927cec1f,
                              0x910003e0, 0xcb090000, 0x9100013f, 0xd65f03c0};
        std::memcpy(bytes.data() + 0x1100, code, sizeof(code));
        ElfImage image(bytes, ElfImage::Machine::Arm64, true);
        image.Protect(ElfImage::Execution::Translated);
        for (unsigned i = 0; i < 128; ++i)
            STONE_CHECK(RunArm64Function(image, image.LoadBias() + 0x1100, 'J', {}, nullptr, nullptr, {}) ==
                        uint64_t(-48));
    });
    suite.Run("arm64-import-dispatch-keeps-guest-branches-cross-images-and-return-validation", [] {
        auto bytes = MakeArm64TlsElf();
        const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
        const uint32_t callee[]{0xd2800540, 0xd65f03c0};
        const uint32_t badReturn[]{0xd280003e, 0xd65f03c0};
        std::memcpy(bytes.data() + 0x1100, caller, sizeof(caller));
        std::memcpy(bytes.data() + 0x1200, callee, sizeof(callee));
        std::memcpy(bytes.data() + 0x1400, badReturn, sizeof(badReturn));
        ElfImage first(bytes, ElfImage::Machine::Arm64, true), second(bytes, ElfImage::Machine::Arm64, true);
        first.Protect(ElfImage::Execution::Translated);
        second.Protect(ElfImage::Execution::Translated);
        static std::array<const ElfImage*, 2> images{};
        static unsigned searches = 0;
        images = {&first, &second};
        searches = 0;
        struct Reset {
            ~Reset()
            {
                images = {};
            }
        } reset;
        Arm64Jni bridge{};
        bridge.findCode = +[](uintptr_t address) -> const ElfImage* {
            ++searches;
            for (const auto* image : images)
                if (image && image->ContainsCode(address, 4))
                    return image;
            return nullptr;
        };
        bridge.findData = +[](uintptr_t, unsigned, bool) -> uint8_t* {
            return nullptr;
        };
        bridge.loadedImages = +[]() -> std::vector<Arm64LoadedImage> {
            return {{"caller.so", images[0]}, {"callee.so", images[1]}};
        };
        const Arm64ImportName names = [&](uintptr_t address) -> std::string_view {
            return address == 1 || address == first.LoadBias() + 0x1200 ? "getpid" : "";
        };
        const auto invoke = [&](uintptr_t target) {
            const Arm64Argument args[]{{'J', target}};
            return RunArm64Function(first, first.LoadBias() + 0x1100, 'J', names, nullptr, &bridge, args);
        };
        STONE_CHECK(invoke(first.LoadBias() + 0x1200) == 42 && searches == 0);
        STONE_CHECK(invoke(second.LoadBias() + 0x1200) == 42 && searches >= 2);
        STONE_CHECK(invoke(1) == GetCurrentProcessId());
        bool rejected = false;
        try {
            RunArm64Function(first, first.LoadBias() + 0x1400, 'J', names, nullptr, &bridge, {});
        }
        catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()).find("return outside executable") != std::string_view::npos;
        }
        STONE_CHECK(rejected);
        const Arm64Argument unknown[]{{'J', 1}};
        rejected = false;
        try {
            RunArm64Function(first, first.LoadBias() + 0x1100, 'J', {}, nullptr, nullptr, unknown);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
    });
    suite.Run("arm64-system-properties-values-callbacks-and-opaque-bounds", CheckArm64Properties);
    suite.Run("arm64-log-callbacks-copy-metadata-and-cross-native-threads", CheckArm64LogCallbacks);
    suite.Run("arm64-signal-set-64-aliases-high-bits-and-bounds", CheckArm64SignalSets);
    TestSignalSets(suite);
    suite.Run("arm64-thread-stack-attributes-guest-stack-and-guard-bounds", CheckArm64ThreadStackAttributes);
    suite.Run("arm64-stack-protection-guards-survive-deep-frame-commit", CheckArm64StackProtection);
    suite.Run("arm64-peer-stack-committed-pages-after-return-permissions-and-retirement",
              CheckArm64SharedCommittedStack);
    suite.Run("arm64-pairwise-long-accumulate-width-sign-alias-and-upper-bits", CheckArm64PairwiseAccumulate);
    suite.Run("arm64-native-loader-system-scope-and-guest-error-ownership", CheckArm64NativeLoader);
    suite.Run("arm64-stack-probes-below-sp-preserve-page-protection", CheckArm64StackProbes);
    suite.Run("arm64-reentrant-user-group-database-layout-and-buffer-bounds", CheckArm64UserDatabase);
    suite.Run("arm64-library-search-path-size-terminator-and-output-bounds", CheckArm64LibrarySearchPath);
    suite.Run("arm64-jit-dual-views-execution-revocation-and-memfd-syscall", CheckArm64MappedCode);
    suite.Run("arm64-cache-type-publication-barriers-and-scheduling-query", CheckArm64CodePublication);
    suite.Run("arm64-futex-compare-requeue-moves-real-waiters", CheckArm64FutexRequeue);
    suite.Run("arm64-memory-fault-siginfo-resume-pc-registers-and-mask", CheckArm64MemoryFaultRecovery);
    suite.Run("arm64-sigwaitinfo-aliases-consume-real-64-bit-signal-queue", CheckArm64SignalWaitInfo);
    suite.Run("arm64-vector-io-real-file-scatter-gather-and-buffer-bounds", CheckArm64VectorIo);
    suite.Run("typed-import-prototypes-mixed-registers-31-arguments-and-jni", CheckTypedImportPrototypes);
    suite.Run("arm64-host-jni-mixed-banks-spills-method-table-and-rejected-targets", CheckArm64HostJni);
    suite.Run("arm64-cfi-image-checker-diagnostics-ownership-and-perror", CheckArm64CfiAndPerror);
    suite.Run("arm64-jni-buffer-ownership-counts-signatures-and-argument-copies", CheckArm64JniBuffers);
    suite.Run("arm64-polynomial-byte-multiply-low-high-and-aliased-registers", CheckArm64PolynomialMultiply);
    suite.Run("arm64-frintm-vector-sizes-aliasing-special-values-and-fpsr", CheckArm64VectorRoundDown);
    suite.Run("arm64-frintn-scalar-vector-halves-aliases-special-values-and-fpsr", [] {
        CheckArm64RoundNearest(false);
    });
    suite.Run("arm64-frinta-scalar-vector-halves-aliases-special-values-and-fpsr", [] {
        CheckArm64RoundNearest(true);
    });
    suite.Run("arm64-nearest-integer-conversions-rounding-limits-aliases-and-fpsr",
              CheckArm64NearestIntegerConversions);
    suite.Run("arm64-saturating-narrow-scalar-vector-halves-aliases-and-sticky-qc", CheckArm64SaturatingNarrow);
    suite.Run("arm64-scalar-ordered-comparisons-signs-aliases-upper-bits-and-flags",
              CheckArm64ScalarOrderedComparisons);
    suite.Run("arm64-high-narrow-add-sub-rounding-halves-overflow-and-aliases", CheckArm64HighNarrowArithmetic);
    suite.Run("arm64-elf-data-write-restoration-readonly-relro-and-code-denial", CheckArm64ElfProtection);
    suite.Run("arm64-floating-string-results-end-pointers-ranges-and-memory-ownership", CheckArm64FloatingStrings);
    if (includeNetwork)
        suite.Run("arm64-reentrant-strings-fortified-stdio-and-resolver-buffer-layouts", CheckArm64StringStdioResolver);
    suite.Run("arm64-getrandom-flags-length-errors-and-writable-guest-buffers", CheckArm64RandomBytes);
    if (includeNetwork)
        suite.Run("arm64-network-resolver-lp64-numeric-loopback-and-guest-buffer-guards", CheckArm64NetworkResolver);
    else
        std::cout << "SKIP network resolver and resolver-buffer groups (external-local)\n";
    suite.Run("arm64-thread-priority-own-task-aliases-errors-and-windows-scheduling", CheckArm64ThreadPriorities);
    suite.Run("arm64-mallinfo-indirect-return-live-and-peak-guest-allocation-bytes", CheckArm64AllocationStatistics);
    suite.Run("arm64-elf-code-read-permissions-preserve-execution-and-deny-writes", CheckArm64CodeReadProtection);
    suite.Run("arm64-syslog-priorities-mask-varargs-truncation-and-guest-tags", CheckArm64Syslog);
    suite.Run("arm64-log-assert-formats-reports-aborter-and-never-returns", CheckArm64LogAssert);
    suite.Run("arm64-statvfs-real-volume-lp64-reserved-fields-fds-and-buffer-ownership", CheckArm64VolumeStatistics);
    suite.Run("arm64-setxattr-typed-path-fd-errors-and-readable-guest-inputs", CheckArm64SetExtendedAttributes);
    suite.Run("arm64-advisory-mmap-hint-stays-near-low-addresses-without-clobbering-host-pages",
              CheckArm64AdvisoryMappingHint);
    suite.Run("arm64-data-mapping-lookup-trims-permissions-publication-reuse-and-abi-ownership",
              CheckArm64DataMappingLookup);
    suite.Run("arm64-fcntl-full-width-lock-pointers-range-permissions-and-real-exclusion", CheckArm64Fcntl);
    suite.Run("typed-import-target-identity-lifetime-and-native-exceptions", CheckTypedImportIdentityAndLifetime);
    suite.Run("elf-code-range-respects-function-symbol-boundaries", CheckElfCodeBoundaries);
    suite.Run("elf-native-sigsegv-context-recovery", CheckNativeSignalRecovery);
    suite.Run("elf-inspection-skips-initializers", CheckElfInspection);
    suite.Run("art-inspection-uses-runtime-native-loader-policy", CheckArtInspectionBindings);
    suite.Run("art-gs-thread-access-rewrite", [] {
        CheckArtGsRewrite();
    });
    suite.Run("art-gs-dynamic-code-publication-and-reuse", CheckDynamicArtGs);
    suite.Run("art-gs-dynamic-packed-switch-and-literal-islands", CheckDynamicArtSwitch);
    suite.Run("art-gs-high-load-preserves-scratch-and-carry", [] {
        CheckArtGsLoadRegisters(0);
    });
    suite.Run("art-gs-narrow-load-zero-extends", [] {
        CheckArtGsLoadRegisters(1);
    });
    suite.Run("art-gs-load-preserves-aliased-address", [] {
        CheckArtGsLoadRegisters(2);
    });
    suite.Run("art-gs-repeated-load-loop", [] {
        CheckArtGsLoadRegisters(3);
    });
    suite.Run("art-gs-load-rejects-stack-pointer", [] {
        CheckArtGsLoadRegisters(4);
    });
    suite.Run("art-gs-skips-data-in-code-sections", [] {
        CheckArtGsRewrite(true);
    });
    suite.Run("art-gs-wraps-32-bit-address", [] {
        CheckArtGsRewrite(true, true);
    });
    suite.Run("art-gs-restores-fs-after-windows-transition", [] {
        CheckArtTlsRecovery(false);
    });
    suite.Run("art-gs-restores-fs-with-narrow-address", [] {
        CheckArtTlsRecovery(true);
    });
    suite.Run("elf-fs-restores-thread-pointer", [] {
        CheckFsTlsRecovery(0, false);
    });
    suite.Run("elf-fs-restores-stack-guard", [] {
        CheckFsTlsRecovery(40, false);
    });
    suite.Run("elf-fs-restores-art-thread", [] {
        CheckFsTlsRecovery(56, false);
    });
    suite.Run("elf-fs-restores-high-register", [] {
        CheckFsTlsRecovery(40, true);
    });
    suite.Run("art-gs-call-without-exception-dispatch", [] {
        CheckArtGsControl(false, false);
    });
    suite.Run("art-gs-tail-call-without-exception-dispatch", [] {
        CheckArtGsControl(true, false);
    });
    suite.Run("art-gs-narrow-call-preserves-registers-and-flags", [] {
        CheckArtGsControl(false, true);
    });
    suite.Run("art-gs-narrow-tail-call-preserves-registers-and-flags", [] {
        CheckArtGsControl(true, true);
    });
    suite.Run("libmmkv-2-4-0", [] {
        STONE_CHECK(MmkvJniPathArguments("jniInitialize", "(Ljava/lang/String;Ljava/lang/String;IZZJ)V") == 3);
        STONE_CHECK(MmkvJniPathArguments("getMMKVWithID",
                                         "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;JZIIZII)J") ==
                    (1u << 3));
    });
    suite.Run("libmmkv-1-3-14-unmapped-jni", [] {
        STONE_CHECK(MmkvJniPathArguments("jniInitialize", "(Ljava/lang/String;Ljava/lang/String;IZ)V") == 0);
        STONE_CHECK(
            MmkvJniPathArguments("getMMKVWithID", "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;J)J") == 0);
    });
    suite.Run("libmatrix-jnihook-placeholder-method-abi", [] {
        constexpr auto method = "initJniHookByPlaceHolderMethod";
        const auto target = MatrixJniHookFallback(method, "(Ljava/lang/reflect/Method;)I");
        const auto call = Host<jint(JNICALL*)(JNIEnv*, jclass, jobject)>(reinterpret_cast<uintptr_t>(target));
        STONE_CHECK(call(nullptr, nullptr, nullptr) == JNI_ERR);
        STONE_CHECK(MatrixJniHookFallback(method, "()I") == nullptr);
    });
    suite.Run("libcso-native-initialize-abi", [] {
        const auto paths = CsoJniPathArguments(
            "nativeInitialize",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;Z)V");
        STONE_CHECK(paths.arguments == 0xf && paths.arrayArguments == 0xd0);
        const auto different = CsoJniPathArguments("nativeInitialize", "(Ljava/lang/String;)V");
        STONE_CHECK(different.arguments == 0 && different.arrayArguments == 0);
    });
    if (includeNetwork) {
        suite.Run("libwebviewchromium-145-0-7632-218-jni", CheckChromiumJni);
        suite.Run("libwebviewchromium-aw-draw-fn-v3", CheckDrawTable);
    }
    else
        std::cout << "SKIP WebView JNI and draw ABI groups (external-local)\n";
    suite.Run("libclang-rt-ubsan-unknown-build", CheckUnknownUbsan);
}
}
