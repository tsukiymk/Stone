#include "ElfImage.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <bit>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace stone::windows {
namespace {
using namespace elf;

constexpr uint64_t MaxImage = 512ull << 20;
constexpr uint64_t MaxSymbols = 1ull << 20;
constexpr uint64_t MaxTlsBytes = 32ull << 20;
// x86_64 TLS variant II places aligned module blocks before the thread pointer.
// Keep a fixed address on each thread when a later dlopen adds another block.
constexpr size_t StaticTlsCapacity = 64 << 10;
constexpr size_t ThreadControlBytes = 16 * sizeof(uintptr_t);
// Bionic AArch64 uses slots -3..7. Keep TP 64-byte aligned and ELF TLS
// modules after its eight positive slots (variant I), separate from x64 TLS.
constexpr size_t Arm64ThreadPrefix = StaticTlsCapacity;
constexpr size_t Arm64ControlBytes = 8 * sizeof(uintptr_t);
constexpr size_t Arm64NegativeBytes = 3 * sizeof(uintptr_t);
constexpr uint32_t Read = 4, Write = 2, Execute = 1;
std::atomic<uint64_t> dataProtectionGeneration{0};
std::atomic<uint64_t> tlsModuleIdentity{0};
std::atomic<uint64_t> imageIdentity{0};
constexpr uint32_t Load = 1, Dynamic = 2, Interpreter = 3, Tls = 7, Relro = 0x6474e552;
constexpr int64_t AndroidRela = 0x60000011, AndroidRelaSize = 0x60000012;
constexpr int64_t AndroidRelr = 0x6fffe000, AndroidRelrSize = 0x6fffe001, AndroidRelrEntry = 0x6fffe003;

struct Dyn {
    int64_t tag;
    uint64_t value;
};

template <typename T> T ReadValue(const void* bytes)
{
    T value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool Fits(uint64_t offset, uint64_t count, uint64_t length)
{
    return offset <= length && count <= length - offset;
}

[[noreturn]] void Invalid(const char* message)
{
    throw std::runtime_error(message);
}

// Android's APS2 stream encodes signed integers as SLEB128. Keep the
// accumulated bits unsigned so malformed deltas cannot cause signed overflow.
uint64_t ReadSleb(const uint8_t*& input, const uint8_t* end)
{
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (input == end)
            Invalid("truncated APS2 relocation stream");
        const uint8_t byte = *input++;
        if (shift == 63 && byte != 0 && byte != 0x7f)
            Invalid("oversized APS2 integer");
        value |= uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            if (shift < 57 && (byte & 0x40))
                value |= UINT64_MAX << (shift + 7);
            return value;
        }
    }
    Invalid("invalid APS2 integer");
}

bool External(const Elf64Sym& symbol)
{
    const auto binding = symbol.st_info >> 4;
    const auto visibility = symbol.st_other & 3;
    return (binding == 1 || binding == 2) && (visibility == 0 || visibility == 3);
}

// Keep guest TLS allocated until all C++ thread-local destructors have run.
struct GuestThreadStorage {
    using Block = std::unique_ptr<uint8_t, decltype(&_aligned_free)>;

    struct Destructor {
        void (*invoke)(uintptr_t, void*);
        uintptr_t callback;
        void* argument;
    };

    std::unordered_map<uint64_t, Block> blocks;
    std::vector<Destructor> destructors;
    std::shared_ptr<void> execution;
    void (*keyCleanup)() = nullptr;
    uint8_t* staticMemory = nullptr;
    uint8_t* arm64Memory = nullptr;

    void RunDestructors()
    {
        while (!destructors.empty()) {
            const auto entry = destructors.back();
            destructors.pop_back();
            entry.invoke(entry.callback, entry.argument);
        }
        // Bionic cleans pthread keys after thread_local destructors and while
        // the thread's ELF TLS is still available.
        if (const auto cleanup = std::exchange(keyCleanup, nullptr))
            cleanup();
    }
};

struct StaticTlsRegistry {
    std::mutex mutex;
    std::vector<GuestThreadStorage*> threads;
    std::vector<const ElfImage*> images;
};

StaticTlsRegistry& StaticTls()
{
    static auto* registry = new StaticTlsRegistry;
    return *registry;
}

StaticTlsRegistry& Arm64StaticTls()
{
    static auto* registry = new StaticTlsRegistry;
    return *registry;
}

void CommitStaticTls(uint8_t* threadPointer, size_t used)
{
    if (used && !VirtualAlloc(threadPointer - used, used, MEM_COMMIT, PAGE_READWRITE))
        throw std::bad_alloc{};
}

thread_local GuestThreadStorage* activeThreadStorage = nullptr;
thread_local uintptr_t* currentGuestThreadPointer = nullptr;
thread_local uintptr_t* currentArm64ThreadPointer = nullptr;

void CALLBACK DestroyGuestThreadStorage(void* value)
{
    auto* storage = static_cast<GuestThreadStorage*>(value);
    activeThreadStorage = storage;
    storage->RunDestructors();
    if (storage->staticMemory) {
        if (reinterpret_cast<uint8_t*>(currentGuestThreadPointer) == storage->staticMemory + StaticTlsCapacity)
            currentGuestThreadPointer = nullptr;
        auto& registry = StaticTls();
        const std::lock_guard lock(registry.mutex);
        std::erase(registry.threads, storage);
        VirtualFree(storage->staticMemory, 0, MEM_RELEASE);
    }
    if (storage->arm64Memory) {
        currentArm64ThreadPointer = nullptr;
        auto& registry = Arm64StaticTls();
        const std::lock_guard lock(registry.mutex);
        std::erase(registry.threads, storage);
        VirtualFree(storage->arm64Memory, 0, MEM_RELEASE);
    }
    delete storage;
    activeThreadStorage = nullptr;
}

GuestThreadStorage* ThreadStorage(bool allocate)
{
    if (activeThreadStorage)
        return activeThreadStorage;
    static const DWORD index = FlsAlloc(DestroyGuestThreadStorage);
    if (index == FLS_OUT_OF_INDEXES)
        return nullptr;
    if (auto* storage = static_cast<GuestThreadStorage*>(FlsGetValue(index)))
        return storage;
    if (!allocate)
        return nullptr;
    auto storage = std::make_unique<GuestThreadStorage>();
    if (!FlsSetValue(index, storage.get()))
        return nullptr;
    return storage.release();
}
} // namespace

