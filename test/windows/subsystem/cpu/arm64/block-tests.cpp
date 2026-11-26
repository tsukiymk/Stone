#include "../../support/test-suite.h"

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
#include <sstream>
#include <xmmintrin.h>

#include "subsystem/support/native-checks.h"

namespace stone::windows::test {
using namespace checks;

namespace {
struct BlockMemoryFixture {
    uint8_t byte = 7;
    bool fail = false, unwind = false, trace = false, rx = false, aligned = false;
    unsigned calls = 0, failAfter = 0;
};

__declspec(noinline) void BlockMemoryCallback(Arm64BlockContext* context, uint32_t word, Arm64BlockMemory kind) noexcept
{
    struct Preserve {
        DWORD win = GetLastError();
        int error = errno;

        ~Preserve()
        {
            errno = error;
            SetLastError(win);
        }
    } preserve;

    auto& fixture = *static_cast<BlockMemoryFixture*>(context->owner);
    DWORD64 base = 0;
    fixture.unwind = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(_ReturnAddress()), &base, nullptr) != nullptr;
    MEMORY_BASIC_INFORMATION region{};
    fixture.rx = VirtualQuery(_ReturnAddress(), &region, sizeof(region)) && region.Protect == PAGE_EXECUTE_READ;
    fixture.trace = false;
    CONTEXT state{};
    RtlCaptureContext(&state);
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    const auto nativeReturn = reinterpret_cast<DWORD64>(_ReturnAddress());
    for (unsigned i = 0; i < 16 && state.Rip && state.Rsp >= low && state.Rsp <= high - 64; ++i) {
        const auto pc = state.Rip;
        const auto* function = RtlLookupFunctionEntry(pc, &base, nullptr);
        if (function) {
            void* handler = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, const_cast<PRUNTIME_FUNCTION>(function), &state, &handler,
                             &frame, nullptr);
        }
        else {
            state.Rip = *reinterpret_cast<const DWORD64*>(state.Rsp);
            state.Rsp += 8;
        }
        if (pc == nativeReturn) {
            HMODULE module = nullptr;
            fixture.trace = state.Rsp == reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) + 56 &&
                            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               reinterpret_cast<LPCWSTR>(state.Rip), &module) &&
                            module == GetModuleHandleW(nullptr);
            break;
        }
    }
    fixture.aligned = (reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) & 15) == 8;
    ++fixture.calls;
    if (fixture.fail || (fixture.failAfter && fixture.calls == fixture.failAfter) ||
        kind != Arm64BlockMemory::Transfer) {
        context->exit = Arm64BlockExit::Failure;
        return;
    }
    const auto reg = word & 31;
    if ((word >> 22) & 3) {
        if (reg != 31)
            context->registers[reg] = fixture.byte;
    }
    else
        fixture.byte = reg == 31 ? 0 : static_cast<uint8_t>(context->registers[reg]);
}
}

void TestArm64Dispatch(Suite& suite)
{
    suite.Run("arm64-local-block-collisions-preserve-loop-results-without-shared-cache-churn", [] {
        auto bytes = MakeArm64TlsElf();
        // All four starts collide in the original 256-slot direct mapping.
        // The interpreter executes this branch chain, including signal-budget
        // boundaries, rather than calling generated blocks from the test.
        constexpr std::array<size_t, 4> offsets{0x1100, 0x1504, 0x1908, 0x1d0c};
        for (size_t i = 0; i < offsets.size(); ++i) {
            std::array<uint32_t, 4> words{0x91000400, 0xd503201f, 0xd503201f, 0};
            if (i + 1 < offsets.size())
                words[3] = 0x14000000u | (static_cast<uint32_t>((offsets[i + 1] - offsets[i] - 12) / 4) & 0x3ffffff);
            else {
                words[2] = 0xf1000421;
                words[3] = 0x54000001u |
                           ((static_cast<uint32_t>(
                                 (static_cast<int64_t>(offsets[0]) - static_cast<int64_t>(offsets[i]) - 12) / 4) &
                             0x7ffff)
                            << 5);
            }
            std::memcpy(bytes.data() + offsets[i], words.data(), sizeof(words));
        }
        const uint32_t ret = 0xd65f03c0;
        std::memcpy(bytes.data() + offsets.back() + 16, &ret, 4);
        ElfImage image(bytes, ElfImage::Machine::Arm64, true);
        image.Protect(ElfImage::Execution::Translated);
        const auto hash = [](uintptr_t pc) {
            return ((pc >> 2) ^ (pc >> 10)) & 255;
        };
        for (auto offset : offsets)
            STONE_CHECK(hash(image.LoadBias() + offset) == hash(image.LoadBias() + offsets[0]));
        const auto before = GetArm64BlockStatistics();
        const Arm64Argument arguments[]{{'J', 0}, {'J', 4096}};
        const auto result =
            RunArm64Function(image, image.LoadBias() + offsets[0], 'J', {}, nullptr, nullptr, arguments);
        const auto after = GetArm64BlockStatistics();
        STONE_CHECK(result == 16384 && after.failures == before.failures);
        std::cout << "Local block collision loop: result=" << result
                  << " shared-lookups=" << after.lookups - before.lookups
                  << " native-calls=" << after.executions - before.executions
                  << " dispatches=" << after.dispatches - before.dispatches << '\n';
        wchar_t control[2]{};
        const bool direct = GetEnvironmentVariableW(L"STONE_ARM64_LOCAL_DIRECT", control, 2) == 1 && control[0] == L'1';
        const bool chain = GetEnvironmentVariableW(L"STONE_ARM64_BLOCK_CHAIN", control, 2) == 1 && control[0] == L'1';
        if (Arm64BlockJitEnabled()) {
            STONE_CHECK(after.executions > before.executions);
            if (direct)
                STONE_CHECK(after.lookups - before.lookups > 8000);
            else
                STONE_CHECK(after.lookups - before.lookups < 32);
            const auto calls = after.executions - before.executions, dispatches = after.dispatches - before.dispatches;
            if (chain && !direct)
                STONE_CHECK(dispatches < calls / 8);
            else
                STONE_CHECK(dispatches == calls);
        }
        else
            STONE_CHECK(after.lookups == before.lookups && after.executions == before.executions);
    });
}

