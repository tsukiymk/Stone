#include "../support/test-suite.h"

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

void CheckElfImports()
{
    // LLD can emit an empty TLS segment in an Android PIE (dex2oat64).
    auto emptyTls = MakeElf(true);
    const uint16_t count = 3;
    std::memcpy(emptyTls.data() + 56, &count, sizeof(count));
    const elf::Elf64Phdr tls{7, 4, 0x300, 0x300, 0, 0, 0, 64};
    std::memcpy(emptyTls.data() + sizeof(elf::Elf64Ehdr) + 2 * sizeof(tls), &tls, sizeof(tls));
    ElfImage empty(emptyTls, ElfImage::Machine::X64, true);
    STONE_CHECK(!empty.TlsModule() && !empty.ThreadLocalAddress(0));
    // A nonempty file template with zero memory size remains invalid.
    auto invalidTls = tls;
    invalidTls.p_filesz = 1;
    std::memcpy(emptyTls.data() + sizeof(elf::Elf64Ehdr) + 2 * sizeof(tls), &invalidTls, sizeof(tls));
    bool invalid = false;
    try {
        ElfImage rejected(emptyTls, ElfImage::Machine::X64, true);
    }
    catch (const std::runtime_error&) {
        invalid = true;
    }
    STONE_CHECK(invalid);
    const auto required = MakeElf(false);
    constexpr auto symbol = "stone_fixture_import";
    constexpr uintptr_t address = 0x12345678;
    ElfImage resolved(required);
    resolved.Relocate([=](std::string_view name) -> uintptr_t {
        return name == symbol ? address : 0;
    });
    STONE_CHECK(*reinterpret_cast<const uintptr_t*>(resolved.LoadBias() + 0x250) == address);

    ElfImage missing(required);
    std::set<std::string> unresolved;
    const auto emptyResolver = [](std::string_view) -> uintptr_t {
        return 0;
    };
    missing.Relocate(emptyResolver, &unresolved);
    STONE_CHECK(unresolved == std::set<std::string>{symbol});
    ElfImage weak(MakeElf(true));
    unresolved.clear();
    weak.Relocate(emptyResolver, &unresolved);
    STONE_CHECK(unresolved.empty());
    STONE_CHECK(*reinterpret_cast<const uintptr_t*>(weak.LoadBias() + 0x250) == 0);
}

void CheckArm64TlsDescriptor()
{
    ElfImage image(MakeArm64TlsElf(), ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto run = [&] {
        return RunArm64Initializer(
            image, image.LoadBias() + 0x1100, 64,
            [](uintptr_t) -> std::string_view {
                return {};
            },
            nullptr);
    };
    STONE_CHECK(run() == 127 && run() == 128);
    uint64_t other = 0;
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            other = run();
        }
        catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();
    if (failure)
        std::rethrow_exception(failure);
    STONE_CHECK(other == 127 && run() == 129);
    auto* block = static_cast<uint8_t*>(image.ThreadLocalAddress(0));
    STONE_CHECK(reinterpret_cast<uintptr_t>(block) % 16 == 0);
    STONE_CHECK(*reinterpret_cast<const uint64_t*>(block + 16) == 0);
    STONE_CHECK(image.ThreadLocalDataAt(reinterpret_cast<uintptr_t>(block) + 31, 1) == block + 31);
    STONE_CHECK(!image.ThreadLocalDataAt(reinterpret_cast<uintptr_t>(block) + 31, 2));
    STONE_CHECK(!image.ThreadLocalDataAt(reinterpret_cast<uintptr_t>(block) - 1, 1));
    const auto* descriptor = reinterpret_cast<const uintptr_t*>(image.LoadBias() + 0x280);
    uintptr_t address = 0;
    STONE_CHECK(!image.ResolveTlsDescriptor(descriptor[1] + 1, address));
    STONE_CHECK(image.ResolveTlsDescriptor(descriptor[1], address) &&
                address == reinterpret_cast<uintptr_t>(block + 8));
}