ElfImage::ElfImage(std::span<const uint8_t> file, Machine machine, bool dynamicTls, bool artCompiler)
    : machine_(machine), identity_(imageIdentity.fetch_add(1, std::memory_order_relaxed) + 1)
{
    if (file.size() < sizeof(Elf64Ehdr))
        Invalid("truncated ELF header");
    const auto header = ReadValue<Elf64Ehdr>(file.data());
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    if (std::memcmp(header.e_ident, ident, sizeof(ident)) != 0 || header.e_type != 3 ||
        header.e_machine != static_cast<uint16_t>(machine) || header.e_version != 1)
        Invalid(machine == Machine::Arm64 ? "expected an Android arm64-v8a ELF shared library"
                                          : "expected an Android x86_64 ELF shared library");
    if (header.e_phentsize != sizeof(Segment) || header.e_phnum == 0 || header.e_phnum > 1024 ||
        !Fits(header.e_phoff, uint64_t(header.e_phnum) * sizeof(Segment), file.size()))
        Invalid("invalid ELF program headers");

    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    page_ = system.dwPageSize;
    floor_ = MaxImage;
    uint64_t ceiling = 0;
    Segment dynamic{};
    bool sawTls = false;
    headers_.reserve(header.e_phnum);
    for (uint16_t i = 0; i < header.e_phnum; ++i) {
        const auto p = ReadValue<Segment>(file.data() + header.e_phoff + i * sizeof(Segment));
        headers_.push_back(p);
        if (p.p_type == Interpreter) {
            // The compiler host calls dex2oat's main directly, after linking and
            // constructors. It never executes Bionic's process startup or linker.
            constexpr char linker[] = "/system/bin/linker64";
            if (!artCompiler || p.p_filesz != sizeof(linker) || !Fits(p.p_offset, p.p_filesz, file.size()) ||
                std::memcmp(file.data() + p.p_offset, linker, sizeof(linker)))
                Invalid("ELF interpreter is unsupported");
        }
        if (p.p_type == Tls) {
            if (!dynamicTls)
                Invalid("ELF TLS is unsupported for this backend");
            if (sawTls || p.p_memsz > MaxTlsBytes || p.p_filesz > p.p_memsz ||
                !Fits(p.p_offset, p.p_filesz, file.size()) || p.p_align > MaxTlsBytes ||
                (p.p_align > 1 && (!std::has_single_bit(p.p_align) || p.p_vaddr % p.p_align)))
                Invalid("invalid or unsupported ELF TLS segment");
            sawTls = true;
            // Android PIE executables may retain an empty PT_TLS. It has no
            // module storage or TLS symbols; keep all header bounds checks.
            if (!p.p_memsz)
                continue;
            tlsIdentity_ = ++tlsModuleIdentity;
            tls_ = p;
        }
        if (p.p_type == Dynamic) {
            if (dynamic.p_type)
                Invalid("multiple ELF dynamic segments");
            dynamic = p;
        }
        if (p.p_type == Relro)
            relro_.push_back(p);
        if (p.p_type != Load || p.p_memsz == 0)
            continue;
        if (p.p_filesz > p.p_memsz || !Fits(p.p_offset, p.p_filesz, file.size()) ||
            !Fits(p.p_vaddr, p.p_memsz, MaxImage) || (p.p_offset % page_) != (p.p_vaddr % page_))
            Invalid("invalid ELF load segment");
        for (const auto& earlier : loads_) {
            if (earlier.p_vaddr < p.p_vaddr + p.p_memsz && p.p_vaddr < earlier.p_vaddr + earlier.p_memsz)
                Invalid("overlapping ELF load segments");
        }
        floor_ = std::min(floor_, p.p_vaddr / page_ * page_);
        ceiling = std::max(ceiling, (p.p_vaddr + p.p_memsz + page_ - 1) / page_ * page_);
        loads_.push_back(p);
    }
    if (loads_.empty() || !dynamic.p_type || dynamic.p_filesz < sizeof(Dyn) || dynamic.p_filesz % sizeof(Dyn) != 0 ||
        dynamic.p_filesz / sizeof(Dyn) > 4096)
        Invalid("missing ELF load or dynamic metadata");
    size_ = static_cast<size_t>(ceiling - floor_);
    const auto release = [](void* p) {
        if (p)
            VirtualFree(p, 0, MEM_RELEASE);
    };
    std::unique_ptr<void, decltype(release)> staging(
        VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), release);
    if (!staging)
        Invalid("could not allocate memory for ELF image");
    memory_ = static_cast<uint8_t*>(staging.get());
    for (const auto& p : loads_)
        std::memcpy(At(p.p_vaddr, p.p_filesz), file.data() + p.p_offset, static_cast<size_t>(p.p_filesz));

    const auto* entries = At(dynamic.p_vaddr, dynamic.p_filesz, Read);
    bool ended = false;
    for (uint64_t offset = 0; offset < dynamic.p_filesz; offset += sizeof(Dyn)) {
        const auto item = ReadValue<Dyn>(entries + offset);
        if (item.tag == 0) {
            ended = true;
            break;
        }
        dynamic_.emplace_back(item.tag, item.value);
    }
    if (!ended || Tag(11) != sizeof(Symbol) || !Tag(5) || !Tag(6) || !Tag(10))
        Invalid("invalid ELF dynamic symbol tables");
    if (Tag(17) || Tag(18) || Tag(22) || Tag(0x6000000f) || (Tag(30) & 4))
        Invalid("ELF uses unsupported REL or text relocations");
    if ((Tag(30) & 0x10) &&
        (!dynamicTls || (machine == Machine::X64 && !IsProcessorFeaturePresent(PF_RDWRFSGSBASE_AVAILABLE))))
        Invalid("ELF static TLS requires an enabled thread-pointer backend");
    if (tls_.p_filesz)
        At(tls_.p_vaddr, tls_.p_filesz, Read);
    strings_ = {reinterpret_cast<const char*>(At(Tag(5), Tag(10), Read)), static_cast<size_t>(Tag(10))};
    uint64_t count = 0;
    if (const auto sysvHash = Tag(4))
        count = ReadValue<uint32_t>(At(sysvHash, 8, Read) + 4);
    else if (const auto gnuHash = Tag(0x6ffffef5))
        count = CountGnuSymbols(gnuHash);
    if (count == 0 || count > MaxSymbols)
        Invalid("missing or oversized ELF symbol hash table");
    const auto* table = At(Tag(6), count * sizeof(Symbol), Read);
    symbols_.resize(static_cast<size_t>(count));
    std::memcpy(symbols_.data(), table, symbols_.size() * sizeof(Symbol));
    for (size_t i = 0; i < symbols_.size(); ++i) {
        const auto& symbol = symbols_[i];
        if (symbol.st_shndx && symbol.st_shndx < 0xff00 && (symbol.st_info & 15) == 2)
            codeSymbols_.push_back({symbol.st_value, symbol.st_size});
        if (!symbol.st_shndx || !External(symbol))
            continue;
        auto name = String(symbol.st_name);
        if ((symbol.st_info & 15) == 6) {
            if (!tls_.p_type || !Fits(symbol.st_value, symbol.st_size, tls_.p_memsz))
                Invalid("invalid ELF TLS symbol");
            tlsExports_.try_emplace(std::string(name), symbol.st_value);
        }
        else {
            exports_.try_emplace(std::string(name), i);
        }
    }
    std::ranges::sort(codeSymbols_, {}, &CodeSymbol::address);
    size_t unique = 0;
    for (const auto symbol : codeSymbols_) {
        if (unique && codeSymbols_[unique - 1].address == symbol.address) {
            auto& size = codeSymbols_[unique - 1].size;
            if (symbol.size && (!size || symbol.size < size))
                size = symbol.size;
        }
        else
            codeSymbols_[unique++] = symbol;
    }
    codeSymbols_.resize(unique);
    for (const auto& [type, value] : dynamic_) {
        if (type == 1) {
            const auto name = String(value);
            if (name.size() <= 3 || !name.ends_with(".so") ||
                name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_+@.-") !=
                    name.npos)
                Invalid("invalid DT_NEEDED name");
            needed_.emplace_back(name);
        }
    }
    staging.release();
}