void TestArm64StackTrace(Suite& suite)
{
    suite.Run("mapped-stack-stores-continue-without-executing-stale-self-modified-code", [] {
        STONE_CHECK(Arm64BlockJitEnabled());
        wchar_t value[2]{};
        const bool trace = GetEnvironmentVariableW(L"STONE_ARM64_STACK_STORE_TRACE", value, 2) == 1 && value[0] == L'1';
        auto bytes = MakeArm64TlsElf();
        ElfImage image(bytes, ElfImage::Machine::Arm64, true);
        image.Protect(ElfImage::Execution::Translated);
        auto* memory = AndroidMapArm64(nullptr, 8192, 7, 0x22, -1, 0);
        STONE_CHECK(memory != reinterpret_cast<void*>(-1));
        struct Free {
            void* p;
            ~Free()
            {
                AndroidUnmapArm64(p, 8192);
            }
        } free{memory};
        const auto entry = reinterpret_cast<uintptr_t>(memory);
        const uint32_t loop[]{0xd10083ff, 0xf90003e1, 0xf90007e1, 0xf94003e2, 0x91000442, 0xf9000be2,
                              0xf1000400, 0x54ffff41, 0xf9400be0, 0x910083ff, 0xd65f03c0};
        std::memcpy(memory, loop, sizeof(loop));
        const Arm64Argument arguments[]{{'J', 2048}, {'J', 41}};
        const auto before = GetArm64BlockStatistics();
        STONE_CHECK(RunArm64Function(image, entry, 'J', {}, nullptr, nullptr, arguments) == 42);
        const auto after = GetArm64BlockStatistics();
        const auto calls = after.executions - before.executions, insns = after.instructions - before.instructions;
        std::cout << "Mapped stack loop: trace=" << trace << " native-calls=" << calls
                  << " native-instructions=" << insns << '\n';
        if (trace)
            STONE_CHECK(calls < 512 && insns > 10000);
        else
            STONE_CHECK(calls > 1000 && insns < 6000);
        const uint32_t modifying[]{0xb9000041, 0xd2800020, 0xd503201f, 0xd503201f, 0xd65f03c0};
        const auto target = entry + 256;
        std::memcpy(static_cast<uint8_t*>(memory) + 256, modifying, sizeof(modifying));
        const Arm64Argument modifyArgs[]{{'J', 0}, {'J', 0xd2800540}, {'J', target + 4}};
        const auto beforeModify = GetArm64BlockStatistics().executions;
        for (unsigned i = 0; i < 128; ++i) {
            std::memcpy(reinterpret_cast<void*>(target + 4), modifying + 1, 4);
            STONE_CHECK(RunArm64Function(image, target, 'J', {}, nullptr, nullptr, modifyArgs) == 42);
        }
        if (trace)
            STONE_CHECK(GetArm64BlockStatistics().executions > beforeModify);
        const uint32_t faulting[]{0xd10043ff, 0xf90003e0, 0xf94003e2, 0xf9000022, 0x910043ff, 0xd65f03c0};
        const auto fault = entry + 512;
        std::memcpy(static_cast<uint8_t*>(memory) + 512, faulting, sizeof(faulting));
        const Arm64Argument good[]{{'J', 42}, {'J', entry + 4096}};
        for (unsigned i = 0; i < 128; ++i)
            STONE_CHECK(RunArm64Function(image, fault, 'J', {}, nullptr, nullptr, good) == 42);
        const auto beforeFault = GetArm64BlockStatistics().executions;
        const Arm64Argument bad[]{{'J', 42}, {'J', 0}};
        bool rejected = false;
        try {
            RunArm64Function(image, fault, 'J', {}, nullptr, nullptr, bad);
        }
        catch (const std::runtime_error& error) {
            std::ostringstream expected;
            expected << std::hex << (fault + 12);
            rejected = std::string(error.what()).find(expected.str()) != std::string::npos;
        }
        STONE_CHECK(rejected && *reinterpret_cast<const uint64_t*>(entry + 4096) == 42);
        if (trace)
            STONE_CHECK(GetArm64BlockStatistics().executions > beforeFault);
        STONE_CHECK(GetArm64BlockStatistics().failures == before.failures);
    });
    suite.Run("mapped-integer-and-vector-pairs-preserve-stack-writeback-and-results", [] {
        auto bytes = MakeArm64TlsElf();
        ElfImage image(bytes, ElfImage::Machine::Arm64, true);
        image.Protect(ElfImage::Execution::Translated);
        auto* memory = AndroidMapArm64(nullptr, 8192, 7, 0x22, -1, 0);
        STONE_CHECK(memory != reinterpret_cast<void*>(-1));
        struct Free {
            void* p;
            ~Free()
            {
                AndroidUnmapArm64(p, 8192);
            }
        } free{memory};
        const auto entry = reinterpret_cast<uintptr_t>(memory), source = entry + 4096, destination = source + 64;
        const uint32_t words[]{0xa9bc07e0, 0xa90007e0, 0xa9400fe2, 0xa9010fe2, 0xad400480, 0xad0107e0, 0xad410fe2,
                               0xf10004c6, 0x54ffff21, 0x3d8000a2, 0x3d8004a3, 0x8b030040, 0xa8c41fe6, 0xd65f03c0};
        std::memcpy(memory, words, sizeof(words));
        std::array<uint8_t, 32> expected{};
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = static_cast<uint8_t>(i * 37 + 9);
        std::memcpy(reinterpret_cast<void*>(source), expected.data(), expected.size());
        const Arm64Argument arguments[]{{'J', 19},     {'J', 23},          {'J', 0},   {'J', 0},
                                        {'J', source}, {'J', destination}, {'J', 2048}};
        const auto before = GetArm64BlockStatistics();
        for (unsigned i = 0; i < 2; ++i) {
            std::memset(reinterpret_cast<void*>(destination), 0, expected.size());
            STONE_CHECK(RunArm64Function(image, entry, 'J', {}, nullptr, nullptr, arguments) == 42);
            STONE_CHECK(!std::memcmp(reinterpret_cast<void*>(destination), expected.data(), expected.size()));
        }
        STONE_CHECK(GetArm64BlockStatistics().executions > before.executions);
    });
    suite.Run("mapped-stack-continuations-respect-polling-budget-and-callback-fault-prefix", [] {
        if (!Arm64StackStoreTraceEnabled())
            return;
        const uint32_t words[]{0xf90003e1, 0xf1000400, 0x54ffffc1, 0xd2800540, 0xd65f03c0};
        auto block = FindArm64Block(0, 0x1000, 0, 1, {reinterpret_cast<const uint8_t*>(words), sizeof(words)});
        STONE_CHECK(block && block->Instructions() == 5);
        struct Fixture {
            unsigned calls = 0, failAfter = 0;
            uint64_t stored = 0;
        } fixture;
        std::array<uint64_t, 31> registers{};
        Arm64BlockContext context;
        context.registers = registers.data();
        context.owner = &fixture;
        context.memory = [](Arm64BlockContext* c, uint32_t, Arm64BlockMemory) noexcept {
            auto& f = *static_cast<Fixture*>(c->owner);
            ++f.calls;
            if (f.failAfter && f.calls == f.failAfter) {
                c->exit = Arm64BlockExit::Failure;
                return;
            }
            f.stored = c->registers[1];
            c->stackStoreOnly = 1;
        };
        for (const auto budget : {5u, 8u, 16u, 255u, 256u}) {
            fixture = {};
            registers[0] = 1000;
            registers[1] = 42;
            context.budget = budget;
            block->Run(context);
            STONE_CHECK(context.executed <= budget && context.executed == fixture.calls * 3);
            STONE_CHECK(registers[0] == 1000 - fixture.calls && fixture.stored == 42 && context.pc == 0x1000);
        }
        fixture = {0, 3};
        registers[0] = 1000;
        context.budget = 256;
        block->Run(context);
        STONE_CHECK(context.exit == Arm64BlockExit::Failure && fixture.calls == 3 && fixture.stored == 42);
        STONE_CHECK(registers[0] == 998 && context.executed == 7 && context.instruction == 0x1000);
    });
}