void CheckArm64ExternalTls()
{
    auto bytes = MakeArm64TlsElf();
    ElfImage provider(bytes, ElfImage::Machine::Arm64, true);
    bytes[0x1de] = 0; // Undefined TLS symbol in the consumer.
    ElfImage consumer(bytes, ElfImage::Machine::Arm64, true);
    consumer.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr,
        [&](std::string_view name) {
            return provider.FindTlsExport(name);
        },
        {}, Arm64TlsDescriptorResolver());
    const auto* descriptor = reinterpret_cast<const uintptr_t*>(consumer.LoadBias() + 0x280);
    uintptr_t address = 0;
    STONE_CHECK(consumer.ResolveTlsDescriptor(descriptor[1], address));
    STONE_CHECK(address == reinterpret_cast<uintptr_t>(provider.ThreadLocalAddress(8)));
    bytes[0x1dc] = 0x26; // Undefined weak TLS resolves to the null address.
    ElfImage weak(bytes, ElfImage::Machine::Arm64, true);
    weak.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    descriptor = reinterpret_cast<const uintptr_t*>(weak.LoadBias() + 0x280);
    STONE_CHECK(weak.ResolveTlsDescriptor(descriptor[1], address) && address == 0);
    // Reusing an ElfImage object's address must not reuse its old TLS template.
    std::optional<ElfImage> reused;
    reused.emplace(MakeArm64TlsElf(), ElfImage::Machine::Arm64, true);
    STONE_CHECK(*static_cast<uint64_t*>(reused->ThreadLocalAddress(8)) == 41);
    reused.reset();
    bytes = MakeArm64TlsElf();
    const uint64_t replacement = 73;
    std::memcpy(bytes.data() + 0x408, &replacement, sizeof(replacement));
    reused.emplace(bytes, ElfImage::Machine::Arm64, true);
    STONE_CHECK(*static_cast<uint64_t*>(reused->ThreadLocalAddress(8)) == replacement);
}

