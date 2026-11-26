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
#include <xmmintrin.h>

#include "subsystem/support/native-checks.h"

namespace stone::windows::test::checks {
thread_local const ArtGsCode* recoveringArtGs = nullptr;
thread_local unsigned artTlsFaults = 0;
thread_local const ElfImage* recoveringFsImage = nullptr;
thread_local bool scalarFsRecovery = false;
thread_local bool forceFsRetry = false;

LONG CALLBACK RecoverTestArtTls(EXCEPTION_POINTERS* exception)
{
    if (!recoveringArtGs)
        return EXCEPTION_CONTINUE_SEARCH;
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        recoveringArtGs->RecoverTlsLoad(*exception->ContextRecord)) {
        ++artTlsFaults;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        recoveringArtGs->Dispatch(reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress),
                                  *exception->ContextRecord))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK RecoverTestFs(EXCEPTION_POINTERS* exception)
{
    const auto& fault = *exception->ExceptionRecord;
    auto& context = *exception->ContextRecord;
    if (!recoveringFsImage || fault.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || fault.NumberParameters < 2 ||
        fault.ExceptionInformation[0] != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!recoveringFsImage->ContainsCode(context.Rip, 9))
        return EXCEPTION_CONTINUE_SEARCH;
    if (scalarFsRecovery) {
        // Model the old handler: correct value, but the next TLS read faults again.
        const auto* code = reinterpret_cast<const uint8_t*>(context.Rip);
        DWORD64* registers[]{&context.Rax, &context.Rcx, &context.Rdx, &context.Rbx, nullptr,      &context.Rbp,
                             &context.Rsi, &context.Rdi, &context.R8,  &context.R9,  &context.R10, &context.R11,
                             &context.R12, &context.R13, &context.R14, &context.R15};
        const auto index = ((code[3] >> 3) & 7) + ((code[1] & 4) ? 8 : 0);
        if (!registers[index] || fault.ExceptionInformation[1] > 56)
            return EXCEPTION_CONTINUE_SEARCH;
        *registers[index] = ElfImage::CurrentGuestThreadPointer()[fault.ExceptionInformation[1] / 8];
        context.Rip += 9;
    }
    else {
        if (!RecoverAndroidFsLoad(context, fault.ExceptionInformation[1]))
            return EXCEPTION_CONTINUE_SEARCH;
        if (forceFsRetry) {
            // Model losing FS immediately after WRFSBASE: the recovery's
            // pointer-based MOV must work, then the next guest read repairs FS.
            context.Rip += 5;
            forceFsRetry = false;
        }
    }
    ++artTlsFaults;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void CheckArtInspectionBindings()
{
    auto bytes = MakeElf(false);
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    constexpr char symbol[] = "InitializeNativeLoader";
    constexpr char dependency[] = "libnativeloader.so";
    put(0x1a0 + 1, symbol);
    put(0x260, dependency);
    put(0x118, uint64_t{0xc0 + sizeof(dependency)}); // DT_STRSZ
    put(0x180, std::array<uint64_t, 2>{1, 0xc0});    // DT_NEEDED
    put(0x190, std::array<uint64_t, 2>{0, 0});
    constexpr auto dynamic = sizeof(stone::elf::Elf64Ehdr) + sizeof(stone::elf::Elf64Phdr);
    put(dynamic + 32, uint64_t{160});
    put(dynamic + 40, uint64_t{160});
    const AndroidElfReader read = [&](const std::string& name) {
        STONE_CHECK(name == "libc.so" || name == "libfixture.so");
        return name == "libfixture.so" ? std::vector<uint8_t>(bytes.begin(), bytes.end()) : std::vector<uint8_t>{};
    };
    const auto report = InspectAndroidElf("libfixture.so", "", read, true);
    STONE_CHECK(report.libraries == 1 && report.missingImports.empty() && report.bindings.size() == 1);
    const auto& binding = report.bindings.front();
    STONE_CHECK(binding.consumer == "libfixture.so" && binding.symbol == symbol && binding.provider == dependency &&
                binding.hostBridge);
    // The temporary ART scope must not turn a later non-ART dlopen into a
    // successful host-library handle (RTLD_NOLOAD cannot load an ELF).
    STONE_CHECK(DlOpen(dependency, 4) == nullptr);
}

void CheckArtGsRewrite(bool symbols, bool narrow)
{
    const auto original = MakeElf(true);
    std::vector<uint8_t> bytes(original.begin(), original.end());
    bytes.resize(0x400);
    const auto put = [&](size_t offset, const auto& value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    put(sizeof(elf::Elf64Ehdr) + 4, uint32_t{5});
    put(sizeof(elf::Elf64Ehdr) + 32, uint64_t{bytes.size()});
    put(sizeof(elf::Elf64Ehdr) + 40, uint64_t{bytes.size()});
    put(40, uint64_t{0x2c0});
    put(58, uint16_t{sizeof(elf::Elf64Shdr)});
    put(60, uint16_t{symbols ? 3u : 2u});
    elf::Elf64Shdr section{};
    section.sh_type = 1;
    section.sh_flags = 6;
    section.sh_addr = section.sh_offset = 0x260;
    constexpr uint8_t wideCode[]{0x65, 0x48, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0, 0xc3};
    constexpr uint8_t narrowCode[]{0x65, 0x67, 0x48, 0x8b, 0x07, 0xc3}; // MOV RAX,GS:[EDI].
    const auto code = narrow ? std::span<const uint8_t>(narrowCode) : std::span<const uint8_t>(wideCode);
    section.sh_size = code.size();
    std::ranges::copy(code, bytes.begin() + 0x260);
    put(0x2c0 + sizeof(elf::Elf64Shdr), section);
    if (symbols) {
        section.sh_size = 0x30;
        put(0x2c0 + sizeof(elf::Elf64Shdr), section);
        // A data island in .text must not be decoded or patched as GS code.
        std::ranges::copy(code, bytes.begin() + 0x270);
        bytes[0x27a] = 0xff;
        bytes[0x27b] = 0xff;
        elf::Elf64Shdr table{};
        table.sh_type = 2;
        table.sh_offset = 0x380;
        table.sh_size = 2 * sizeof(elf::Elf64Sym);
        table.sh_entsize = sizeof(elf::Elf64Sym);
        put(0x340, table);
        elf::Elf64Sym function{};
        function.st_info = 2;
        function.st_shndx = 1;
        function.st_value = 0x260;
        function.st_size = code.size();
        put(0x398, function);
    }
    ElfImage image(bytes);
    ArtGsCode rewritten(image, bytes);
    STONE_CHECK(rewritten.Size() == 1);
    CONTEXT context{};
    STONE_CHECK(rewritten.Dispatch(image.LoadBias() + 0x260, context));
    image.Protect();
    std::array<uintptr_t, 16> thread{};
    thread[5] = 42;
    auto* tls = ElfImage::GuestThreadPointer();

    struct RestoreSlot {
        uintptr_t& slot;
        uintptr_t value;

        ~RestoreSlot()
        {
            slot = value;
        }
    } restore{tls[AndroidArtThreadSlot], tls[AndroidArtThreadSlot]};

    tls[AndroidArtThreadSlot] = reinterpret_cast<uintptr_t>(thread.data());
    if (narrow) {
        const auto read = Guest<uintptr_t (*)(uintptr_t)>(context.Rip, "(J)J");
        STONE_CHECK(read(0x1234567800000028) == 42);
    }
    else {
        const auto read = Guest<uintptr_t (*)()>(context.Rip, "()J");
        STONE_CHECK(read() == 42);
    }
}

void CheckArtTlsRecovery(bool narrow)
{
    const auto original = MakeElf(true);
    std::vector<uint8_t> bytes(original.begin(), original.end());
    bytes.resize(0x400);
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    put(sizeof(elf::Elf64Ehdr) + 4, uint32_t{5});
    put(sizeof(elf::Elf64Ehdr) + 32, uint64_t{bytes.size()});
    put(sizeof(elf::Elf64Ehdr) + 40, uint64_t{bytes.size()});
    put(40, uint64_t{0x2c0});
    put(58, uint16_t{sizeof(elf::Elf64Shdr)});
    put(60, uint16_t{2});
    // Clear FS as a Windows transition can do, then read the same ART slot twice.
    std::vector<uint8_t> code{0x49, 0xbb, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // MOV R11,sentinel.
                              0x49, 0xc7, 0xc2, 3,    0,    0,    0,                      // MOV R10,3.
                              0x31, 0xc0, 0xf3, 0x48, 0x0f, 0xae, 0xd0, 0xf9}; // XOR EAX,EAX; WRFSBASE RAX; STC.
    if (narrow)
        code.insert(code.end(), {0x65, 0x67, 0x48, 0x8b, 0x07, 0x65, 0x67, 0x48, 0x8b, 0x0f});
    else
        code.insert(code.end(),
                    {0x65, 0x48, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0, 0x65, 0x48, 0x8b, 0x0c, 0x25, 0x28, 0, 0, 0});
    code.insert(code.end(), {0x48, 0x11, 0xc8, 0x4c, 0x01, 0xd0, 0x4c, 0x31, 0xd8,
                             0xc3}); // ADC RAX,RCX; ADD RAX,R10; XOR RAX,R11; RET.
    std::ranges::copy(code, bytes.begin() + 0x260);
    elf::Elf64Shdr text{};
    text.sh_type = 1;
    text.sh_flags = 6;
    text.sh_addr = text.sh_offset = 0x260;
    text.sh_size = code.size();
    put(0x2c0 + sizeof(elf::Elf64Shdr), text);
    ElfImage image(bytes);
    ArtGsCode rewritten(image, bytes);
    image.Protect();
    STONE_CHECK(rewritten.Size() == 2);
    std::array<uintptr_t, 16> thread{};
    thread[5] = 42;
    auto* tls = ElfImage::GuestThreadPointer();
    const auto previous = tls[AndroidArtThreadSlot];
    tls[AndroidArtThreadSlot] = reinterpret_cast<uintptr_t>(thread.data());

    struct Restore {
        void* handler;
        uintptr_t* tls;
        uintptr_t previous;

        ~Restore()
        {
            recoveringArtGs = nullptr;
            if (handler)
                RemoveVectoredExceptionHandler(handler);
            tls[AndroidArtThreadSlot] = previous;
        }
    } restore{nullptr, tls, previous};

    restore.handler = AddVectoredExceptionHandler(1, RecoverTestArtTls);
    STONE_CHECK(restore.handler);
    recoveringArtGs = &rewritten;
    artTlsFaults = 0;
    const auto read = Guest<uintptr_t (*)(uintptr_t)>(image.LoadBias() + 0x260, "(J)J");
    STONE_CHECK(read(0x1234567800000028) == (88ull ^ 0x1122334455667788ull));
    std::cout << "ART TLS recovery faults=" << artTlsFaults << " for two consecutive reads\n";
    STONE_CHECK(artTlsFaults == 1);
}

void CheckDynamicArtGs()
{
    constexpr uint8_t original[]{
        0xf2, 0x0f, 0x10, 0x05, 25,   0,    0,    0, // MOVSD XMM0,[RIP+literal].
        0xf9, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0,    0x48, 0x83, 0xd0, 0,
        0xc3, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x30, 0, 0, 0,    0xc3, // Cold catch-like entry without an ordinary branch.
        0x65, 0x48, 0x8b, 0x04, 0x25, 0x30, 0,    0, 0, 0xc3, 0xff, 0xff, 0xff}; // Referenced literal includes a GS lookalike.
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    STONE_CHECK(memory);
    const auto release = [](uint8_t* bytes) {
        VirtualFree(bytes, 0, MEM_RELEASE);
    };
    const std::unique_ptr<uint8_t, decltype(release)> allocation(memory, release);
    std::memcpy(memory, original, sizeof(original));
    DWORD previous;
    STONE_CHECK(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &previous));
    ArtGsCode rewritten(std::span<uint8_t>{memory, sizeof(original)});
    STONE_CHECK(rewritten.Size() == 2 && rewritten.MatchesPublishedCode() && memory[9] == 0xe9 && memory[33] == 0x65);
    std::array<uintptr_t, 16> thread{};
    auto* tls = ElfImage::GuestThreadPointer();

    struct Restore {
        void* handler;
        uintptr_t* tls;
        uintptr_t previous;

        ~Restore()
        {
            recoveringArtGs = nullptr;
            RemoveVectoredExceptionHandler(handler);
            tls[AndroidArtThreadSlot] = previous;
        }
    } restore{AddVectoredExceptionHandler(1, RecoverTestArtTls), tls, tls[AndroidArtThreadSlot]};

    STONE_CHECK(restore.handler);
    tls[AndroidArtThreadSlot] = reinterpret_cast<uintptr_t>(thread.data());
    recoveringArtGs = &rewritten;
    const auto run = Guest<uintptr_t (*)()>(reinterpret_cast<uintptr_t>(memory), "()J");
    const auto cold = Guest<uintptr_t (*)()>(reinterpret_cast<uintptr_t>(memory + 23), "()J");
    thread[5] = 41;
    STONE_CHECK(run() == 42);
    thread[6] = 77;
    STONE_CHECK(cold() == 77);
    thread[5] = 99;
    STONE_CHECK(run() == 100);
    STONE_CHECK(VirtualProtect(memory, 4096, PAGE_READWRITE, &previous));
    std::memcpy(memory, original, sizeof(original));
    STONE_CHECK(!rewritten.MatchesPublishedCode());
    STONE_CHECK(VirtualProtect(memory, 4096, previous, &previous));
    recoveringArtGs = nullptr;
    ArtGsCode reused(std::span<uint8_t>{memory, sizeof(original)});
    recoveringArtGs = &reused;
    STONE_CHECK(reused.Size() == 2 && reused.MatchesPublishedCode() && run() == 100 && cold() == 77);
}

void CheckDynamicArtSwitch()
{
    constexpr uint8_t original[]{
        0x83, 0xff, 1,    0x77, 36,                                 // CMP EDI,1; JA default.
        0x4c, 0x8d, 0x1d, 32,   0,    0,    0,                      // LEA R11,[RIP+table].
        0x49, 0x63, 0x04, 0xbb, 0x4c, 0x01, 0xd8, 0xff, 0xe0,       // MOVSXD RAX,[R11+RDI*4]; ADD RAX,R11; JMP RAX.
        0x65, 0x48, 0x8b, 0x04, 0x25, 0x28, 0,    0,    0,    0xc3, // case 0: GS:[40].
        0x65, 0x48, 0x8b, 0x04, 0x25, 0x30, 0,    0,    0,    0xc3, // case 1: GS:[48].
        0x31, 0xc0, 0xc3,                                           // default: zero.
        0xe9, 0xff, 0xff, 0xff, 0xf3, 0xff, 0xff, 0xff};            // Relative table targets.
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    STONE_CHECK(memory);
    const auto release = [](uint8_t* bytes) {
        VirtualFree(bytes, 0, MEM_RELEASE);
    };
    const std::unique_ptr<uint8_t, decltype(release)> allocation(memory, release);
    std::memcpy(memory, original, sizeof(original));
    DWORD previous;
    STONE_CHECK(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &previous));
    ArtGsCode rewritten(std::span<uint8_t>{memory, sizeof(original)});
    STONE_CHECK(rewritten.Size() == 2 && !std::memcmp(memory + 44, original + 44, 8));
    std::array<uintptr_t, 16> thread{};
    thread[5] = 42;
    thread[6] = 99;
    auto* tls = ElfImage::GuestThreadPointer();

    struct Restore {
        void* handler;
        uintptr_t* tls;
        uintptr_t previous;

        ~Restore()
        {
            recoveringArtGs = nullptr;
            RemoveVectoredExceptionHandler(handler);
            tls[AndroidArtThreadSlot] = previous;
        }
    } restore{AddVectoredExceptionHandler(1, RecoverTestArtTls), tls, tls[AndroidArtThreadSlot]};

    STONE_CHECK(restore.handler);
    tls[AndroidArtThreadSlot] = reinterpret_cast<uintptr_t>(thread.data());
    recoveringArtGs = &rewritten;
    const auto run = Guest<uintptr_t (*)(uintptr_t)>(reinterpret_cast<uintptr_t>(memory), "(J)J");
    STONE_CHECK(run(0) == 42 && run(1) == 99 && run(2) == 0 && run(UINT32_MAX) == 0);
}