ElfImage::~ElfImage()
{
    if (staticTlsOffset_) {
        auto& registry = machine_ == Machine::Arm64 ? Arm64StaticTls() : StaticTls();
        const std::lock_guard lock(registry.mutex);
        std::erase(registry.images, this);
    }
    if (memory_) {
        VirtualFree(memory_, 0, MEM_RELEASE);
        dataProtectionGeneration.fetch_add(1, std::memory_order_release);
    }
}

static uintptr_t RandomStackGuard()
{
    uintptr_t value = 0;
    do {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            std::abort();
    } while (!value);
    return value;
}

static uintptr_t& ThreadStackGuard()
{
    thread_local auto guard = RandomStackGuard();
    return guard;
}

uintptr_t AndroidStackGuard()
{
    return ThreadStackGuard();
}

const uintptr_t* AndroidGlobalStackGuard()
{
    static auto guard = RandomStackGuard();
    return &guard;
}

void ResetAndroidStackGuards()
{
    auto& registry = StaticTls();
    auto& arm64 = Arm64StaticTls();
    const std::scoped_lock lock(registry.mutex, arm64.mutex);
    // Bionic exposes this for a single-threaded zygote child. Resetting cookies
    // while other guest threads still have protected frames would corrupt them.
    if (registry.threads.size() > 1 || arm64.threads.size() > 1 ||
        (!registry.threads.empty() && !arm64.threads.empty() && registry.threads[0] != arm64.threads[0])) {
        std::fputs("[android-native] cannot reset stack guards with multiple guest threads\n", stderr);
        std::abort();
    }
    ThreadStackGuard() = RandomStackGuard();
    *const_cast<uintptr_t*>(AndroidGlobalStackGuard()) = RandomStackGuard();
    if (currentGuestThreadPointer)
        currentGuestThreadPointer[AndroidStackGuardSlot] = ThreadStackGuard();
    if (currentArm64ThreadPointer)
        currentArm64ThreadPointer[AndroidStackGuardSlot] = ThreadStackGuard();
}

ElfImage::TlsSymbol ElfImage::FindTlsExport(std::string_view name) const
{
    const auto it = tlsExports_.find(name);
    return it != tlsExports_.end() ? TlsSymbol{TlsModule(), it->second} : TlsSymbol{};
}

void ElfImage::AssignStaticTls(size_t& used)
{
    if (!tls_.p_type || staticTlsOffset_ ||
        (machine_ == Machine::X64 && !IsProcessorFeaturePresent(PF_RDWRFSGSBASE_AVAILABLE)))
        return;
    const size_t alignment = static_cast<size_t>(std::max<uint64_t>(tls_.p_align, 1));
    if (alignment > StaticTlsCapacity || used > StaticTlsCapacity || tls_.p_memsz > StaticTlsCapacity - used)
        return;
    if (machine_ == Machine::Arm64) {
        const size_t start = (std::max(used, Arm64ControlBytes) + alignment - 1) & ~(alignment - 1);
        if (start > StaticTlsCapacity || tls_.p_memsz > StaticTlsCapacity - start)
            return;
        staticTlsOffset_ = start;
        used = start + static_cast<size_t>(tls_.p_memsz);
        return;
    }
    const size_t end = (used + static_cast<size_t>(tls_.p_memsz) + alignment - 1) & ~(alignment - 1);
    if (end > StaticTlsCapacity)
        return;
    staticTlsOffset_ = used = end;
}

void ElfImage::CopyTlsTemplate(uint8_t* destination) const
{
    if (tls_.p_filesz) {
        std::memcpy(destination, At(tls_.p_vaddr, tls_.p_filesz, Read), static_cast<size_t>(tls_.p_filesz));
    }
    std::memset(destination + tls_.p_filesz, 0, static_cast<size_t>(tls_.p_memsz - tls_.p_filesz));
}

void ElfImage::PublishStaticTls(std::span<const ElfImage* const> images)
{
    for (const auto machine : {Machine::X64, Machine::Arm64}) {
        auto& registry = machine == Machine::Arm64 ? Arm64StaticTls() : StaticTls();
        const std::lock_guard lock(registry.mutex);
        std::vector<const ElfImage*> added;
        for (const auto* image : images) {
            if (image->machine_ == machine && image->staticTlsOffset_ &&
                std::ranges::find(registry.images, image) == registry.images.end() &&
                std::ranges::find(added, image) == added.end())
                added.push_back(image);
        }
        if (added.empty())
            continue;
        registry.images.reserve(registry.images.size() + added.size());
        size_t used = 0;
        for (const auto* image : added)
            used = std::max(used, image->staticTlsOffset_ +
                                      (machine == Machine::Arm64 ? static_cast<size_t>(image->tls_.p_memsz) : 0));
        for (auto* thread : registry.threads) {
            if (machine == Machine::Arm64) {
                if (!VirtualAlloc(thread->arm64Memory, Arm64ThreadPrefix + used, MEM_COMMIT, PAGE_READWRITE))
                    throw std::bad_alloc{};
                for (const auto* image : added)
                    image->CopyTlsTemplate(thread->arm64Memory + Arm64ThreadPrefix + image->staticTlsOffset_);
            }
            else {
                CommitStaticTls(thread->staticMemory + StaticTlsCapacity, used);
                for (const auto* image : added)
                    image->CopyTlsTemplate(thread->staticMemory + StaticTlsCapacity - image->staticTlsOffset_);
            }
        }
        registry.images.insert(registry.images.end(), added.begin(), added.end());
    }
}