void CheckArm64InvalidTls()
{
    const auto reject = [](auto action) {
        bool rejected = false;
        try {
            action();
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
    };
    reject([] {
        ElfImage image(MakeArm64TlsElf(), ElfImage::Machine::Arm64);
    });
    reject([] {
        ElfImage image(MakeArm64TlsElf(), ElfImage::Machine::Arm64, true);
        image.Relocate([](std::string_view) -> uintptr_t {
            return 0;
        });
    });
    for (const auto addend : {int64_t{24}, int64_t{-16}})
        reject([&] {
            auto bytes = MakeArm64TlsElf();
            std::memcpy(bytes.data() + 0x230, &addend, sizeof(addend));
            ElfImage image(bytes, ElfImage::Machine::Arm64, true);
            image.Relocate(
                [](std::string_view) -> uintptr_t {
                    return 0;
                },
                nullptr, {}, {}, Arm64TlsDescriptorResolver());
        });
    reject([] {
        auto bytes = MakeArm64TlsElf();
        const uint64_t offset = 0xff8;
        std::memcpy(bytes.data() + 0x220, &offset, sizeof(offset));
        ElfImage image(bytes, ElfImage::Machine::Arm64, true);
        image.Relocate(
            [](std::string_view) -> uintptr_t {
                return 0;
            },
            nullptr, {}, {}, Arm64TlsDescriptorResolver());
    });
}

void CheckArm64InspectionRestoresBackend()
{
    const auto reader = [](const std::string& name) {
        return name == "libarm64-fixture.so" ? MakeArm64TlsElf() : std::vector<uint8_t>{};
    };
    const auto report = InspectAndroidElf("libarm64-fixture.so", "", reader, false, ElfImage::Machine::Arm64);
    STONE_CHECK(report.libraries == 1 && report.missingImports.empty());
    bool rejected = false;
    try {
        InspectAndroidElf("libarm64-fixture.so", "missing-entry", reader, false, ElfImage::Machine::Arm64);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    const auto original = MakeElf(true);
    const auto native = InspectAndroidElf("libx64-fixture.so", "", [&](const std::string& name) {
        return name == "libx64-fixture.so" ? std::vector<uint8_t>(original.begin(), original.end())
                                           : std::vector<uint8_t>{};
    });
    STONE_CHECK(native.libraries == 1 && native.missingImports.empty());
}

void CheckArm64StaticTls()
{
    auto bytes = MakeArm64StaticTlsElf();
    ElfImage first(bytes, ElfImage::Machine::Arm64, true);
    size_t used = 0;
    first.AssignStaticTls(used);
    first.Relocate([](std::string_view) -> uintptr_t {
        return 0;
    });
    first.Protect(ElfImage::Execution::Translated);
    const ElfImage* initial[]{&first};
    ElfImage::PublishStaticTls(initial);
    auto* native = ElfImage::GuestThreadPointer();
    auto* arm = ElfImage::GuestArm64ThreadPointer();
    const auto originalNative = native[7], originalArm = arm[7];

    struct Restore {
        uintptr_t* native;
        uintptr_t* arm;
        uintptr_t n, a;

        ~Restore()
        {
            native[7] = n;
            arm[7] = a;
        }
    } restore{native, arm, originalNative, originalArm};

    native[7] = 0x5566;
    STONE_CHECK(native != arm && reinterpret_cast<uintptr_t>(arm) % 64 == 0);
    const auto tlsOffset = *reinterpret_cast<const uintptr_t*>(first.LoadBias() + 0x280);
    STONE_CHECK(tlsOffset >= 64 && tlsOffset + 8 <= used);
    STONE_CHECK(first.ThreadLocalAddress(8) == reinterpret_cast<uint8_t*>(arm) + tlsOffset);
    const auto mainData = reinterpret_cast<uintptr_t>(first.ThreadLocalAddress(8, false));
    STONE_CHECK(mainData && first.ThreadLocalDataAt(mainData, 8) == reinterpret_cast<uint8_t*>(mainData));
    STONE_CHECK(!first.ThreadLocalDataAt(mainData, SIZE_MAX));
    const auto run = [](const ElfImage& image, size_t offset = 0x1100) {
        return RunArm64Initializer(
            image, image.LoadBias() + offset, 64,
            [](uintptr_t) -> std::string_view {
                return {};
            },
            nullptr);
    };
    STONE_CHECK(run(first) == 42 && run(first) == 43);
    STONE_CHECK(run(first, 0x1180) == 0x1122 && arm[7] == 0x1122 && native[7] == 0x5566);
    const auto pointer = reinterpret_cast<uintptr_t>(arm);
    STONE_CHECK(ElfImage::Arm64ThreadControlDataAt(pointer - 24, 24));
    STONE_CHECK(!ElfImage::Arm64ThreadControlDataAt(pointer - 25, 1));
    STONE_CHECK(!ElfImage::Arm64ThreadControlDataAt(pointer + 63, 2));
    STONE_CHECK(!ElfImage::Arm64ThreadControlDataAt(reinterpret_cast<uintptr_t>(native), 1));
    bytes = MakeArm64StaticTlsElf();
    const uint64_t value = 73;
    std::memcpy(bytes.data() + 0x408, &value, sizeof(value));
    const uint64_t alignment = 256;
    std::memcpy(bytes.data() + sizeof(elf::Elf64Ehdr) + 2 * sizeof(elf::Elf64Phdr) + 48, &alignment, sizeof(alignment));
    ElfImage second(bytes, ElfImage::Machine::Arm64, true);
    second.AssignStaticTls(used);
    second.Relocate([](std::string_view) -> uintptr_t {
        return 0;
    });
    second.Protect(ElfImage::Execution::Translated);
    const auto ready = CreateEventW(nullptr, TRUE, FALSE, nullptr), go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    STONE_CHECK(ready && go);
    uint64_t before = 0, after = 0, added = 0;
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            STONE_CHECK(!first.ThreadLocalAddress(8, false));
            STONE_CHECK(!first.ThreadLocalDataAt(mainData, 8));
            before = run(first);
            SetEvent(ready);
            WaitForSingleObject(go, INFINITE);
            after = run(first);
            added = run(second);
            const auto data = reinterpret_cast<uintptr_t>(second.ThreadLocalAddress(8, false));
            STONE_CHECK(data && second.ThreadLocalDataAt(data, 8) == reinterpret_cast<uint8_t*>(data));
            STONE_CHECK(!first.ThreadLocalDataAt(mainData, 8));
        }
        catch (...) {
            failure = std::current_exception();
            SetEvent(ready);
        }
    });

    struct Join {
        HANDLE ready, go;
        std::thread& worker;

        ~Join()
        {
            SetEvent(go);
            if (worker.joinable())
                worker.join();
            CloseHandle(ready);
            CloseHandle(go);
        }
    } join{ready, go, worker};

    STONE_CHECK(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
    const ElfImage* both[]{&first, &second, &first};
    ElfImage::PublishStaticTls(both); // Update a live worker without resetting first's data.
    STONE_CHECK(run(first) == 44 && run(second) == 74);
    STONE_CHECK(reinterpret_cast<uintptr_t>(second.ThreadLocalAddress(0)) % alignment == 0);
    ElfImage::PublishStaticTls(both);
    STONE_CHECK(run(first) == 45 && run(second) == 75);
    SetEvent(go);
    worker.join();
    if (failure)
        std::rethrow_exception(failure);
    STONE_CHECK(before == 42 && after == 43 && added == 74);
    char benchmark[2]{};
    if (GetEnvironmentVariableA("STONE_ARM64_TLS_BENCHMARK", benchmark, 2) == 1 && benchmark[0] == '1') {
        const ElfImage* images[]{&first, &second};
        const uintptr_t addresses[]{mainData, reinterpret_cast<uintptr_t>(second.ThreadLocalAddress(8, false))};
        constexpr unsigned iterations = 1048576;
        const auto begin = std::chrono::steady_clock::now();
        uint64_t total = 0;
        for (unsigned i = 0; i < iterations; ++i) {
            const auto* data = images[i & 1]->ThreadLocalDataAt(addresses[i & 1], 8);
            STONE_CHECK(data == reinterpret_cast<const uint8_t*>(addresses[i & 1]));
            total += *reinterpret_cast<const uint64_t*>(data);
        }
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
        STONE_CHECK(total == 60ull * iterations);
        std::cout << "ARM64 static TLS lookup benchmark: iterations=" << iterations << " total=" << total
                  << " us=" << us << '\n';
    }
}