void TestArm64CompactPc(Suite& suite)
{
    suite.Run("derived-pc-fields-preserve-callback-faults-wrap-and-branch-state", [] {
        struct Fixture {
            uintptr_t instruction = 0, next = 0;
            bool fail = false;
            unsigned calls = 0;
        } fixture;
        const auto callback = +[](Arm64BlockContext* c, uint32_t, Arm64BlockMemory kind) noexcept {
            auto& f = *static_cast<Fixture*>(c->owner);
            ++f.calls;
            if (kind != Arm64BlockMemory::Transfer || c->instruction != f.instruction || c->pc != f.next) {
                c->exit = Arm64BlockExit::Failure;
                return;
            }
            if (f.fail)
                c->exit = Arm64BlockExit::Failure;
            else
                c->registers[0] = 7;
        };
        std::array<uint64_t, 31> registers{};
        Arm64BlockContext context;
        context.registers = registers.data();
        context.owner = &fixture;
        context.memory = callback;
        const uint32_t load[]{0x39400020, 0x11000400, 0xd503201f, 0xd4200000};
        uint64_t bytes = 0;
        for (const auto pc : {uintptr_t{0x1000}, uintptr_t{0x7ffffffe0000}, UINTPTR_MAX - 3}) {
            auto block = FindArm64Block(0xabc1, pc, 0, 0, {reinterpret_cast<const uint8_t*>(load), sizeof(load)});
            STONE_CHECK(block && block->Instructions() == 3);
            bytes += block->CodeBytes();
            fixture = {pc, pc + 4, false};
            registers = {};
            context.nzcv = 10;
            context.flagsValid = 1;
            block->Run(context);
            STONE_CHECK(context.exit == Arm64BlockExit::Continue && registers[0] == 8 && fixture.calls == 1);
            STONE_CHECK(context.pc == pc + 12 && context.instruction == pc + 8 && context.executed == 3 &&
                        context.nzcv == 10);
            fixture.fail = true;
            registers[0] = 99;
            block->Run(context);
            STONE_CHECK(context.exit == Arm64BlockExit::Failure && registers[0] == 99 && context.executed == 1);
            STONE_CHECK(context.pc == pc + 4 && context.instruction == pc && context.nzcv == 10);
            for (const uint32_t word : {0x14000001u, 0x17ffffffu, 0x15ffffffu, 0x16000000u, 0x94000001u}) {
                auto branch = FindArm64Block(0xabc2, pc, 0, 0, {reinterpret_cast<const uint8_t*>(&word), 4});
                STONE_CHECK(branch && branch->Instructions() == 1);
                context.budget = 0;
                branch->Run(context);
                const auto displacement = static_cast<int64_t>(static_cast<int32_t>(word << 6) >> 6) * 4;
                STONE_CHECK(context.pc == pc + displacement && context.instruction == pc && context.executed == 1);
                if (word >> 31)
                    STONE_CHECK(registers[30] == pc + 4);
                STONE_CHECK(context.nzcv == 10);
            }
        }
        std::cout << "PC emission: compact=" << Arm64CompactPcEnabled() << " load-code-bytes=" << bytes << '\n';
    });
}