uintptr_t* ElfImage::GuestThreadPointer()
{
    auto* storage = ThreadStorage(true);
    if (!storage)
        throw std::bad_alloc{};
    if (!storage->staticMemory) {
        auto& registry = StaticTls();
        const std::lock_guard lock(registry.mutex);
        registry.threads.reserve(registry.threads.size() + 1);
        auto* memory = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, StaticTlsCapacity + ThreadControlBytes, MEM_RESERVE, PAGE_READWRITE));
        if (!memory)
            throw std::bad_alloc{};
        auto* pointer = memory + StaticTlsCapacity;
        try {
            if (!VirtualAlloc(pointer, ThreadControlBytes, MEM_COMMIT, PAGE_READWRITE))
                throw std::bad_alloc{};
            size_t used = 0;
            for (const auto* image : registry.images)
                used = std::max(used, image->staticTlsOffset_);
            CommitStaticTls(pointer, used);
            for (const auto* image : registry.images)
                image->CopyTlsTemplate(pointer - image->staticTlsOffset_);
        }
        catch (...) {
            VirtualFree(memory, 0, MEM_RELEASE);
            throw;
        }
        storage->staticMemory = memory;
        registry.threads.push_back(storage);
    }
    currentGuestThreadPointer = reinterpret_cast<uintptr_t*>(storage->staticMemory + StaticTlsCapacity);
    currentGuestThreadPointer[0] = reinterpret_cast<uintptr_t>(currentGuestThreadPointer);
    currentGuestThreadPointer[AndroidStackGuardSlot] = AndroidStackGuard();
    return currentGuestThreadPointer;
}

uintptr_t* ElfImage::CurrentGuestThreadPointer() noexcept
{
    return currentGuestThreadPointer;
}

uintptr_t* ElfImage::GuestArm64ThreadPointer()
{
    if (currentArm64ThreadPointer) {
        currentArm64ThreadPointer[AndroidStackGuardSlot] = AndroidStackGuard();
        return currentArm64ThreadPointer;
    }
    auto* storage = ThreadStorage(true);
    if (!storage)
        throw std::bad_alloc{};
    if (!storage->arm64Memory) {
        auto& registry = Arm64StaticTls();
        const std::lock_guard lock(registry.mutex);
        registry.threads.reserve(registry.threads.size() + 1);
        auto* memory = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, Arm64ThreadPrefix + StaticTlsCapacity, MEM_RESERVE, PAGE_READWRITE));
        if (!memory)
            throw std::bad_alloc{};
        try {
            size_t used = Arm64ControlBytes;
            for (const auto* image : registry.images)
                used = std::max(used, image->staticTlsOffset_ + static_cast<size_t>(image->tls_.p_memsz));
            if (!VirtualAlloc(memory, Arm64ThreadPrefix + used, MEM_COMMIT, PAGE_READWRITE))
                throw std::bad_alloc{};
            for (const auto* image : registry.images)
                image->CopyTlsTemplate(memory + Arm64ThreadPrefix + image->staticTlsOffset_);
        }
        catch (...) {
            VirtualFree(memory, 0, MEM_RELEASE);
            throw;
        }
        storage->arm64Memory = memory;
        registry.threads.push_back(storage);
    }
    currentArm64ThreadPointer = reinterpret_cast<uintptr_t*>(storage->arm64Memory + Arm64ThreadPrefix);
    currentArm64ThreadPointer[AndroidStackGuardSlot] = AndroidStackGuard();
    return currentArm64ThreadPointer;
}

uint8_t* ElfImage::Arm64ThreadControlDataAt(uintptr_t address, size_t length)
{
    if (!currentArm64ThreadPointer)
        return nullptr;
    const auto base = reinterpret_cast<uintptr_t>(currentArm64ThreadPointer) - Arm64NegativeBytes;
    const auto offset = address - base;
    return offset < Arm64NegativeBytes + Arm64ControlBytes && length <= Arm64NegativeBytes + Arm64ControlBytes - offset
               ? reinterpret_cast<uint8_t*>(address)
               : nullptr;
}

void* ElfImage::ThreadLocalAddress(uint64_t offset, bool allocate) const
{
    if (offset >= tls_.p_memsz)
        return nullptr;
    if (staticTlsOffset_) {
        if (machine_ == Machine::Arm64) {
            if (allocate)
                return reinterpret_cast<uint8_t*>(GuestArm64ThreadPointer()) + staticTlsOffset_ + offset;
            const auto* storage = ThreadStorage(false);
            return storage && storage->arm64Memory
                       ? storage->arm64Memory + Arm64ThreadPrefix + staticTlsOffset_ + offset
                       : nullptr;
        }
        if (allocate)
            return reinterpret_cast<uint8_t*>(GuestThreadPointer()) - staticTlsOffset_ + offset;
        const auto* storage = ThreadStorage(false);
        return storage && storage->staticMemory ? storage->staticMemory + StaticTlsCapacity - staticTlsOffset_ + offset
                                                : nullptr;
    }
    // Published native images live for the process lifetime. Each thread owns
    // its initialized copy of their ELF TLS templates until it exits.
    auto* storage = ThreadStorage(allocate);
    if (!storage)
        return nullptr;
    auto& blocks = storage->blocks;
    auto it = blocks.find(tlsIdentity_);
    if (it == blocks.end()) {
        if (!allocate)
            return nullptr;
        const auto align = static_cast<size_t>(std::max<uint64_t>(tls_.p_align, alignof(void*)));
        const auto size = static_cast<size_t>(tls_.p_memsz);
        GuestThreadStorage::Block data(static_cast<uint8_t*>(_aligned_malloc(size, align)), _aligned_free);
        if (!data)
            return nullptr;
        CopyTlsTemplate(data.get());
        it = blocks.emplace(tlsIdentity_, std::move(data)).first;
    }
    return it->second.get() + offset;
}

uint8_t* ElfImage::ThreadLocalDataAt(uintptr_t address, size_t length) const
{
    auto* data = static_cast<uint8_t*>(ThreadLocalAddress(0, false));
    const auto offset = address - reinterpret_cast<uintptr_t>(data);
    return data && offset < tls_.p_memsz && length <= tls_.p_memsz - offset ? data + offset : nullptr;
}

bool ElfImage::ResolveTlsDescriptor(uintptr_t argument, uintptr_t& address) const
{
    for (const auto& descriptor : tlsDescriptors_) {
        if (reinterpret_cast<uintptr_t>(descriptor.get()) != argument)
            continue;
        if (!descriptor->module) {
            address = descriptor->offset;
            return true;
        }
        const auto* owner = reinterpret_cast<const ElfImage*>(descriptor->module);
        const auto data = owner->ThreadLocalAddress(descriptor->offset);
        if (!data)
            throw std::runtime_error("could not resolve ARM64 ELF TLS descriptor");
        address = reinterpret_cast<uintptr_t>(data);
        return true;
    }
    return false;
}

bool ElfImage::RegisterThreadDestructor(void (*invoke)(uintptr_t, void*), uintptr_t callback, void* argument)
{
    auto* storage = ThreadStorage(true);
    if (!storage)
        return false;
    storage->destructors.push_back({invoke, callback, argument});
    return true;
}

bool ElfImage::SetThreadKeyCleanup(void (*cleanup)())
{
    auto* storage = ThreadStorage(true);
    if (!storage)
        return false;
    storage->keyCleanup = cleanup;
    return true;
}

