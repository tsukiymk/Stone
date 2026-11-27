#include "support/test-suite.h"
#include "abi/AndroidLibraryImports.h"
#include "../../../src/windows/client/BrokerClient.h"

#include "cpu/arm64/Arm64Initializer.h"
#include "cpu/arm64/Arm64BlockJit.h"
#include "loader/ElfImage.h"

#include <windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <vector>

namespace stone::windows::test {
void Suite::Report(std::string_view module, const char* error) const
{
    auto& stream = error ? std::cerr : std::cout;
    stream << (error ? "FAIL " : "PASS ") << profile_ << " / " << module;
    if (error)
        stream << ": " << error;
    stream << '\n';
}

int Suite::Finish() const
{
    std::cout << profile_ << ": " << total_ - failed_ << '/' << total_ << " passed\n";
    return failed_ || !total_ ? 1 : 0;
}

}

int wmain(int argc, wchar_t** argv)
{
    using namespace stone::windows::test;
    try {
        const std::wstring_view group = argc > 1 ? argv[1] : L"";
        if (group == L"environment" && argc == 3) {
            Suite suite("Unicode host environment");
            STONE_CHECK(stone::windows::InitializeAndroidNativeBackend());
            TestLibcEnvironment(suite, argv[2]);
            return suite.Finish();
        }
        if (group == L"local-listener-probe" && (argc == 2 || argc == 3))
            return ProbeLocalListener(argc == 3 ? argv[2] : nullptr);
        if (group == L"inotify-storage" && argc == 2) {
            Suite suite("Inotify storage access");
            STONE_CHECK(stone::windows::InitializeAndroidNativeBackend());
            TestInotifyStorage(suite);
            return suite.Finish();
        }
        if (group == L"allocation" && argc == 2) {
            // Pure allocator checks need neither Service startup nor sockets.
            Suite suite("Guest allocation ownership");
            TestAllocations(suite);
            STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
            return suite.Finish();
        }
        if (group == L"hle" && argc == 2) {
            Suite suite("HLE log and math");
            STONE_CHECK(stone::windows::InitializeAndroidNativeBackend());
            TestLogAndMath(suite);
            return suite.Finish();
        }
        // ELF ABI checks do not query host state, launch an APK or need Service.
        if ((group == L"external" || group == L"external-local") && argc == 2) {
            Suite suite("External library ABIs");
            TestExternal(suite, group == L"external");
            if (stone::windows::Arm64BlockJitEnabled()) {
                const auto blocks = stone::windows::GetArm64BlockStatistics();
                STONE_CHECK(blocks.executions && blocks.instructions && !blocks.failures);
                std::cout << "Native ARM64 blocks: " << blocks.executions << " calls, " << blocks.instructions
                          << " instructions\n";
            }
            return suite.Finish();
        }
        if (group == L"arm64-profile" && argc == 2) {
            Suite suite("ARM64 execution profiling");
            TestArm64ExecutionProfile(suite);
            STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
            return suite.Finish();
        }
        if (group == L"arm64-block" && argc == 2) {
            Suite suite("ARM64 native blocks");
            TestArm64Blocks(suite);
            STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
            return suite.Finish();
        }
        if (group == L"arm64-compact-pc" && argc == 2) {
            Suite suite("ARM64 compact PC fields");
            TestArm64CompactPc(suite);
            STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
            return suite.Finish();
        }
        if (group == L"arm64-data-cache" && argc == 2) {
            Suite suite("ARM64 ELF data ownership cache");
            TestArm64DataCache(suite);
            STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
            return suite.Finish();
        }
        if (group == L"arm64-stack-trace" && argc == 2) {
            Suite suite("ARM64 mapped stack traces");
            TestArm64StackTrace(suite);
            STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
            return suite.Finish();
        }
        if (group == L"arm64-dispatch" && argc == 2) {
            Suite suite("ARM64 block dispatch");
            TestArm64Dispatch(suite);
            STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
            return suite.Finish();
        }
        if (group == L"arm64-block-benchmark" && argc == 2) {
            Suite suite("ARM64 block microbenchmarks");
            BenchmarkArm64Blocks(suite);
            STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
            return suite.Finish();
        }
        if ((group == L"art-prepared-arm64" || group == L"art-prepared-arm64-framework") && argc == 3) {
            InspectPreparedArm64Art(argv[2], group == L"art-prepared-arm64-framework");
            return 0;
        }
        if (group == L"arm64-image-zlib" && argc == 3) {
            Suite suite("ARM64 image libz.so");
            TestArm64ImageZlib(suite, argv[2]);
            return suite.Finish();
        }
        // Pure instruction execution does not query the Service host snapshot.
        if (group == L"arm64" && argc == 2) {
            Suite suite("ARM64 instruction decoding");
            suite.Run("scalar-fmadd-and-ushr", CheckArm64ScalarDecode);
            return suite.Finish();
        }
        stone::windows::ConfigureClientService(STONE_TEST_SERVICE);
        stone::windows::InitializeClientServices();
        if ((group == L"system" && argc == 3) || (group == L"system-image" && argc == 4)) {
            const int api = std::stoi(argv[2]);
            Suite suite("Android API " + std::to_string(api));
            if (group == L"system-image") {
                TestImage(suite, api, argv[3]);
            }
            else {
                TestAndroid(suite, api);
                TestGraphics(suite);
                TestLibc(suite, api);
                TestMedia(suite);
                STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
            }
            return suite.Finish();
        }
        if (group == L"ubsan" && argc == 3) {
            Suite suite("Android 37 compiler-rt build f52a119338b344d371e59c0bf08717555ec6fd82");
            TestUbsanImage(suite, argv[2]);
            return suite.Finish();
        }
        if (group == L"art-image" && argc == 4) {
            InspectArtImage(argv[2], argv[3]);
            return 0;
        }
        if (group == L"art-runtime" && argc == 4) {
            TestArtRuntime(argv[2], argv[3]);
            return 0;
        }
        std::cerr
            << "Expected environment <environ|getenv|arm64>, allocation, local-listener-probe, system <api>, system-image <api> <img>, external[-local], arm64-profile, arm64-block, arm64-dispatch, arm64-stack-trace, arm64-data-cache, arm64-compact-pc, arm64, "
               "ubsan <elf>, art-image <img> <cache>, art-prepared-arm64[-framework] <directory>, or arm64-image-zlib <directory>\n";
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