void TestArm64Blocks(Suite& suite)
{
    suite.Run("arm64-native-block-integer-results-and-nzcv-against-interpreter", [] {
        STONE_CHECK(!Arm64BlockJitEnabled()); // The reference must remain the interpreter.
        const std::array<uint64_t, 8> values{
            0, 1, 2, UINT64_MAX, INT64_MAX, uint64_t{1} << 63, 0x123456789abcdef0ull, 0xffffffff};
        const std::array<uint32_t, 22> operations{
            0x8b010000, 0xab010000, 0xcb010000, 0xeb010000, 0x0b010000, 0x2b010000, 0x4b010000, 0x6b010000,
            0x8a010000, 0xaa010000, 0xca010000, 0xea010000, 0x8a210000, 0xea210000, 0x1ac12000, 0x1ac12400,
            0x1ac12800, 0x1ac12c00, 0xd344fc00, 0x9344fc00, 0x53047c00, 0x13047c00};
        const auto output = AllocateArm64GuestMemory(8, true);
        STONE_CHECK(output);
        struct Free {
            uintptr_t p;
            ~Free()
            {
                FreeArm64GuestMemory(p);
            }
        } free{output};
        for (const auto operation : operations)
            for (const auto a : values)
                for (const auto b : values) {
                    auto bytes = MakeArm64TlsElf();
                    const uint32_t reference[]{0xeb1f03ff, operation, 0xf9000040, 0xd53b4200, 0xd65f03c0};
                    const uint32_t translated[]{operation, 0xd503201f, 0xd4200000};
                    std::memcpy(bytes.data() + 0x1100, reference, sizeof(reference));
                    std::memcpy(bytes.data() + 0x1200, translated, sizeof(translated));
                    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
                    image.Protect(ElfImage::Execution::Translated);
                    const Arm64Argument arguments[]{{'J', a}, {'J', b}, {'J', output}};
                    const auto flags =
                        RunArm64Function(image, image.LoadBias() + 0x1100, 'J', {}, nullptr, nullptr, arguments) >> 28;
                    const auto expected = *reinterpret_cast<const uint64_t*>(output);
                    auto block = FindArm64Block(image.Identity(), image.LoadBias() + 0x1200, image.LoadBias(), 0,
                                                image.CodeAt(image.LoadBias() + 0x1200, sizeof(translated)));
                    STONE_CHECK(block && block->Instructions() == 2);
                    std::array<uint64_t, 31> registers{};
                    registers[0] = a;
                    registers[1] = b;
                    registers[2] = output;
                    Arm64BlockContext context;
                    context.registers = registers.data();
                    context.nzcv = 6;
                    context.flagsValid = 1;
                    block->Run(context);
                    if (registers[0] != expected || context.nzcv != flags)
                        throw std::runtime_error("native block mismatch for opcode " + std::to_string(operation) +
                                                 " operands " + std::to_string(a) + "," + std::to_string(b));
                    STONE_CHECK(context.executed == 2 && context.pc == image.LoadBias() + 0x1208);
                }
    });
    suite.Run("arm64-native-block-immediate-arithmetic-widths-flags-and-register-boundaries", [] {
        STONE_CHECK(!Arm64BlockJitEnabled());
        const auto output = AllocateArm64GuestMemory(16, true);
        STONE_CHECK(output);
        struct Free {
            uintptr_t p;
            ~Free()
            {
                FreeArm64GuestMemory(p);
            }
        } free{output};
        const uint64_t values[]{0,          1,          127,        128, UINT64_MAX, INT64_MAX, uint64_t{1} << 63,
                                0x80000000, 0xffffffff, 0x100000000};
        // X15/X16 straddle the signed disp8 boundary in the register bank.
        const unsigned aliases[][2]{{0, 0}, {15, 16}, {16, 15}, {29, 29}, {31, 0}, {0, 31}};
        for (unsigned wide = 0; wide < 2; ++wide)
            for (unsigned operation = 0; operation < 4; ++operation)
                for (unsigned shift = 0; shift < 2; ++shift)
                    for (unsigned immediate : {0u, 1u, 127u, 128u, 255u, 256u, 2047u, 4095u})
                        for (const auto& alias : aliases) {
                            const auto dest = alias[0], src = alias[1];
                            if (dest == 31 && !(operation & 1))
                                continue; // SP writes retain their guarded callback.
                            const uint32_t word = 0x11000000u | (wide << 31) | (operation << 29) | (shift << 22) |
                                                  (immediate << 10) | (src << 5) | dest;
                            auto bytes = MakeArm64TlsElf();
                            const uint32_t reference[]{0x910003e3,         src == 31 ? 0xd503201fu : 0xaa0003e0u | src,
                                                       0xeb1f03ff,         word,
                                                       0xf9000040u | dest, 0xf9000443,
                                                       0xd53b4200,         0xd65f03c0};
                            const uint32_t translated[]{word, 0xd503201f, 0xd4200000};
                            std::memcpy(bytes.data() + 0x1100, reference, sizeof(reference));
                            std::memcpy(bytes.data() + 0x1200, translated, sizeof(translated));
                            ElfImage image(bytes, ElfImage::Machine::Arm64, true);
                            image.Protect(ElfImage::Execution::Translated);
                            auto block = FindArm64Block(image.Identity(), image.LoadBias() + 0x1200, image.LoadBias(),
                                                        0, image.CodeAt(image.LoadBias() + 0x1200, sizeof(translated)));
                            STONE_CHECK(block && block->Instructions() == 2);
                            for (const auto value : values) {
                                const Arm64Argument args[]{{'J', value}, {'J', 0}, {'J', output}};
                                const auto flags = RunArm64Function(image, image.LoadBias() + 0x1100, 'J', {}, nullptr,
                                                                    nullptr, args) >>
                                                   28;
                                const auto* expected = reinterpret_cast<const uint64_t*>(output);
                                std::array<uint64_t, 31> registers;
                                registers.fill(0xa5a5a5a5a5a5a5a5ull);
                                if (src != 31)
                                    registers[src] = value;
                                const auto before = registers;
                                Arm64BlockContext context;
                                context.registers = registers.data();
                                context.nzcv = 6;
                                context.flagsValid = 1;
                                context.sp = expected[1];
                                block->Run(context);
                                if ((dest != 31 && registers[dest] != expected[0]) || context.nzcv != flags)
                                    throw std::runtime_error("native immediate arithmetic mismatch for opcode " +
                                                             std::to_string(word) + " value " + std::to_string(value));
                                for (unsigned reg = 0; reg < 31; ++reg)
                                    if (reg != dest)
                                        STONE_CHECK(registers[reg] == before[reg]);
                                STONE_CHECK(context.flagsValid && context.sp == expected[1] && context.executed == 2 &&
                                            context.pc == image.LoadBias() + 0x1208);
                            }
                        }
    });
    suite.Run("arm64-native-block-extended-arithmetic-matches-interpreter-and-stack-sources", [] {
        STONE_CHECK(!Arm64BlockJitEnabled());
        const auto output = AllocateArm64GuestMemory(24, true);
        STONE_CHECK(output);
        struct Free {
            uintptr_t p;
            ~Free()
            {
                FreeArm64GuestMemory(p);
            }
        } free{output};
        const uint64_t values[]{0,
                                UINT64_MAX,
                                0x8000000080008080ull,
                                0x7fffffff7fff7f7full,
                                0xffffffffffffff80ull,
                                0x123456789abcdef0ull,
                                1,
                                0xaaaaaaaa55555555ull};
        const unsigned aliases[][3]{{0, 0, 1},  {1, 0, 1},  {0, 1, 0},  {0, 1, 1},
                                    {31, 0, 1}, {0, 31, 1}, {0, 1, 31}, {31, 31, 31}};
        for (unsigned wide = 0; wide < 2; ++wide)
            for (unsigned operation = 0; operation < 4; ++operation)
                for (unsigned extension = 0; extension < 8; ++extension)
                    for (unsigned shift = 0; shift <= 4; ++shift)
                        for (const auto& alias : aliases) {
                            const unsigned bits = 8u << (extension & 3);
                            if (bits > (wide ? 64u : 32u) || (alias[0] == 31 && !(operation & 1)))
                                continue;
                            const uint32_t word = 0x0b200000u | (wide << 31) | (operation << 29) | (alias[2] << 16) |
                                                  (extension << 13) | (shift << 10) | (alias[1] << 5) | alias[0];
                            auto bytes = MakeArm64TlsElf();
                            const uint32_t reference[]{0x910003e3, 0xeb1f03ff, word,      0xa9000440,
                                                       0xf9000843, 0xd53b4200, 0xd65f03c0};
                            const uint32_t translated[]{word, 0xd503201f, 0xd4200000};
                            std::memcpy(bytes.data() + 0x1100, reference, sizeof(reference));
                            std::memcpy(bytes.data() + 0x1200, translated, sizeof(translated));
                            ElfImage image(bytes, ElfImage::Machine::Arm64, true);
                            image.Protect(ElfImage::Execution::Translated);
                            auto block = FindArm64Block(image.Identity(), image.LoadBias() + 0x1200, image.LoadBias(),
                                                        0, image.CodeAt(image.LoadBias() + 0x1200, sizeof(translated)));
                            STONE_CHECK(block && block->Instructions() == 2);
                            for (unsigned sample = 0; sample < 8; ++sample) {
                                const auto a = values[sample], b = values[(sample * 3 + extension + shift) & 7];
                                const Arm64Argument args[]{{'J', a}, {'J', b}, {'J', output}};
                                const auto flags = RunArm64Function(image, image.LoadBias() + 0x1100, 'J', {}, nullptr,
                                                                    nullptr, args) >>
                                                   28;
                                const auto* expected = reinterpret_cast<const uint64_t*>(output);
                                std::array<uint64_t, 31> registers{};
                                registers[0] = a;
                                registers[1] = b;
                                registers[2] = output;
                                registers[3] = expected[2];
                                const auto before = registers;
                                Arm64BlockContext context;
                                context.registers = registers.data();
                                context.nzcv = 6;
                                context.flagsValid = 1;
                                context.sp = expected[2];
                                block->Run(context);
                                if (registers[0] != expected[0] || registers[1] != expected[1] || context.nzcv != flags)
                                    throw std::runtime_error("native extended arithmetic mismatch for opcode " +
                                                             std::to_string(word));
                                for (unsigned reg = 2; reg < 31; ++reg)
                                    STONE_CHECK(registers[reg] == before[reg]);
                                STONE_CHECK(context.sp == expected[2] && context.executed == 2 &&
                                            context.pc == image.LoadBias() + 0x1208);
                            }
                        }
        // Reserved shifts and the interpreter's unsupported 32-bit doubleword
        // extensions must retain the original fallback, including SP writes.
        for (const auto word : {0x8b211400u, 0x8b211800u, 0x8b211c00u, 0x0b216000u, 0x0b21e000u, 0x0b21401fu}) {
            const uint32_t words[]{word, 0xd503201f, 0xd4200000};
            const auto block =
                FindArm64Block(0xadd64, 0x1000, 0, 0, {reinterpret_cast<const uint8_t*>(words), sizeof(words)});
            if (block && block->Instructions())
                throw std::runtime_error("native extended arithmetic accepted unsupported opcode " +
                                         std::to_string(word));
        }
        {
            // The actual libart hotspot: CMP X23, W8, UXTW.
            auto bytes = MakeArm64TlsElf();
            const uint32_t reference[]{0xaa0003f7, 0xaa0103e8, 0xeb2842ff, 0xd53b4200, 0xd65f03c0};
            const uint32_t translated[]{0xeb2842ff, 0xd503201f, 0xd4200000};
            std::memcpy(bytes.data() + 0x1100, reference, sizeof(reference));
            std::memcpy(bytes.data() + 0x1200, translated, sizeof(translated));
            ElfImage image(bytes, ElfImage::Machine::Arm64, true);
            image.Protect(ElfImage::Execution::Translated);
            auto block = FindArm64Block(image.Identity(), image.LoadBias() + 0x1200, image.LoadBias(), 0,
                                        image.CodeAt(image.LoadBias() + 0x1200, sizeof(translated)));
            STONE_CHECK(block && block->Instructions() == 2);
            for (const auto a : values)
                for (const auto b : values) {
                    const Arm64Argument args[]{{'J', a}, {'J', b}};
                    const auto flags =
                        RunArm64Function(image, image.LoadBias() + 0x1100, 'J', {}, nullptr, nullptr, args) >> 28;
                    std::array<uint64_t, 31> registers{};
                    registers[23] = a;
                    registers[8] = b;
                    const auto before = registers;
                    Arm64BlockContext context;
                    context.registers = registers.data();
                    context.flagsValid = 0;
                    block->Run(context);
                    STONE_CHECK(registers == before && context.flagsValid && context.nzcv == flags &&
                                context.executed == 2);
                }
        }
        for (const auto word : {0x8b21401fu, 0xcb21401fu}) {
            const uint32_t words[]{word, 0xd503201f, 0xd4200000};
            auto block =
                FindArm64Block(0xadd64, 0x2000, 0, 0, {reinterpret_cast<const uint8_t*>(words), sizeof(words)});
            STONE_CHECK(block && block->Instructions() == 2);
            std::array<uint64_t, 31> registers{};
            BlockMemoryFixture fixture;
            Arm64BlockContext context;
            context.registers = registers.data();
            context.owner = &fixture;
            context.memory = BlockMemoryCallback;
            context.sp = 0x1234;
            block->Run(context); // The fixture rejects StackArithmetic; no direct SP write is allowed.
            STONE_CHECK(fixture.calls == 1 && context.exit == Arm64BlockExit::Failure && context.sp == 0x1234 &&
                        context.executed == 1 && context.instruction == 0x2000 && context.pc == 0x2004);
        }
    });
    suite.Run("arm64-native-block-all-bitfields-match-interpreter-with-register-aliases", [] {
        const auto output = AllocateArm64GuestMemory(16, true);
        STONE_CHECK(output);
        struct Free {
            uintptr_t p;
            ~Free()
            {
                FreeArm64GuestMemory(p);
            }
        } free{output};
        const uint64_t values[]{0,
                                UINT64_MAX,
                                0x8000000080000000ull,
                                0x0123456789abcdefull,
                                0xfedcba9876543210ull,
                                1,
                                0x7fffffff7fffffffull,
                                0xaaaaaaaa55555555ull};
        for (unsigned wide = 0; wide < 2; ++wide)
            for (unsigned kind = 0; kind < 3; ++kind)
                for (unsigned r = 0; r < (wide ? 64u : 32u); ++r)
                    for (unsigned s = 0; s < (wide ? 64u : 32u); ++s)
                        for (unsigned alias = 0; alias < 4; ++alias) {
                            const unsigned src = alias == 2 ? 31 : alias == 1 ? 0 : 1, dest = alias == 3 ? 31 : 0;
                            const uint32_t operation = 0x13000000u | (wide << 31) | (wide << 22) | (kind << 29) |
                                                       (r << 16) | (s << 10) | (src << 5) | dest;
                            auto bytes = MakeArm64TlsElf();
                            const uint32_t reference[]{0xeb1f03ff, operation, 0xa9000440, 0xd53b4200, 0xd65f03c0};
                            const uint32_t translated[]{operation, 0xd503201f, 0xd4200000};
                            std::memcpy(bytes.data() + 0x1100, reference, sizeof(reference));
                            std::memcpy(bytes.data() + 0x1200, translated, sizeof(translated));
                            ElfImage image(bytes, ElfImage::Machine::Arm64, true);
                            image.Protect(ElfImage::Execution::Translated);
                            auto block = FindArm64Block(image.Identity(), image.LoadBias() + 0x1200, image.LoadBias(),
                                                        0, image.CodeAt(image.LoadBias() + 0x1200, sizeof(translated)));
                            STONE_CHECK(block && block->Instructions() == 2);
                            for (unsigned sample = 0; sample < 2; ++sample) {
                                const auto a = values[(r + s + alias + sample * 4) & 7],
                                           b = values[(r * 3 + s + kind + sample * 3) & 7];
                                const Arm64Argument args[]{{'J', a}, {'J', b}, {'J', output}};
                                const auto flags = RunArm64Function(image, image.LoadBias() + 0x1100, 'J', {}, nullptr,
                                                                    nullptr, args) >>
                                                   28;
                                const auto* expected = reinterpret_cast<const uint64_t*>(output);
                                std::array<uint64_t, 31> registers{};
                                registers[0] = a;
                                registers[1] = b;
                                registers[2] = output;
                                Arm64BlockContext context;
                                context.registers = registers.data();
                                context.nzcv = 6;
                                context.flagsValid = 1;
                                block->Run(context);
                                if (registers[0] != expected[0] || registers[1] != expected[1] || context.nzcv != flags)
                                    throw std::runtime_error("native bitfield mismatch for opcode " +
                                                             std::to_string(operation));
                                STONE_CHECK(context.executed == 2 && context.pc == image.LoadBias() + 0x1208);
                            }
                        }
    });
    suite.Run("arm64-native-block-integer-vectors-match-interpreter-with-all-aliases", [] {
        const auto memory = AllocateArm64GuestMemory(128, true);
        STONE_CHECK(memory);
        struct Free {
            uintptr_t p;
            ~Free()
            {
                FreeArm64GuestMemory(p);
            }
        } free{memory};
        std::vector<uint32_t> operations;
        for (unsigned full = 0; full < 2; ++full)
            for (unsigned size = 0; size < 4; ++size) {
                if (size == 3 && !full)
                    continue;
                for (auto base : {0x0e208400u, 0x2e208400u})
                    operations.push_back(base | (full << 30) | (size << 22));
            }
        for (unsigned full = 0; full < 2; ++full)
            for (unsigned size = 0; size < 3; ++size) {
                for (auto base : {0x2e200000u, 0x2e201000u, 0x2e202000u, 0x2e203000u, 0x2e202800u, 0x2e206800u})
                    operations.push_back(base | (full << 30) | (size << 22));
            }
        const unsigned aliases[][3]{{0, 1, 2},  {0, 0, 2},  {0, 1, 0},  {0, 1, 1},   {0, 0, 0},
                                    {31, 1, 2}, {0, 31, 2}, {0, 1, 31}, {31, 31, 31}};
        const unsigned saved[]{0, 1, 2, 31};
        const uint64_t patterns[]{0,
                                  UINT64_MAX,
                                  0x8000800080008000ull,
                                  0x7fff7fff7fff7fffull,
                                  0x8000000080000000ull,
                                  0x0123456789abcdefull,
                                  0xfedcba9876543210ull,
                                  0xaaaaaaaa55555555ull};
        bool checkedMissingBank = false;
        for (auto base : operations)
            for (const auto& alias : aliases) {
                const auto pair = base & 0x9f3ffc00u;
                const bool unary = pair == 0x0e202800 || pair == 0x0e206800;
                const auto operation = base | alias[0] | (alias[1] << 5) | (unary ? 0 : alias[2] << 16);
                auto bytes = MakeArm64TlsElf();
                const uint32_t reference[]{0x3dc00000, 0x3dc00401, 0x3dc00802, 0x3dc00c1f, 0xeb1f03ff, operation,
                                           0x3d800020, 0x3d800421, 0x3d800822, 0x3d800c3f, 0xd53b4200, 0xd65f03c0};
                const uint32_t translated[]{operation, 0xd503201f, 0xd4200000};
                std::memcpy(bytes.data() + 0x1100, reference, sizeof(reference));
                std::memcpy(bytes.data() + 0x1200, translated, sizeof(translated));
                ElfImage image(bytes, ElfImage::Machine::Arm64, true);
                image.Protect(ElfImage::Execution::Translated);
                auto block = FindArm64Block(image.Identity(), image.LoadBias() + 0x1200, image.LoadBias(), 0,
                                            image.CodeAt(image.LoadBias() + 0x1200, sizeof(translated)));
                if (!block || block->Instructions() != 2)
                    throw std::runtime_error("native vector opcode not translated: " + std::to_string(operation));
                for (unsigned sample = 0; sample < 8; ++sample) {
                    std::array<std::array<uint64_t, 2>, 32> bank{};
                    for (unsigned i = 0; i < 4; ++i) {
                        bank[saved[i]] = {patterns[(sample + i * 2) & 7], patterns[(sample + i * 2 + 1) & 7]};
                        std::memcpy(reinterpret_cast<void*>(memory + i * 16), bank[saved[i]].data(), 16);
                    }
                    const Arm64Argument args[]{{'L', memory}, {'L', memory + 64}};
                    const auto flags =
                        RunArm64Function(image, image.LoadBias() + 0x1100, 'J', {}, nullptr, nullptr, args) >> 28;
                    std::array<uint64_t, 31> registers{};
                    registers[0] = memory;
                    registers[1] = memory + 64;
                    Arm64BlockContext context;
                    context.registers = registers.data();
                    context.nzcv = 6;
                    context.flagsValid = 1;
                    if (!checkedMissingBank) {
                        bool rejected = false;
                        try {
                            block->Run(context);
                        }
                        catch (const std::invalid_argument&) {
                            rejected = true;
                        }
                        STONE_CHECK(rejected);
                        checkedMissingBank = true;
                    }
                    context.vectors = bank.data();
                    const auto csr = _mm_getcsr();
                    block->Run(context);
                    STONE_CHECK(_mm_getcsr() == csr && registers[0] == memory && registers[1] == memory + 64 &&
                                context.nzcv == flags && context.executed == 2 &&
                                context.pc == image.LoadBias() + 0x1208);
                    for (unsigned i = 0; i < 4; ++i) {
                        if (std::memcmp(bank[saved[i]].data(), reinterpret_cast<const void*>(memory + 64 + i * 16), 16))
                            throw std::runtime_error("native vector mismatch for opcode " + std::to_string(operation) +
                                                     " sample " + std::to_string(sample));
                    }
                }
            }
    });
    suite.Run("arm64-native-block-all-conditions-and-unavailable-flags", [] {
        for (unsigned condition = 0; condition < 16; ++condition)
            for (unsigned flags = 0; flags < 16; ++flags) {
                const uint32_t words[]{0xd503201f, 0x54000000u | (2u << 5) | condition};
                const auto source = std::span(reinterpret_cast<const uint8_t*>(words), sizeof(words));
                auto block = FindArm64Block(0xfeed, 0x1000, 0, 0, source);
                STONE_CHECK(block && block->Instructions() == 2);
                std::array<uint64_t, 31> registers{};
                Arm64BlockContext context;
                context.registers = registers.data();
                context.nzcv = flags;
                context.flagsValid = 1;
                block->Run(context);
                const bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1;
                const bool choices[]{z, c, n, v, c && !z, n == v, !z && n == v};
                const bool taken = condition >= 14 || choices[condition / 2] != bool(condition & 1);
                STONE_CHECK(context.pc == (taken ? 0x100c : 0x1008) && context.executed == 2 && context.nzcv == flags);
                context.flagsValid = 0;
                if (condition < 14) {
                    bool rejected = false;
                    try {
                        block->Run(context);
                    }
                    catch (const std::invalid_argument&) {
                        rejected = true;
                    }
                    STONE_CHECK(rejected);
                }
                else
                    block->Run(context);
            }
    });
    suite.Run("arm64-native-block-memory-helper-failure-store-boundary-and-windows-unwind", [] {
        const uint32_t words[]{0x39400020, 0x11000400, 0x39000020, 0xd2800540};
        auto block = FindArm64Block(0, 0x2000, 0, 0, {reinterpret_cast<const uint8_t*>(words), sizeof(words)});
        STONE_CHECK(block && block->Instructions() == 3);
        std::array<uint64_t, 31> registers{};
        BlockMemoryFixture fixture;
        Arm64BlockContext context;
        context.registers = registers.data();
        context.owner = &fixture;
        context.memory = BlockMemoryCallback;
        block->Run(context);
        STONE_CHECK(fixture.byte == 8 && registers[0] == 8 && context.executed == 3 && context.pc == 0x200c &&
                    fixture.unwind && fixture.trace && fixture.rx && fixture.aligned);
        fixture.fail = true;
        registers[0] = 99;
        block->Run(context);
        STONE_CHECK(context.exit == Arm64BlockExit::Failure && context.executed == 1 && registers[0] == 99 &&
                    fixture.byte == 8 && context.instruction == 0x2000);
        ClearArm64BlockCache();
        fixture.fail = false;
        block->Run(context);
        STONE_CHECK(fixture.byte == 9 && fixture.unwind && fixture.trace && fixture.rx);
        STONE_CHECK(GetArm64BlockStatistics().activeUnwindTables == 1);
        block.reset();
        STONE_CHECK(GetArm64BlockStatistics().activeUnwindTables == 0 && !GetArm64BlockStatistics().committedCodeBytes);
    });
    suite.Run("arm64-native-block-source-generation-identity-and-pinned-code", [] {
        uint32_t words[]{0xd2800020, 0x91000800, 0xd65f03c0};
        const auto source = std::span(reinterpret_cast<const uint8_t*>(words), sizeof(words));
        auto first = FindArm64Block(123, 0x3000, 0, 1, source);
        STONE_CHECK(first && first->Instructions() == 3);
        STONE_CHECK(FindArm64Block(123, 0x3000, 0, 1, source) == first);
        words[0] = 0xd2800040;
        auto changed = FindArm64Block(123, 0x3000, 0, 1, source);
        STONE_CHECK(changed && changed != first);
        auto generation = FindArm64Block(123, 0x3000, 0, 2, source);
        STONE_CHECK(generation && generation != changed);
        auto image = FindArm64Block(124, 0x3000, 0, 2, source);
        STONE_CHECK(image && image != generation);
        std::array<uint64_t, 31> registers{};
        Arm64BlockContext context;
        context.registers = registers.data();
        first->Run(context);
        STONE_CHECK(registers[0] == 3 && context.exit == Arm64BlockExit::Return);
        changed->Run(context);
        STONE_CHECK(registers[0] == 4);
        ClearArm64BlockCache();
        first->Run(context);
        STONE_CHECK(registers[0] == 3);
    });
    suite.Run("arm64-native-block-repeated-loop-budgets-flags-and-later-memory-fault", [] {
        const uint32_t words[]{0xf1000400, 0x54ffffe1};
        auto block = FindArm64Block(0x1234, 0x1000, 0, 0, {reinterpret_cast<const uint8_t*>(words), sizeof(words)});
        STONE_CHECK(block && block->Instructions() == 2);
        std::array<uint64_t, 31> registers{};
        registers[0] = 10;
        Arm64BlockContext context;
        context.registers = registers.data();
        context.budget = 6;
        block->Run(context);
        STONE_CHECK(registers[0] == 7 && context.executed == 6 && context.pc == 0x1000 &&
                    context.instruction == 0x1004);
        context.budget = 5;
        block->Run(context);
        STONE_CHECK(registers[0] == 5 && context.executed == 4 && context.pc == 0x1000);
        context.budget = 20;
        block->Run(context);
        STONE_CHECK(registers[0] == 0 && context.executed == 10 && context.pc == 0x1008 && context.nzcv == 6);
        context.budget = 1;
        bool rejected = false;
        try {
            block->Run(context);
        }
        catch (const std::invalid_argument&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
        const uint32_t continuation[]{0xf1000400, 0x54ffffe1, 0xd2800540, 0xd503201f};
        auto continued = FindArm64Block(0x1234, 0x3000, 0, 0,
                                        {reinterpret_cast<const uint8_t*>(continuation), sizeof(continuation)});
        registers[0] = 3;
        context.budget = 6;
        continued->Run(context);
        STONE_CHECK(registers[0] == 1 && context.executed == 4 && context.pc == 0x3000);
        continued->Run(context);
        STONE_CHECK(registers[0] == 42 && context.executed == 4 && context.pc == 0x3010);
        const uint32_t load[]{0x39400022, 0x91000421, 0xf1000400, 0x54ffffa1};
        block = FindArm64Block(0x1234, 0x2000, 0, 0, {reinterpret_cast<const uint8_t*>(load), sizeof(load)});
        BlockMemoryFixture fixture;
        fixture.failAfter = 3;
        context.owner = &fixture;
        context.memory = BlockMemoryCallback;
        context.budget = 256;
        registers[0] = 10;
        registers[1] = 100;
        block->Run(context);
        STONE_CHECK(context.exit == Arm64BlockExit::Failure && context.executed == 9 && context.instruction == 0x2000);
        STONE_CHECK(registers[0] == 8 && registers[1] == 102 && registers[2] == 7 && fixture.calls == 3 &&
                    fixture.unwind);
        // The longest block reserves up to 64 instructions for its current
        // prefix and next iteration; exercise both patched immediate operands.
        std::array<uint32_t, 32> longest;
        longest.fill(0xd503201f);
        longest[30] = 0xf1000400;
        longest[31] = 0x54000001u | ((uint32_t(-31) & 0x7ffff) << 5);
        block =
            FindArm64Block(0x1234, 0x4000, 0, 0, {reinterpret_cast<const uint8_t*>(longest.data()), sizeof(longest)});
        STONE_CHECK(block && block->Instructions() == 32);
        for (const auto budget : {0u, 32u, 33u, 63u, 64u, 65u, 95u, 96u, 255u, 256u}) {
            registers[0] = 100;
            context.budget = budget;
            block->Run(context);
            const auto iterations = budget ? budget / 32 : 1;
            STONE_CHECK(registers[0] == 100 - iterations && context.executed == iterations * 32 &&
                        context.pc == 0x4000 && context.instruction == 0x407c);
        }
    });
    suite.Run("arm64-native-block-concurrent-cache-eviction-and-error-state", [] {
        ClearArm64BlockCache();
        const uint32_t words[]{0xd2800540, 0xd65f03c0};
        const auto source = std::span(reinterpret_cast<const uint8_t*>(words), sizeof(words));
        const auto before = GetArm64BlockStatistics();
        auto pinned = FindArm64Block(0xface, 0x1000, 0, 0, source);
        const uint32_t loads[]{0x39400020, 0xd503201f, 0xd65f03c0};
        auto unwind = FindArm64Block(0, 0x4000, 0, 0, {reinterpret_cast<const uint8_t*>(loads), sizeof(loads)});
        // Collide in one four-way bucket without filling the entire cache.
        for (unsigned i = 0; i < 32; ++i)
            STONE_CHECK(FindArm64Block(0xface, 0x2000 + (uint64_t{i} << 32), 0, 0, source));
        const auto full = GetArm64BlockStatistics();
        STONE_CHECK(full.entries <= 32768 && full.evictions > before.evictions);
        std::array<std::exception_ptr, 4> errors{};
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < errors.size(); ++worker)
            workers.emplace_back([&, worker] {
                try {
                    for (unsigned i = 0; i < 256; ++i) {
                        errno = ERANGE;
                        SetLastError(ERROR_ACCESS_DENIED);
                        auto block = FindArm64Block(0xbead, 0x1000 + (i % 8) * 8, 0, i, source);
                        STONE_CHECK(block && errno == ERANGE && GetLastError() == ERROR_ACCESS_DENIED);
                        std::array<uint64_t, 31> registers{};
                        Arm64BlockContext context;
                        context.registers = registers.data();
                        block->Run(context);
                        pinned->Run(context);
                        STONE_CHECK(registers[0] == 42);
                        BlockMemoryFixture fixture;
                        context.owner = &fixture;
                        context.memory = BlockMemoryCallback;
                        unwind->Run(context);
                        STONE_CHECK(registers[0] == 7 && fixture.trace && fixture.rx && fixture.unwind);
                        if (!(i % 17))
                            ClearArm64BlockCache();
                        STONE_CHECK(errno == ERANGE && GetLastError() == ERROR_ACCESS_DENIED);
                    }
                }
                catch (...) {
                    errors[worker] = std::current_exception();
                }
            });
        for (auto& worker : workers)
            worker.join();
        for (const auto& error : errors)
            if (error)
                std::rethrow_exception(error);
        STONE_CHECK(GetArm64BlockStatistics().failures == before.failures);
    });
}