std::shared_ptr<void>& ElfImage::ThreadExecutionStorage()
{
    auto* storage = ThreadStorage(true);
    if (!storage)
        throw std::bad_alloc{};
    return storage->execution;
}

void ElfImage::RunThreadDestructors()
{
    if (auto* storage = ThreadStorage(false))
        storage->RunDestructors();
}

bool ElfImage::ContainsCode(uintptr_t address, size_t length) const
{
    const auto range = ExecutableRangeAt(address);
    return !range.empty() && length <= range.size() - (address - reinterpret_cast<uintptr_t>(range.data()));
}

std::span<const uint8_t> ElfImage::ExecutableRangeAt(uintptr_t address) const
{
    const auto base = reinterpret_cast<uintptr_t>(memory_);
    if (address < base || address - base >= size_)
        return {};
    const auto offset = address - base + floor_;
    for (const auto& segment : loads_) {
        if ((segment.p_flags & Execute) && ((segment.p_flags & Read) || machine_ == Machine::Arm64) &&
            offset >= segment.p_vaddr && offset - segment.p_vaddr < segment.p_memsz) {
            const auto start = base + (segment.p_vaddr - floor_);
            return {reinterpret_cast<const uint8_t*>(start), static_cast<size_t>(segment.p_memsz)};
        }
    }
    return {};
}

uint8_t* ElfImage::DataAt(uintptr_t address, size_t length, bool write) const
{
    const auto base = reinterpret_cast<uintptr_t>(memory_);
    if (address < base || address - base >= size_)
        return nullptr;

    struct WritableRange {
        const ElfImage* image = nullptr;
        uintptr_t begin = 0, end = 0;
        uint64_t generation = 0;
    };

    thread_local WritableRange writable;
    // Cache only the intersection of the writable ELF segment and the writable
    // Windows region. mprotect invalidates cached ranges before returning.
    const auto generation = write ? dataProtectionGeneration.load(std::memory_order_acquire) : 0;
    if (write && writable.image == this && writable.generation == generation && address >= writable.begin &&
        address < writable.end && length <= writable.end - address)
        return reinterpret_cast<uint8_t*>(address);
    const auto offset = address - base + floor_;
    const uint32_t required = write ? Write : Read;
    for (const auto& segment : loads_) {
        if (offset < segment.p_vaddr || !Fits(offset - segment.p_vaddr, length, segment.p_memsz))
            continue;
        if (!write && machine_ == Machine::Arm64 && (segment.p_flags & Execute)) {
            const std::shared_lock lock(codePermissionsMutex_);
            bool readable = true;
            const auto last = address + std::max(size_t{1}, length) - 1;
            for (auto page = address - address % page_;; page += page_) {
                const auto changed = codeReadablePages_.find(page);
                if (!(changed == codeReadablePages_.end() ? (segment.p_flags & Read) != 0 : changed->second)) {
                    readable = false;
                    break;
                }
                if (last - page < page_)
                    break;
            }
            if (!readable)
                continue;
        }
        else if ((segment.p_flags & required) != required)
            continue;
        auto* pointer = reinterpret_cast<uint8_t*>(address);
        if (!write)
            return pointer;
        // PT_LOAD marks RELRO data writable before relocation. Check the actual
        // Windows protection before giving an executor access to the page.
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(pointer, &region, sizeof(region)) && region.State == MEM_COMMIT &&
            region.Protect == PAGE_READWRITE &&
            Fits(address - reinterpret_cast<uintptr_t>(region.BaseAddress), length, region.RegionSize)) {
            const auto segmentStart = base + (segment.p_vaddr - floor_);
            const auto regionStart = reinterpret_cast<uintptr_t>(region.BaseAddress);
            writable = {this, std::max(segmentStart, regionStart),
                        std::min(segmentStart + static_cast<size_t>(segment.p_memsz), regionStart + region.RegionSize),
                        generation};
            return pointer;
        }
    }
    return nullptr;
}

bool ElfImage::ProtectData(uintptr_t address, size_t length, bool writable) const
{
    const auto base = reinterpret_cast<uintptr_t>(memory_);
    if (address < base || (address - base) % page_ || !length || !Fits(address - base, length, size_))
        return false;
    const auto rounded = (length + page_ - 1) / page_ * page_;
    const auto offset = address - base + floor_;
    if (writable) {
        // The dynamic linker permanently removes write access from RELRO.
        for (const auto& segment : relro_) {
            if (!segment.p_memsz)
                continue;
            const auto start = segment.p_vaddr / page_ * page_;
            const auto end = (segment.p_vaddr + segment.p_memsz + page_ - 1) / page_ * page_;
            if (offset < end && start < offset + rounded)
                return false;
        }
    }
    for (const auto& segment : loads_) {
        const auto start = segment.p_vaddr / page_ * page_;
        const auto end = (segment.p_vaddr + segment.p_memsz + page_ - 1) / page_ * page_;
        if ((segment.p_flags & Execute) || !(segment.p_flags & Read) || (writable && !(segment.p_flags & Write)) ||
            offset < start || !Fits(offset - start, rounded, end - start))
            continue;
        for (auto page = address; page < address + rounded;) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<const void*>(page), &region, sizeof(region)) ||
                region.State != MEM_COMMIT || (region.Protect != PAGE_READONLY && region.Protect != PAGE_READWRITE))
                return false;
            page = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        }
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(address), rounded, writable ? PAGE_READWRITE : PAGE_READONLY,
                            &previous))
            return false;
        dataProtectionGeneration.fetch_add(1, std::memory_order_release);
        return true;
    }
    return false;
}

bool ElfImage::ProtectArm64Code(uintptr_t address, size_t length, bool readable) const
{
    const auto base = reinterpret_cast<uintptr_t>(memory_);
    if (machine_ != Machine::Arm64 || address < base || (address - base) % page_ || !length ||
        length > SIZE_MAX - (page_ - 1) || !Fits(address - base, length, size_))
        return false;
    const auto rounded = (length + page_ - 1) / page_ * page_;
    if (!Fits(address - base, rounded, size_))
        return false;
    const auto offset = address - base + floor_;
    for (const auto& segment : loads_) {
        const auto first = segment.p_vaddr / page_ * page_;
        const auto last = (segment.p_vaddr + segment.p_memsz + page_ - 1) / page_ * page_;
        if (!(segment.p_flags & Execute) || offset < first || !Fits(offset - first, rounded, last - first))
            continue;
        for (auto page = address; page < address + rounded;) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(page), &region, sizeof(region)) || region.State != MEM_COMMIT ||
                (region.Protect != PAGE_READONLY && region.Protect != PAGE_READWRITE))
                return false;
            page = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        }
        const std::unique_lock lock(codePermissionsMutex_);
        auto permissions = codeReadablePages_;
        for (auto page = address; page < address + rounded; page += page_)
            permissions.insert_or_assign(page, readable);
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(address), rounded, PAGE_READONLY, &previous))
            return false;
        codeReadablePages_.swap(permissions);
        dataProtectionGeneration.fetch_add(1, std::memory_order_release);
        return true;
    }
    return false;
}