void CheckArm64StaticTlsRejectedWithoutAllocation()
{
    ElfImage image(MakeArm64StaticTlsElf(), ElfImage::Machine::Arm64, true);
    bool rejected = false;
    try {
        image.Relocate([](std::string_view) -> uintptr_t {
            return 0;
        });
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
}

void CheckArm64IndirectRelocations()
{
    for (const bool relative : {false, true}) {
        auto bytes = MakeArm64TlsElf();
        const auto put = [&](size_t at, const auto& value) {
            std::memcpy(bytes.data() + at, &value, sizeof(value));
        };
        put(0x1dc, uint8_t{0x1a});
        put(0x1e0, uint64_t{0x1140}); // STT_GNU_IFUNC.
        put(0x220, elf::Elf64Rela{0x280, relative ? 1032 : (uint64_t{1} << 32) | 1025, relative ? 0x1140 : 0});
        put(0x1140, std::array<uint32_t, 2>{0x10000100, 0xd65f03c0}); // ADR x0,+32; RET.
        put(0x1160, std::array<uint32_t, 2>{0x52800540, 0xd65f03c0}); // MOV w0,#42; RET.
        ElfImage image(bytes, ElfImage::Machine::Arm64, true);
        image.Relocate([](std::string_view) -> uintptr_t {
            return 0;
        });
        STONE_CHECK(image.IndirectRelocationCount() == 1);
        const auto run = [&](uintptr_t entry) {
            return RunArm64Initializer(
                image, entry, 64,
                [](uintptr_t) -> std::string_view {
                    return {};
                },
                nullptr);
        };
        unsigned calls = 0;
        image.ResolveIndirectRelocations([&](uintptr_t entry) {
            ++calls;
            return run(entry);
        });
        image.Protect(ElfImage::Execution::Translated);
        const auto target = *reinterpret_cast<const uintptr_t*>(image.LoadBias() + 0x280);
        STONE_CHECK(calls == 1 && image.IndirectRelocationCount() == 0);
        STONE_CHECK(target == image.LoadBias() + 0x1160 && run(target) == 42);
        MEMORY_BASIC_INFORMATION memory{};
        STONE_CHECK(VirtualQuery(reinterpret_cast<const void*>(target), &memory, sizeof(memory)));
        STONE_CHECK(
            !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)));
    }
}