void CheckArtGsLoadRegisters(unsigned kind)
{
    const auto original = MakeElf(true);
    std::vector<uint8_t> bytes(original.begin(), original.end());
    bytes.resize(0x400);
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    put(sizeof(elf::Elf64Ehdr) + 4, uint32_t{5});
    put(sizeof(elf::Elf64Ehdr) + 32, uint64_t{bytes.size()});
    put(sizeof(elf::Elf64Ehdr) + 40, uint64_t{bytes.size()});
    put(40, uint64_t{0x2c0});
    put(58, uint16_t{sizeof(elf::Elf64Shdr)});
    put(60, uint16_t{2});
    std::vector<uint8_t> code;
    if (kind < 2) {
        code = {0x49, 0xba, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}; // R10 sentinel.
        if (kind == 1)
            code.insert(code.end(), {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff}); // RAX=-1.
        code.push_back(0xf9); // STC: the MOV and trampoline must preserve carry.
        if (kind == 0)
            code.insert(code.end(),
                        {0x65, 0x4c, 0x8b, 0x1c, 0x25, 0x28, 0, 0, 0, 0x4c, 0x89, 0xd8}); // R11=GS:[40]; RAX=R11.
        else
            code.insert(code.end(), {0x65, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0});   // EAX=GS:[40], zero extending.
        code.insert(code.end(), {0x48, 0x83, 0xd0, 0, 0x4c, 0x31, 0xd0, 0xc3}); // ADC RAX,0; XOR RAX,R10; RET.
    }
    else if (kind == 2) {
        code = {0xf9, 0x65, 0x48, 0x8b, 0x3f, 0x48, 0x89,
                0xf8, 0x48, 0x83, 0xd0, 0,    0xc3}; // RDI=GS:[RDI]; RAX=RDI+carry.
    }
    else if (kind == 3) {
        code = {0xb9, 0x80, 0x96, 0x98, 0,    0x65, 0x48, 0x8b, 0x04, 0x25,
                0x28, 0,    0,    0,    0xff, 0xc9, 0x75, 0xf3, 0xc3}; // Ten million loads.
    }
    else
        code = {0x65, 0x48, 0x8b, 0x24, 0x25, 0x28, 0, 0, 0, 0xc3}; // MOV RSP,GS:[40] stays unsupported.
    std::ranges::copy(code, bytes.begin() + 0x260);
    elf::Elf64Shdr text{};
    text.sh_type = 1;
    text.sh_flags = 6;
    text.sh_addr = text.sh_offset = 0x260;
    text.sh_size = code.size();
    put(0x2c0 + sizeof(elf::Elf64Shdr), text);
    ElfImage image(bytes);
    if (kind == 4) {
        bool rejected = false;
        try {
            ArtGsCode rewritten(image, bytes);
        }
        catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()).find("stack pointer") != std::string_view::npos;
        }
        STONE_CHECK(rejected);
        return;
    }
    ArtGsCode rewritten(image, bytes);
    image.Protect();
    STONE_CHECK(rewritten.Size() == 1);
    constexpr uint64_t value = 0x8877665544332211ull, sentinel = 0x1122334455667788ull;
    std::array<uintptr_t, 16> thread{};
    thread[5] = value;
    auto* tls = ElfImage::GuestThreadPointer();

    struct Restore {
        void* handler = nullptr;
        uintptr_t* tls;
        uintptr_t previous;

        ~Restore()
        {
            recoveringArtGs = nullptr;
            if (handler)
                RemoveVectoredExceptionHandler(handler);
            tls[AndroidArtThreadSlot] = previous;
        }
    } restore{nullptr, tls, tls[AndroidArtThreadSlot]};

    tls[AndroidArtThreadSlot] = reinterpret_cast<uintptr_t>(thread.data());
    restore.handler = AddVectoredExceptionHandler(1, RecoverTestArtTls);
    STONE_CHECK(restore.handler);
    recoveringArtGs = &rewritten;
    const auto read = Guest<uintptr_t (*)(uintptr_t)>(image.LoadBias() + 0x260, "(J)J");
    const auto start = std::chrono::steady_clock::now();
    const auto result = read(40);
    if (kind < 2)
        STONE_CHECK(result == (((kind == 1 ? (value & 0xffffffffull) : value) + 1) ^ sentinel));
    else
        STONE_CHECK(result == value + (kind == 2 ? 1 : 0));
    if (kind == 3)
        std::cout
            << "ART GS ten million loads "
            << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()
            << " us\n";
}