std::span<const uint8_t> ElfImage::CodeAt(uintptr_t address, size_t limit) const
{
    const auto base = reinterpret_cast<uintptr_t>(memory_);
    if (address < base || address - base >= size_)
        return {};
    const auto offset = address - base + floor_;
    for (const auto& segment : loads_) {
        if (!(segment.p_flags & Execute) || (!(segment.p_flags & Read) && machine_ != Machine::Arm64) ||
            offset < segment.p_vaddr)
            continue;
        const auto within = offset - segment.p_vaddr;
        if (within < segment.p_memsz) {
            limit = std::min<uint64_t>(limit, segment.p_memsz - within);
            // Decoders ask for every instruction, so avoid rescanning the full
            // dynamic symbol table. Preserve both the next function boundary
            // and the smallest nonzero size of aliases at this exact address.
            auto symbol = std::ranges::lower_bound(codeSymbols_, offset, {}, &CodeSymbol::address);
            if (symbol != codeSymbols_.end()) {
                if (symbol->address == offset) {
                    if (symbol->size)
                        limit = std::min<uint64_t>(limit, symbol->size);
                    ++symbol;
                }
                if (symbol != codeSymbols_.end())
                    limit = std::min<uint64_t>(limit, symbol->address - offset);
            }
            return {reinterpret_cast<const uint8_t*>(address), limit};
        }
    }
    return {};
}

uint8_t* ElfImage::At(uint64_t address, uint64_t size, uint32_t flags) const
{
    for (const auto& p : loads_) {
        if (address >= p.p_vaddr && Fits(address - p.p_vaddr, size, p.p_memsz) && (p.p_flags & flags) == flags)
            return memory_ + (address - floor_);
    }
    Invalid("ELF address outside the required load segment");
}

uint64_t ElfImage::Tag(int64_t type) const
{
    for (const auto& [tag, value] : dynamic_)
        if (tag == type)
            return value;
    return 0;
}

std::string_view ElfImage::String(uint64_t offset) const
{
    if (offset >= strings_.size())
        Invalid("invalid ELF string offset");
    const auto tail = strings_.substr(static_cast<size_t>(offset));
    const auto end = tail.find('\0');
    if (end == tail.npos)
        Invalid("unterminated ELF string");
    return tail.substr(0, end);
}

uint64_t ElfImage::CountGnuSymbols(uint64_t hash) const
{
    const auto* head = At(hash, 16, Read);
    const uint64_t bucketCount = ReadValue<uint32_t>(head);
    const uint64_t first = ReadValue<uint32_t>(head + 4);
    const uint64_t bloom = ReadValue<uint32_t>(head + 8);
    if (!bucketCount || !bloom || bucketCount > MaxSymbols || first > MaxSymbols || bloom > MaxSymbols)
        Invalid("invalid ELF GNU hash table");
    const uint64_t buckets = hash + 16 + bloom * 8;
    const auto* values = At(buckets, bucketCount * 4, Read);
    uint64_t last = 0;
    for (uint64_t i = 0; i < bucketCount; ++i)
        last = std::max(last, uint64_t(ReadValue<uint32_t>(values + i * 4)));
    if (last == 0)
        return first;
    if (last < first)
        Invalid("invalid ELF GNU hash bucket");
    const uint64_t chains = buckets + bucketCount * 4;
    for (; last < MaxSymbols; ++last) {
        if (ReadValue<uint32_t>(At(chains + (last - first) * 4, 4, Read)) & 1)
            return last + 1;
    }
    Invalid("unterminated ELF GNU hash chain");
}

ElfImage::LinkSymbol ElfImage::Defined(const Symbol& symbol, bool codeOnly) const
{
    const auto type = symbol.st_info & 15;
    if (type == 10 && symbol.st_shndx < 0xff00) {
        return {reinterpret_cast<uintptr_t>(At(symbol.st_value, 1, Execute)), true}; // STT_GNU_IFUNC
    }
    if (type != 0 && type != 1 && type != 2) {
        throw std::runtime_error("unsupported ELF symbol type " + std::to_string(type) + " for " +
                                 std::string(String(symbol.st_name)));
    }
    if (symbol.st_shndx == 0xfff1 && !codeOnly)
        return static_cast<uintptr_t>(symbol.st_value);
    if (symbol.st_shndx >= 0xff00)
        Invalid("unsupported ELF symbol section");
    return reinterpret_cast<uintptr_t>(At(symbol.st_value, 1, codeOnly ? Execute : 0));
}

uintptr_t ElfImage::Export(std::string_view name, bool codeOnly) const
{
    const auto symbol = FindLinkSymbol(name, codeOnly);
    if (symbol.indirect)
        Invalid("ELF indirect function requires native symbol lookup");
    return symbol.address;
}

ElfImage::LinkSymbol ElfImage::FindLinkSymbol(std::string_view name, bool codeOnly) const
{
    const auto symbol = exports_.find(name);
    return symbol == exports_.end() ? LinkSymbol{} : Defined(symbols_[symbol->second], codeOnly);
}

bool ElfImage::AddressInfo(uintptr_t address, void*& base, const char*& name, void*& symbol) const
{
    const auto start = reinterpret_cast<uintptr_t>(memory_);
    if (address < start || address - start >= size_)
        return false;
    const auto offset = address - start + floor_;
    for (const auto& segment : loads_) {
        if (offset < segment.p_vaddr || offset - segment.p_vaddr >= segment.p_memsz)
            continue;
        base = memory_;
        name = nullptr;
        symbol = nullptr;
        const Symbol* nearest = nullptr;
        for (const auto& [exportName, index] : exports_) {
            const auto& candidate = symbols_[index];
            if (candidate.st_shndx >= 0xff00 || candidate.st_value < segment.p_vaddr || candidate.st_value > offset ||
                (nearest && candidate.st_value <= nearest->st_value))
                continue;
            nearest = &candidate;
            name = exportName.c_str();
        }
        if (nearest)
            symbol = memory_ + (nearest->st_value - floor_);
        return true;
    }
    return false;
}

std::vector<std::string> ElfImage::ExportNames() const
{
    std::vector<std::string> result;
    result.reserve(exports_.size());
    for (const auto& [name, index] : exports_)
        result.push_back(name);
    return result;
}

void ElfImage::ApplyRelaTable(uint64_t address, uint64_t size, const RelocationSymbols& symbols)
{
    if (!size)
        return;
    if (!address || size % sizeof(Elf64Rela))
        Invalid("invalid ELF RELA table");
    const auto* table = At(address, size, Read);
    for (uint64_t offset = 0; offset < size; offset += sizeof(Elf64Rela))
        ApplyRela(ReadValue<Elf64Rela>(table + offset), symbols);
}