void CheckElfCodeBoundaries()
{
    const auto original = MakeElf(false);
    std::vector<uint8_t> bytes(original.begin(), original.end());
    bytes.resize(0x800);
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    put(sizeof(elf::Elf64Ehdr) + 4, uint32_t{5});
    put(sizeof(elf::Elf64Ehdr) + 32, uint64_t{bytes.size()});
    put(sizeof(elf::Elf64Ehdr) + 40, uint64_t{bytes.size()});
    put(0x128, uint64_t{0x300}); // DT_SYMTAB.
    put(0x148, uint64_t{0x500}); // DT_HASH.
    std::vector<elf::Elf64Sym> symbols(1);
    const auto add = [&](uint16_t section, uint64_t address, uint64_t size, uint8_t info = 0x12) {
        elf::Elf64Sym symbol{};
        symbol.st_name = 1;
        symbol.st_info = info;
        symbol.st_shndx = section;
        symbol.st_value = address;
        symbol.st_size = size;
        symbols.push_back(symbol);
    };
    add(1, 0x660, 0);
    add(1, 0x620, 16);
    add(1, 0x620, 0);
    add(1, 0x620, 8);
    add(1, 0x610, 4, 0x11); // Object symbols do not split code.
    add(0, 0x630, 4);
    add(0xfff1, 0x640, 4);     // Undefined and absolute symbols are excluded.
    add(1, 0x650, 0x20, 0x02); // Defined local functions still bound decoding.
    add(1, 0x645, 4, 0x1a);    // GNU IFUNC is not an ordinary function boundary.
    std::memcpy(bytes.data() + 0x300, symbols.data(), symbols.size() * sizeof(symbols[0]));
    put(0x500, uint32_t{1});
    put(0x504, static_cast<uint32_t>(symbols.size()));
    put(0x508, uint32_t{1});
    ElfImage image(bytes);
    const auto base = image.LoadBias();
    STONE_CHECK(image.CodeAt(base + 0x600, 0x200).size() == 0x20);
    STONE_CHECK(image.CodeAt(base + 0x620, 0x200).size() == 8);
    STONE_CHECK(image.CodeAt(base + 0x620, 3).size() == 3);
    STONE_CHECK(image.CodeAt(base + 0x621, 0x200).size() == 0x2f);
    STONE_CHECK(image.CodeAt(base + 0x630, 0x200).size() == 0x20);
    STONE_CHECK(image.CodeAt(base + 0x640, 0x200).size() == 0x10);
    STONE_CHECK(image.CodeAt(base + 0x650, 0x200).size() == 0x10);
    STONE_CHECK(image.CodeAt(base + 0x660, 0x200).size() == 0x1a0);
    STONE_CHECK(image.CodeAt(base + 0x7ff, 0x200).size() == 1);
    STONE_CHECK(image.CodeAt(base + 0x800, 1).empty());
}

void CheckElfInspection()
{
    auto bytes = MakeElf(false);
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    using stone::elf::Elf64Ehdr;
    using stone::elf::Elf64Phdr;
    // Export the existing symbol as a function in an executable load segment.
    put(sizeof(Elf64Ehdr) + 4, uint32_t{7});
    put(0x1c0 + 28, uint8_t{0x12});
    put(0x1c0 + 30, uint16_t{1});
    put(0x1c0 + 32, uint64_t{0x280});
    // Add a DT_INIT that traps if the inspector ever executes constructors.
    put(0x180, std::array<uint64_t, 2>{12, 0x280});
    put(0x190, std::array<uint64_t, 2>{0, 0});
    constexpr auto dynamicHeader = sizeof(Elf64Ehdr) + sizeof(Elf64Phdr);
    put(dynamicHeader + 32, uint64_t{160});
    put(dynamicHeader + 40, uint64_t{160});
    bytes[0x280] = 0xcc; // INT3.
    const auto report = InspectAndroidElf("libfixture.so", "stone_fixture_import", [&](const std::string& name) {
        return name == "libfixture.so" ? std::vector<uint8_t>(bytes.begin(), bytes.end()) : std::vector<uint8_t>{};
    });
    STONE_CHECK(report.libraries == 1 && report.indirectRelocations == 0 && report.missingImports.empty());
    STONE_CHECK(ElfImage(bytes).ExportNames() == std::vector<std::string>{"stone_fixture_import"});
    constexpr char mathSymbol[] = "sqrt";
    put(0x1a1, mathSymbol);
    put(0x118, uint64_t{1 + sizeof(mathSymbol)});
    const auto overlaps = InspectAndroidStubExports([&](const std::string& name) {
        return name == "libm.so" ? std::vector<uint8_t>(bytes.begin(), bytes.end()) : std::vector<uint8_t>{};
    });
    bool foundMath = false, foundUnavailable = false;
    for (const auto& overlap : overlaps) {
        if (overlap.library == "libm.so") {
            STONE_CHECK(overlap.imageAvailable && overlap.imagePreferred);
            STONE_CHECK(overlap.overlappingExports == std::vector<std::string>{mathSymbol});
            foundMath = true;
        }
        if (overlap.library == "libc.so")
            foundUnavailable = !overlap.imageAvailable;
    }
    STONE_CHECK(foundMath && foundUnavailable);
}
}
