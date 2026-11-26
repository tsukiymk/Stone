#include "loader/AndroidLoadedImage.h"
#include "cpu/arm64/Arm64ExecutionSession.h"
#include "cpu/arm64/Arm64ExecutionProfile.h"
#include "cpu/arm64/cache/Arm64BlockCache.h"
#include "cpu/arm64/Arm64BlockJit.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "loader/ElfImage.h"
#include "platform/windows/Arm64CacheStorage.h"
#include <windows.h>
#include <wincrypt.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

using namespace stone::windows;

namespace {
void Check(bool good, const char* message)
{
    if (!good)
        throw std::runtime_error(message);
}

void Codec()
{
    using namespace arm64_cache;
    std::vector<uint8_t> original(1024 * 1024);
    for (size_t i = 0; i < original.size(); ++i)
        original[i] = static_cast<uint8_t>(i % 251);
    const auto packed = Pack(original);
    Check(packed.size() < original.size() / 2 && Unpack(packed) == original, "compressed cache round trip failed");
    const std::vector<uint8_t> raw{0x53, 0x54, 0x42, 0x43, 1, 0, 0, 0};
    Check(Unpack(raw) == raw, "legacy raw payload changed");
    const auto rejects = [](std::vector<uint8_t> input) {
        try {
            Unpack(std::move(input));
            return false;
        }
        catch (const std::exception&) {
            return true;
        }
    };
    for (size_t size = 4; size < 16; ++size)
        Check(rejects({packed.begin(), packed.begin() + size}), "truncated header accepted");
    auto bad = packed;
    bad[4] = 2;
    Check(rejects(bad), "unknown compression version accepted");
    bad = packed;
    for (size_t i = 8; i < 12; ++i)
        bad[i] = 255;
    Check(rejects(bad), "unbounded expansion accepted");
    bad = packed;
    for (size_t i = 8; i < 12; ++i)
        bad[i] = 0;
    Check(rejects(bad), "empty expansion accepted");
    bad = packed;
    bad.push_back(0);
    Check(rejects(bad), "trailing compressed bytes accepted");
    bad = packed;
    bad.pop_back();
    Check(rejects(bad), "truncated compressed bytes accepted");
    bad = packed;
    bad[8] ^= 1;
    Check(rejects(bad), "wrong expanded length accepted");
    std::cout << "PASS bounded compressed and legacy cache payloads: " << original.size() << " -> " << packed.size()
              << " bytes\n";
}

void Inspect(const std::filesystem::path& directory)
{
    using namespace arm64_cache;
    const auto files = ListFiles(directory);
    Check(files.size() <= MaximumFiles, "inspection file budget exceeded");
    uint64_t diskBytes = 0;
    for (const auto& file : files)
        diskBytes += file.bytes;
    Check(diskBytes <= MaximumDisk, "inspection disk budget exceeded");
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const uint64_t page = system.dwPageSize;
    Check(page > 0, "inspection could not determine page size");
    std::cout
        << "key,diskBytes,artifactBytes,blocks,instructions,uniqueSourceBytes,sourceBytes,nativeBytes,pageBytesIfAllPublished,relocations\n";
    Arm64DiskCacheStatistics stats;
    for (const auto& file : files) {
        // Authenticated format inspection only. No code publication, cache
        // configuration, source authorization or writes are performed here.
        const auto bundle = Read(Open(file.path), file.key, stats, false);
        Check(bool(bundle), "inspection file disappeared");
        uint64_t instructions = 0, source = 0, native = 0, pages = 0, relocations = 0, unique = 0;
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        ranges.reserve(bundle->blocks.size());
        for (const auto& [key, saved] : bundle->blocks) {
            const auto& a = saved.artifact;
            instructions += a.instructions;
            source += a.source.size();
            native += a.code.size();
            relocations += a.relocations.size();
            pages += (a.code.size() + page - 1) / page * page;
            Check(key.first <= UINT64_MAX - uint64_t{a.instructions} * 4, "inspection source range overflow");
            ranges.emplace_back(key.first, key.first + uint64_t{a.instructions} * 4);
        }
        std::sort(ranges.begin(), ranges.end());
        uint64_t end = 0;
        for (const auto& range : ranges) {
            if (range.second > end) {
                unique += range.second - std::max(end, range.first);
                end = range.second;
            }
        }
        std::cout << file.path.stem().string() << ',' << file.bytes << ',' << bundle->bytes << ','
                  << bundle->blocks.size() << ',' << instructions << ',' << unique << ',' << source << ',' << native
                  << ',' << pages << ',' << relocations << '\n';
    }
    Check(!GetModuleHandleW(L"jvm.dll"), "inspection loaded a Windows JVM");
}

std::vector<uint8_t> Image(bool changed)
{
    using namespace stone::elf;
    std::vector<uint8_t> bytes(8192);
    const auto put = [&](size_t offset, const auto& value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    Elf64Ehdr header{};
    const uint8_t ident[]{0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(header.e_ident, ident, sizeof(ident));
    header.e_type = 3;
    header.e_machine = 183;
    header.e_version = 1;
    header.e_ehsize = sizeof(header);
    header.e_phoff = sizeof(header);
    header.e_phentsize = sizeof(Elf64Phdr);
    header.e_phnum = 3;
    put(0, header);
    put(64, Elf64Phdr{1, 4, 0, 0, 0, 4096, 4096, 4096});
    put(120, Elf64Phdr{1, 5, 4096, 4096, 0, 4096, 4096, 4096});
    constexpr uint64_t dynamic[][2]{{5, 0x300}, {10, 1}, {6, 0x320}, {11, 24}, {4, 0x380}, {0, 0}};
    put(176, Elf64Phdr{2, 4, 0x200, 0x200, 0, sizeof(dynamic), sizeof(dynamic), 8});
    put(0x200, dynamic);
    put(0x380, std::array<uint32_t, 4>{1, 1, 0, 0});
    put(0x1100, std::array<uint32_t, 4>{0x10000000, 0x90000001, 0xd503201f, 0x94000002}); // ADR, ADRP, NOP, BL.
    put(0x1180, std::array<uint32_t, 4>{0x58000080, 0xd503201f, 0xd503201f, 0xd65f03c0});
    put(0x1190, uint64_t{42});
    put(0x1200, std::array<uint32_t, 2>{0xf1000400, 0x54ffffe1});                         // SUBS, B.NE self-loop.
    put(0x1280, std::array<uint32_t, 4>{0x910043ff, 0xd503201f, 0xd503201f, 0xd65f03c0}); // guarded SP write.
    put(0x1300, std::array<uint32_t, 4>{0x4e228420, 0xd503201f, 0xd503201f, 0xd65f03c0}); // ADD v0.16b,v1.16b,v2.16b.
    put(0x1380, std::array<uint32_t, 4>{0x9a820020, 0xd503201f, 0xd503201f, 0xd65f03c0}); // CSEL x0,x1,x2,EQ.
    put(0x1400, std::array<uint32_t, 4>{0x39400020, 0x11000400, 0xd503201f, 0xd65f03c0});
    put(0x1480, std::array<uint32_t, 4>{0xd28000e0, 0xd503201f, 0xd503201f, 0xd65f03c0});
    put(0x1500, std::array<uint32_t, 4>{0xd2800120, 0xd503201f, 0xd503201f, 0xd65f03c0});
    bytes[0x600] = changed ? 1 : 0;
    return bytes;
}

struct Memory {
    const ElfImage* image;
    bool fault = false, unwind = false;
};

void Access(Arm64BlockContext* context, uint32_t, Arm64BlockMemory kind) noexcept
{
    auto& owner = *static_cast<Memory*>(context->owner);
    DWORD64 base = 0;
    owner.unwind = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(_ReturnAddress()), &base, nullptr) != nullptr;
    if (owner.fault) {
        context->exit = Arm64BlockExit::Failure;
        return;
    }
    if (kind == Arm64BlockMemory::StackArithmetic) {
        context->sp += 16;
        return;
    }
    if (kind == Arm64BlockMemory::Literal) {
        const auto* data = owner.image->DataAt(context->instruction + 16, 8, false);
        if (data) {
            std::memcpy(context->registers, data, 8);
            return;
        }
    }
    context->exit = Arm64BlockExit::Failure;
}

std::shared_ptr<const Arm64Block> Block(const ElfImage& image, size_t offset)
{
    const auto pc = image.LoadBias() + offset;
    auto block = FindArm64Block(image.Identity(), pc, image.LoadBias(), 0, image.CodeAt(pc, 128), 4);
    Check(block && block->Instructions() != 0, "native block unavailable");
    return block;
}

void Execute(const ElfImage& image)
{
    const auto bias = image.LoadBias();
    std::array<uint64_t, 31> registers{};
    std::array<std::array<uint64_t, 2>, 32> vectors{};
    Arm64BlockContext context;
    context.registers = registers.data();
    context.vectors = vectors.data();
    context.flagsValid = 1;
    Block(image, 0x1100)->Run(context);
    Check(registers[0] == bias + 0x1100 && registers[1] == bias + 0x1000 && registers[30] == bias + 0x1110 &&
              context.pc == bias + 0x1114 && context.instruction == bias + 0x110c,
          "PC/ADRP/BL relocation failed");
    registers = {};
    Memory memory{&image};
    context.owner = &memory;
    context.memory = Access;
    Block(image, 0x1180)->Run(context);
    Check(registers[0] == 42 && memory.unwind && context.pc == 0, "literal load or Windows unwind registration failed");
    const auto interpreted = RunArm64Function(image, bias + 0x1180, 'J', {}, nullptr, nullptr, {});
    Check(interpreted == registers[0], "cached literal differs from ARM64 interpreter");
    registers = {};
    registers[0] = 10;
    context.budget = 6;
    Block(image, 0x1200)->Run(context);
    Check(registers[0] == 7 && context.executed == 6 && context.pc == bias + 0x1200 &&
              context.instruction == bias + 0x1204,
          "cached loop budget failed");
    context.budget = 20;
    Block(image, 0x1200)->Run(context);
    Check(registers[0] == 0 && context.executed == 14 && context.pc == bias + 0x1208, "cached loop exit failed");
    context.budget = 0;
    registers = {};
    context.sp = 0x10000;
    Block(image, 0x1280)->Run(context);
    Check(context.sp == 0x10010 && memory.unwind, "cached guarded SP callback failed");
    vectors[1] = {0x0101010101010101ull, 0x0101010101010101ull};
    vectors[2] = {0x0202020202020202ull, 0x0202020202020202ull};
    Block(image, 0x1300)->Run(context);
    Check(vectors[0][0] == 0x0303030303030303ull && vectors[0][1] == vectors[0][0], "cached SSE2 vector block failed");
    registers[1] = 42;
    registers[2] = 7;
    context.nzcv = 4;
    Block(image, 0x1380)->Run(context);
    Check(registers[0] == 42, "cached input NZCV failed");
    context.flagsValid = 0;
    bool rejected = false;
    try {
        Block(image, 0x1380)->Run(context);
    }
    catch (const std::invalid_argument&) {
        rejected = true;
    }
    Check(rejected, "cached block lost input flags requirement");
    context.flagsValid = 1;
    registers[0] = 99;
    memory.fault = true;
    Block(image, 0x1400)->Run(context);
    Check(registers[0] == 99 && context.exit == Arm64BlockExit::Failure && context.executed == 1 &&
              context.instruction == bias + 0x1400 && context.pc == bias + 0x1404,
          "cached memory fault lost precise state");
}

void Extra(const ElfImage& image, size_t offset, uint64_t expected)
{
    std::array<uint64_t, 31> registers{};
    Arm64BlockContext context;
    context.registers = registers.data();
    Block(image, offset)->Run(context);
    Check(registers[0] == expected && context.pc == 0, "merged native block execution failed");
}

void CheckReadPin(const std::filesystem::path& directory)
{
    std::filesystem::path file;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == L".stbc") {
            Check(file.empty(), "expected exactly one cache bundle");
            file = entry.path();
        }
    Check(!file.empty(), "missing pinned cache bundle");
    const auto write = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto error = GetLastError();
    if (write != INVALID_HANDLE_VALUE)
        CloseHandle(write);
    Check(write == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION,
          "authenticated file was not pinned against in-place writes");
}

int Merge(const std::filesystem::path& directory, std::wstring_view mode)
{
    Check(ConfigureArm64BlockDiskCache(directory), "merge cache configuration failed");
    stone::windows::android_libraries::AndroidLoadedImage image(Image(false), ElfImage::Machine::Arm64, true);
    image.Relocate({});
    image.Protect(ElfImage::Execution::Translated);
    Execute(image);
    Check(GetArm64BlockStatistics().compiled == 0, "merge test did not start from persisted blocks");
    CheckReadPin(directory);
    if (mode == L"merge-check") {
        Extra(image, 0x1480, 7);
        Extra(image, 0x1500, 9);
        Check(GetArm64BlockStatistics().compiled == 0, "cross-process merge lost a native artifact");
    }
    else if (mode == L"refresh") {
        Extra(image, 0x1480, 7);
        FlushArm64BlockDiskCache();
        CheckReadPin(directory);
        Extra(image, 0x1500, 9);
    }
    else if (mode == L"merge-parent") {
        Extra(image, 0x1480, 7);
        {
            std::ofstream ready(directory / L"parent.ready");
            ready << "ready";
            Check(bool(ready), "cannot publish merge readiness");
        }
        const auto deadline = GetTickCount64() + 15000;
        while (!std::filesystem::exists(directory / L"parent.go")) {
            Check(GetTickCount64() < deadline, "merge child did not complete");
            Sleep(10);
        }
    }
    else
        Extra(image, 0x1500, 9);
    FlushArm64BlockDiskCache();
    const auto stats = GetArm64DiskCacheStatistics();
    std::cout << "Native merge counters: reads=" << stats.mergeReads << " skipped=" << stats.mergeSkipped
              << " merged=" << stats.blocksMerged << " loaded=" << stats.blocksLoaded
              << " written=" << stats.filesWritten << " errors=" << stats.errors << " rejected=" << stats.rejected
              << " limited=" << stats.limited << '\n';
    Check(stats.imagesLoaded == 1 && !stats.errors && !stats.rejected && !stats.limited, "merge cache reported errors");
    if (mode == L"merge-parent")
        Check(stats.mergeReads == 1 && stats.blocksMerged == 1 && stats.filesWritten == 1,
              "changed baseline was not merged");
    else if (mode == L"refresh")
        Check(stats.mergeSkipped == 2 && stats.mergeReads == 0 && stats.filesWritten == 2,
              "unchanged baseline was unnecessarily decrypted again");
    else if (mode == L"merge-child")
        Check(stats.mergeSkipped == 1 && stats.mergeReads == 0 && stats.filesWritten == 1,
              "child did not reuse its authenticated baseline");
    else
        Check(stats.blocksLoaded >= 9 && stats.filesWritten == 0, "merged bundle evidence missing");
    Check(!GetModuleHandleW(L"jvm.dll"), "merge tests loaded a host JVM");
    std::cout << "PASS native merge: reads=" << stats.mergeReads << " skipped=" << stats.mergeSkipped
              << " merged=" << stats.blocksMerged << " loaded=" << stats.blocksLoaded
              << " written=" << stats.filesWritten << '\n';
    return 0;
}

int AuthenticatedEmpty(const std::filesystem::path& directory, bool oversized = false)
{
    std::filesystem::path file;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == L".stbc") {
            Check(file.empty(), "expected one authenticated empty fixture");
            file = entry.path();
        }
    const auto name = file.filename().wstring();
    Check(name.size() == 69, "missing cache identity");
    std::array<BYTE, 32> key{};
    const auto digit = [](wchar_t value) {
        Check((value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f'), "invalid cache key");
        return value <= L'9' ? value - L'0' : value - L'a' + 10;
    };
    for (size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<BYTE>((digit(name[2 * i]) << 4) | digit(name[2 * i + 1]));
    BYTE empty = 0;
    DATA_BLOB input{0, &empty}, entropy{static_cast<DWORD>(key.size()), key.data()}, output{};
    std::array<BYTE, 17> packed{0x53, 0x42, 0x43, 0x5a, 1, 0, 0, 0, 255, 255, 255, 255, 1, 0, 0, 0, 0};
    if (oversized)
        input = {static_cast<DWORD>(packed.size()), packed.data()};
    Check(CryptProtectData(&input, L"Stone empty cache fixture", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                           &output) != FALSE,
          "cannot authenticate empty cache fixture");
    const std::unique_ptr<void, decltype(&LocalFree)> owner(output.pbData, LocalFree);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    stream.close();
    Check(bool(stream), "cannot write authenticated empty cache fixture");
    std::cout << "PASS authenticated " << (oversized ? "oversized compressed" : "empty") << " cache fixture\n";
    return 0;
}
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3) {
        std::cerr
            << "Usage: stone-arm64-cache-tests <cache-directory> <cold|warm|concurrent-warm|unused|corrupt|changed|wrong-image|translator|trim|disabled|unavailable|refresh|merge-parent|merge-child|merge-check|auth-empty|auth-packed-oversized|codec|session|inspect>\n";
        return 2;
    }
    try {
        const std::wstring_view mode = argv[2];
        const bool warm = mode == L"warm" || mode == L"concurrent-warm",
                   changed = mode == L"changed" || mode == L"wrong-image";
        if (mode == L"codec") {
            Codec();
            return 0;
        }
        if (mode == L"inspect") {
            Inspect(std::filesystem::absolute(argv[1]));
            return 0;
        }
        wchar_t eagerValue[2]{};
        const bool eager =
            GetEnvironmentVariableW(L"STONE_ARM64_CACHE_EAGER", eagerValue, 2) == 1 && eagerValue[0] == L'1';
        wchar_t preloadValue[2]{};
        const bool preload =
            GetEnvironmentVariableW(L"STONE_ARM64_CACHE_PRELOAD", preloadValue, 2) == 1 && preloadValue[0] == L'1';
        if (mode == L"auth-empty")
            return AuthenticatedEmpty(std::filesystem::absolute(argv[1]));
        if (mode == L"auth-packed-oversized")
            return AuthenticatedEmpty(std::filesystem::absolute(argv[1]), true);
        if (mode == L"session") {
            Check(ConfigureArm64BlockDiskCache(std::filesystem::absolute(argv[1])),
                  "session cache configuration failed");
            {
                ElfImage raw(Image(false), ElfImage::Machine::Arm64, true);
                raw.Relocate({});
                raw.Protect(ElfImage::Execution::Translated);
                Block(raw, 0x1180);
                Check(!GetArm64DiskCacheStatistics().recorded, "generic ELF unexpectedly attached a CPU cache");
            }
            uint64_t identity = 0;
            std::vector<uint8_t> source;
            {
                stone::windows::android_libraries::AndroidLoadedImage image(Image(false), ElfImage::Machine::Arm64,
                                                                            true);
                image.Relocate({});
                image.Protect(ElfImage::Execution::Translated);
                identity = image.Identity();
                Block(image, 0x1180);
                const auto bytes = image.CodeAt(image.LoadBias() + 0x1180, 16);
                source.assign(bytes.begin(), bytes.end());
                Check(GetArm64DiskCacheStatistics().recorded == 1, "loaded image did not attach the CPU cache");
                DumpArm64ExecutionProfile();
                Check(!GetArm64DiskCacheStatistics().filesWritten, "statistics flushed dirty products");
                FinishArm64ExecutionSession();
                Check(GetArm64DiskCacheStatistics().filesWritten == 1, "session did not save dirty products");
                FinishArm64ExecutionSession();
                Check(GetArm64DiskCacheStatistics().filesWritten == 1, "unchanged session wrote products twice");
            }
            Check(!LoadArm64DiskBlock(identity, 0x1180, 4, source), "unloaded image retained a cache registration");
            Check(!GetModuleHandleW(L"jvm.dll"), "session test loaded a Windows JVM");
            std::cout << "PASS explicit session save, read-only statistics, and paired image lifetime\n";
            return 0;
        }
        if (mode == L"refresh" || mode == L"merge-parent" || mode == L"merge-child" || mode == L"merge-check") {
            Check(!Arm64BlockJitEnabled(), "merge tests require interpreter reference mode");
            return Merge(std::filesystem::absolute(argv[1]), mode);
        }
        Check(mode == L"cold" || warm || changed || mode == L"corrupt" || mode == L"disabled" ||
                  mode == L"unavailable" || mode == L"translator" || mode == L"trim" || mode == L"unused",
              "invalid test mode");
        Check(!Arm64BlockJitEnabled(), "interpreter reference requires native block dispatch disabled");
        const auto directory = std::filesystem::absolute(argv[1]);
        if (mode != L"disabled")
            Check(ConfigureArm64BlockDiskCache(directory) == (mode != L"unavailable"),
                  "unexpected cache configuration result");
        uintptr_t previous = 0;
        void* reservation = nullptr;
        if (warm) {
            std::ifstream input(directory.parent_path() / "previous-base.txt");
            input >> std::hex >> previous;
            Check(previous != 0, "missing cold base");
            reservation = VirtualAlloc(reinterpret_cast<void*>(previous), 8192, MEM_RESERVE, PAGE_NOACCESS);
        }

        struct Release {
            void* p;

            ~Release()
            {
                if (p)
                    VirtualFree(p, 0, MEM_RELEASE);
            }
        } release{reservation};

        stone::windows::android_libraries::AndroidLoadedImage image(Image(changed), ElfImage::Machine::Arm64, true);
        image.Relocate({});
        image.Protect(ElfImage::Execution::Translated);
        const auto registered = GetArm64DiskCacheStatistics();
        if (mode == L"unused") {
            Check(!eager && !preload && !registered.imagesLoaded && !registered.rejected,
                  "unused image read its cache payload");
            FlushArm64BlockDiskCache();
            const auto final = GetArm64DiskCacheStatistics();
            Check(!final.imagesLoaded && !final.rejected && !final.filesWritten && !final.errors,
                  "unused cache performed payload work");
            Check(!GetModuleHandleW(L"jvm.dll"), "unused cache test loaded a JVM");
            std::cout << "PASS unused image: loaded=0 rejected=0 written=0\n";
            return 0;
        }
        if (warm)
            Check(registered.imagesLoaded == ((eager || preload) ? 1u : 0u), "cache loading policy was not respected");
        if (warm)
            Check(image.LoadBias() != previous, "rebasing was not exercised");
        else if (mode == L"cold") {
            std::ofstream output(directory.parent_path() / "previous-base.txt");
            output << std::hex << image.LoadBias();
        }
        Check(WarmArm64Block(image.Identity(), image.LoadBias() + 0x1100, image.LoadBias()) == (warm && eager),
              "unexpected cached block admission policy");
        const auto before = GetArm64BlockStatistics();
        if (mode == L"concurrent-warm") {
            std::array<std::exception_ptr, 4> failures{};
            std::vector<std::thread> workers;
            for (size_t i = 0; i < failures.size(); ++i)
                workers.emplace_back([&, i] {
                    try {
                        Execute(image);
                    }
                    catch (...) {
                        failures[i] = std::current_exception();
                    }
                });
            for (auto& worker : workers)
                worker.join();
            for (auto failure : failures)
                if (failure)
                    std::rethrow_exception(failure);
        }
        else
            Execute(image);
        auto compiled = GetArm64BlockStatistics().compiled - before.compiled;
        Check(MayHaveArm64DiskBlock(image.Identity(), 0x1100, 4) == (warm && eager),
              "newly recorded blocks changed cold admission policy");
        if (warm && !eager) {
            for (unsigned hit = 2; hit < 64; ++hit)
                Check(!WarmArm64Block(image.Identity(), image.LoadBias() + 0x1100, image.LoadBias()),
                      "hot-only cache bypassed the normal warmup threshold");
            Check(WarmArm64Block(image.Identity(), image.LoadBias() + 0x1100, image.LoadBias()),
                  "persisted block did not become eligible at normal warmup");
        }
        if (warm) {
            Check(compiled == 0, "warm cache unexpectedly invoked the native compiler");
            auto source = image.CodeAt(image.LoadBias() + 0x1100, 16);
            std::vector<uint8_t> changedSource(source.begin(), source.end());
            changedSource[0] ^= 1;
            Check(!LoadArm64DiskBlock(image.Identity(), 0x1100, 4, changedSource),
                  "live source mutation accepted a stale artifact");
            std::array<std::exception_ptr, 4> errors{};
            std::vector<std::thread> threads;
            for (size_t i = 0; i < errors.size(); ++i)
                threads.emplace_back([&, i] {
                    try {
                        for (unsigned j = 0; j < 8; ++j)
                            Execute(image);
                    }
                    catch (...) {
                        errors[i] = std::current_exception();
                    }
                });
            for (auto& thread : threads)
                thread.join();
            for (auto error : errors)
                if (error)
                    std::rethrow_exception(error);
        }
        else
            Check(compiled >= 7, "cold fallback did not compile expected native blocks");
        // Anonymous/dynamic ART JIT code has no immutable ELF identity.
        const uint32_t dynamic[]{0xd2800540, 0xd65f03c0};
        const auto recorded = GetArm64DiskCacheStatistics().recorded;
        auto transient = FindArm64Block(0, 0x5000, 0, 1, {reinterpret_cast<const uint8_t*>(dynamic), sizeof(dynamic)});
        Check(transient && transient->Instructions() == 2 && GetArm64DiskCacheStatistics().recorded == recorded,
              "dynamic code was persisted");
        if (mode == L"corrupt" || mode == L"wrong-image")
            Check(GetArm64DiskCacheStatistics().rejected == 1, "malformed bundle was repeatedly read before flush");
        errno = ERANGE;
        SetLastError(ERROR_ACCESS_DENIED);
        FlushArm64BlockDiskCache();
        Check(errno == ERANGE && GetLastError() == ERROR_ACCESS_DENIED, "cache changed caller error state");
        const auto stats = GetArm64DiskCacheStatistics();
        if (warm)
            Check(stats.imagesLoaded == 1 && stats.blocksLoaded >= 7 && stats.hits >= 7 && stats.filesWritten == 0 &&
                      !stats.errors,
                  "warm cache evidence missing");
        else if (mode == L"cold" || changed || mode == L"translator" || mode == L"trim")
            Check(stats.imagesLoaded == 0 && stats.filesWritten == 1 && !stats.errors, "cold cache evidence missing");
        else if (mode == L"corrupt")
            Check(stats.rejected && stats.filesWritten == 1 && !stats.errors, "corrupt cache did not safely rebuild");
        else
            Check(stats.filesWritten == 0 && !stats.imagesLoaded, "disabled cache performed disk work");
        if (mode == L"wrong-image")
            Check(stats.rejected > 0, "artifact replay across image identities was accepted");
        if (mode == L"trim")
            Check(stats.evicted > 0, "disk capacity was not enforced");
        if (mode == L"disabled")
            Check(stats.errors == 0, "disabled cache performed failing work");
        if (mode == L"unavailable")
            Check(stats.errors == 1, "unavailable cache error was not recorded");
        if (warm) {
            // Bloom hints may outlive an ELF registration. They never authorize
            // artifact loading or replace the exact code-identity checks.
            UnregisterArm64BlockCacheImage(image.Identity());
            Check(!LoadArm64DiskBlock(image.Identity(), 0x1100, 4, image.CodeAt(image.LoadBias() + 0x1100, 16)),
                  "stale admission hint authorized an unregistered image");
        }
        Check(!GetModuleHandleW(L"jvm.dll"), "cache tests loaded a Windows JVM");
        std::cout << "PASS native cache: compiled=" << compiled << " loaded=" << stats.blocksLoaded
                  << " hits=" << stats.hits << " rejected=" << stats.rejected << " written=" << stats.filesWritten
                  << " errors=" << stats.errors << " base=0x" << std::hex << image.LoadBias() << '\n';
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