void ElfImage::ApplyRela(const Elf64Rela& rel, const RelocationSymbols& symbols)
{
    const auto type = static_cast<uint32_t>(rel.r_info);
    if (!type)
        return;
    const bool arm64 = machine_ == Machine::Arm64;
    const unsigned absolute = arm64 ? 257 : 1;
    const unsigned global = arm64 ? 1025 : 6;
    const unsigned jump = arm64 ? 1026 : 7;
    const unsigned relative = arm64 ? 1027 : 8;
    uint64_t value;
    if (type == relative) {
        if ((rel.r_info >> 32) != 0)
            Invalid("invalid relative ELF relocation");
        value = reinterpret_cast<uintptr_t>(memory_) - floor_ + static_cast<uint64_t>(rel.r_addend);
    }
    else if ((!arm64 && type == 37) || (arm64 && type == 1032)) { // IRELATIVE.
        if (rel.r_info >> 32)
            Invalid("invalid ELF indirect-relative relocation");
        auto* target = At(rel.r_offset, sizeof(value), Write);
        const auto resolver = reinterpret_cast<uintptr_t>(At(static_cast<uint64_t>(rel.r_addend), 1, Execute));
        indirect_.push_back({target, resolver, 0});
        return;
    }
    else if ((!arm64 && (type == 16 || type == 17 || type == 18)) ||
             (arm64 && (type == 1028 || type == 1029 || type == 1030 || type == 1031))) {
        // x86_64 DTPMOD64/DTPOFF64/TPOFF64; AArch64 DTPMOD/DTPREL/TLSDESC.
        const auto index = rel.r_info >> 32;
        if (index >= symbols_.size())
            Invalid("invalid ELF TLS relocation symbol");
        const auto& symbol = symbols_[index];
        if (index && (symbol.st_info & 15) != 6)
            Invalid("expected an ELF TLS symbol");
        TlsSymbol tls{};
        if (!index || symbol.st_shndx) {
            if (!tls_.p_type)
                Invalid("local ELF TLS relocation has no TLS segment");
            tls = {TlsModule(), index ? symbol.st_value : 0};
        }
        else {
            const auto name = String(symbol.st_name);
            if (symbols.resolveTls)
                tls = symbols.resolveTls(name);
            if (!tls.module && (symbol.st_info >> 4) != 2) {
                if (!symbols.missingImports)
                    throw std::runtime_error("unresolved Android TLS symbol: " + std::string(name));
                symbols.missingImports->emplace(name);
                return;
            }
        }
        const uint64_t offset = tls.offset + static_cast<uint64_t>(rel.r_addend);
        if (arm64 && type == 1031) {
            if (!symbols.tlsDescriptorResolver || (rel.r_offset & 7))
                Invalid("invalid ARM64 ELF TLS descriptor resolver or alignment");
            if (tls.module && offset >= reinterpret_cast<const ElfImage*>(tls.module)->tls_.p_memsz)
                Invalid("ARM64 ELF TLS descriptor exceeds its module");
            auto* destination = At(rel.r_offset, 2 * sizeof(uintptr_t), Write);
            auto descriptor = std::make_unique<TlsSymbol>(TlsSymbol{tls.module, offset});
            const uintptr_t words[]{symbols.tlsDescriptorResolver, reinterpret_cast<uintptr_t>(descriptor.get())};
            tlsDescriptors_.push_back(std::move(descriptor));
            std::memcpy(destination, words, sizeof(words));
            return;
        }
        else if ((!arm64 && type == 18) || (arm64 && type == 1030)) {
            const auto* owner = reinterpret_cast<const ElfImage*>(tls.module);
            if (owner && (owner->machine_ != machine_ || !owner->staticTlsOffset_ || offset >= owner->tls_.p_memsz))
                Invalid("ELF initial-exec TLS has no valid static allocation");
            value = !owner ? offset : arm64 ? owner->staticTlsOffset_ + offset : offset - owner->staticTlsOffset_;
        }
        else {
            value = type == 16 || type == 1028 ? tls.module : offset;
        }
    }
    else if (type == absolute || type == global || type == jump) {
        const auto index = rel.r_info >> 32;
        if (!index || index >= symbols_.size())
            Invalid("invalid ELF relocation symbol");
        const auto& symbol = symbols_[index];
        const auto name = String(symbol.st_name);
        LinkSymbol resolved;
        if (symbol.st_shndx) {
            if (symbols.interpose && (type == jump || type == global) && (symbol.st_other & 3) == 0 &&
                (symbol.st_info >> 4) != 0)
                resolved = symbols.interpose(name);
            if (!resolved)
                resolved = Defined(symbol, false);
        }
        else {
            resolved = symbols.resolve(name);
        }
        if (!resolved && !symbol.st_shndx && (symbol.st_info >> 4) != 2) {
            if (!symbols.missingImports)
                throw std::runtime_error("unresolved Android symbol: " + std::string(name));
            symbols.missingImports->emplace(name);
            return;
        }
        const uint64_t addend = arm64 || type == absolute ? static_cast<uint64_t>(rel.r_addend) : 0;
        if (resolved.indirect) {
            auto* target = At(rel.r_offset, sizeof(value), Write);
            indirect_.push_back({target, resolved.address, addend});
            return;
        }
        value = resolved.address + addend;
    }
    else {
        throw std::runtime_error(
            std::string(arm64 ? "unsupported AArch64 ELF relocation: " : "unsupported x86_64 ELF relocation: ") +
            std::to_string(type));
    }
    std::memcpy(At(rel.r_offset, sizeof(value), Write), &value, sizeof(value));
}

void ElfImage::ApplyAndroidRela(uint64_t address, uint64_t size, const RelocationSymbols& symbols)
{
    if (!address && !size)
        return;
    if (!address || size < 4)
        Invalid("invalid APS2 relocation table");
    const auto* input = At(address, size, Read);
    const auto* end = input + size;
    if (std::memcmp(input, "APS2", 4) != 0)
        Invalid("invalid APS2 relocation header");
    input += 4;
    auto remaining = ReadSleb(input, end);
    if (remaining > MaxImage / sizeof(uint64_t))
        Invalid("oversized APS2 relocation table");
    uint64_t offset = ReadSleb(input, end), addend = 0;
    constexpr uint64_t SharedInfo = 1, SharedOffset = 2, SharedAddend = 4, HasAddend = 8;
    while (remaining) {
        const auto count = ReadSleb(input, end);
        const auto flags = ReadSleb(input, end);
        if (!count || count > remaining || flags > 15)
            Invalid("invalid APS2 relocation group");
        const auto delta = flags & SharedOffset ? ReadSleb(input, end) : 0;
        const auto info = flags & SharedInfo ? ReadSleb(input, end) : 0;
        if (!(flags & HasAddend))
            addend = 0;
        else if (flags & SharedAddend)
            addend += ReadSleb(input, end);
        for (uint64_t i = 0; i < count; ++i) {
            offset += flags & SharedOffset ? delta : ReadSleb(input, end);
            const auto type = flags & SharedInfo ? info : ReadSleb(input, end);
            if ((flags & (HasAddend | SharedAddend)) == HasAddend)
                addend += ReadSleb(input, end);
            ApplyRela({offset, type, std::bit_cast<int64_t>(addend)}, symbols);
        }
        remaining -= count;
    }
}