void BenchmarkArm64Blocks(Suite& suite)
{
    const auto memory = AllocateArm64GuestMemory(262144, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t p;

        ~Free()
        {
            FreeArm64GuestMemory(p);
        }
    } free{memory};

    std::memset(reinterpret_cast<void*>(memory), 1, 262144);
    const auto run = [&](const char* name, std::span<const uint32_t> words, uint64_t iterations, uint64_t expected) {
        suite.Run(name, [&] {
            auto bytes = MakeArm64TlsElf();
            std::memcpy(bytes.data() + 0x1100, words.data(), words.size_bytes());
            ElfImage image(bytes, ElfImage::Machine::Arm64, true);
            image.Protect(ElfImage::Execution::Translated);
            const Arm64Argument arguments[]{{'J', iterations}, {'J', memory}};
            LARGE_INTEGER frequency, start, finish;
            QueryPerformanceFrequency(&frequency);
            const auto before = GetArm64BlockStatistics();
            QueryPerformanceCounter(&start);
            const auto result =
                RunArm64Function(image, image.LoadBias() + 0x1100, 'J', {}, nullptr, nullptr, arguments);
            QueryPerformanceCounter(&finish);
            const auto after = GetArm64BlockStatistics();
            STONE_CHECK(result == expected);
            std::printf(
                "arm64-benchmark %s enabled=%d iterations=%llu elapsed-us=%llu native-calls=%llu native-instructions=%llu\n",
                name, Arm64BlockJitEnabled(), iterations,
                (finish.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart, after.executions - before.executions,
                after.instructions - before.instructions);
        });
    };
    const uint32_t integers[]{0xf1000400, 0x54ffffe1, 0xd65f03c0};
    run("integer-conditional-loop", integers, 1000000, 0);
    const uint32_t bytes[]{0x39400023, 0x91000421, 0x0b030042, 0xf1000400, 0x54ffff81, 0x2a0203e0, 0xd65f03c0};
    run("guarded-byte-sum", bytes, 262144, 262144);
    *reinterpret_cast<uint64_t*>(memory) = 0;
    const uint32_t stores[]{0xf9400022, 0x91000442, 0xf9000022, 0xf1000400, 0x54ffff81, 0xaa0203e0, 0xd65f03c0};
    run("guarded-load-store-loop", stores, 262144, 262144);
}

}
