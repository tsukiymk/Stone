#include "Arm64BlockJit.h"
#include "cache/Arm64BlockCache.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <vector>

namespace stone::windows {
namespace {
constexpr size_t MaximumInstructions = 32, MaximumEntries = 32768;

struct Hotness {
    uint64_t tag;
    uint32_t hits;
};

thread_local std::array<Hotness, 4096> hotness{};
static_assert(std::is_trivially_destructible_v<decltype(hotness)>);
std::atomic<uint64_t> warmupSkipped{0};

struct HostState {
    DWORD error = GetLastError();
    int savedErrno = errno;

    ~HostState()
    {
        errno = savedErrno;
        SetLastError(error);
    }
};

struct GrowableApi {
    decltype(&RtlAddGrowableFunctionTable) add;
    decltype(&RtlGrowFunctionTable) grow;
    decltype(&RtlDeleteGrowableFunctionTable) remove;

    GrowableApi()
    {
        const auto module = GetModuleHandleW(L"ntdll.dll");
        add = reinterpret_cast<decltype(add)>(GetProcAddress(module, "RtlAddGrowableFunctionTable"));
        grow = reinterpret_cast<decltype(grow)>(GetProcAddress(module, "RtlGrowFunctionTable"));
        remove = reinterpret_cast<decltype(remove)>(GetProcAddress(module, "RtlDeleteGrowableFunctionTable"));
        if (!add || !grow || !remove)
            throw std::runtime_error("growable native unwind tables are unavailable");
    }
};

const GrowableApi& UnwindApi()
{
    static const auto* api = new GrowableApi;
    return *api;
}

std::atomic<uint64_t> arenasCreated{0}, activeArenas{0}, committedCodeBytes{0}, arenaErrors{0};

struct Arena {
    static constexpr size_t Capacity = 1024, MaximumBlockBytes = 16384;
    std::array<RUNTIME_FUNCTION, Capacity> functions{};
    void* base = nullptr;
    void* table = nullptr;
    size_t page = 0, metadata = 0, reserved = 0, next = 0, count = 0;
    std::atomic<uint64_t> committed{0};

    size_t Rounded(size_t bytes) const
    {
        return (bytes + page - 1) & ~(page - 1);
    }

    Arena()
    {
        const auto& api = UnwindApi();
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        page = system.dwPageSize;
        metadata = Rounded((Capacity + 1) * 8);
        next = metadata;
        reserved = metadata + Capacity * MaximumBlockBytes;
        base = VirtualAlloc(nullptr, reserved, MEM_RESERVE, PAGE_NOACCESS);
        if (!base)
            throw std::bad_alloc();
        if (!VirtualAlloc(base, metadata, MEM_COMMIT, PAGE_READWRITE) ||
            api.add(&table, functions.data(), 0, Capacity, reinterpret_cast<ULONG_PTR>(base),
                    reinterpret_cast<ULONG_PTR>(base) + reserved)) {
            VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
            throw std::runtime_error("cannot register native code arena unwind table");
        }
        arenasCreated.fetch_add(1, std::memory_order_relaxed);
        activeArenas.fetch_add(1, std::memory_order_relaxed);
    }

    ~Arena()
    {
        const HostState hostState;
        UnwindApi().remove(table);
        committedCodeBytes.fetch_sub(committed.load(std::memory_order_relaxed), std::memory_order_relaxed);
        VirtualFree(base, 0, MEM_RELEASE);
        activeArenas.fetch_sub(1, std::memory_order_relaxed);
    }

    bool Fits(size_t bytes) const
    {
        return count < Capacity && bytes <= MaximumBlockBytes && Rounded(bytes) <= reserved - next;
    }

    std::pair<void*, size_t> Publish(std::span<const uint8_t> code)
    {
        if (!Fits(code.size()))
            throw std::runtime_error("native code arena capacity exceeded");
        const auto bytes = Rounded(code.size());
        auto* address = static_cast<uint8_t*>(base) + next;
        if (!VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE))
            throw std::bad_alloc();
        std::memcpy(address, code.data(), code.size());
        DWORD previous = 0;
        if (!VirtualProtect(address, bytes, PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), address, bytes)) {
            VirtualFree(address, bytes, MEM_DECOMMIT);
            throw std::runtime_error("cannot publish native ARM64 block");
        }
        constexpr uint8_t info[]{1, 5, 2, 0, 5, 0x32, 1, 0x30}; // ALLOC_SMALL 32; PUSH_NONVOL RBX.
        const auto unwind = (count + 1) * sizeof(info);
        std::memcpy(static_cast<uint8_t*>(base) + unwind, info, sizeof(info));
        functions[count] = {static_cast<DWORD>(next), static_cast<DWORD>(next + code.size()),
                            static_cast<DWORD>(unwind)};
        // Only append sorted immutable entries. Metadata stays committed even
        // after an individual code page is retired, until the last owner exits.
        UnwindApi().grow(table, static_cast<DWORD>(count + 1));
        ++count;
        next += bytes;
        committed.fetch_add(bytes, std::memory_order_relaxed);
        committedCodeBytes.fetch_add(bytes, std::memory_order_relaxed);
        return {address, bytes};
    }

    void Release(void* address, size_t bytes)
    {
        if (VirtualFree(address, bytes, MEM_DECOMMIT)) {
            committed.fetch_sub(bytes, std::memory_order_relaxed);
            committedCodeBytes.fetch_sub(bytes, std::memory_order_relaxed);
        }
        else
            arenaErrors.fetch_add(1, std::memory_order_relaxed);
    }
};

enum Register : unsigned {
    Ax = 0,
    Cx = 1,
    Dx = 2,
    Bx = 3,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11
};

static_assert(std::is_standard_layout_v<Arm64BlockContext>);

int64_t Signed(uint64_t value, unsigned bits)
{
    return std::bit_cast<int64_t>((value ^ (uint64_t{1} << (bits - 1))) - (uint64_t{1} << (bits - 1)));
}