void ElfImage::Relocate(const Resolver& resolve, std::set<std::string>* missingImports, const TlsResolver& resolveTls,
                        const Resolver& interpose, uintptr_t tlsDescriptorResolver)
{
    if ((Tag(8) && Tag(9) != sizeof(Elf64Rela)) || (Tag(2) && Tag(20) != 7))
        Invalid("ELF uses unsupported PLT or relocation encoding");
    const RelocationSymbols symbols{resolve, resolveTls, interpose, missingImports, tlsDescriptorResolver};
    ApplyAndroidRela(Tag(AndroidRela), Tag(AndroidRelaSize), symbols);
    ApplyRelaTable(Tag(7), Tag(8), symbols);
    ApplyRelaTable(Tag(23), Tag(2), symbols);
    const bool standard = Tag(36) || Tag(35);
    const auto address = Tag(standard ? 36 : AndroidRelr);
    const auto size = Tag(standard ? 35 : AndroidRelrSize);
    if (!address && !size)
        return;
    if (Tag(standard ? 37 : AndroidRelrEntry) != 8 || !size || size % 8 != 0 || !address)
        Invalid("invalid ELF RELR table");
    const auto* table = At(address, size, Read);
    const uint64_t bias = reinterpret_cast<uintptr_t>(memory_) - floor_;
    const auto patch = [&](uint64_t address) {
        auto* slot = At(address, 8, Write);
        const uint64_t value = ReadValue<uint64_t>(slot) + bias;
        std::memcpy(slot, &value, sizeof(value));
    };
    uint64_t next = 0;
    for (uint64_t i = 0; i < size; i += 8) {
        const auto word = ReadValue<uint64_t>(table + i);
        if ((word & 1) == 0) {
            if (word % 8 != 0)
                Invalid("unaligned ELF RELR target");
            patch(word);
            next = word + 8;
        }
        else {
            if (!next || next > MaxImage)
                Invalid("invalid ELF RELR bitmap");
            for (unsigned bit = 1; bit < 64; ++bit)
                if ((word >> bit) & 1)
                    patch(next + (bit - 1) * 8);
            next += 63 * 8;
        }
    }
}

std::vector<void*> ElfImage::Initializers() const
{
    std::vector<void*> result;
    if (const auto init = Tag(12))
        result.push_back(At(init, 1, Execute));
    const auto size = Tag(27);
    if (!size)
        return result;
    if (!Tag(25) || size % 8 || size > 65536)
        Invalid("invalid ELF initializer table");
    const auto* table = At(Tag(25), size, Read);
    const auto bias = reinterpret_cast<uintptr_t>(memory_) - floor_;
    for (uint64_t i = 0; i < size; i += 8) {
        const auto entry = ReadValue<uint64_t>(table + i);
        if (entry && entry != UINT64_MAX)
            result.push_back(At(entry - bias, 1, Execute));
    }
    return result;
}

void ElfImage::ResolveIndirectRelocations(const IndirectCall& call)
{
    if (indirect_.empty())
        return;
    // Make resolver code executable after ordinary relocations. GOT writes
    // still require RELRO pages to remain writable in this phase.
    ProtectSegments(machine_ == Machine::Arm64 ? Execution::Translated : Execution::Native, false);
    for (const auto& entry : indirect_) {
        const uint64_t value = call(entry.resolver) + entry.addend;
        std::memcpy(entry.destination, &value, sizeof(value));
    }
    indirect_.clear();
}

void ElfImage::TrapLinuxSyscall(uint64_t virtualAddress)
{
    auto* code = At(virtualAddress, 2, Execute);
    if (machine_ != Machine::X64 || code[0] != 0x0f || code[1] != 0x05)
        Invalid("unexpected Linux syscall instruction in ELF");
    // UD2 has the same size. Preserve the existing branches and unwind metadata.
    code[1] = 0x0b;
}

void ElfImage::PatchCode(uint64_t virtualAddress, std::span<const uint8_t> bytes)
{
    std::memcpy(At(virtualAddress, bytes.size(), Execute), bytes.data(), bytes.size());
}

void ElfImage::Protect(Execution execution)
{
    if (!indirect_.empty())
        Invalid("ELF indirect relocations require resolution before publication");
    ProtectSegments(execution, true);
    dataProtectionGeneration.fetch_add(1, std::memory_order_release);
}

void ElfImage::ProtectSegments(Execution execution, bool applyRelro)
{
    if (machine_ == Machine::Arm64 && execution == Execution::Native)
        Invalid("ARM64 ELF requires translation");
    std::vector<uint8_t> flags(size_ / page_);
    for (const auto& p : loads_) {
        const auto start = (p.p_vaddr - floor_) / page_;
        const auto end = (p.p_vaddr - floor_ + p.p_memsz + page_ - 1) / page_;
        for (auto i = start; i < end; ++i)
            flags[i] |= static_cast<uint8_t>(p.p_flags & 7);
    }
    for (const auto& p : relro_) {
        if (!p.p_memsz)
            continue;
        if (p.p_vaddr < floor_ || !Fits(p.p_vaddr - floor_, p.p_memsz, size_))
            Invalid("invalid ELF RELRO range");
        const auto start = (p.p_vaddr - floor_) / page_;
        const auto end = (p.p_vaddr - floor_ + p.p_memsz) / page_;
        if (applyRelro) {
            for (auto i = start; i < end; ++i)
                flags[i] &= static_cast<uint8_t>(~Write);
        }
    }
    for (size_t start = 0; start < flags.size();) {
        const auto access = flags[start];
        if ((access & (Write | Execute)) == (Write | Execute))
            Invalid("ELF requires a writable executable page");
        size_t end = start + 1;
        while (end < flags.size() && flags[end] == access)
            ++end;
        // Translation fetches ARM64 instructions as host data, including X-only
        // Android segments. DataAt still enforces the original guest read flags.
        const bool readable = (access & Read) || ((access & Execute) && machine_ == Machine::Arm64);
        const DWORD protection = (access & Execute) && execution == Execution::Native
                                     ? ((access & Read) ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
                                     : ((access & Write) ? PAGE_READWRITE : (readable ? PAGE_READONLY : PAGE_NOACCESS));
        DWORD old;
        if (!VirtualProtect(memory_ + start * page_, (end - start) * page_, protection, &old))
            Invalid("could not set ELF page protections");
        start = end;
    }
    if (execution == Execution::Native && !FlushInstructionCache(GetCurrentProcess(), memory_, size_))
        Invalid("could not flush ELF instruction cache");
}
} // namespace stone::windows