void CheckFsTlsRecovery(uint8_t offset, bool high)
{
    const auto original = MakeElf(true);
    std::vector<uint8_t> bytes(original.begin(), original.end());
    bytes.resize(0x400);
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    put(sizeof(elf::Elf64Ehdr) + 4, uint32_t{5});
    put(sizeof(elf::Elf64Ehdr) + 32, uint64_t{bytes.size()});
    put(sizeof(elf::Elf64Ehdr) + 40, uint64_t{bytes.size()});
    std::vector<uint8_t> code{0x49, 0xbb, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x49,
                              0xc7, 0xc2, 3,    0,    0,    0,    0x4c, 0x89, 0x5c, 0x24, 0x80, // Red-zone marker.
                              0x31, 0xc0, 0xf3, 0x48, 0x0f, 0xae, 0xd0, 0xf9}; // Clear FS and set carry.
    code.insert(code.end(), {0x64, static_cast<uint8_t>(high ? 0x4c : 0x48), 0x8b, 0x04, 0x25, offset, 0, 0, 0});
    code.insert(code.end(), {0x64, 0x48, 0x8b, 0x0c, 0x25, offset, 0, 0, 0});
    if (high)
        code.insert(code.end(), {0x4c, 0x89, 0xc0}); // MOV RAX,R8.
    code.insert(code.end(), {0x48, 0x11, 0xc8, 0x4c, 0x01, 0xd0, 0x4c, 0x31, 0xd8, 0x4c,
                             0x39, 0x5c, 0x24, 0x80, 0x75, 0x01, 0xc3, 0x31, 0xc0, 0xc3});
    std::ranges::copy(code, bytes.begin() + 0x260);
    ElfImage image(bytes);
    image.Protect();
    auto* tls = ElfImage::GuestThreadPointer();

    struct Restore {
        void* handler;
        uintptr_t* tls;
        uintptr_t previous;

        ~Restore()
        {
            recoveringFsImage = nullptr;
            if (handler)
                RemoveVectoredExceptionHandler(handler);
            tls[7] = previous;
        }
    } restore{nullptr, tls, tls[7]};

    tls[7] = 42;
    const auto expected = ((static_cast<uint64_t>(tls[offset / 8]) * 2 + 4) ^ 0x1122334455667788ull);
    restore.handler = AddVectoredExceptionHandler(1, RecoverTestFs);
    STONE_CHECK(restore.handler);
    recoveringFsImage = &image;
    const auto read = Guest<uintptr_t (*)()>(image.LoadBias() + 0x260, "()J");
    for (unsigned mode : {0u, 1u, 2u, 3u}) {
        scalarFsRecovery = mode == 0;
        forceFsRetry = mode == 3;
        artTlsFaults = 0;
        STONE_CHECK(read() == expected);
        STONE_CHECK(artTlsFaults == (mode == 0 || mode == 3 ? 2u : 1u));
    }
}