bool Condition(unsigned condition, unsigned flags)
{
    if (condition >= 14)
        return true;
    const bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1;
    const bool choices[]{z, c, n, v, c && !z, n == v, !z && n == v};
    return choices[condition / 2] != bool(condition & 1);
}

class Compiler {
public:
    Compiler(uintptr_t pc, uintptr_t bias, std::span<const uint8_t> source, bool immutable)
        : start(pc), bias(bias), source(source), immutable(immutable),
          persistent(immutable && Arm64BlockDiskCacheEnabled())
    {
    }

    void Build()
    {
        Bytes({0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xcb}); // push rbx; shadow space; rbx=context.
        loopStart = code.size();
        Reload();
        const auto limit = std::min(MaximumInstructions, source.size() / 4);
        for (size_t i = 0; i < limit && !ended; ++i) {
            uint32_t word;
            std::memcpy(&word, source.data() + i * 4, 4);
            const auto saved = code.size(), fixes = exits.size(), addresses = relocations.size();
            const bool required = requiresFlags, written = flagsWritten;
            if (!Instruction(word, start + i * 4, static_cast<uint32_t>(i + 1))) {
                code.resize(saved);
                exits.resize(fixes);
                relocations.resize(addresses);
                requiresFlags = required;
                flagsWritten = written;
                observed = i + 1;
                break;
            }
            words.push_back(word);
            observed = i + 1;
        }
        terminates = terminates || ended;
        static_assert(MaximumInstructions * 2 <= 127);
        for (const auto& fix : repeats) {
            const auto next = static_cast<uint32_t>(words.size());
            const auto total = next + static_cast<uint32_t>(fix[2]);
            code[fix[0]] = static_cast<uint8_t>(total);
            code[fix[1]] = static_cast<uint8_t>(next);
        }
        if (!ended)
            Finish(start + words.size() * 4, Arm64BlockExit::Continue, static_cast<uint32_t>(words.size()));
        const auto epilogue = code.size();
        Bytes({0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
        for (const auto offset : exits)
            Patch(offset, epilogue);
    }

    std::vector<uint8_t> code;
    std::vector<uint32_t> words;
    std::vector<uint32_t> relocations;
    size_t observed = 0;
    bool requiresFlags = false;
    bool needsMemory = false;
    bool needsVectors = false;
    bool terminates = false;

    bool Repeats() const
    {
        return !repeats.empty();
    }

private:
    void Bytes(std::initializer_list<uint8_t> values)
    {
        code.insert(code.end(), values);
    }

    void Immediate(uint64_t value, unsigned bytes)
    {
        for (unsigned i = 0; i < bytes; ++i)
            code.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }

    void Rex(bool wide, unsigned reg, unsigned rm)
    {
        const auto value = static_cast<uint8_t>(0x40 | (wide ? 8 : 0) | ((reg & 8) >> 1) | ((rm & 8) >> 3));
        if (value != 0x40)
            code.push_back(value);
    }

    void Literal(unsigned reg, uint64_t value, bool wide = true)
    {
        Rex(wide, 0, reg);
        code.push_back(static_cast<uint8_t>(0xb8 + (reg & 7)));
        Immediate(value, wide ? 8 : 4);
    }

    void PcLiteral(uintptr_t value)
    {
        Literal(Ax, value);
        if (persistent)
            relocations.push_back(static_cast<uint32_t>(code.size() - 8));
    }

    void Move(unsigned dest, unsigned src, bool wide = true)
    {
        Rex(wide, src, dest);
        Bytes({0x89, static_cast<uint8_t>(0xc0 | ((src & 7) << 3) | (dest & 7))});
    }

    void Address(unsigned reg, unsigned base, size_t offset)
    {
        // Current bases are RBX, R10 and R11, so no SIB is needed. Positive
        // disp8 stops at 127; register/vector bank offsets above it stay disp32.
        const bool small = offset <= 127;
        code.push_back(static_cast<uint8_t>((small ? 0x40 : 0x80) | ((reg & 7) << 3) | (base & 7)));
        Immediate(offset, small ? 1 : 4);
    }

    void Load(unsigned reg, unsigned base, size_t offset, bool wide = true)
    {
        Rex(wide, reg, base);
        code.push_back(0x8b);
        Address(reg, base, offset);
    }

    void Store(unsigned base, size_t offset, unsigned reg, bool wide = true)
    {
        Rex(wide, reg, base);
        code.push_back(0x89);
        Address(reg, base, offset);
    }

    void Field(size_t offset, uint32_t value)
    {
        code.push_back(0xc7);
        Address(0, Bx, offset);
        Immediate(value, 4);
    }

    void Reload()
    {
        Load(R10, Bx, offsetof(Arm64BlockContext, registers));
    }

    void Read(unsigned host, unsigned guest, unsigned width, bool stack = false)
    {
        if (guest != 31)
            Load(host, R10, guest * 8, width == 64);
        else if (stack)
            Load(host, Bx, offsetof(Arm64BlockContext, sp), width == 64);
        else
            Literal(host, 0, false);
    }

    void Write(unsigned guest, unsigned host)
    {
        if (guest != 31)
            Store(R10, guest * 8, host);
    }

    void Binary(uint8_t opcode, unsigned dest, unsigned right, bool wide)
    {
        Rex(wide, right, dest);
        Bytes({opcode, static_cast<uint8_t>(0xc0 | ((right & 7) << 3) | (dest & 7))});
    }

    void ArithmeticImmediate(unsigned dest, uint32_t value, bool subtract, bool wide)
    {
        // ARM's unsigned imm12 (optionally shifted by 12) fits a positive
        // signed imm32. Only 0..127 can use x64's sign-extended imm8 form.
        const bool small = value <= 127;
        Rex(wide, 0, dest);
        Bytes({static_cast<uint8_t>(small ? 0x83 : 0x81),
               static_cast<uint8_t>(0xc0 | (subtract ? 0x28 : 0) | (dest & 7))});
        Immediate(value, small ? 1 : 4);
    }

    void Sse(uint8_t opcode, unsigned dest, unsigned right)
    {
        Bytes({0x66, 0x0f, opcode, static_cast<uint8_t>(0xc0 | (dest << 3) | right)});
    }

    void VectorRead(unsigned host, unsigned guest)
    {
        Bytes({0xf3});
        Rex(false, host, R11);
        Bytes({0x0f, 0x6f});
        Address(host, R11, guest * 16);
    }

    void VectorWrite(unsigned guest, unsigned host, bool full = true)
    {
        if (!full)
            Bytes({0xf3, 0x0f, 0x7e, static_cast<uint8_t>(0xc0 | (host << 3) | host)}); // MOVQ clears the upper half.
        Bytes({0xf3});
        Rex(false, host, R11);
        Bytes({0x0f, 0x7f});
        Address(host, R11, guest * 16);
    }

    void SseShift(uint8_t opcode, unsigned operation, unsigned reg, unsigned count)
    {
        Bytes({0x66, 0x0f, opcode, static_cast<uint8_t>(0xc0 | (operation << 3) | reg), static_cast<uint8_t>(count)});
    }

    void Shuffle(unsigned dest, unsigned src, unsigned control)
    {
        Bytes({0x66, 0x0f, 0x70, static_cast<uint8_t>(0xc0 | (dest << 3) | src), static_cast<uint8_t>(control)});
    }

    bool VectorInstruction(uint32_t word);

    void Unary(unsigned reg, unsigned operation, bool wide)
    {
        Rex(wide, 0, reg);
        Bytes({0xf7, static_cast<uint8_t>(0xc0 | (operation << 3) | (reg & 7))});
    }

    void Shift(unsigned reg, unsigned kind, unsigned amount, bool wide, bool variable = false)
    {
        static constexpr unsigned operations[]{4, 5, 7, 1};
        if (!variable && !amount)
            return;
        Rex(wide, 0, reg);
        Bytes({static_cast<uint8_t>(variable ? 0xd3 : 0xc1),
               static_cast<uint8_t>(0xc0 | (operations[kind] << 3) | (reg & 7))});
        if (!variable)
            code.push_back(static_cast<uint8_t>(amount));
    }

    size_t Jump(unsigned condition = 16)
    {
        if (condition == 16)
            code.push_back(0xe9);
        else
            Bytes({0x0f, static_cast<uint8_t>(0x80 + condition)});
        const auto at = code.size();
        Immediate(0, 4);
        return at;
    }

    void Patch(size_t at, size_t target)
    {
        const auto displacement = static_cast<int32_t>(target - at - 4);
        std::memcpy(code.data() + at, &displacement, 4);
    }

    void Finish(uintptr_t target, Arm64BlockExit kind, uint32_t count)
    {
        PcLiteral(target);
        Store(Bx, offsetof(Arm64BlockContext, pc), Ax);
        const auto instruction = start + (count - 1) * 4;
        // These fields are usually only four bytes apart. Derive the second
        // address from the already relocated first one; distant/wrapping
        // targets retain the full independent literal.
        if (compactPc && instruction == target) {
        }
        else if (compactPc && instruction > target && instruction - target <= INT32_MAX)
            ArithmeticImmediate(Ax, static_cast<uint32_t>(instruction - target), false, true);
        else if (compactPc && target > instruction && target - instruction <= INT32_MAX)
            ArithmeticImmediate(Ax, static_cast<uint32_t>(target - instruction), true, true);
        else
            PcLiteral(instruction);
        Store(Bx, offsetof(Arm64BlockContext, instruction), Ax);
        FinishState(kind, count);
    }

    void FinishRegister(Arm64BlockExit kind, uint32_t count)
    {
        PcLiteral(start + (count - 1) * 4);
        Store(Bx, offsetof(Arm64BlockContext, instruction), Ax);
        FinishState(kind, count);
    }

    void FinishState(Arm64BlockExit kind, uint32_t count)
    {
        Field(offsetof(Arm64BlockContext, exit), static_cast<uint32_t>(kind));
        Count(count);
        exits.push_back(Jump());
        ended = true;
    }

    void Count(uint32_t count)
    {
        Load(Ax, Bx, offsetof(Arm64BlockContext, completed), false);
        ArithmeticImmediate(Ax, count, false, false);
        Store(Bx, offsetof(Arm64BlockContext, executed), Ax, false);
    }

    void Repeat(uintptr_t target, uint32_t count)
    {
        if (target != start)
            return;
        // Re-enter the same validated block only while a complete next
        // iteration fits the caller's signal and instruction budget.
        Load(Ax, Bx, offsetof(Arm64BlockContext, completed), false);
        ArithmeticImmediate(Ax, 0, false, false);
        const auto addition = code.size() - 1;
        Load(Dx, Bx, offsetof(Arm64BlockContext, budget), false);
        Binary(0x39, Ax, Dx, false);
        const auto done = Jump(7); // JA: next iteration exceeds budget.
        ArithmeticImmediate(Ax, 0, true, false);
        const auto subtraction = code.size() - 1;
        repeats.push_back({addition, subtraction, count});
        Store(Bx, offsetof(Arm64BlockContext, completed), Ax, false);
        const auto again = Jump();
        Patch(again, loopStart);
        Patch(done, code.size());
    }

    void Flags(bool subtract = false, bool logical = false)
    {
        // Capture all flags before any packing operation changes host EFLAGS.
        Bytes({0x41, 0x0f, 0x98, 0xc0, 0x41, 0x0f, 0x94, 0xc1}); // SETS r8b; SETZ r9b.
        Bytes({0x0f, static_cast<uint8_t>(subtract ? 0x93 : 0x92), 0xc2, 0x41, 0x0f, 0x90, 0xc3});
        Bytes({0x0f, 0xb6, 0xd2, 0x45, 0x0f, 0xb6, 0xc0, 0x45, 0x0f, 0xb6, 0xc9, 0x45, 0x0f, 0xb6, 0xdb});
        Bytes({0x41, 0xc1, 0xe0, 0x03, 0x41, 0xc1, 0xe1, 0x02, 0xd1, 0xe2});
        Binary(0x09, R8, R9, false);
        if (!logical) {
            Binary(0x09, R8, Dx, false);
            Binary(0x09, R8, R11, false);
        }
        Store(Bx, offsetof(Arm64BlockContext, nzcv), R8, false);
        Field(offsetof(Arm64BlockContext, flagsValid), 1);
        flagsWritten = true;
    }

    void TestCondition(unsigned condition)
    {
        if (condition < 14 && !flagsWritten)
            requiresFlags = true;
        uint32_t mask = 0;
        for (unsigned flags = 0; flags < 16; ++flags)
            if (Condition(condition, flags))
                mask |= 1u << flags;
        Literal(R8, mask, false);
        Load(R9, Bx, offsetof(Arm64BlockContext, nzcv), false);
        Bytes({0x45, 0x0f, 0xa3, 0xc8}); // BT r8d,r9d; CF is the condition result.
    }

    void Conditional(size_t jumpToTaken, uintptr_t target, uint32_t count)
    {
        // Keep the untaken path in native code; a taken branch exits with the
        // exact prefix count. Repeating paths reserve the longest continuation.
        const auto continued = Jump();
        Patch(jumpToTaken, code.size());
        Repeat(target, count);
        Finish(target, Arm64BlockExit::Branch, count);
        Patch(continued, code.size());
        ended = false;
        terminates = true;
    }

    void Memory(uint32_t word, uintptr_t pc, uint32_t count, Arm64BlockMemory kind, bool store)
    {
        needsMemory = true;
        const bool stackTrace = store && !immutable && Arm64StackStoreTraceEnabled();
        if (stackTrace)
            Field(offsetof(Arm64BlockContext, stackStoreOnly), 0);
        PcLiteral(pc);
        Store(Bx, offsetof(Arm64BlockContext, instruction), Ax);
        if (compactPc)
            ArithmeticImmediate(Ax, 4, false, true);
        else
            PcLiteral(pc + 4);
        Store(Bx, offsetof(Arm64BlockContext, pc), Ax);
        Count(count);
        Move(Cx, Bx);
        Literal(Dx, word, false);
        Literal(R8, static_cast<uint32_t>(kind), false);
        code.push_back(0xff);
        Address(2, Bx, offsetof(Arm64BlockContext, memory));
        code.push_back(0x83);
        Address(7, Bx, offsetof(Arm64BlockContext, exit));
        code.push_back(static_cast<uint8_t>(Arm64BlockExit::Failure));
        exits.push_back(Jump(4)); // JE epilogue on captured failure.
        Reload();
        // ELF executable pages cannot be made writable by the guest. Mapped
        // code can have a writable alias. Only an explicit certificate for
        // writes to the owner's non-executable stack permits continuation.
        if (store && !immutable) {
            size_t safe = 0;
            if (stackTrace) {
                code.push_back(0x83);
                Address(7, Bx, offsetof(Arm64BlockContext, stackStoreOnly));
                code.push_back(1);
                safe = Jump(4); // JE: only verified stack writes may continue.
            }
            Finish(pc + 4, Arm64BlockExit::Continue, count);
            if (stackTrace) {
                Patch(safe, code.size());
                ended = false;
            }
        }
    }

    static uint64_t LogicalMask(uint32_t word, unsigned width)
    {
        const unsigned n = (word >> 22) & 1, r = (word >> 16) & 63, s = (word >> 10) & 63;
        const unsigned length = std::bit_width((n << 6) | (s ^ 63));
        if (length < 2 || (n && width == 32))
            return 0;
        const unsigned size = 1u << (length - 1), levels = size - 1, ones = (s & levels) + 1, rotation = r & levels;
        if (ones == size)
            return 0;
        const auto element = (uint64_t{1} << ones) - 1, mask = UINT64_MAX >> (64 - size);
        const auto value = ((element >> rotation) | (element << ((size - rotation) & levels))) & mask;
        uint64_t result = 0;
        for (unsigned bit = 0; bit < width; bit += size)
            result |= value << bit;
        return result;
    }

    bool Instruction(uint32_t word, uintptr_t pc, uint32_t count);
    uintptr_t start, bias;
    std::span<const uint8_t> source;
    std::vector<size_t> exits;
    std::vector<std::array<size_t, 3>> repeats;
    bool flagsWritten = false, ended = false;
    const bool immutable;
    const bool compactPc = Arm64CompactPcEnabled();
    const bool persistent;
    size_t loopStart = 0;
};

bool Compiler::Instruction(uint32_t word, uintptr_t pc, uint32_t count)
{
    const unsigned dest = word & 31, src = (word >> 5) & 31, right = (word >> 16) & 31, width = word >> 31 ? 64 : 32;
    const bool wide = width == 64;
    if (word == 0xd503201f || word == 0xd503241f || word == 0xd503245f || word == 0xd503249f || word == 0xd50324df ||
        word == 0xd503233f || word == 0xd50323bf || word == 0xd50320ff || word == 0xd503219f || word == 0xd50321df)
        return true;
    if ((word & 0xfffffc1f) == 0xd65f0000 || (word & 0xffdffc1f) == 0xd61f0000) {
        Read(Ax, src, 64);
        Store(Bx, offsetof(Arm64BlockContext, pc), Ax);
        if ((word & 0xfffffc1f) != 0xd65f0000 && (word & 0x00200000)) {
            PcLiteral(pc + 4);
            Write(30, Ax);
        }
        FinishRegister((word & 0xfffffc1f) == 0xd65f0000 ? Arm64BlockExit::Return : Arm64BlockExit::Branch, count);
        return true;
    }
    if ((word & 0x7c000000) == 0x14000000) {
        if (word >> 31) {
            PcLiteral(pc + 4);
            Write(30, Ax);
        }
        const auto target = pc + Signed(word & 0x03ffffff, 26) * 4;
        Repeat(target, count);
        Finish(target, Arm64BlockExit::Branch, count);
        return true;
    }
    if ((word & 0xff000010) == 0x54000000) {
        TestCondition(word & 15);
        const auto taken = Jump(2);
        Conditional(taken, pc + Signed((word >> 5) & 0x7ffff, 19) * 4, count);
        return true;
    }
    if ((word & 0x7e000000) == 0x34000000 || (word & 0x7e000000) == 0x36000000) {
        Read(Ax, dest, width);
        if ((word & 0x7e000000) == 0x34000000)
            Binary(0x85, Ax, Ax, wide);
        else {
            const unsigned bit = ((word >> 26) & 32) | ((word >> 19) & 31);
            Rex(wide, 0, Ax);
            Bytes({0x0f, 0xba, 0xe0, static_cast<uint8_t>(bit)}); // BT rax,imm8.
        }
        const bool testBit = (word & 0x7e000000) == 0x36000000;
        const auto taken = Jump(testBit ? (word & 0x01000000 ? 2 : 3) : (word & 0x01000000 ? 5 : 4));
        Conditional(taken, pc + Signed((word >> 5) & (testBit ? 0x3fff : 0x7ffff), testBit ? 14 : 19) * 4, count);
        return true;
    }
    if ((word & 0x1f000000) == 0x10000000) {
        const auto delta = Signed((((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3), 21);
        PcLiteral(word >> 31 ? bias + ((pc - bias) & ~uint64_t{4095}) + delta * 4096 : pc + delta);
        Write(dest, Ax);
        return true;
    }
    if ((word & 0x1f800000) == 0x12800000) {
        const unsigned kind = (word >> 29) & 3, shift = ((word >> 21) & 3) * 16;
        if (kind == 1 || shift >= width)
            return false;
        const auto value = uint64_t((word >> 5) & 65535) << shift;
        if (kind == 3) {
            Read(Ax, dest, width);
            Literal(Dx, ~(uint64_t{65535} << shift), wide);
            Binary(0x21, Ax, Dx, wide);
            Literal(Dx, value, wide);
            Binary(0x09, Ax, Dx, wide);
        }
        else
            Literal(Ax, kind == 0 ? ~value : value, wide);
        Write(dest, Ax);
        return true;
    }
    const bool immediate = (word & 0x1f800000) == 0x11000000, shifted = (word & 0x1f200000) == 0x0b000000;
    const bool extended = (word & 0x1f200000) == 0x0b200000;
    if (immediate || shifted || extended) {
        const unsigned extension = (word >> 13) & 7, bits = 8u << (extension & 3), amount = (word >> 10) & 7;
        if (extended && ((word & 0x00c00000) || bits > width || amount > 4))
            return false;
        if ((immediate || extended) && dest == 31 && !(word & 0x20000000)) {
            if (!wide)
                return false;
            Memory(word, pc, count, Arm64BlockMemory::StackArithmetic, false);
            return true;
        }
        if (shifted && (((word >> 22) & 3) == 3 || ((word >> 10) & 63) >= width))
            return false;
        Read(Ax, src, width, immediate || extended);
        if (immediate)
            ArithmeticImmediate(Ax, ((word >> 10) & 4095) << ((word & 0x00400000) ? 12 : 0), word & 0x40000000, wide);
        else {
            Read(Dx, right, width);
            if (extended) {
                Shift(Dx, 0, width - bits, wide);
                Shift(Dx, extension & 4 ? 2 : 1, width - bits, wide);
                Shift(Dx, 0, amount, wide);
            }
            else
                Shift(Dx, (word >> 22) & 3, (word >> 10) & 63, wide);
            Binary(word & 0x40000000 ? 0x29 : 0x01, Ax, Dx, wide);
        }
        Write(dest, Ax);
        if (word & 0x20000000)
            Flags(word & 0x40000000);
        return true;
    }
    const bool logicalImmediate = (word & 0x1f800000) == 0x12000000;
    if (logicalImmediate || (word & 0x1f000000) == 0x0a000000) {
        const unsigned operation = (word >> 29) & 3;
        const auto mask = logicalImmediate ? LogicalMask(word, width) : 0;
        if ((logicalImmediate && !mask) || (!logicalImmediate && ((word >> 10) & 63) >= width))
            return false;
        if (logicalImmediate && dest == 31 && operation != 3) {
            Memory(word, pc, count, Arm64BlockMemory::StackArithmetic, false);
            return true;
        }
        Read(Ax, src, width);
        if (logicalImmediate)
            Literal(Dx, mask, wide);
        else {
            Read(Dx, right, width);
            Shift(Dx, (word >> 22) & 3, (word >> 10) & 63, wide);
            if (word & 0x00200000)
                Unary(Dx, 2, wide);
        }
        static constexpr uint8_t opcodes[]{0x21, 0x09, 0x31, 0x21};
        Binary(opcodes[operation], Ax, Dx, wide);
        Write(dest, Ax);
        if (operation == 3)
            Flags(false, true);
        return true;
    }
    if ((word & 0x7fe0f000) == 0x1ac02000) {
        Read(Ax, src, width);
        Read(Cx, right, 64);
        Shift(Ax, (word >> 10) & 3, 0, wide, true);
        Write(dest, Ax);
        return true;
    }
    if ((word & 0x1f800000) == 0x13000000 && ((word >> 29) & 3) != 3) {
        const unsigned kind = (word >> 29) & 3, r = (word >> 16) & 63, s = (word >> 10) & 63;
        if (((word >> 22) & 1) != wide || r >= width || s >= width)
            return false;
        // Keep the frequent full-width shift aliases compact.
        if (kind == 2 && s == width - 1) {
            Read(Ax, src, width);
            Shift(Ax, 1, r, wide);
            Write(dest, Ax);
            return true;
        }
        if (kind == 0 && s == width - 1) {
            Read(Ax, src, width);
            Shift(Ax, 2, r, wide);
            Write(dest, Ax);
            return true;
        }
        const unsigned position = r > s ? width - r : 0, bits = s + 1 - (position ? 0 : r);
        const auto mask = (UINT64_MAX >> (64 - bits)) << position;
        Read(Ax, src, width);
        Shift(Ax, position ? 0 : 1, position ? position : r, wide);
        Literal(Dx, mask, wide);
        Binary(0x21, Ax, Dx, wide);
        if (kind == 0) {
            Shift(Ax, 0, width - position - bits, wide);
            Shift(Ax, 2, width - position - bits, wide);
        }
        else if (kind == 1) {
            Read(Dx, dest, width);
            Literal(R8, ~mask, wide);
            Binary(0x21, Dx, R8, wide);
            Binary(0x09, Ax, Dx, wide);
        }
        Write(dest, Ax);
        return true;
    }
    if ((word & 0x3fe00800) == 0x1a800000) {
        Read(Ax, src, width);
        Read(Dx, right, width);
        if (word & 0x40000000)
            Unary(Dx, 2, wide);
        if (word & 0x00000400) {
            Literal(R8, 1, false);
            Binary(0x01, Dx, R8, wide);
        }
        TestCondition((word >> 12) & 15);
        Rex(wide, Ax, Dx);
        Bytes({0x0f, 0x43, 0xc2});
        Write(dest, Ax);
        return true;
    }
    if (VectorInstruction(word))
        return true;
    if ((word & 0x3b000000) == 0x18000000) {
        if ((word & 0x04000000) && word >> 30 == 3)
            return false;
        Memory(word, pc, count, Arm64BlockMemory::Literal, false);
        return true;
    }
    if ((word & 0x3b000000) == 0x39000000 || (word & 0x3b200000) == 0x38000000 || (word & 0x3b200c00) == 0x38200800) {
        const bool vector = word & 0x04000000;
        const auto op = (word >> 22) & 3;
        if (!vector && word >> 30 == 3 && (word & 0x00800000))
            return true;
        if (!vector && word >> 30 == 2 && op == 3)
            return false;
        Memory(word, pc, count, Arm64BlockMemory::Transfer, vector ? !(op & 1) : !op);
        return true;
    }
    const auto pair = word & 0xfe000000;
    if (pair == 0x28000000 || (word & 0xfe400000) == 0x68400000 || pair == 0xa8000000 || pair == 0x2c000000 ||
        pair == 0x6c000000 || pair == 0xac000000) {
        const auto kind = pair == 0xa8000000                           ? Arm64BlockMemory::Pair8
                          : pair == 0xac000000                         ? Arm64BlockMemory::Pair16Vector
                          : (pair == 0x2c000000 || pair == 0x6c000000) ? Arm64BlockMemory::Pair4Vector
                                                                       : Arm64BlockMemory::Pair4;
        // 64-bit vector pairs transfer eight bytes, represented by a distinct callback tag below.
        if (pair == 0x6c000000)
            return false;
        Memory(word, pc, count, kind, !(word & 0x00400000));
        return true;
    }
    return false;
}

bool Compiler::VectorInstruction(uint32_t word)
{
    const auto alu = word & 0xbf20fc00u, pair = word & 0x9f3ffc00u, widen = alu & ~0x20000000u;
    const unsigned size = (word >> 22) & 3, dest = word & 31, left = (word >> 5) & 31, right = (word >> 16) & 31;
    const bool full = word & 0x40000000u;
    const bool add = alu == 0x0e208400 || alu == 0x2e208400;
    const bool longPair = (word & 0x20000000u) && (pair == 0x0e202800 || pair == 0x0e206800);
    const bool longAdd = (word & 0x20000000u) && (widen & ~0x3000u) == 0x0e200000;
    if (!add && !longPair && !longAdd)
        return false;
    if ((add && size == 3 && !full) || (!add && size == 3))
        return false;
    needsVectors = true;
    Load(R11, Bx, offsetof(Arm64BlockContext, vectors));
    VectorRead(0, left);
    static constexpr uint8_t sums[]{0xfc, 0xfd, 0xfe, 0xd4}, differences[]{0xf8, 0xf9, 0xfa, 0xfb};
    static constexpr uint8_t unpack[]{0x60, 0x61, 0x62};
    if (add) {
        VectorRead(1, right);
        Sse(alu == 0x0e208400 ? sums[size] : differences[size], 0, 1);
        VectorWrite(dest, 0, full);
        return true;
    }
    if (longAdd) {
        VectorRead(1, right);
        Sse(0xef, 2, 2);
        if (full)
            SseShift(0x73, 3, 1, 8); // PSRLDQ selects the high narrow half.
        Sse(unpack[size], 1, 2);
        if (!(widen & 0x1000)) {
            if (full)
                SseShift(0x73, 3, 0, 8);
            Sse(unpack[size], 0, 2);
        }
        Sse((widen & 0x2000) ? differences[size + 1] : sums[size + 1], 0, 1);
        VectorWrite(dest, 0);
        return true;
    }
    if (size < 2) {
        Sse(0x75, 3, 3);
        SseShift(0x71, 2, 3, 15); // Eight signed 16-bit ones.
        if (!size) {
            Sse(0x6f, 1, 0);
            Sse(0xef, 2, 2);
            Sse(0x60, 0, 2);
            Sse(0x68, 1, 2);
            Sse(0xf5, 0, 3);
            Sse(0xf5, 1, 3);
            Sse(0x6b, 0, 1); // Byte-pair sums fit signed words (0..510).
        }
        else {
            // PMADDWD is signed; each negative input word needs +65536.
            Sse(0xef, 2, 2);
            Sse(0x65, 2, 0);
            Sse(0xf5, 2, 3);
            SseShift(0x72, 6, 2, 16);
            Sse(0xf5, 0, 3);
            Sse(0xfa, 0, 2);
        }
    }
    else {
        Sse(0x6f, 1, 0);
        Sse(0xef, 2, 2);
        Sse(0x62, 0, 2);
        Sse(0x6a, 1, 2);
        Shuffle(2, 0, 0x4e);
        Sse(0xd4, 0, 2);
        Shuffle(2, 1, 0x4e);
        Sse(0xd4, 1, 2);
        Sse(0x6c, 0, 1);
    }
    if (pair == 0x0e206800) {
        VectorRead(1, dest);
        Sse(sums[size + 1], 0, 1);
    }
    VectorWrite(dest, 0, full);
    return true;
}

using Key = std::tuple<uint64_t, uintptr_t, uintptr_t, uint32_t>;

struct Entry {
    Key key{};
    uint64_t used = 0;
    std::shared_ptr<const Arm64Block> block;
};

struct Cache {
    std::mutex mutex;
    Arm64BlockStatistics stats;
    // Four ways per bucket bound lookup cost and keep hot blocks when cold
    // code arrives. No tree nodes or iterator bookkeeping in guest dispatch.
    std::array<Entry, MaximumEntries> entries;
    std::shared_ptr<Arena> arena;
    uint64_t serial = 0;
    std::atomic<uint64_t> executions{0}, instructions{0}, dispatches{0};
};

Cache& Blocks()
{
    static auto* cache = new Cache;
    return *cache;
}
}

struct Arm64Block::Implementation {
    uint64_t image = 0, generation = 0;
    uintptr_t pc = 0, bias = 0;
    std::vector<uint8_t> source;
    std::vector<uint32_t> words;
    bool requiresFlags = false, needsMemory = false, needsVectors = false;
    void* memory = nullptr;
    size_t bytes = 0, codeBytes = 0;
    std::shared_ptr<Arena> arena;

    ~Implementation()
    {
        const HostState hostState;
        if (memory)
            arena->Release(memory, bytes);
    }
};

Arm64Block::Arm64Block() : impl(std::make_unique<Implementation>())
{
}

Arm64Block::~Arm64Block() = default;

bool Arm64Block::Matches(uint64_t image, uintptr_t pc, uintptr_t bias, uint64_t generation,
                         std::span<const uint8_t> source) const
{
    return impl->image == image && impl->pc == pc && impl->bias == bias && impl->generation == generation &&
           source.size() >= impl->source.size() &&
           !std::memcmp(source.data(), impl->source.data(), impl->source.size());
}

uint32_t Arm64Block::Instructions() const
{
    return static_cast<uint32_t>(impl->words.size());
}

size_t Arm64Block::CodeBytes() const
{
    return impl->codeBytes;
}

bool Arm64Block::RequiresFlags() const
{
    return impl->requiresFlags;
}

std::span<const uint32_t> Arm64Block::Words() const
{
    return impl->words;
}

void Arm64Block::Run(Arm64BlockContext& context) const
{
    if (!impl->memory || !context.registers || (impl->needsMemory && !context.memory) ||
        (impl->needsVectors && !context.vectors) || (RequiresFlags() && !context.flagsValid) || context.nzcv > 15 ||
        (context.budget && context.budget < Instructions()))
        throw std::invalid_argument("invalid native ARM64 block context");
    context.exit = Arm64BlockExit::Continue;
    context.executed = context.completed = 0;
    reinterpret_cast<void (*)(Arm64BlockContext*)>(impl->memory)(&context);
}

bool Arm64StackStoreTraceEnabled()
{
    static const bool enabled = [] {
        const HostState preserve;
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"STONE_ARM64_STACK_STORE_TRACE", value, 2) == 1 && value[0] == L'1';
    }();
    return enabled;
}

bool Arm64CompactPcEnabled()
{
    static const bool enabled = [] {
        const HostState preserve;
        wchar_t value[2]{};
        return !(GetEnvironmentVariableW(L"STONE_ARM64_COMPACT_PC", value, 2) == 1 && value[0] == L'0');
    }();
    return enabled;
}

bool Arm64BlockJitEnabled()
{
    const HostState hostState;
    static const bool enabled = [] {
        const DWORD error = GetLastError();
        char value[2]{};
        const auto size = GetEnvironmentVariableA("STONE_ARM64_BLOCK_JIT", value, 2);
        SetLastError(error);
        // Remains opt-in until the native-block validation stage is complete.
        return size == 1 && value[0] == '1';
    }();
    return enabled;
}

bool WarmArm64Block(uint64_t image, uintptr_t pc, uintptr_t bias)
{
    // A bounded thread-local admission hint, never a code identity check.
    // Collisions can delay or accelerate compilation but cannot reuse code.
    const auto hash = (pc >> 2) ^ (pc >> 14) ^ (bias >> 16) ^ (image * 0x9e3779b97f4a7c15ull);
    const auto tag = pc ^ std::rotl(image, 17) ^ std::rotl(uint64_t{bias}, 29);
    auto& entry = hotness[hash & (hotness.size() - 1)];
    if (entry.tag != tag) {
        entry = {tag, MayHaveArm64DiskBlock(image, pc - bias, 4) ? 64u : 1u};
    }
    else if (entry.hits < 64)
        ++entry.hits;
    return entry.hits >= 64;
}

std::shared_ptr<const Arm64Block> FindArm64Block(uint64_t image, uintptr_t pc, uintptr_t bias, uint64_t generation,
                                                 std::span<const uint8_t> source, uint32_t minimumInstructions)
{
    const HostState hostState;
    if (source.size() < 4 || (pc & 3))
        return {};
    source = source.first(std::min(source.size(), MaximumInstructions * 4));
    auto& cache = Blocks();
    std::shared_ptr<const Arm64Block> retired;
    std::shared_ptr<Arena> retiredArena;
    const std::unique_lock lock(cache.mutex);
    ++cache.stats.lookups;
    const Key key{image, pc, bias, minimumInstructions};
    const auto hash = (pc >> 2) ^ (pc >> 12) ^ (bias >> 16) ^ (image * 0x9e3779b97f4a7c15ull);
    const auto start = (hash & (MaximumEntries / 4 - 1)) * 4;
    auto* replacement = &cache.entries[start];
    for (size_t way = 0; way < 4; ++way) {
        auto& entry = cache.entries[start + way];
        if (entry.block && entry.key == key) {
            if (entry.block->Matches(image, pc, bias, generation, source)) {
                ++cache.stats.hits;
                entry.used = ++cache.serial;
                return entry.block;
            }
            ++cache.stats.invalidations;
            replacement = &entry;
            break;
        }
        if (!entry.block || (replacement->block && entry.used < replacement->used))
            replacement = &entry;
    }
    try {
        auto disk = LoadArm64DiskBlock(image, pc - bias, minimumInstructions, source);
        Compiler compiler(pc, bias, source, image != 0);
        if (disk) {
            compiler.code = disk->code;
            for (auto offset : disk->relocations) {
                uint64_t value;
                std::memcpy(&value, compiler.code.data() + offset, 8);
                value += bias;
                std::memcpy(compiler.code.data() + offset, &value, 8);
            }
            compiler.words.resize(disk->instructions);
            std::memcpy(compiler.words.data(), disk->source.data(), disk->instructions * 4);
            compiler.requiresFlags = (disk->flags & 1) != 0;
            compiler.needsMemory = (disk->flags & 2) != 0;
            compiler.needsVectors = (disk->flags & 4) != 0;
        }
        else {
            const auto start = Arm64CacheTimingStart();
            compiler.Build();
            if (start)
                cache.stats.compileTicks += Arm64CacheTimingElapsed(start);
        }
        auto result = std::shared_ptr<Arm64Block>(new Arm64Block);
        auto& block = *result->impl;
        block.image = image;
        block.pc = pc;
        block.bias = bias;
        block.generation = generation;
        block.requiresFlags = compiler.requiresFlags;
        block.needsMemory = compiler.needsMemory;
        block.needsVectors = compiler.needsVectors;
        const bool supported =
            bool(disk) || compiler.words.size() >= 2 || (compiler.terminates && !compiler.words.empty());
        if (supported && (disk || compiler.words.size() >= minimumInstructions || compiler.Repeats())) {
            block.words = std::move(compiler.words);
            block.source.assign(source.begin(), source.begin() + block.words.size() * 4);
            if (!cache.arena || !cache.arena->Fits(compiler.code.size())) {
                auto next = std::make_shared<Arena>();
                retiredArena = std::move(cache.arena);
                cache.arena = std::move(next);
            }
            block.arena = cache.arena;
            const auto publishStart = Arm64CacheTimingStart();
            const auto published = block.arena->Publish(compiler.code);
            if (publishStart)
                cache.stats.publishTicks += Arm64CacheTimingElapsed(publishStart);
            block.memory = published.first;
            block.bytes = published.second;
            block.codeBytes = compiler.code.size();
            if (!disk) {
                ++cache.stats.compiled;
                if (image && Arm64BlockDiskCacheEnabled()) {
                    Arm64BlockArtifact artifact;
                    artifact.instructions = static_cast<uint32_t>(block.words.size());
                    artifact.flags = (block.requiresFlags ? 1u : 0u) | (block.needsMemory ? 2u : 0u) |
                                     (block.needsVectors ? 4u : 0u) | (compiler.Repeats() ? 8u : 0u);
                    artifact.source = block.source;
                    artifact.code = compiler.code;
                    artifact.relocations = std::move(compiler.relocations);
                    for (auto offset : artifact.relocations) {
                        uint64_t value;
                        std::memcpy(&value, artifact.code.data() + offset, 8);
                        value -= bias;
                        std::memcpy(artifact.code.data() + offset, &value, 8);
                    }
                    RecordArm64DiskBlock(image, pc - bias, minimumInstructions, std::move(artifact));
                }
            }
        }
        else {
            const auto size = std::min(source.size(), std::max(size_t{4}, compiler.observed * 4));
            block.source.assign(source.begin(), source.begin() + size);
            if (supported)
                ++cache.stats.unprofitable;
            else
                ++cache.stats.unsupported;
        }
        if (!replacement->block)
            ++cache.stats.entries;
        else if (replacement->key != key)
            ++cache.stats.evictions;
        retired = std::move(replacement->block);
        *replacement = {key, ++cache.serial, result};
        return result;
    }
    catch (...) {
        ++cache.stats.failures;
        return {};
    }
}

void RecordArm64BlockExecution(uint64_t instructions, uint64_t calls, uint64_t warmupSkips, uint64_t dispatches)
{
    const HostState hostState;
    auto& cache = Blocks();
    if (calls)
        cache.executions.fetch_add(calls, std::memory_order_relaxed);
    if (instructions)
        cache.instructions.fetch_add(instructions, std::memory_order_relaxed);
    if (warmupSkips)
        warmupSkipped.fetch_add(warmupSkips, std::memory_order_relaxed);
    if (dispatches)
        cache.dispatches.fetch_add(dispatches, std::memory_order_relaxed);
}

Arm64BlockStatistics GetArm64BlockStatistics()
{
    const HostState hostState;
    auto& cache = Blocks();
    const std::lock_guard lock(cache.mutex);
    auto result = cache.stats;
    result.warmupSkipped = warmupSkipped.load(std::memory_order_relaxed);
    result.unwindTables = arenasCreated.load(std::memory_order_relaxed);
    result.activeUnwindTables = activeArenas.load(std::memory_order_relaxed);
    result.committedCodeBytes = committedCodeBytes.load(std::memory_order_relaxed);
    result.failures += arenaErrors.load(std::memory_order_relaxed);
    result.executions = cache.executions.load(std::memory_order_relaxed);
    result.instructions = cache.instructions.load(std::memory_order_relaxed);
    result.dispatches = cache.dispatches.load(std::memory_order_relaxed);
    return result;
}

void ClearArm64BlockCache()
{
    const HostState hostState;
    auto& cache = Blocks();
    std::vector<std::shared_ptr<const Arm64Block>> retired;
    retired.reserve(MaximumEntries);
    std::shared_ptr<Arena> retiredArena;
    {
        const std::lock_guard lock(cache.mutex);
        for (auto& entry : cache.entries) {
            retired.push_back(std::move(entry.block));
            entry = {};
        }
        retiredArena = std::move(cache.arena);
        cache.stats.entries = 0;
    }
}
} // namespace stone::windows