void CheckArtGsControl(bool tail, bool narrow)
{
    const auto original = MakeElf(true);
    std::vector<uint8_t> bytes(original.begin(), original.end());
    bytes.resize(0x400);
    const auto put = [&](size_t offset, const auto& value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    put(sizeof(elf::Elf64Ehdr) + 4, uint32_t{5});
    put(sizeof(elf::Elf64Ehdr) + 32, uint64_t{bytes.size()});
    put(sizeof(elf::Elf64Ehdr) + 40, uint64_t{bytes.size()});
    put(40, uint64_t{0x2c0});
    put(58, uint16_t{sizeof(elf::Elf64Shdr)});
    put(60, uint16_t{2});
    elf::Elf64Shdr text{};
    text.sh_type = 1;
    text.sh_flags = 6;
    text.sh_addr = text.sh_offset = 0x260;
    text.sh_size = 0x60;
    put(0x2c0 + sizeof(elf::Elf64Shdr), text);
    // Set the two scratch registers and carry flag; the GS callee observes all three.
    const uint8_t prefix[]{0x49, 0xbb, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
                           0x11, 0x49, 0xc7, 0xc2, 3,    0,    0,    0,    0xf9};
    std::ranges::copy(prefix, bytes.begin() + 0x260);
    const size_t operation = 0x260 + sizeof(prefix);
    std::vector<uint8_t> instruction =
        narrow ? std::vector<uint8_t>{0x65, 0x67, 0xff, static_cast<uint8_t>(tail ? 0xa7 : 0x97), 0, 0, 0, 0}
               : std::vector<uint8_t>{0x65, 0xff, static_cast<uint8_t>(tail ? 0x24 : 0x14), 0x25, 0x28, 0, 0, 0};
    std::ranges::copy(instruction, bytes.begin() + operation);
    bytes[operation + instruction.size()] = 0xc3;
    std::vector<uint8_t> callee{0x4c, 0x89, 0xd8, 0x48, 0x83, 0xd0, 0, 0x4c, 0x01, 0xd0};
    if (!tail) {
        const auto relative = static_cast<int32_t>(operation + instruction.size() - (0x2a0 + callee.size() + 7));
        callee.insert(callee.end(), {0x48, 0x8d, 0x0d}); // LEA RCX,[RIP+original-return-PC].
        const auto* displacement = reinterpret_cast<const uint8_t*>(&relative);
        callee.insert(callee.end(), displacement, displacement + sizeof(relative));
        callee.insert(callee.end(), {0x48, 0x39, 0x0c, 0x24, 0x75, 0x01, 0xc3, 0x31, 0xc0});
    }
    callee.push_back(0xc3);
    std::ranges::copy(callee, bytes.begin() + 0x2a0);
    ElfImage image(bytes);
    ArtGsCode rewritten(image, bytes);
    STONE_CHECK(rewritten.Size() == 1 && image.CodeAt(image.LoadBias() + operation, 1)[0] == 0xe9);
    image.Protect();
    std::array<uintptr_t, 16> thread{};
    thread[5] = image.LoadBias() + 0x2a0;
    auto* tls = ElfImage::GuestThreadPointer();

    struct Restore {
        uintptr_t& slot;
        uintptr_t value;

        ~Restore()
        {
            slot = value;
        }
    } restore{tls[AndroidArtThreadSlot], tls[AndroidArtThreadSlot]};

    tls[AndroidArtThreadSlot] = reinterpret_cast<uintptr_t>(thread.data());
    const auto invoke = Guest<uintptr_t (*)(uintptr_t)>(image.LoadBias() + 0x260, "(J)J");
    STONE_CHECK(invoke(0x1234567800000028) == 0x112233445566778c);
}
}
