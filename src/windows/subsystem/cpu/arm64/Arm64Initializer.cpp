#include "memory/GuestHeap.h"
#include "signals/Arm64SignalContext.h"
#include "abi/Arm64GuestJniTables.h"
#include "Arm64Initializer.h"
#include "Arm64ExecutionProfile.h"
#include "Arm64BlockJit.h"
#include "../../abi/Arm64HostJni.h"
#include "../../runtime/art/ArtJni.h"
#include "../../abi/AndroidLibraryImports.h"
#include "hle/system/stub-libOpenSLES.h"
#include "hle/system/stub-libandroid.h"
#include "hle/system/stub-libmediandk-format.h"
#include "hle/system/stub-libc.h"
#include "hle/system/stub-libc-sockets.h"
#include "hle/system/stub-libc-properties.h"
#include "hle/system/stub-libc-users.h"
#include "memory/GuestMappings.h"
#include "stub-liblog.h"
#include "hle/system/stub-libnativeloader.h"
#include "hle/system/stub-libdl.h"
#include "hle/system/stub-libvulkan.h"
#include "hle/system/stub-libz.h"
#include "../../loader/ElfImage.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <exception>
#include <iterator>
#include <intrin.h>
#include <emmintrin.h>
#include <limits>
#include <locale.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <process.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace stone::windows {
using namespace arm64_abi;

namespace {

struct FloatEnvironment {
    uint32_t control, status;
};

thread_local FloatEnvironment floatEnvironment{};

enum class FloatIntegralRounding {
    Floor,
    NearestEven,
    NearestAway
};

uint64_t RoundFloatIntegralBits(uint64_t bits, unsigned width, FloatIntegralRounding rounding)
{
    const bool nearest = rounding != FloatIntegralRounding::Floor,
               away = rounding == FloatIntegralRounding::NearestAway;
    const unsigned fractionBits = width == 64 ? 52 : 23;
    const unsigned bias = width == 64 ? 1023 : 127, maximum = 2 * bias + 1;
    const auto fractionMask = (uint64_t{1} << fractionBits) - 1;
    const auto fraction = bits & fractionMask, sign = bits & (uint64_t{1} << (width - 1));
    const auto exponent = static_cast<unsigned>((bits >> fractionBits) & maximum);
    if (exponent == maximum) {
        if (!fraction)
            return bits; // Infinity.
        const auto quiet = uint64_t{1} << (fractionBits - 1);
        if (!(fraction & quiet))
            floatEnvironment.status |= 1; // Signaling NaN: FPSR.IOC.
        return floatEnvironment.control & (1u << 25) ? (uint64_t{maximum} << fractionBits) | quiet : bits | quiet;
    }
    if (!exponent) {
        if (!fraction)
            return bits; // Preserve signed zero.
        if (floatEnvironment.control & (1u << 24)) {
            floatEnvironment.status |= 1u << 7; // Flush-to-zero input: FPSR.IDC.
            return sign;
        }
    }
    const auto power = static_cast<int>(exponent) - static_cast<int>(bias);
    if (power < 0) {
        const bool one = nearest ? power == -1 && (fraction != 0 || away) : sign != 0;
        return sign | (one ? uint64_t{bias} << fractionBits : 0);
    }
    if (power >= static_cast<int>(fractionBits))
        return bits;
    const auto unit = uint64_t{1} << (fractionBits - power), mask = unit - 1;
    const auto integral = bits & ~mask;
    // FRINTM/FRINTN/FRINTA have exact=false: fractional input does not raise FPSR.IXC.
    // Operate on IEEE bits so host rounding/denormal modes cannot affect it.
    const auto remainder = bits & mask, half = unit >> 1;
    const bool increment =
        nearest ? remainder > half || (remainder == half && (away || (integral & unit))) : sign && remainder;
    return integral + (increment ? unit : 0);
}

uint64_t FloatToNearestIntegerBits(uint64_t bits, unsigned width, bool signedResult)
{
    const unsigned fractionBits = width == 64 ? 52 : 23, bias = width == 64 ? 1023 : 127;
    const auto fractionMask = (uint64_t{1} << fractionBits) - 1;
    const auto fraction = bits & fractionMask;
    const auto exponent = unsigned((bits >> fractionBits) & (2 * bias + 1));
    const bool negative = (bits >> (width - 1)) != 0;
    const auto mask = UINT64_MAX >> (64 - width);
    const auto limit = signedResult ? (uint64_t{1} << (width - 1)) - unsigned(!negative) : mask;
    const auto invalid = [&]() -> uint64_t {
        floatEnvironment.status |= 1; // FPSR.IOC; invalid conversion does not also raise IXC.
        return negative ? (signedResult ? uint64_t{1} << (width - 1) : 0) : limit;
    };
    if (exponent == 2 * bias + 1) {
        if (fraction) {
            floatEnvironment.status |= 1;
            return 0;
        } // Both quiet and signaling NaNs.
        return invalid();
    }
    if (!exponent) {
        if (!fraction)
            return 0;
        if (floatEnvironment.control & (1u << 24)) {
            floatEnvironment.status |= 1u << 7; // FPCR.FZ input flush: FPSR.IDC.
            return 0;
        }
    }
    const int power = int(exponent) - int(bias);
    if (power >= int(width))
        return invalid();
    uint64_t magnitude = 0;
    bool inexact = false;
    if (power < 0) {
        magnitude = power == -1 && fraction != 0 ? 1 : 0; // Exactly 0.5 ties to zero.
        inexact = true;
    }
    else {
        const auto significand = (uint64_t{1} << fractionBits) | fraction;
        if (power >= int(fractionBits))
            magnitude = significand << (power - fractionBits);
        else {
            const unsigned shift = fractionBits - power;
            const auto remainder = significand & ((uint64_t{1} << shift) - 1), half = uint64_t{1} << (shift - 1);
            magnitude = significand >> shift;
            magnitude += remainder > half || (remainder == half && (magnitude & 1));
            inexact = remainder != 0;
        }
    }
    if (magnitude > limit || (negative && !signedResult && magnitude))
        return invalid();
    if (inexact)
        floatEnvironment.status |= 1u << 4; // FPSR.IXC.
    return (negative ? uint64_t{0} - magnitude : magnitude) & mask;
}

uint64_t ReverseBitsInBytes(uint64_t value)
{
    value = ((value >> 1) & 0x5555555555555555ULL) | ((value & 0x5555555555555555ULL) << 1);
    value = ((value >> 2) & 0x3333333333333333ULL) | ((value & 0x3333333333333333ULL) << 2);
    return ((value >> 4) & 0x0f0f0f0f0f0f0f0fULL) | ((value & 0x0f0f0f0f0f0f0f0fULL) << 4);
}

template <typename Float> Float FloatMinMax(Float left, Float right, bool minimum, bool number)
{
    using Bits = std::conditional_t<sizeof(Float) == 8, uint64_t, uint32_t>;
    constexpr auto exponent = std::bit_cast<Bits>(std::numeric_limits<Float>::infinity());
    constexpr auto fraction = (Bits{1} << (std::numeric_limits<Float>::digits - 1)) - 1;
    constexpr auto quiet = (fraction + 1) >> 1;
    const auto leftBits = std::bit_cast<Bits>(left), rightBits = std::bit_cast<Bits>(right);
    const bool leftNaN = (leftBits & exponent) == exponent && (leftBits & fraction);
    const bool rightNaN = (rightBits & exponent) == exponent && (rightBits & fraction);
    if (leftNaN || rightNaN) {
        const bool leftSignaling = leftNaN && !(leftBits & quiet);
        const bool rightSignaling = rightNaN && !(rightBits & quiet);
        if (leftSignaling || rightSignaling)
            floatEnvironment.status |= 1;
        else if (number && leftNaN != rightNaN)
            return leftNaN ? right : left;
        if (floatEnvironment.control & (1u << 25))
            return std::bit_cast<Float>(exponent | quiet);
        const auto selected = leftSignaling || (leftNaN && !rightSignaling) ? leftBits : rightBits;
        return std::bit_cast<Float>(selected | quiet);
    }
    if (left == 0 && right == 0) {
        const bool negative =
            minimum ? std::signbit(left) || std::signbit(right) : std::signbit(left) && std::signbit(right);
        return std::copysign(Float{0}, negative ? Float{-1} : Float{1});
    }
    return minimum ? std::fmin(left, right) : std::fmax(left, right);
}

constexpr char programName[] = "Stone";

struct GuestSyslog {
    std::mutex mutex;
    uintptr_t tag = 0;
    int options = 0, mask = 0xff;
};

GuestSyslog& SyslogState()
{
    static GuestSyslog state;
    return state;
}

uint8_t tlsDescriptorToken = 0;
constexpr unsigned Zero = 31, Link = 30;
constexpr size_t StackSize = 8 * 1024 * 1024;
constexpr unsigned MaxInstructions = 20'000'000;
constexpr unsigned PropertyValueSize = 92;
constexpr unsigned ThreadIdSlot = 1;
thread_local std::array<uint8_t, 512> dlErrorBuffer{};
thread_local uintptr_t arm64Tokenizer = 0;
std::mutex arm64EnvironmentMutex;
uintptr_t arm64EnvironmentArray = 0; // Array owned by setenv/unsetenv.

struct GuestThreadBuffer {
    explicit GuestThreadBuffer(size_t bytes) : address(Arm64GuestHeap().Allocate(bytes, false))
    {
        if (!address)
            throw std::bad_alloc();
    }

    ~GuestThreadBuffer()
    {
        Arm64GuestHeap().Free(address);
    }

    GuestThreadBuffer(const GuestThreadBuffer&) = delete;
    GuestThreadBuffer& operator=(const GuestThreadBuffer&) = delete;
    const uintptr_t address;
};

struct GuestStack {
    struct Release {
        void operator()(uint8_t* address) const
        {
            VirtualFree(address, 0, MEM_RELEASE);
        }
    };

    const std::unique_ptr<uint8_t, Release> bytes{
        static_cast<uint8_t*>(VirtualAlloc(nullptr, StackSize, MEM_RESERVE, PAGE_READWRITE))};
    const uintptr_t base = reinterpret_cast<uintptr_t>(bytes.get());
    uintptr_t committedLow = base + StackSize;
    const size_t pageSize = [] {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        return size_t{system.dwPageSize};
    }();
    const std::unique_ptr<std::atomic<uint8_t>[]> permissions{
        std::make_unique<std::atomic<uint8_t>[]>(StackSize / pageSize)};
    mutable std::mutex mutex;

    GuestStack()
    {
        if (!bytes)
            throw std::bad_alloc();
        for (size_t i = 0; i < StackSize / pageSize; ++i)
            permissions[i].store(3, std::memory_order_relaxed);
    }

    uint8_t* At(uintptr_t address, size_t length, bool write) const
    {
        const auto offset = address - base;
        if (offset >= StackSize || length > StackSize - offset)
            return nullptr;
        if (length) {
            const auto end = (offset + length - 1) / pageSize;
            for (auto page = offset / pageSize; page <= end; ++page) {
                const auto access = permissions[page].load(std::memory_order_acquire);
                if (write ? !(access & 2) : !(access & 3))
                    return nullptr;
            }
        }
        return bytes.get() + offset;
    }

    uint8_t* CommittedAt(uintptr_t address, size_t length, bool write) const
    {
        const std::lock_guard lock(mutex);
        auto* result = At(address, length, write);
        if (!result)
            return nullptr;
        const auto end = address + length;
        while (address < end) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) || region.State != MEM_COMMIT)
                return nullptr;
            const auto next = std::min(end, reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize);
            if (next <= address)
                return nullptr;
            address = next;
        }
        return result;
    }

    bool Commit(uintptr_t address)
    {
        const std::lock_guard lock(mutex);
        if (address >= committedLow)
            return true;
        const auto page = address & ~(pageSize - 1);
        if (!CommitRange(page, committedLow - page))
            return false;
        committedLow = page;
        return true;
    }

    int Protect(uintptr_t address, size_t length, int access)
    {
        if (access & ~3) {
            errno = EACCES;
            return -1;
        }
        if (address % pageSize) {
            errno = EINVAL;
            return -1;
        }
        if (!length)
            return 0;
        const auto offset = address - base;
        if (offset >= StackSize || length > StackSize - offset) {
            errno = ENOMEM;
            return -1;
        }
        const auto rounded = (length + pageSize - 1) & ~(pageSize - 1);
        if (rounded > StackSize - offset) {
            errno = ENOMEM;
            return -1;
        }
        const std::lock_guard lock(mutex);
        if (!CommitRange(address, rounded)) {
            errno = ENOMEM;
            return -1;
        }
        const auto first = offset / pageSize, count = rounded / pageSize;
        std::vector<uint8_t> previous(count);
        for (size_t i = 0; i < count; ++i)
            previous[i] = permissions[first + i].exchange(static_cast<uint8_t>(access), std::memory_order_acq_rel);
        DWORD old;
        const DWORD protection = access & 2 ? PAGE_READWRITE : access & 1 ? PAGE_READONLY : PAGE_NOACCESS;
        if (!VirtualProtect(reinterpret_cast<void*>(address), rounded, protection, &old)) {
            for (size_t i = 0; i < count; ++i)
                permissions[first + i].store(previous[i], std::memory_order_release);
            errno = ENOMEM;
            return -1;
        }
        return 0;
    }

private:
    bool CommitRange(uintptr_t address, size_t length)
    {
        const auto end = address + length;
        while (address < end) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)))
                return false;
            const auto next = std::min(end, reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize);
            if (next <= address || (region.State != MEM_RESERVE && region.State != MEM_COMMIT))
                return false;
            // Committing a deeper frame must preserve already protected pages.
            if (region.State == MEM_RESERVE &&
                !VirtualAlloc(reinterpret_cast<void*>(address), next - address, MEM_COMMIT, PAGE_READWRITE))
                return false;
            address = next;
        }
        return true;
    }
};

struct GuestStacks {
    std::mutex mutex;
    std::map<uintptr_t, std::shared_ptr<GuestStack>> stacks;
};

GuestStacks& Stacks()
{
    static auto* stacks = new GuestStacks;
    return *stacks;
}

struct ThreadStack {
    const std::shared_ptr<GuestStack> stack = std::make_shared<GuestStack>();

    ThreadStack()
    {
        auto& stacks = Stacks();
        const std::lock_guard lock(stacks.mutex);
        stacks.stacks.emplace(stack->base, stack);
    }

    ~ThreadStack()
    {
        auto& stacks = Stacks();
        const std::lock_guard lock(stacks.mutex);
        stacks.stacks.erase(stack->base);
    }
};

std::shared_ptr<GuestStack> CurrentThreadStack()
{
    // Guest TLS destructors can still call ARM64 code during thread teardown.
    auto& storage = ElfImage::ThreadExecutionStorage();
    if (!storage)
        storage = std::make_shared<ThreadStack>();
    return static_cast<ThreadStack*>(storage.get())->stack;
}

std::minstd_rand arm64Random{1};
std::mutex arm64RandomMutex;

struct GlobalReferences {
    std::mutex mutex;
    std::unordered_multiset<jobject> handles;
    std::unordered_multiset<jobject> weakHandles;
    enum class NativeLoad {
        None,
        Path,
        Name
    };

    struct Method {
        std::string signature;
        bool isStatic;
        bool returnsFilePath;
        jclass constructorOwner;
        NativeLoad nativeLoad;
    };

    std::unordered_multimap<jmethodID, Method> methods;

    struct Field {
        jclass owner;
        char type;
        bool isStatic;
    };

    std::unordered_multimap<jfieldID, Field> fields;
};

struct DirectBufferRange {
    uintptr_t address;
    uint64_t size;
    bool writable;

    bool Contains(uintptr_t pointer, unsigned bytes, bool write) const
    {
        return (!write || writable) && pointer >= address && pointer - address < size &&
               bytes <= size - (pointer - address);
    }
};

struct DirectBuffers {
    std::atomic<bool> registered{false};
    std::mutex mutex;
    std::vector<std::pair<jweak, DirectBufferRange>> regions;
};

DirectBuffers& Buffers()
{
    static auto* buffers = new DirectBuffers;
    return *buffers;
}

struct GuestFiles {
    std::mutex mutex;
    std::unordered_set<void*> open;
};

GuestFiles& Files()
{
    static auto* files = new GuestFiles;
    return *files;
}

struct GuestDirectories {
    std::mutex mutex;
    std::unordered_map<void*, uintptr_t> open;
};

GuestDirectories& Directories()
{
    static auto* directories = new GuestDirectories;
    return *directories;
}

struct GuestLocales {
    std::mutex mutex;
    std::unordered_map<_locale_t, unsigned> open;
};

std::atomic<unsigned> arm64GlobalMultibyteMax{4};  // Bionic starts in C.UTF-8.
thread_local unsigned arm64ThreadMultibyteMax = 0; // LC_GLOBAL_LOCALE.

GuestLocales& Locales()
{
    static auto* locales = new GuestLocales;
    return *locales;
}

struct GuestAssets {
    std::mutex mutex;
    std::unordered_set<jobject> managers;
    std::unordered_map<void*, uintptr_t> open;
    std::unordered_map<void*, uintptr_t> directories;
    std::unordered_map<uintptr_t, std::unique_ptr<Arm64Configuration>> configurations;
};

GuestAssets& Assets()
{
    static auto* assets = new GuestAssets;
    return *assets;
}

struct GuestWindows {
    std::mutex mutex;

    struct Window {
        unsigned references = 1;
        uintptr_t pixels = 0;
        size_t pixelBytes = 0;
    };

    std::unordered_map<void*, Window> windows;
};

GuestWindows& Windows()
{
    static auto* windows = new GuestWindows;
    return *windows;
}

struct GuestAudio {
    std::mutex mutex;
    std::unordered_set<void*> builders, streams;
};

GuestAudio& Audio()
{
    static auto* audio = new GuestAudio;
    return *audio;
}

uintptr_t CopyGuestConstantString(const char* value)
{
    if (!value)
        return 0;
    static std::mutex mutex;
    static auto* strings = new std::unordered_map<std::string, uintptr_t>;
    const std::lock_guard lock(mutex);
    auto [entry, inserted] = strings->try_emplace(value, 0);
    if (inserted || !entry->second) {
        const auto& text = entry->first;
        entry->second = Arm64GuestHeap().Allocate(text.size() + 1, false);
        if (entry->second)
            std::memcpy(reinterpret_cast<void*>(entry->second), text.c_str(), text.size() + 1);
    }
    return entry->second;
}

struct GuestPixelStore {
    unsigned alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0, buffer = 0;
    unsigned imageHeight = 0, skipImages = 0;
};

struct GuestGraphicsState {
    GuestPixelStore pack, unpack;
    GLuint vertexArray = 0;

    struct VertexAttribute {
        uintptr_t pixels = 0;
        unsigned width = 0, stride = 0;
        bool enabled = false;
    };

    std::array<VertexAttribute, 32> attributes;
};

void WithGraphicsState(EGLContext context, const std::function<void(GuestGraphicsState&)>& callback)
{
    static std::mutex mutex;
    static auto* states = new std::unordered_map<EGLContext, GuestGraphicsState>;
    const std::lock_guard lock(mutex);
    callback((*states)[context]);
}

GlobalReferences& Globals()
{
    static auto* globals = new GlobalReferences;
    return *globals;
}

struct ExitCallback {
    const ElfImage* image;
    uintptr_t code, argument, module;
};

struct ThreadExitCallback {
    const ElfImage* image;
    uintptr_t argument;
    Arm64ImportName importName;
    Arm64PropertyGet propertyGet;
    std::optional<Arm64Jni> jni;
};

std::mutex exitMutex;

std::vector<ExitCallback>& ExitCallbacks()
{
    static auto* callbacks = new std::vector<ExitCallback>;
    return *callbacks;
}

void FinalizeArm64(uintptr_t module, const Arm64ImportName& importName, Arm64PropertyGet propertyGet, unsigned depth,
                   const Arm64Jni* jni);

uint64_t SignExtend(uint64_t value, unsigned width)
{
    const uint64_t sign = uint64_t{1} << (width - 1);
    return (value ^ sign) - sign;
}

uint64_t FloatImmediate(unsigned imm, bool wide)
{
    const unsigned sign = imm >> 7, b = (imm >> 6) & 1;
    const unsigned expSize = wide ? 11 : 8, fracSize = wide ? 52 : 23;
    const unsigned exp = ((b ^ 1) << (expSize - 1)) | (b ? ((1u << (expSize - 3)) - 1) << 2 : 0) | ((imm >> 4) & 3);
    return (uint64_t(sign) << (wide ? 63 : 31)) | (uint64_t(exp) << fracSize) | (uint64_t(imm & 15) << (fracSize - 4));
}

class Interpreter;
thread_local Interpreter* activeArm64Interpreter = nullptr;

struct Arm64MemoryFault : std::runtime_error {
    const Interpreter* owner;
    uintptr_t address;

    Arm64MemoryFault(const Interpreter* owner, uintptr_t address, const std::string& message)
        : std::runtime_error(message), owner(owner), address(address)
    {
    }
};

class Interpreter {
public:
    Interpreter(const ElfImage& image, uintptr_t entry, const Arm64ImportName& importName, Arm64PropertyGet propertyGet,
                unsigned callbackDepth = 0, const Arm64Jni* jni = nullptr,
                Interpreter* stackCaller = activeArm64Interpreter)
        : image(&image), importName(importName), propertyGet(propertyGet),
          stack(stackCaller ? stackCaller->stack : CurrentThreadStack()),
          sp(stackCaller ? stackCaller->sp : StackTop()), entrySp(sp), frameTop(sp), pc(entry),
          callbackDepth(callbackDepth), jni(jni)
    {
    }

    ~Interpreter()
    {
        if (blockCalls || blockWarmupSkips)
            RecordArm64BlockExecution(blockInstructions, blockCalls, blockWarmupSkips, blockDispatches);
        if (jniEnv) {
            ReleaseDirectBuffers();
            for (size_t i = localFrames.size(); i; --i)
                jniEnv->PopLocalFrame(nullptr);
        }
    }

    uint64_t RunInitializer(unsigned resultWidth, const uintptr_t* argument = nullptr)
    {
        if (argument)
            Set(0, *argument);
        if (jni && resultWidth) {
            Set(0, reinterpret_cast<uintptr_t>(&guestJni.vm));
            Set(1, 0); // Reserved JNI_OnLoad argument.
        }
        return Run(resultWidth, false);
    }

    uint64_t RunCallback(std::span<const uint64_t> arguments, std::span<uint8_t> writable, bool bounded = false)
    {
        if (arguments.size() > 8)
            Fail("too many ARM64 callback arguments");
        std::copy(arguments.begin(), arguments.end(), registers.begin());
        callbackMemory = writable;
        return Run(64, false, bounded);
    }

    void RunSignal(uintptr_t handler, int signal, uint64_t previousMask, bool withInfo,
                   std::optional<std::pair<uintptr_t, int>> fault = {}, uint64_t* resumedMask = nullptr)
    {
        const auto* owner = CodeImage(handler);
        if (!owner || callbackDepth >= 32)
            Fail("invalid ARM64 signal handler");
        std::optional<Interpreter> holder(std::in_place, *owner, handler, importName, propertyGet, callbackDepth + 1,
                                          jni, this);
        auto& callback = *holder;
        callback.jniEnv = jniEnv;
        callback.registers = registers;
        callback.vectors = vectors;
        callback.Set(Link, 0);
        std::array<uint64_t, 3> arguments{static_cast<uint64_t>(signal), 0, 0};
        uintptr_t contextAddress = 0;
        if (withInfo) {
            Arm64SignalContext context;
            context.mask = previousMask;
            context.regs = registers;
            context.sp = sp;
            context.pc = fault ? instruction : pc;
            context.fault = fault ? fault->first : 0;
            context.pstate = (uint64_t{n} << 31) | (uint64_t{z} << 30) | (uint64_t{c} << 29) | (uint64_t{v} << 28);
            context.fp.regs = vectors;
            context.fp.fpsr = floatEnvironment.status;
            context.fp.fpcr = floatEnvironment.control;
            std::array<int32_t, 32> info{};
            info[0] = signal;
            info[2] = fault ? fault->second : -6; // SEGV_MAPERR/ACCERR or SI_TKILL.
            if (fault)
                std::memcpy(info.data() + 4, &fault->first, sizeof(uintptr_t));
            else {
                info[4] = _getpid();
                info[5] = static_cast<int32_t>(AndroidApplicationUid());
            }
            callback.SetSp(sp - sizeof(context) - sizeof(info));
            callback.entrySp = callback.sp;
            arguments[1] = callback.sp;
            arguments[2] = callback.sp + sizeof(info);
            contextAddress = arguments[2];
            std::memcpy(callback.Memory(arguments[1], sizeof(info), true), info.data(), sizeof(info));
            std::memcpy(callback.Memory(arguments[2], sizeof(context), true), &context, sizeof(context));
        }
        {
            struct RestoreFloatEnvironment {
                FloatEnvironment previous;

                ~RestoreFloatEnvironment()
                {
                    floatEnvironment = previous;
                }
            } restore{floatEnvironment};

            callback.RunCallback(arguments, {});
        }
        if (fault) {
            if (contextAddress) {
                Arm64SignalContext context;
                std::memcpy(&context, callback.ReadMemory(contextAddress, sizeof(context)), sizeof(context));
                if (!CodeImage(context.pc) && AndroidArm64CodeAt(context.pc).empty())
                    Fail("invalid ARM64 signal resume PC");
                // ART's implicit-NPE handler pushes one return PC before
                // resuming its assembly trampoline. That transient SP is only
                // pointer-aligned; the trampoline then restores 16-byte alignment.
                if ((context.sp & (sizeof(uintptr_t) - 1)) || context.sp < StackBase() || context.sp > StackTop()) {
                    std::ostringstream reason;
                    reason << "invalid ARM64 signal resume SP 0x" << std::hex << context.sp << " (original 0x" << sp
                           << ", stack 0x" << StackBase() << "..0x" << StackTop() << ", resume PC 0x" << context.pc
                           << ", fault 0x" << fault->first << ')';
                    Fail(reason.str());
                }
                holder.reset(); // Restore the callback's stack lifetime before applying the resumed SP.
                registers = context.regs;
                vectors = context.fp.regs;
                SetConditionFlags(static_cast<unsigned>(context.pstate >> 28));
                floatEnvironment = {context.fp.fpcr, context.fp.fpsr};
                SetSp(context.sp);
                if (resumedMask)
                    *resumedMask = context.mask;
                BranchTo(context.pc);
            }
            else {
                holder.reset();
                pc = instruction;
            }
        }
        exclusiveSize = 0;
    }

    uint64_t RunNative(char result, JNIEnv* env, jobject receiver, std::span<const Arm64Argument> arguments)
    {
        jniEnv = env;
        Set(0, guestEnvAddress);
        Set(1, reinterpret_cast<uintptr_t>(receiver));
        references.push_back(receiver);
        SetArguments(arguments, 2, true);
        const auto value = Run(ScalarWidth(result), result == 'F' || result == 'D', false);
        if (!localFrames.empty())
            Fail("unbalanced ARM64 JNI local frames");
        if (result == 'L' && !KnownReference(reinterpret_cast<jobject>(value)))
            Fail("invalid ARM64 JNI return reference");
        return value;
    }

    uint64_t RunFunction(char result, std::span<const Arm64Argument> arguments)
    {
        SetArguments(arguments, 0, false);
        return Run(ScalarWidth(result), result == 'F' || result == 'D', false);
    }

private:
    void SetArguments(std::span<const Arm64Argument> arguments, unsigned integers, bool objects)
    {
        unsigned floats = 0;
        std::vector<uint64_t> spilled;
        for (const auto& argument : arguments) {
            const bool fp = argument.type == 'F' || argument.type == 'D';
            const unsigned width = ScalarWidth(argument.type);
            auto bits = width == 32 ? static_cast<uint32_t>(argument.bits) : argument.bits;
            if (argument.type == 'Z' || argument.type == 'B')
                bits = static_cast<uint8_t>(bits);
            if (argument.type == 'C' || argument.type == 'S')
                bits = static_cast<uint16_t>(bits);
            if (objects && argument.type == 'L')
                references.push_back(reinterpret_cast<jobject>(bits));
            if (fp && floats < 8)
                SetVector(floats++, {bits, 0});
            else if (!fp && integers < 8)
                Set(integers++, bits, width);
            else
                spilled.push_back(bits);
        }
        const auto bytes = spilled.size() * sizeof(uint64_t);
        SetSp(sp - ((bytes + 15) & ~size_t{15}));
        entrySp = sp;
        if (bytes)
            std::memcpy(Memory(sp, static_cast<unsigned>(bytes), true), spilled.data(), bytes);
    }

    static unsigned ScalarWidth(char type)
    {
        return type == 'V' ? 0 : type == 'J' || type == 'D' || type == 'L' ? 64 : 32;
    }

    bool InExecutableRange(uintptr_t address) const
    {
        if (executableMapped && executableGeneration != AndroidArm64CodeGeneration())
            return false;
        const auto offset = address - executableStart;
        return !(address & 3) && offset < executableSize && executableSize - offset >= sizeof(uint32_t);
    }

    static void StackBlockMemory(Arm64BlockContext* context, uint32_t word, Arm64BlockMemory kind) noexcept
    {
        auto& owner = *static_cast<Interpreter*>(context->owner);

        struct Restore {
            Arm64BlockContext*& slot;
            Arm64BlockContext* previous;

            ~Restore()
            {
                slot = previous;
            }
        } restore{owner.activeBlockMemory, std::exchange(owner.activeBlockMemory, context)};

        context->stackStoreOnly = 1;
        BlockMemory(context, word, kind);
    }

    static void BlockMemory(Arm64BlockContext* context, uint32_t word, Arm64BlockMemory kind) noexcept
    {
        auto& owner = *static_cast<Interpreter*>(context->owner);
        owner.instruction = context->instruction;
        owner.pc = context->pc;
        try {
            // Use the original guarded accesses and SP validation. No guest
            // memory or C++ exception crosses the generated-code boundary.
            switch (kind) {
            case Arm64BlockMemory::Transfer:
                owner.Transfer(word, word & 0x04000000);
                break;
            case Arm64BlockMemory::Pair4:
                owner.Pair(word, 4);
                break;
            case Arm64BlockMemory::Pair8:
                owner.Pair(word, 8);
                break;
            case Arm64BlockMemory::Pair4Vector:
                owner.Pair(word, 4, true);
                break;
            case Arm64BlockMemory::Pair16Vector:
                owner.Pair(word, 16, true);
                break;
            case Arm64BlockMemory::Literal:
            case Arm64BlockMemory::StackArithmetic:
                owner.Step(word);
                break;
            }
        }
        catch (...) {
            owner.blockFailure = std::current_exception();
            context->exit = Arm64BlockExit::Failure;
        }
        context->sp = owner.sp;
        // These callback kinds never consume or change NZCV. Keep the native
        // flags in context until return, including a fault after native CMP.
        owner.stackPins.clear();
    }

    const Arm64Block* LocalBlock(uintptr_t address, bool validFlags, uint32_t budget, bool admit)
    {
        const auto source = std::span(reinterpret_cast<const uint8_t*>(address),
                                      std::min(size_t{128}, executableSize - (address - executableStart)));
        const auto identity = executableMapped ? 0 : image->Identity();
        const auto generation = executableMapped ? executableGeneration : 0;
        const auto hash = (address >> 2) ^ (address >> 10);
        auto& bucket = blocks[directBlocks ? ((hash & 255) >> 2) : (hash & 63)];
        auto& slot = bucket.entries[directBlocks ? (hash & 3) : 0];
        const auto tag = static_cast<uint32_t>((address >> 2) ^ (address >> 34));
        if (!slot || (!directBlocks && bucket.tags[0] != tag) ||
            !slot->Matches(identity, address, image->LoadBias(), generation, source)) {
            size_t way = 4;
            if (!directBlocks) {
                // Tags are admission hints only. A single SSE2 comparison
                // avoids calling Matches for three unrelated PCs on every
                // cold instruction; every candidate still gets the full check.
                const auto equal = _mm_cmpeq_epi32(_mm_load_si128(reinterpret_cast<const __m128i*>(bucket.tags.data())),
                                                   _mm_set1_epi32(std::bit_cast<int32_t>(tag)));
                auto candidates = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(equal))) & 14u;
                while (candidates) {
                    const auto candidate = std::countr_zero(candidates);
                    candidates &= candidates - 1;
                    if (bucket.entries[candidate] &&
                        bucket.entries[candidate]->Matches(identity, address, image->LoadBias(), generation, source)) {
                        way = candidate;
                        break;
                    }
                }
            }
            if (way < 4) {
                std::swap(slot, bucket.entries[way]);
                std::swap(bucket.tags[0], bucket.tags[way]);
            }
            else {
                // Chaining never changes admission or compiles a new target.
                if (!admit)
                    return nullptr;
                if (!WarmArm64Block(identity, address, image->LoadBias())) {
                    if (++blockWarmupSkips == 4096) {
                        RecordArm64BlockExecution(0, 0, blockWarmupSkips);
                        blockWarmupSkips = 0;
                    }
                    return nullptr;
                }
                auto next = FindArm64Block(identity, address, image->LoadBias(), generation, source, 4);
                if (!next)
                    return nullptr;
                // Move the new entry to the front without extra shared_ptr
                // increments on hits. The bucket pins all executable owners.
                if (!directBlocks)
                    for (size_t i = 3; i > 0; --i) {
                        bucket.entries[i] = std::move(bucket.entries[i - 1]);
                        bucket.tags[i] = bucket.tags[i - 1];
                    }
                slot = std::move(next);
                if (!directBlocks)
                    bucket.tags[0] = tag;
            }
        }
        if (!slot || !slot->Instructions() || (slot->RequiresFlags() && !validFlags) || slot->Instructions() > budget)
            return nullptr;
        return slot.get();
    }

    bool RunBlock(unsigned& count, bool bounded, uint32_t& word)
    {
        if (!blockJit)
            return false;
        // A block never crosses the interpreter's signal polling or bounded
        // execution deadline, including when a prior block ended early.
        const auto remaining = 256 - (count & 255);
        const auto budget = bounded ? std::min(remaining, MaxInstructions - count) : remaining;
        const auto* block = LocalBlock(pc, flagsValid, budget, true);
        if (!block)
            return false;
        // The interpreter owns this slot; its guarded memory callbacks cannot
        // change it. It pins generated code through execution without another
        // pair of atomic shared_ptr increments for every guest branch.
        Arm64BlockContext context;
        context.registers = registers.data();
        context.sp = sp;
        context.pc = pc;
        context.owner = this;
        context.memory = (stackStoreTrace && executableMapped) ? StackBlockMemory : BlockMemory;
        context.vectors = vectors.data();
        context.budget = budget;
        context.nzcv = (n ? 8 : 0) | (z ? 4 : 0) | (c ? 2 : 0) | (v ? 1 : 0);
        context.flagsValid = flagsValid;
        ++blockDispatches;
        unsigned executed = 0;
        for (;;) {
            const auto start = context.pc;
            block->Run(context);
            blockInstructions += context.executed;
            executed += context.executed;
            word = block->Words()[(context.instruction - start) / 4];
            if (++blockCalls == 4096) {
                RecordArm64BlockExecution(blockInstructions, blockCalls, blockWarmupSkips, blockDispatches);
                blockCalls = blockInstructions = blockWarmupSkips = blockDispatches = 0;
            }
            // This is the same in-range branch accepted by BranchTo. Never
            // chain mutable mappings, imports, cross-image calls or failures.
            // Each target still gets a complete identity/source check, and
            // all blocks together consume the original polling budget.
            if (!chainBlocks || executableMapped || context.exit == Arm64BlockExit::Failure ||
                !InExecutableRange(context.pc))
                break;
            context.budget = budget - executed;
            if (!context.budget)
                break;
            block = LocalBlock(context.pc, context.flagsValid != 0, context.budget, false);
            if (!block)
                break;
        }
        pc = context.pc;
        sp = context.sp;
        SetConditionFlags(context.nzcv);
        flagsValid = context.flagsValid != 0;
        instruction = context.instruction;
        count += executed - 1;
        if (context.exit == Arm64BlockExit::Failure) {
            if (!blockFailure || !*blockFailure)
                Fail("native block callback lost its exception");
            auto failure = std::move(*blockFailure);
            blockFailure.reset();
            std::rethrow_exception(failure);
        }
        if (context.exit == Arm64BlockExit::Branch || context.exit == Arm64BlockExit::Return)
            BranchTo(pc, context.exit == Arm64BlockExit::Return);
        return true;
    }

    uint64_t Run(unsigned resultWidth, bool floatingResult, bool bounded = true)
    {
        if (profile)
            ++profile->runs;

        struct RestoreInterpreter {
            Interpreter* previous;

            ~RestoreInterpreter()
            {
                activeArm64Interpreter = previous;
            }
        } restore{std::exchange(activeArm64Interpreter, this)};

        if (!restore.previous)
            RegisterArm64SignalThread();
        for (unsigned i = 0; !bounded || i < MaxInstructions; ++i) {
            if (pc == 0) {
                if (sp != entrySp)
                    Fail("unbalanced stack at return");
                if (floatingResult)
                    return GetVector(0)[0];
                return resultWidth ? Get(0, resultWidth) : 0;
            }
            if (!(i & 255))
                PollArm64Signals();
            instruction = pc;
            if (!InExecutableRange(pc)) {
                auto range = image->ExecutableRangeAt(pc);
                executableMapped = range.empty();
                if (executableMapped) {
                    executableGeneration = AndroidArm64CodeGeneration();
                    range = AndroidArm64CodeAt(pc);
                }
                executableStart = reinterpret_cast<uintptr_t>(range.data());
                executableSize = range.size();
                // CodeAt already validated execute permission under the mapping
                // lock. An unrelated JIT publication may change the generation
                // before this check; invalidate the cache on the next fetch
                // instead of reporting a valid current instruction as a fault.
                const auto offset = pc - executableStart;
                if ((pc & 3) || offset >= executableSize || executableSize - offset < sizeof(uint32_t))
                    Fail("branch outside executable ARM64 memory");
            }
            uint32_t word = 0;
            try {
                if (!RunBlock(i, bounded, word)) {
                    std::memcpy(&word, reinterpret_cast<const void*>(pc), sizeof(word));
                    if (profileInstructions)
                        RecordArm64Instruction(*profile, executableMapped ? 0 : image->Identity(),
                                               executableMapped ? pc : pc - image->LoadBias(), word, pc);
                    pc += 4;
                    Step(word);
                }
            }
            catch (const Arm64MemoryFault& fault) {
                // Only real guest load/store instructions produce synchronous
                // signals. Invalid host-import buffers retain their diagnostics.
                if (fault.owner != this || (word & 0x0a000000) != 0x08000000 ||
                    !DeliverAndroidArm64MemoryFault(fault.address))
                    throw;
            }
            stackPins.clear();
        }
        Fail("instruction limit exceeded");
    }

    std::string FailureMessage(std::string_view reason) const
    {
        std::ostringstream message;
        message << "ARM64 code";
        if (jni) {
            const auto images = jni->loadedImages();
            const auto named = std::ranges::find(images, image, &Arm64LoadedImage::image);
            if (named != images.end())
                message << " in " << named->name;
        }
        if (executableMapped)
            message << " at mapped code address 0x" << std::hex << instruction;
        else
            message << " at ELF offset 0x" << std::hex << instruction - image->LoadBias();
        message << ": " << reason;
        return message.str();
    }

    [[noreturn]] void Fail(std::string_view reason) const
    {
        throw std::runtime_error(FailureMessage(reason));
    }

    uintptr_t StackBase() const
    {
        return stack->base;
    }

    uintptr_t StackTop() const
    {
        return StackBase() + StackSize;
    }

    uint64_t Get(unsigned reg, unsigned width = 64) const
    {
        if (reg == Zero)
            return 0;
        return width == 32 ? uint32_t(registers[reg]) : registers[reg];
    }

    void Set(unsigned reg, uint64_t value, unsigned width = 64)
    {
        if (reg == Zero)
            return;
        registers[reg] = width == 32 ? uint32_t(value) : value;
    }

    void SetSp(uintptr_t value)
    {
        if (value < StackBase() || value > StackTop())
            Fail("stack pointer outside the guest stack");
        if (!stack->Commit(value))
            Fail("could not commit ARM64 guest stack pages");
        sp = value;
    }

    const uint8_t* ReadMemory(uintptr_t address, unsigned width) const
    {
        if (const auto* memory = FindReadMemory(address, width))
            return memory;
        FailMemoryAccess(address, width, false);
    }

    uint8_t* Memory(uintptr_t address, unsigned width, bool write) const
    {
        // GuestStack is a private reservation: its protection API rejects X,
        // and mmap replacement/aliases require a registered guest mapping.
        // Certification never substitutes for the permission checks below.
        if (write && activeBlockMemory &&
            !(address >= StackBase() && address < StackTop() && width <= StackTop() - address))
            activeBlockMemory->stackStoreOnly = 0;
        if (auto* memory = FindMemory(address, width, write))
            return memory;
        FailMemoryAccess(address, width, write);
    }

    const uint8_t* FindReadMemory(uintptr_t address, unsigned width) const
    {
        const auto name = reinterpret_cast<uintptr_t>(programName);
        if (address >= name && address - name < sizeof(programName) && width <= sizeof(programName) - (address - name))
            return reinterpret_cast<const uint8_t*>(address);
        const auto ipv6 = reinterpret_cast<uintptr_t>(Arm64AnyIpv6.data());
        if (address >= ipv6 && address - ipv6 < Arm64AnyIpv6.size() && width <= Arm64AnyIpv6.size() - (address - ipv6))
            return reinterpret_cast<const uint8_t*>(address);
        const auto errorBase = reinterpret_cast<uintptr_t>(dlErrorBuffer.data());
        if (address >= errorBase && address - errorBase < dlErrorBuffer.size() &&
            width <= dlErrorBuffer.size() - (address - errorBase))
            return dlErrorBuffer.data() + (address - errorBase);
        if (jni) {
            const auto base = reinterpret_cast<uintptr_t>(&guestJni);
            if (address >= base && address - base < sizeof(guestJni) && width <= sizeof(guestJni) - (address - base))
                return reinterpret_cast<const uint8_t*>(address);
        }
        return FindMemory(address, width, false);
    }

    uint8_t* FindMemory(uintptr_t address, unsigned width, bool write) const
    {
        if (address >= StackBase() && address < StackTop() && width <= StackTop() - address) {
            // ART probes below SP before allocating a frame. The entire guest
            // stack is owned memory; commit readable pages on demand without
            // bypassing its protected pages or admitting the Windows stack.
            auto* memory = stack->At(address, width, write);
            if (!memory || (address < sp && !stack->Commit(address)))
                return nullptr;
            return memory;
        }
        const auto envOffset = address - guestEnvAddress;
        if (guestEnvAddress && envOffset < sizeof(guestJniEnv) && width <= sizeof(guestJniEnv) - envOffset)
            return reinterpret_cast<uint8_t*>(address);
        const auto shared = reinterpret_cast<uintptr_t>(callbackMemory.data());
        if (address >= shared && address - shared < callbackMemory.size() &&
            width <= callbackMemory.size() - (address - shared))
            return callbackMemory.data() + (address - shared);
        // This is only a permission-checked hit in an owned mmap range. Try
        // it before unrelated ELF/TLS ownership probes for ART heap accesses;
        // misses still follow the full validation path below.
        if (mappingFirst)
            if (auto* memory = AndroidCachedArm64Mapping(address, width, write))
                return memory;
        if (auto* memory = image->DataAt(address, width, write))
            return memory;
        if (auto* memory = image->ThreadLocalDataAt(address, width))
            return memory;
        if (auto* memory = ElfImage::Arm64ThreadControlDataAt(address, width))
            return memory;
        if (!mappingFirst)
            if (auto* memory = AndroidCachedArm64Mapping(address, width, write))
                return memory;
        if (auto* memory = Arm64GuestHeap().At(address, width))
            return memory;
        for (const auto& array : criticalArrays) {
            const auto base = reinterpret_cast<uintptr_t>(array.data.get());
            if (address >= base && address - base < array.size && width <= array.size - (address - base))
                return array.data.get() + (address - base);
        }
        for (const bool directory : {false, true}) {
            const auto buffer = AndroidPathStorage(directory);
            const auto base = reinterpret_cast<uintptr_t>(buffer.data());
            if (address >= base && address - base < buffer.size() && width <= buffer.size() - (address - base))
                return reinterpret_cast<uint8_t*>(address);
        }
        if (!write && width == sizeof(void*)) {
            if (address == reinterpret_cast<uintptr_t>(AndroidGlobalStackGuard()))
                return reinterpret_cast<uint8_t*>(address);
            // The Bionic standard stream symbols are read-only FILE* slots.
            for (const auto symbol : {"stdin", "stdout", "stderr"})
                if (address == AndroidLibcImport(symbol, true))
                    return reinterpret_cast<uint8_t*>(address);
        }
        const auto error = reinterpret_cast<uintptr_t>(_errno());
        if (address >= error && address - error < sizeof(int) && width <= sizeof(int) - (address - error))
            return reinterpret_cast<uint8_t*>(address);
        if (jni) {
            if (auto* memory = jni->findData(address, width, write))
                return memory;
            auto& windows = Windows();
            const std::lock_guard lock(windows.mutex);
            for (const auto& [handle, window] : windows.windows)
                if (address >= window.pixels && address - window.pixels < window.pixelBytes &&
                    width <= window.pixelBytes - (address - window.pixels))
                    return reinterpret_cast<uint8_t*>(address);
        }
        if (auto* memory = AndroidMappedArm64Data(address, width, write))
            return memory;
        if (auto* memory = Arm64VulkanMappedData(address, width))
            return memory;
        if (jniEnv && PinDirectBufferAt(address, width, write))
            return reinterpret_cast<uint8_t*>(address);
        // Guest threads share an address space. An argument passed to a worker can
        // point into another live guest stack, for example an std::promise on its caller's stack.
        {
            auto& stacks = Stacks();
            const std::lock_guard lock(stacks.mutex);
            const auto next = stacks.stacks.upper_bound(address);
            if (next != stacks.stacks.begin()) {
                const auto& [base, candidate] = *std::prev(next);
                const auto offset = address - base;
                if (candidate != stack && offset < StackSize && width <= StackSize - offset) {
                    // SP is not a hardware memory permission boundary. A peer
                    // can still access committed pages after a frame returns;
                    // never commit another thread's reserved pages on its behalf.
                    auto* memory = candidate->CommittedAt(address, width, write);
                    if (!memory)
                        return nullptr;
                    // Keep the backing memory alive through this instruction or host import.
                    if (std::ranges::find(stackPins, candidate) == stackPins.end())
                        stackPins.push_back(candidate);
                    return memory;
                }
            }
        }
        return nullptr;
    }

    [[noreturn]] void FailMemoryAccess(uintptr_t address, unsigned width, bool write) const
    {
        std::ostringstream reason;
        reason << (write ? "write outside writable ARM64 memory" : "read outside readable ARM64 memory") << " ("
               << width << " bytes at 0x" << std::hex << address << std::dec;
        wchar_t diagnostics[2]{};
        if (GetEnvironmentVariableW(L"STONE_ART_DIAGNOSTICS", diagnostics, 2) == 1 && diagnostics[0] == L'1') {
            auto& heap = Arm64GuestHeap();
            const std::lock_guard lock(heap.mutex);
            const auto next = heap.allocations.upper_bound(address);
            if (next != heap.allocations.begin()) {
                const auto [base, size] = *std::prev(next);
                if (address - base <= size + 64)
                    reason << ", guest allocation offset " << address - base << " of " << size;
            }
            for (const unsigned index : {0u, 1u, 10u, 11u, 12u, 13u})
                reason << ", x" << index << "=0x" << std::hex << Get(index) << std::dec;
        }
        const auto returnAddress = Get(30);
        if (!image->CodeAt(returnAddress, sizeof(uint32_t)).empty())
            reason << ", return ELF offset 0x" << std::hex << returnAddress - image->LoadBias() << std::dec;
        if (address >= StackBase() && address - StackBase() <= StackSize + 4096) {
            reason << ", stack offset " << (static_cast<int64_t>(address) - static_cast<int64_t>(sp))
                   << ", stack bytes remaining " << StackTop() - sp;
        }
        if (write && jni) {
            for (const auto& module : jni->loadedImages()) {
                if (!module.image->DataAt(address, width, false))
                    continue;
                reason << ", read-only range in " << module.name << " at ELF offset 0x" << std::hex
                       << address - module.image->LoadBias();
                break;
            }
        }
        reason << ')';
        throw Arm64MemoryFault(this, address, FailureMessage(reason.str()));
    }

    uint64_t Load(uintptr_t address, unsigned width) const
    {
        uint64_t value = 0;
        std::memcpy(&value, ReadMemory(address, width), width);
        return value;
    }

    void Store(uintptr_t address, unsigned width, uint64_t value) const
    {
        std::memcpy(Memory(address, width, true), &value, width);
    }

    std::string ReadString(uintptr_t address, unsigned limit, bool requireTerminator = true) const
    {
        std::string result;
        for (unsigned i = 0; i < limit; ++i) {
            const auto byte = static_cast<char>(Load(address + i, 1));
            if (!byte)
                return result;
            result.push_back(byte);
        }
        if (requireTerminator)
            Fail("unterminated ARM64 string");
        return result;
    }

    int CompareGuestStrings(uintptr_t left, uintptr_t right, uint64_t size, bool ignoreCase) const
    {
        for (uint64_t offset = 0; offset < size;) {
            // Short or unequal prefixes usually end the comparison before a range check is needed.
            const auto count = static_cast<unsigned>(std::min<uint64_t>(size - offset, offset ? 32 : 4));
            const bool probeRange = offset && count > 1;
            const auto* leftBytes = probeRange ? FindReadMemory(left + offset, count) : nullptr;
            const auto* rightBytes = probeRange ? FindReadMemory(right + offset, count) : nullptr;
            for (unsigned i = 0; i < count; ++i) {
                auto l = static_cast<int>(leftBytes ? leftBytes[i] : *ReadMemory(left + offset + i, 1));
                auto r = static_cast<int>(rightBytes ? rightBytes[i] : *ReadMemory(right + offset + i, 1));
                if (ignoreCase) {
                    if (l >= 'A' && l <= 'Z')
                        l += 'a' - 'A';
                    if (r >= 'A' && r <= 'Z')
                        r += 'a' - 'A';
                }
                if (!l || l != r)
                    return l - r;
            }
            offset += count;
        }
        return 0;
    }

    int Format(char* output, size_t size, uintptr_t formatAddress, const std::function<uint64_t(bool)>& next) const
    {
        const auto format = ReadString(formatAddress, 65536);
        std::string text;
        return FormatAndroidArguments(output, size, format.c_str(), next, [&](uint64_t address) -> const char* {
            if (!address)
                return nullptr;
            text = ReadString(address, 1u << 20);
            return text.c_str();
        });
    }

    auto VariadicArguments(uintptr_t list) const
    {
        auto stackArguments = Load(list, 8);
        const auto integerTop = Load(list + 8, 8), floatTop = Load(list + 16, 8);
        auto integerOffset = std::bit_cast<int32_t>(static_cast<uint32_t>(Load(list + 24, 4)));
        auto floatOffset = std::bit_cast<int32_t>(static_cast<uint32_t>(Load(list + 28, 4)));
        if (integerOffset < -64 || floatOffset < -128 || integerOffset % 8 || floatOffset % 16)
            Fail("invalid ARM64 va_list offsets");
        return [=, this](bool floating) mutable {
            auto& offset = floating ? floatOffset : integerOffset;
            if (offset < 0) {
                const auto result = Load((floating ? floatTop : integerTop) + static_cast<uint64_t>(offset), 8);
                offset += floating ? 16 : 8;
                return result;
            }
            if (stackArguments % 8)
                Fail("unaligned ARM64 variadic stack");
            const auto result = Load(stackArguments, 8);
            stackArguments += 8;
            return result;
        };
    }

    auto RegisterArguments(unsigned integer) const
    {
        return [this, integer, floating = 0u, spilled = sp](bool fp) mutable {
            if (fp && floating < 8)
                return GetVector(floating++)[0];
            if (!fp && integer < 8)
                return Get(integer++);
            const auto value = Load(spilled, 8);
            spilled += 8;
            return value;
        };
    }

    int LogPrint(std::string_view name) const
    {
        const bool buffered = name == "__android_log_buf_print";
        const auto priority = buffered ? 1u : 0u;
        const auto tagAddress = Get(priority + 1);
        const auto tag = tagAddress ? ReadString(tagAddress, 1024) : std::string{};
        std::array<char, 4096> output{};
        const auto count =
            name == "__android_log_vprint"
                ? Format(output.data(), output.size(), Get(2), VariadicArguments(Get(3)))
                : Format(output.data(), output.size(), Get(priority + 2), RegisterArguments(priority + 3));
        return count < 0 ? count
                         : WriteAndroidLog(static_cast<int>(Get(priority, 32)), tagAddress ? tag.c_str() : nullptr,
                                           output.data(), buffered ? static_cast<int>(Get(0, 32)) : 0);
    }

    void Syslog(bool variadic) const
    {
        uintptr_t tagAddress;
        int options;
        const auto priority = static_cast<unsigned>(Get(0, 32)) & 7;
        {
            auto& state = SyslogState();
            const std::lock_guard lock(state.mutex);
            if (!(state.mask & (1u << priority)))
                return;
            tagAddress = state.tag;
            options = state.options;
        }
        const int saved = errno;
        const auto tag = tagAddress ? ReadString(tagAddress, 1u << 20) : std::string{programName};
        std::array<char, 1024> output{};
        const auto count = variadic ? Format(output.data(), output.size(), Get(1), VariadicArguments(Get(2)))
                                    : Format(output.data(), output.size(), Get(1), RegisterArguments(2));
        if (count >= 0) {
            const int androidPriority = priority <= 3 ? 6 : priority == 4 ? 5 : priority <= 6 ? 4 : 3;
            const AndroidLogMessage message{sizeof(message), 0, androidPriority, tag.c_str(),
                                            nullptr,         0, output.data()};
            WriteAndroidLogMessage(&message);
            if (options & 32) { // Bionic LOG_PERROR; no host syslog socket.
                const bool newline = count > 0 && count < static_cast<int>(output.size()) && output[count - 1] == '\n';
                const auto text = tag + ": " + output.data() + (newline ? "" : "\n");
                const auto write =
                    reinterpret_cast<intptr_t (*)(int, const void*, size_t)>(AndroidLibcImport("write", true));
                write(2, text.data(), text.size());
            }
        }
        errno = saved;
    }

    int Once()
    {
        const auto address = Get(0), code = Get(1);
        if (address % std::atomic_ref<int>::required_alignment)
            Fail("unaligned ARM64 pthread_once state");
        const auto* owner = CodeImage(code);
        if (!owner)
            Fail("invalid ARM64 pthread_once target");
        auto* once = reinterpret_cast<int*>(Memory(address, sizeof(int), true));
        return RunAndroidOnce(*once, [&] {
            if (callbackDepth >= 32)
                Fail("ARM64 callback recursion limit exceeded");
            Interpreter callback(*owner, code, importName, propertyGet, callbackDepth + 1, jni);
            callback.jniEnv = jniEnv;
            callback.Run(0, false);
        });
    }

    bool KnownReference(jobject object, bool allowWeak = false) const
    {
        if (!object || std::ranges::find(references, object) != references.end())
            return true;
        auto& global = Globals();
        const std::lock_guard lock(global.mutex);
        return global.handles.contains(object) || (allowWeak && global.weakHandles.contains(object));
    }

    struct ReleaseLocalReference {
        JNIEnv* env;

        void operator()(jobject object) const
        {
            env->DeleteLocalRef(object);
        }
    };

    std::unique_ptr<_jobject, ReleaseLocalReference> PromoteWeakReference(jobject object)
    {
        if (!KnownReference(object, true))
            Fail("invalid ARM64 JNI reference");
        return {jniEnv->NewLocalRef(object), ReleaseLocalReference{jniEnv}};
    }

    uint64_t ReturnReference(jobject object)
    {
        if (object)
            references.push_back(object);
        return reinterpret_cast<uintptr_t>(object);
    }

    jclass ClassReference(jobject object, bool validated = false)
    {
        if (!object || (!validated && !KnownReference(object)))
            Fail("invalid ARM64 JNI class reference");
        const auto type = jniEnv->FindClass("java/lang/Class");
        if (!type)
            return nullptr;
        const auto isClass = jniEnv->IsInstanceOf(object, type);
        jniEnv->DeleteLocalRef(type);
        if (!isClass)
            Fail("invalid ARM64 JNI class reference");
        return static_cast<jclass>(object);
    }

    jstring StringReference(jobject object)
    {
        if (!object || !KnownReference(object))
            Fail("invalid ARM64 JNI string reference");
        const auto type = jniEnv->FindClass("java/lang/String");
        if (!type)
            return nullptr;
        const auto valid = jniEnv->IsInstanceOf(object, type);
        jniEnv->DeleteLocalRef(type);
        if (!valid)
            Fail("invalid ARM64 JNI string reference");
        return static_cast<jstring>(object);
    }

    uintptr_t RememberJniMethod(jmethodID method, jclass type, std::string_view name, const std::string& signature,
                                bool isStatic)
    {
        if (!method)
            return 0;
        auto nativeLoad = GlobalReferences::NativeLoad::None;
        if (isStatic && signature == "(Ljava/lang/String;)V" && (name == "load" || name == "loadLibrary")) {
            const auto system = jniEnv->FindClass("java/lang/System");
            if (!system)
                return 0;
            if (jniEnv->IsSameObject(type, system)) {
                nativeLoad = name == "load" ? GlobalReferences::NativeLoad::Path : GlobalReferences::NativeLoad::Name;
            }
            jniEnv->DeleteLocalRef(system);
        }
        bool returnsFilePath = false;
        if (!isStatic && signature == "()Ljava/lang/String;") {
            const char* pathOwner = nullptr;
            if (name == "getPath" || name == "getAbsolutePath" || name == "getCanonicalPath")
                pathOwner = "java/io/File";
            else if (name == "getPackageCodePath" || name == "getPackageResourcePath")
                pathOwner = "android/content/Context";
            if (pathOwner) {
                const auto owner = jniEnv->FindClass(pathOwner);
                if (!owner)
                    return 0;
                returnsFilePath = jniEnv->IsAssignableFrom(type, owner);
                jniEnv->DeleteLocalRef(owner);
            }
        }
        auto& global = Globals();
        const std::lock_guard lock(global.mutex);
        const auto [first, last] = global.methods.equal_range(method);
        if (std::none_of(first, last, [isStatic](const auto& value) {
                return value.second.isStatic == isStatic;
            })) {
            jclass constructorOwner = nullptr;
            if (!isStatic && name == "<init>") {
                constructorOwner = static_cast<jclass>(jniEnv->NewGlobalRef(type));
                if (!constructorOwner)
                    return 0;
            }
            try {
                global.methods.emplace(method, GlobalReferences::Method{signature, isStatic, returnsFilePath,
                                                                        constructorOwner, nativeLoad});
            }
            catch (...) {
                if (constructorOwner)
                    jniEnv->DeleteGlobalRef(constructorOwner);
                throw;
            }
        }
        return reinterpret_cast<uintptr_t>(method);
    }

    uintptr_t FromReflectedMethod(jobject object)
    {
        if (!object || !KnownReference(object))
            Fail("invalid ARM64 JNI reflected method reference");
        const auto local = [this](jobject ref) {
            return std::unique_ptr<_jobject, ReleaseLocalReference>(ref, {jniEnv});
        };
        const auto executable = local(jniEnv->FindClass("java/lang/reflect/Executable"));
        if (!executable)
            return 0;
        const auto memberClass = static_cast<jclass>(executable.get());
        if (!jniEnv->IsInstanceOf(object, memberClass))
            Fail("ARM64 FromReflectedMethod requires a method or constructor");
        const auto reflectMethod = local(jniEnv->FindClass("java/lang/reflect/Method"));
        if (!reflectMethod)
            return 0;
        const auto methodClass = static_cast<jclass>(reflectMethod.get());
        const bool constructor = !jniEnv->IsInstanceOf(object, methodClass);

        const auto declaringClass = jniEnv->GetMethodID(memberClass, "getDeclaringClass", "()Ljava/lang/Class;");
        if (!declaringClass)
            return 0;
        const auto owner = local(jniEnv->CallObjectMethod(object, declaringClass));
        if (!owner)
            return 0;
        const auto parameterTypes = jniEnv->GetMethodID(memberClass, "getParameterTypes", "()[Ljava/lang/Class;");
        if (!parameterTypes)
            return 0;
        const auto parameters = local(jniEnv->CallObjectMethod(object, parameterTypes));
        if (!parameters)
            return 0;
        const auto returnType = [&]() -> jobject {
            if (!constructor) {
                const auto get = jniEnv->GetMethodID(methodClass, "getReturnType", "()Ljava/lang/Class;");
                return get ? jniEnv->CallObjectMethod(object, get) : nullptr;
            }
            const auto voidType = local(jniEnv->FindClass("java/lang/Void"));
            if (!voidType)
                return nullptr;
            const auto type = static_cast<jclass>(voidType.get());
            const auto field = jniEnv->GetStaticFieldID(type, "TYPE", "Ljava/lang/Class;");
            return field ? jniEnv->GetStaticObjectField(type, field) : nullptr;
        };
        const auto returns = local(returnType());
        if (!returns)
            return 0;
        const auto methodTypeRef = local(jniEnv->FindClass("java/lang/invoke/MethodType"));
        if (!methodTypeRef)
            return 0;
        const auto methodType = static_cast<jclass>(methodTypeRef.get());
        const auto create = jniEnv->GetStaticMethodID(
            methodType, "methodType", "(Ljava/lang/Class;[Ljava/lang/Class;)Ljava/lang/invoke/MethodType;");
        if (!create)
            return 0;
        const auto type = local(jniEnv->CallStaticObjectMethod(methodType, create, returns.get(), parameters.get()));
        if (!type)
            return 0;
        const auto describe = jniEnv->GetMethodID(methodType, "toMethodDescriptorString", "()Ljava/lang/String;");
        if (!describe)
            return 0;
        const auto descriptor = local(jniEnv->CallObjectMethod(type.get(), describe));
        if (!descriptor)
            return 0;
        const auto modifiers = jniEnv->GetMethodID(memberClass, "getModifiers", "()I");
        if (!modifiers)
            return 0;
        const bool isStatic = (jniEnv->CallIntMethod(object, modifiers) & 8) != 0;
        if (jniEnv->ExceptionCheck())
            return 0;
        std::string name = "<init>";
        if (!constructor) {
            const auto get = jniEnv->GetMethodID(memberClass, "getName", "()Ljava/lang/String;");
            if (!get)
                return 0;
            const auto reflectedName = local(jniEnv->CallObjectMethod(object, get));
            if (!reflectedName)
                return 0;
            name = Utf(jniEnv, static_cast<jstring>(reflectedName.get()));
            if (jniEnv->ExceptionCheck())
                return 0;
        }
        const auto signature = Utf(jniEnv, static_cast<jstring>(descriptor.get()));
        if (jniEnv->ExceptionCheck())
            return 0;
        return RememberJniMethod(jniEnv->FromReflectedMethod(object), static_cast<jclass>(owner.get()), name, signature,
                                 isStatic);
    }

    uint64_t InvokeJniMethod(jobject receiver, unsigned variant, char resultType, bool isStatic,
                             bool constructor = false)
    {
        if (!receiver)
            Fail("invalid ARM64 JNI method receiver");
        std::unique_ptr<_jobject, ReleaseLocalReference> local(nullptr, {jniEnv});
        if (!KnownReference(receiver)) {
            local = PromoteWeakReference(receiver);
            if (!local)
                return 0;
            receiver = local.get();
        }
        const auto method = reinterpret_cast<jmethodID>(Get(2));
        std::string signature;
        bool returnsFilePath = false;
        jclass constructorOwner = nullptr;
        auto nativeLoad = GlobalReferences::NativeLoad::None;
        {
            auto& global = Globals();
            const std::lock_guard lock(global.mutex);
            const auto [first, last] = global.methods.equal_range(method);
            for (auto it = first; it != last; ++it) {
                if (it->second.isStatic == isStatic) {
                    signature = it->second.signature;
                    returnsFilePath = it->second.returnsFilePath;
                    constructorOwner = it->second.constructorOwner;
                    nativeLoad = it->second.nativeLoad;
                    break;
                }
            }
        }
        if (signature.empty())
            Fail("unknown ARM64 JNI method ID");
        const auto closing = signature.find(')');
        if (closing == signature.npos)
            Fail("invalid ARM64 JNI method signature");
        size_t offset = closing + 1;
        if (ReadArm64JniType(signature, offset, true) != resultType)
            Fail("wrong ARM64 JNI call result type");

        unsigned integer = 3, floating = 0;
        auto spilled = sp, array = Get(3);
        std::function<uint64_t(bool)> fromVaList;
        if (variant == 1 && closing > 1)
            fromVaList = VariadicArguments(Get(3));
        const auto next = [&](bool fp) {
            if (variant == 1)
                return fromVaList(fp);
            if (variant == 2) {
                const auto bits = Load(array, sizeof(jvalue));
                array += sizeof(jvalue);
                return bits;
            }
            if (fp && floating < 8)
                return GetVector(floating++)[0];
            if (!fp && integer < 8)
                return Get(integer++);
            const auto bits = Load(spilled, 8);
            spilled += 8;
            return bits;
        };
        std::vector<jvalue> values;
        std::vector<decltype(local)> promotedArguments;
        for (offset = 1; offset < closing;) {
            const char type = ReadArm64JniType(signature, offset, false);
            const auto bits = next(type == 'F' || type == 'D');
            jvalue value{};
            switch (type) {
            case 'Z':
                value.z = static_cast<jboolean>(bits);
                break;
            case 'B':
                value.b = static_cast<jbyte>(bits);
                break;
            case 'C':
                value.c = static_cast<jchar>(bits);
                break;
            case 'S':
                value.s = static_cast<jshort>(bits);
                break;
            case 'I':
                value.i = static_cast<jint>(bits);
                break;
            case 'J':
                value.j = static_cast<jlong>(bits);
                break;
            case 'L':
                value.l = reinterpret_cast<jobject>(bits);
                if (!KnownReference(value.l)) {
                    auto promoted = PromoteWeakReference(value.l);
                    if (!promoted && jniEnv->ExceptionCheck())
                        return 0;
                    value.l = promoted.get();
                    if (promoted)
                        promotedArguments.push_back(std::move(promoted));
                }
                break;
            case 'F':
                value.f = variant == 2 ? std::bit_cast<jfloat>(uint32_t(bits))
                                       : static_cast<jfloat>(std::bit_cast<jdouble>(bits));
                break;
            case 'D':
                value.d = std::bit_cast<jdouble>(bits);
                break;
            }
            values.push_back(value);
        }
        const auto type = isStatic || constructor ? ClassReference(receiver, true) : nullptr;
        if ((isStatic || constructor) && !type)
            return 0;
        const auto* args = values.data();
        if (constructor) {
            if (!constructorOwner || !jniEnv->IsSameObject(type, constructorOwner))
                Fail("invalid ARM64 JNI constructor owner");
            return ReturnReference(jniEnv->NewObjectA(type, method, args));
        }
        if (nativeLoad != GlobalReferences::NativeLoad::None) {
            // DEX conversion redirects Java call sites. JNI calls to System.load
            // need the same APK linker instead of HotSpot's Win32 DLL loader.
            const auto bridge = jniEnv->FindClass("com/trebledot/stone/loader/StoneNativeLoad");
            if (!bridge)
                return 0;
            const char* name = nativeLoad == GlobalReferences::NativeLoad::Path ? "load" : "loadLibrary";
            const auto target = jniEnv->GetStaticMethodID(bridge, name, signature.c_str());
            if (target)
                jniEnv->CallStaticVoidMethodA(bridge, target, args);
            jniEnv->DeleteLocalRef(bridge);
            return 0;
        }
#define INVOKE(Type)                                                                                                   \
    (isStatic ? jniEnv->CallStatic##Type##MethodA(type, method, args)                                                  \
              : jniEnv->Call##Type##MethodA(receiver, method, args))
        switch (resultType) {
        case 'L': {
            auto value = INVOKE(Object);
            if (returnsFilePath && value) {
                // Native path parsing expects an absolute path with Unix separators.
                const auto path = Utf(jniEnv, static_cast<jstring>(value));
                if (jniEnv->ExceptionCheck())
                    return 0;
                const auto guestPath = AndroidGuestFilePath(path);
                if (guestPath != path) {
                    jniEnv->DeleteLocalRef(value);
                    value = jniEnv->NewStringUTF(guestPath.c_str());
                }
            }
            return ReturnReference(value);
        }
        case 'Z':
            return INVOKE(Boolean);
        case 'B':
            return uint32_t(INVOKE(Byte));
        case 'C':
            return INVOKE(Char);
        case 'S':
            return uint32_t(INVOKE(Short));
        case 'I':
            return uint32_t(INVOKE(Int));
        case 'J':
            return uint64_t(INVOKE(Long));
        case 'F':
            importFloatResult = std::bit_cast<uint32_t>(INVOKE(Float));
            return 0;
        case 'D':
            importFloatResult = std::bit_cast<uint64_t>(INVOKE(Double));
            return 0;
        case 'V':
            INVOKE(Void);
            return 0;
        }
#undef INVOKE
        Fail("invalid ARM64 JNI method result");
    }

    // A copy lets translated code access the array without pinning JVM memory.
    struct CriticalArray {
        struct ReleaseReference {
            JNIEnv* env;

            void operator()(jarray object) const
            {
                env->DeleteGlobalRef(object);
            }
        };

        std::unique_ptr<_jarray, ReleaseReference> object;
        jsize length;
        size_t type, size;
        std::unique_ptr<uint8_t[]> data;
    };

    void CopyCriticalArray(const CriticalArray& array, bool toJava)
    {
        if (!array.length)
            return;
#define ARRAY_REGION(Type, JniType)                                                                                    \
    do {                                                                                                               \
        const auto target = static_cast<JniType##Array>(array.object.get());                                           \
        auto* buffer = reinterpret_cast<JniType*>(array.data.get());                                                   \
        if (toJava)                                                                                                    \
            jniEnv->Set##Type##ArrayRegion(target, 0, array.length, buffer);                                           \
        else                                                                                                           \
            jniEnv->Get##Type##ArrayRegion(target, 0, array.length, buffer);                                           \
        return;                                                                                                        \
    } while (false)
        switch (array.type) {
        case 0:
            ARRAY_REGION(Boolean, jboolean);
        case 1:
            ARRAY_REGION(Byte, jbyte);
        case 2:
            ARRAY_REGION(Char, jchar);
        case 3:
            ARRAY_REGION(Short, jshort);
        case 4:
            ARRAY_REGION(Int, jint);
        case 5:
            ARRAY_REGION(Long, jlong);
        case 6:
            ARRAY_REGION(Float, jfloat);
        case 7:
            ARRAY_REGION(Double, jdouble);
        default:
            Fail("invalid ARM64 JNI primitive array type");
        }
#undef ARRAY_REGION
    }

    jarray PrimitiveArrayReference(jobject object, size_t type)
    {
        if (!object || !KnownReference(object))
            Fail("invalid ARM64 JNI primitive array reference");
        constexpr std::string_view types = "ZBCSIJFD";
        const char descriptor[]{'[', types.at(type), '\0'};
        const auto clazz = jniEnv->FindClass(descriptor);
        if (!clazz)
            return nullptr;
        const auto valid = jniEnv->IsInstanceOf(object, clazz);
        jniEnv->DeleteLocalRef(clazz);
        if (!valid)
            Fail("invalid ARM64 JNI primitive array type");
        return static_cast<jarray>(object);
    }

    uint64_t NewPrimitiveArray(size_t type)
    {
        const auto length = static_cast<jsize>(Get(1, 32));
        if (length < 0)
            Fail("invalid ARM64 JNI array length");
#define NEW_ARRAY(Type) ReturnReference(jniEnv->New##Type##Array(length))
        switch (type) {
        case 0:
            return NEW_ARRAY(Boolean);
        case 1:
            return NEW_ARRAY(Byte);
        case 2:
            return NEW_ARRAY(Char);
        case 3:
            return NEW_ARRAY(Short);
        case 4:
            return NEW_ARRAY(Int);
        case 5:
            return NEW_ARRAY(Long);
        case 6:
            return NEW_ARRAY(Float);
        case 7:
            return NEW_ARRAY(Double);
        }
#undef NEW_ARRAY
        Fail("invalid ARM64 JNI array type");
    }

    void TransferPrimitiveRegion(jobject object, size_t type, bool toJava)
    {
        const auto array = PrimitiveArrayReference(object, type);
        if (!array)
            return;
        const auto start = static_cast<jsize>(Get(2, 32));
        const auto size = static_cast<jsize>(Get(3, 32));
        const auto length = jniEnv->GetArrayLength(array);
        if (start < 0 || size < 0 || start > length || size > length - start) {
            if (const auto error = jniEnv->FindClass("java/lang/ArrayIndexOutOfBoundsException")) {
                jniEnv->ThrowNew(error, "invalid primitive array region");
                jniEnv->DeleteLocalRef(error);
            }
            return;
        }
        if (!size)
            return;
#define TRANSFER_REGION(Type, JniType)                                                                                 \
    do {                                                                                                               \
        if (static_cast<uint64_t>(size) > UINT_MAX / sizeof(JniType))                                                  \
            Fail("ARM64 JNI array region is too large");                                                               \
        const auto bytes = static_cast<unsigned>(size * sizeof(JniType));                                              \
        const auto target = static_cast<JniType##Array>(array);                                                        \
        if (toJava)                                                                                                    \
            jniEnv->Set##Type##ArrayRegion(target, start, size,                                                        \
                                           reinterpret_cast<const JniType*>(ReadMemory(Get(4), bytes)));               \
        else                                                                                                           \
            jniEnv->Get##Type##ArrayRegion(target, start, size,                                                        \
                                           reinterpret_cast<JniType*>(Memory(Get(4), bytes, true)));                   \
        return;                                                                                                        \
    } while (false)
        switch (type) {
        case 0:
            TRANSFER_REGION(Boolean, jboolean);
        case 1:
            TRANSFER_REGION(Byte, jbyte);
        case 2:
            TRANSFER_REGION(Char, jchar);
        case 3:
            TRANSFER_REGION(Short, jshort);
        case 4:
            TRANSFER_REGION(Int, jint);
        case 5:
            TRANSFER_REGION(Long, jlong);
        case 6:
            TRANSFER_REGION(Float, jfloat);
        case 7:
            TRANSFER_REGION(Double, jdouble);
        }
#undef TRANSFER_REGION
        Fail("invalid ARM64 JNI array type");
    }

    uint64_t GetCriticalArray(jobject object, std::optional<size_t> expected = std::nullopt)
    {
        if (!object || !KnownReference(object))
            Fail("invalid ARM64 JNI primitive array");
        auto* copied = Get(2) ? Memory(Get(2), 1, true) : nullptr;
        constexpr std::string_view types = "ZBCSIJFD";
        constexpr size_t widths[]{1, 1, 2, 2, 4, 8, 4, 8};
        size_t type = expected.value_or(0);
        const auto limit = expected ? type + 1 : types.size();
        for (; type < limit; ++type) {
            const char descriptor[]{'[', types[type], '\0'};
            const auto clazz = jniEnv->FindClass(descriptor);
            if (!clazz)
                return 0;
            const bool matches = jniEnv->IsInstanceOf(object, clazz) != 0;
            jniEnv->DeleteLocalRef(clazz);
            if (matches)
                break;
        }
        if (type == limit)
            Fail("invalid ARM64 JNI primitive array type");
        const auto array = static_cast<jarray>(object);
        const auto length = jniEnv->GetArrayLength(array);
        const auto size = static_cast<size_t>(length) * widths[type];
        auto data = std::make_unique<uint8_t[]>(std::max(size, size_t{1}));
        const auto address = reinterpret_cast<uintptr_t>(data.get());
        const auto reference = static_cast<jarray>(jniEnv->NewGlobalRef(array));
        if (!reference)
            return 0;
        CriticalArray entry{{reference, CriticalArray::ReleaseReference{jniEnv}}, length, type, size, std::move(data)};
        CopyCriticalArray(entry, false);
        if (jniEnv->ExceptionCheck())
            return 0;
        criticalArrays.push_back(std::move(entry));
        if (copied)
            *copied = JNI_TRUE;
        return address;
    }

    void ReleaseCriticalArray(jobject object, std::optional<size_t> expected = std::nullopt)
    {
        if (!object || !KnownReference(object))
            Fail("invalid ARM64 JNI primitive array");
        const auto address = Get(2);
        const auto mode = Get(3, 32);
        if (mode != 0 && mode != JNI_COMMIT && mode != JNI_ABORT)
            Fail("invalid ARM64 JNI primitive array release mode");
        const auto entry = std::ranges::find_if(criticalArrays, [&](const auto& array) {
            return (!expected || array.type == *expected) && reinterpret_cast<uintptr_t>(array.data.get()) == address &&
                   jniEnv->IsSameObject(object, array.object.get());
        });
        if (entry == criticalArrays.end())
            Fail("invalid ARM64 JNI primitive array release");
        if (mode != JNI_ABORT)
            CopyCriticalArray(*entry, true);
        if (mode != JNI_COMMIT)
            criticalArrays.erase(entry);
    }

    void ReleaseDirectBuffers()
    {
        for (const auto& [reference, region] : pinnedDirectBuffers)
            jniEnv->DeleteGlobalRef(reference);
        pinnedDirectBuffers.clear();
    }

    bool PinDirectBuffer(jobject reference, DirectBufferRange region) const
    {
        pinnedDirectBuffers.reserve(pinnedDirectBuffers.size() + 1);
        const auto strong = jniEnv->NewGlobalRef(reference);
        if (!strong)
            return false;
        pinnedDirectBuffers.emplace_back(strong, region);
        return true;
    }

    bool PinDirectBufferAt(uintptr_t address, unsigned width, bool write) const
    {
        if (std::ranges::any_of(pinnedDirectBuffers, [&](const auto& entry) {
                return entry.second.Contains(address, width, write);
            }))
            return true;
        auto& buffers = Buffers();
        if (!buffers.registered.load(std::memory_order_acquire))
            return false;
        const std::lock_guard lock(buffers.mutex);
        for (const auto& [reference, region] : buffers.regions)
            if (region.Contains(address, width, write) && PinDirectBuffer(reference, region))
                return true;
        return false;
    }

    uint64_t GetDirectBufferAddress(jobject buffer)
    {
        const auto address = reinterpret_cast<uintptr_t>(jniEnv->GetDirectBufferAddress(buffer));
        const auto capacity = jniEnv->GetDirectBufferCapacity(buffer);
        if (!address || capacity <= 0)
            return address;
        constexpr std::pair<const char*, unsigned> bufferTypes[]{
            {"java/nio/ByteBuffer", 1},   {"java/nio/ShortBuffer", 2}, {"java/nio/CharBuffer", 2},
            {"java/nio/IntBuffer", 4},    {"java/nio/FloatBuffer", 4}, {"java/nio/LongBuffer", 8},
            {"java/nio/DoubleBuffer", 8},
        };
        unsigned elementSize = 0;
        for (const auto& [name, size] : bufferTypes) {
            const auto type = jniEnv->FindClass(name);
            if (!type)
                return 0;
            const bool instance = jniEnv->IsInstanceOf(buffer, type);
            jniEnv->DeleteLocalRef(type);
            if (instance) {
                elementSize = size;
                break;
            }
        }
        if (!elementSize || static_cast<uint64_t>(capacity) > (UINTPTR_MAX - address) / elementSize)
            return 0;
        const auto bytes = static_cast<uint64_t>(capacity) * elementSize;
        DirectBufferRange region{address, bytes, true};
        for (uintptr_t pointer = address; pointer - address < bytes;) {
            MEMORY_BASIC_INFORMATION page{};
            if (!VirtualQuery(reinterpret_cast<void*>(pointer), &page, sizeof(page)) || page.State != MEM_COMMIT ||
                (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                return 0;
            const auto protection = page.Protect & 0xff;
            if (protection == PAGE_EXECUTE)
                return 0;
            region.writable &= protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                               protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
            pointer = reinterpret_cast<uintptr_t>(page.BaseAddress) + page.RegionSize;
        }
        if (!PinDirectBuffer(buffer, region))
            return 0;
        auto& buffers = Buffers();
        const std::lock_guard lock(buffers.mutex);
        std::erase_if(buffers.regions, [&](const auto& entry) {
            if (!jniEnv->IsSameObject(entry.first, nullptr))
                return false;
            jniEnv->DeleteWeakGlobalRef(entry.first);
            return true;
        });
        if (std::ranges::none_of(buffers.regions, [&](const auto& entry) {
                return entry.second.address == address && jniEnv->IsSameObject(entry.first, buffer);
            })) {
            buffers.regions.reserve(buffers.regions.size() + 1);
            const auto weak = jniEnv->NewWeakGlobalRef(buffer);
            if (!weak)
                return 0;
            buffers.regions.emplace_back(weak, region);
        }
        buffers.registered.store(true, std::memory_order_release);
        return address;
    }

    uint64_t CallJni(JniEntry entry)
    {
        if (!jni)
            Fail("JNI call outside an ARM64 JNI entry");
        if (entry >= JniEntry::VmBase) {
            if (Get(0) != reinterpret_cast<uintptr_t>(&guestJni.vm))
                Fail("invalid ARM64 JavaVM handle");
            if (entry == JniEntry::DetachThread) {
                if (!criticalArrays.empty())
                    Fail("cannot detach with a borrowed ARM64 JNI array");
                if (jniEnv)
                    ReleaseDirectBuffers();
                const auto result = jni->vm->DetachCurrentThread();
                if (result == JNI_OK) {
                    jniEnv = nullptr;
                    references.clear();
                    localFrames.clear();
                    guestJniEnv = reinterpret_cast<uintptr_t>(guestJni.envTable.data());
                }
                return uint32_t(result);
            }
            const auto slot = Get(1);
            auto* output = Memory(slot, sizeof(uintptr_t), true);
            void* env = nullptr;
            jint result;
            if (entry == JniEntry::GetEnv) {
                result = jni->vm->GetEnv(&env, static_cast<jint>(Get(2, 32)));
            }
            else if (entry == JniEntry::AttachThread || entry == JniEntry::AttachDaemon) {
                JavaVMAttachArgs attach{};
                std::string name;
                const auto args = Get(2);
                if (args) {
                    attach.version = static_cast<jint>(Load(args, 4));
                    if (const auto nameAddress = Load(args + 8, 8)) {
                        name = ReadString(nameAddress, 1024);
                        attach.name = name.data();
                    }
                    attach.group = reinterpret_cast<jobject>(Load(args + 16, 8));
                    if (!KnownReference(attach.group))
                        Fail("invalid ARM64 JNI thread group");
                }
                result = entry == JniEntry::AttachDaemon
                             ? jni->vm->AttachCurrentThreadAsDaemon(&env, args ? &attach : nullptr)
                             : jni->vm->AttachCurrentThread(&env, args ? &attach : nullptr);
            }
            else {
                Fail("unsupported ARM64 JavaVM table slot");
            }
            const auto pointer = result == JNI_OK ? guestEnvAddress : uintptr_t{0};
            std::memcpy(output, &pointer, sizeof(pointer));
            if (result == JNI_OK)
                jniEnv = static_cast<JNIEnv*>(env);
            return static_cast<uint32_t>(result);
        }
        if (Get(0) != guestEnvAddress || !jniEnv)
            Fail("invalid ARM64 JNIEnv handle");
        const auto object = reinterpret_cast<jobject>(Get(1));
        if (entry == JniEntry::FatalError) {
            // Stop the guest invocation without aborting the Windows JVM.
            Fail("JNI FatalError: " + (Get(1) ? ReadString(Get(1), 65536) : std::string("(no message)")));
        }
        if (entry == JniEntry::ExceptionCheck)
            return jniEnv->ExceptionCheck();
        if (entry == JniEntry::ExceptionClear) {
            jniEnv->ExceptionClear();
            return 0;
        }
        if (entry == JniEntry::ExceptionOccurred)
            return ReturnReference(jniEnv->ExceptionOccurred());
        if (entry == JniEntry::ExceptionDescribe) {
            jniEnv->ExceptionDescribe();
            return 0;
        }
        if (entry == JniEntry::Throw) {
            if (!object || !KnownReference(object))
                Fail("invalid ARM64 JNI throwable");
            const auto type = jniEnv->FindClass("java/lang/Throwable");
            if (!type)
                return uint32_t(JNI_ERR);
            const auto valid = jniEnv->IsInstanceOf(object, type);
            jniEnv->DeleteLocalRef(type);
            if (!valid)
                Fail("invalid ARM64 JNI throwable");
            return uint32_t(jniEnv->Throw(static_cast<jthrowable>(object)));
        }
        if (entry == JniEntry::PushLocalFrame) {
            const auto capacity = static_cast<jint>(Get(1, 32));
            if (capacity < 0)
                Fail("invalid ARM64 JNI local frame capacity");
            localFrames.push_back(references.size());
            const auto status = jniEnv->PushLocalFrame(capacity);
            if (status < 0)
                localFrames.pop_back();
            return uint32_t(status);
        }
        if (entry == JniEntry::PopLocalFrame) {
            if (localFrames.empty() || !KnownReference(object))
                Fail("invalid ARM64 JNI local frame");
            const auto result = jniEnv->PopLocalFrame(object);
            references.resize(localFrames.back());
            localFrames.pop_back();
            return ReturnReference(result);
        }
        if (entry == JniEntry::DeleteGlobalRef || entry == JniEntry::DeleteWeakGlobalRef) {
            if (!object)
                return 0;
            const bool weak = entry == JniEntry::DeleteWeakGlobalRef;
            auto& global = Globals();
            const std::lock_guard lock(global.mutex);
            auto& handles = weak ? global.weakHandles : global.handles;
            const auto found = handles.find(object);
            if (found == handles.end())
                Fail("invalid ARM64 JNI global reference");
            handles.erase(found);
            if (weak)
                jniEnv->DeleteWeakGlobalRef(object);
            else
                jniEnv->DeleteGlobalRef(object);
            return 0;
        }
        if (entry == JniEntry::DeleteLocalRef) {
            if (!object)
                return 0;
            const auto found = std::ranges::find(references, object);
            if (found == references.end())
                Fail("invalid ARM64 JNI local reference");
            const auto position = static_cast<size_t>(found - references.begin());
            references.erase(found);
            for (auto& frame : localFrames)
                if (position < frame)
                    --frame;
            jniEnv->DeleteLocalRef(object);
            return 0;
        }
        if (entry == JniEntry::ReleasePrimitiveArrayCritical) {
            ReleaseCriticalArray(object);
            return 0;
        }
        const auto releaseElements =
            static_cast<size_t>(entry) - static_cast<size_t>(JniEntry::ReleasePrimitiveElements);
        if (releaseElements < 8) {
            ReleaseCriticalArray(object, releaseElements);
            return 0;
        }
        if (jniEnv->ExceptionCheck())
            return entry == JniEntry::RegisterNatives || entry == JniEntry::EnsureLocalCapacity ? uint32_t(JNI_ERR) : 0;
        if (entry == JniEntry::EnsureLocalCapacity) {
            const auto capacity = static_cast<jint>(Get(1, 32));
            if (capacity < 0)
                return uint32_t(JNI_ERR);
            return uint32_t(jniEnv->EnsureLocalCapacity(capacity));
        }
        const auto newPrimitive = static_cast<size_t>(entry) - static_cast<size_t>(JniEntry::NewPrimitiveArray);
        if (newPrimitive < 8)
            return NewPrimitiveArray(newPrimitive);
        if (entry == JniEntry::GetPrimitiveArrayCritical)
            return GetCriticalArray(object);
        const auto getElements = static_cast<size_t>(entry) - static_cast<size_t>(JniEntry::GetPrimitiveElements);
        if (getElements < 8)
            return GetCriticalArray(object, getElements);
        const auto getRegion = static_cast<size_t>(entry) - static_cast<size_t>(JniEntry::GetPrimitiveRegion);
        if (getRegion < 8) {
            TransferPrimitiveRegion(object, getRegion, false);
            return 0;
        }
        const auto setRegion = static_cast<size_t>(entry) - static_cast<size_t>(JniEntry::SetPrimitiveRegion);
        if (setRegion < 8) {
            TransferPrimitiveRegion(object, setRegion, true);
            return 0;
        }
        if (entry == JniEntry::NewLocalRef) {
            if (!KnownReference(object, true))
                Fail("invalid ARM64 JNI reference");
            return ReturnReference(jniEnv->NewLocalRef(object));
        }
        if (entry == JniEntry::NewDirectByteBuffer) {
            const auto address = Get(1), length = Get(2);
            if (!address || length > INT_MAX)
                Fail("invalid ARM64 JNI direct byte buffer range");
            auto* data =
                length ? Memory(address, static_cast<unsigned>(length), true) : reinterpret_cast<uint8_t*>(address);
            return ReturnReference(jniEnv->NewDirectByteBuffer(data, static_cast<jlong>(length)));
        }
        if (entry == JniEntry::GetDirectBufferAddress || entry == JniEntry::GetDirectBufferCapacity) {
            if (!KnownReference(object, true))
                Fail("invalid ARM64 JNI direct buffer reference");
            if (entry == JniEntry::GetDirectBufferCapacity)
                return object ? static_cast<uint64_t>(jniEnv->GetDirectBufferCapacity(object)) : UINT64_MAX;
            return object ? GetDirectBufferAddress(object) : 0;
        }
        if (entry == JniEntry::GetObjectRefType) {
            if (!KnownReference(object, true))
                Fail("invalid ARM64 JNI reference");
            return jniEnv->GetObjectRefType(object);
        }
        if (entry == JniEntry::GetJavaVm) {
            Store(Get(1), sizeof(uintptr_t), reinterpret_cast<uintptr_t>(&guestJni.vm));
            return JNI_OK;
        }
        if (entry == JniEntry::GetObjectClass) {
            if (!object)
                Fail("invalid ARM64 JNI object reference");
            if (KnownReference(object))
                return ReturnReference(jniEnv->GetObjectClass(object));
            const auto local = PromoteWeakReference(object);
            return local ? ReturnReference(jniEnv->GetObjectClass(local.get())) : 0;
        }
        if (entry == JniEntry::IsInstanceOf) {
            if (!KnownReference(object, true))
                Fail("invalid ARM64 JNI object reference");
            const auto type = ClassReference(reinterpret_cast<jobject>(Get(2)));
            return type ? jniEnv->IsInstanceOf(object, type) : JNI_FALSE;
        }
        if (entry == JniEntry::IsSameObject) {
            const auto other = reinterpret_cast<jobject>(Get(2));
            if (!KnownReference(object, true) || !KnownReference(other, true))
                Fail("invalid ARM64 JNI comparison reference");
            return jniEnv->IsSameObject(object, other);
        }
        if (entry == JniEntry::GetArrayLength) {
            if (!object || !KnownReference(object))
                Fail("invalid ARM64 JNI array reference");
            return uint32_t(jniEnv->GetArrayLength(static_cast<jarray>(object)));
        }
        if (entry == JniEntry::NewObjectArray) {
            const auto length = static_cast<jsize>(Get(1, 32));
            const auto type = ClassReference(reinterpret_cast<jobject>(Get(2)));
            const auto initial = reinterpret_cast<jobject>(Get(3));
            if (length < 0 || !KnownReference(initial))
                Fail("invalid ARM64 JNI object array arguments");
            return type ? ReturnReference(jniEnv->NewObjectArray(length, type, initial)) : 0;
        }
        if (entry == JniEntry::GetObjectArrayElement || entry == JniEntry::SetObjectArrayElement) {
            if (!object || !KnownReference(object))
                Fail("invalid ARM64 JNI object array reference");
            const auto type = jniEnv->FindClass("[Ljava/lang/Object;");
            if (!type)
                return 0;
            const auto valid = jniEnv->IsInstanceOf(object, type);
            jniEnv->DeleteLocalRef(type);
            if (!valid)
                Fail("invalid ARM64 JNI object array reference");
            const auto array = static_cast<jobjectArray>(object);
            const auto index = static_cast<jsize>(Get(2, 32));
            if (entry == JniEntry::GetObjectArrayElement) {
                return ReturnReference(jniEnv->GetObjectArrayElement(array, index));
            }
            const auto value = reinterpret_cast<jobject>(Get(3));
            if (!KnownReference(value))
                Fail("invalid ARM64 JNI object array element");
            jniEnv->SetObjectArrayElement(array, index, value);
            return 0;
        }
        if (entry == JniEntry::NewString) {
            const auto length = static_cast<jsize>(Get(2, 32));
            if (length < 0)
                Fail("invalid ARM64 JNI UTF-16 length");
            const auto bytes = static_cast<unsigned>(length) * sizeof(jchar);
            const auto* input = bytes ? ReadMemory(Get(1), static_cast<unsigned>(bytes)) : nullptr;
            std::vector<jchar> characters(static_cast<size_t>(length));
            if (bytes)
                std::memcpy(characters.data(), input, bytes);
            const jchar empty = 0;
            return ReturnReference(jniEnv->NewString(length ? characters.data() : &empty, length));
        }
        if (entry == JniEntry::GetStringLength || entry == JniEntry::GetStringUtfLength) {
            const auto string = StringReference(object);
            if (!string)
                return 0;
            return uint32_t(entry == JniEntry::GetStringLength ? jniEnv->GetStringLength(string)
                                                               : jniEnv->GetStringUTFLength(string));
        }
        if (entry == JniEntry::GetStringChars) {
            const auto string = StringReference(object);
            if (!string)
                return 0;
            const auto bytes = static_cast<unsigned>(jniEnv->GetStringLength(string)) * sizeof(jchar);
            auto* copied = Get(2) ? Memory(Get(2), 1, true) : nullptr;
            const auto source = jniEnv->GetStringChars(string, nullptr);
            if (!source)
                return 0;
            const auto release = [&](const jchar* chars) {
                jniEnv->ReleaseStringChars(string, chars);
            };
            const std::unique_ptr<const jchar, decltype(release)> hold(source, release);
            const auto address = Arm64GuestHeap().Allocate(bytes, false);
            if (!address)
                throw std::bad_alloc();
            if (bytes)
                std::memcpy(Memory(address, static_cast<unsigned>(bytes), true), source, bytes);
            if (copied)
                *copied = JNI_TRUE;
            return address;
        }
        if (entry == JniEntry::GetStringUtfChars) {
            const auto string = StringReference(object);
            if (!string)
                return 0;
            const auto bytes = static_cast<unsigned>(jniEnv->GetStringUTFLength(string)) + 1;
            auto* copied = Get(2) ? Memory(Get(2), 1, true) : nullptr;
            const auto source = jniEnv->GetStringUTFChars(string, nullptr);
            if (!source)
                return 0;
            const auto release = [&](const char* value) {
                jniEnv->ReleaseStringUTFChars(string, value);
            };
            const std::unique_ptr<const char, decltype(release)> hold(source, release);
            const auto address = Arm64GuestHeap().Allocate(bytes, false);
            if (!address)
                throw std::bad_alloc();
            std::memcpy(Memory(address, bytes, true), source, bytes);
            if (copied)
                *copied = JNI_TRUE;
            return address;
        }
        if (entry == JniEntry::ReleaseStringUtfChars || entry == JniEntry::ReleaseStringChars) {
            if (!KnownReference(object) || !Arm64GuestHeap().Free(Get(2)))
                Fail("invalid ARM64 JNI string release");
            return 0;
        }
        if (entry == JniEntry::NewGlobalRef || entry == JniEntry::NewWeakGlobalRef) {
            if (!KnownReference(object, true))
                Fail("invalid ARM64 JNI reference");
            const bool weak = entry == JniEntry::NewWeakGlobalRef;
            const auto handle = weak ? jniEnv->NewWeakGlobalRef(object) : jniEnv->NewGlobalRef(object);
            if (handle) {
                auto& global = Globals();
                const std::lock_guard lock(global.mutex);
                try {
                    (weak ? global.weakHandles : global.handles).insert(handle);
                }
                catch (...) {
                    if (weak)
                        jniEnv->DeleteWeakGlobalRef(handle);
                    else
                        jniEnv->DeleteGlobalRef(handle);
                    throw;
                }
            }
            return reinterpret_cast<uintptr_t>(handle);
        }
        if (entry == JniEntry::FromReflectedMethod)
            return FromReflectedMethod(object);
        if (entry == JniEntry::GetMethodId || entry == JniEntry::GetStaticMethodId) {
            const auto type = ClassReference(object);
            if (!type)
                return 0;
            const auto name = ReadString(Get(2), 1024), signature = ReadString(Get(3), 4096);
            const bool isStatic = entry == JniEntry::GetStaticMethodId;
            const auto method = !isStatic ? jniEnv->GetMethodID(type, name.c_str(), signature.c_str())
                                          : jniEnv->GetStaticMethodID(type, name.c_str(), signature.c_str());
            return RememberJniMethod(method, type, name, signature, isStatic);
        }
        if (entry == JniEntry::GetFieldId || entry == JniEntry::GetStaticFieldId) {
            const auto owner = ClassReference(object);
            if (!owner)
                return 0;
            const auto name = ReadString(Get(2), 1024), signature = ReadString(Get(3), 4096);
            size_t consumed = 0;
            const char descriptor = ReadArm64JniType(signature, consumed, false);
            if (consumed != signature.size())
                Fail("invalid ARM64 JNI field signature");
            const bool isStatic = entry == JniEntry::GetStaticFieldId;
            const auto field = isStatic ? jniEnv->GetStaticFieldID(owner, name.c_str(), signature.c_str())
                                        : jniEnv->GetFieldID(owner, name.c_str(), signature.c_str());
            if (!field)
                return 0;
            auto& global = Globals();
            const std::lock_guard lock(global.mutex);
            const auto [first, last] = global.fields.equal_range(field);
            if (std::none_of(first, last, [&](const auto& value) {
                    return value.second.isStatic == isStatic && jniEnv->IsSameObject(owner, value.second.owner);
                })) {
                const auto reference = static_cast<jclass>(jniEnv->NewGlobalRef(owner));
                if (!reference)
                    return 0;
                try {
                    global.fields.emplace(field, GlobalReferences::Field{reference, descriptor, isStatic});
                }
                catch (...) {
                    jniEnv->DeleteGlobalRef(reference);
                    throw;
                }
            }
            return reinterpret_cast<uintptr_t>(field);
        }
        constexpr std::string_view fieldTypes = "LZBCSIJFD";
        for (const auto base : {JniEntry::GetObjectField, JniEntry::SetObjectField, JniEntry::GetStaticObjectField,
                                JniEntry::SetStaticObjectField}) {
            if (entry < base)
                continue;
            const bool set = base == JniEntry::SetObjectField || base == JniEntry::SetStaticObjectField;
            const bool isStatic = base == JniEntry::GetStaticObjectField || base == JniEntry::SetStaticObjectField;
            const auto index = static_cast<size_t>(entry) - static_cast<size_t>(base);
            if (index < fieldTypes.size()) {
                if (!object || !KnownReference(object))
                    Fail("invalid ARM64 JNI field receiver");
                const auto owner = isStatic ? ClassReference(object) : nullptr;
                if (isStatic && !owner)
                    return 0;
                const auto field = reinterpret_cast<jfieldID>(Get(2));
                const auto descriptor = fieldTypes[index];
                {
                    auto& global = Globals();
                    const std::lock_guard lock(global.mutex);
                    const auto [first, last] = global.fields.equal_range(field);
                    if (std::none_of(first, last, [&](const auto& value) {
                            return value.second.type == descriptor && value.second.isStatic == isStatic &&
                                   (isStatic ? jniEnv->IsSameObject(owner, value.second.owner)
                                             : jniEnv->IsInstanceOf(object, value.second.owner));
                        }))
                        Fail("invalid ARM64 JNI field access");
                }
#define SET_FIELD(Type, Value)                                                                                         \
    do {                                                                                                               \
        if (isStatic)                                                                                                  \
            jniEnv->SetStatic##Type##Field(owner, field, Value);                                                       \
        else                                                                                                           \
            jniEnv->Set##Type##Field(object, field, Value);                                                            \
        return 0;                                                                                                      \
    } while (false)
                if (set) {
                    const auto value = Get(3);
                    switch (descriptor) {
                    case 'Z':
                        SET_FIELD(Boolean, static_cast<jboolean>(value));
                    case 'B':
                        SET_FIELD(Byte, static_cast<jbyte>(value));
                    case 'C':
                        SET_FIELD(Char, static_cast<jchar>(value));
                    case 'S':
                        SET_FIELD(Short, static_cast<jshort>(value));
                    case 'I':
                        SET_FIELD(Int, static_cast<jint>(value));
                    case 'J':
                        SET_FIELD(Long, std::bit_cast<jlong>(value));
                    case 'F':
                        SET_FIELD(Float, std::bit_cast<jfloat>(uint32_t(GetVector(0)[0])));
                    case 'D':
                        SET_FIELD(Double, std::bit_cast<jdouble>(GetVector(0)[0]));
                    default:
                        Fail("unsupported ARM64 JNI object field write");
                    }
                }
#undef SET_FIELD
#define GET_FIELD(Type)                                                                                                \
    (isStatic ? jniEnv->GetStatic##Type##Field(owner, field) : jniEnv->Get##Type##Field(object, field))
                switch (descriptor) {
                case 'L':
                    return ReturnReference(GET_FIELD(Object));
                case 'Z':
                    return GET_FIELD(Boolean);
                case 'B':
                    return uint32_t(GET_FIELD(Byte));
                case 'C':
                    return GET_FIELD(Char);
                case 'S':
                    return uint32_t(GET_FIELD(Short));
                case 'I':
                    return uint32_t(GET_FIELD(Int));
                case 'J':
                    return uint64_t(GET_FIELD(Long));
                case 'F':
                    importFloatResult = std::bit_cast<uint32_t>(GET_FIELD(Float));
                    return 0;
                case 'D':
                    importFloatResult = std::bit_cast<uint64_t>(GET_FIELD(Double));
                    return 0;
                }
#undef GET_FIELD
            }
        }
        if (entry == JniEntry::FindClass) {
            const auto name = ReadString(Get(1), 1024);
            return ReturnReference(jni->findClass(jniEnv, name.c_str()));
        }
        if (entry == JniEntry::NewStringUtf) {
            const auto str = ReadString(Get(1), 1u << 20);
            return ReturnReference(jniEnv->NewStringUTF(str.c_str()));
        }
        const auto newObject = static_cast<size_t>(entry) - static_cast<size_t>(JniEntry::NewObject);
        if (newObject < 3)
            return InvokeJniMethod(object, static_cast<unsigned>(newObject), 'V', false, true);
        constexpr std::string_view methodTypes = "LZBCSIJFDV";
        for (const auto base : {JniEntry::CallObject, JniEntry::CallStaticObject}) {
            if (entry < base)
                continue;
            const auto index = static_cast<size_t>(entry) - static_cast<size_t>(base);
            if (index / 3 < methodTypes.size())
                return InvokeJniMethod(object, static_cast<unsigned>(index % 3), methodTypes[index / 3],
                                       base == JniEntry::CallStaticObject);
        }
        if (entry != JniEntry::RegisterNatives)
            Fail("unsupported ARM64 JNI table slot " + std::to_string(static_cast<size_t>(entry)));
        const auto type = ClassReference(object);
        if (!type)
            return uint32_t(JNI_ERR);
        const auto count = static_cast<jint>(Get(3, 32));
        if (count < 0 || count > 4096)
            Fail("invalid ARM64 RegisterNatives count");
        std::vector<Arm64NativeMethod> methods;
        if (count) {
            struct Entry {
                uintptr_t name, signature, code;
            };

            const auto* table = ReadMemory(Get(2), static_cast<unsigned>(count * sizeof(Entry)));
            methods.reserve(static_cast<size_t>(count));
            for (jint i = 0; i < count; ++i) {
                Entry method;
                std::memcpy(&method, table + sizeof(Entry) * i, sizeof(method));
                if (!CodeImage(method.code))
                    Fail("invalid ARM64 RegisterNatives target");
                methods.push_back({ReadString(method.name, 1024), ReadString(method.signature, 4096), method.code});
            }
        }
        return uint32_t(jni->registerNatives(jniEnv, type, *image, methods));
    }

    std::optional<uint64_t> CallThreadImport(std::string_view name)
    {
        // These LP64 Bionic objects have identical layouts on both guest architectures.
        constexpr unsigned MutexSize = 40, CondSize = 48, RwLockSize = 56, AttrSize = sizeof(AndroidThreadAttr);
        const auto memory = [&](unsigned reg, unsigned size, bool write, bool nullable = false) -> void* {
            return nullable && !Get(reg) ? nullptr : Memory(Get(reg), size, write);
        };
        const auto call = [this, name]<typename... Args>(Args... arguments) {
            const auto target = AndroidThreadImport(name, true);
            if (!target)
                Fail("missing ARM64 thread import");
            return uint32_t(reinterpret_cast<int (*)(Args...)>(target)(arguments...));
        };
        if (name == "pthread_mutexattr_init" || name == "pthread_mutexattr_destroy" ||
            name == "pthread_condattr_init" || name == "pthread_condattr_destroy")
            return call(static_cast<int64_t*>(memory(0, 8, true)));
        if (name == "pthread_mutexattr_settype" || name == "pthread_condattr_setclock")
            return call(static_cast<int64_t*>(memory(0, 8, true)), static_cast<int>(Get(1, 32)));
        if (name == "pthread_mutex_init")
            return call(memory(0, MutexSize, true), static_cast<const int64_t*>(memory(1, 8, false, true)));
        if (name == "pthread_mutex_destroy" || name == "pthread_mutex_lock" || name == "pthread_mutex_unlock" ||
            name == "pthread_mutex_trylock")
            return call(memory(0, MutexSize, true));
        if (name == "pthread_cond_init")
            return call(static_cast<int*>(memory(0, CondSize, true)),
                        static_cast<const int64_t*>(memory(1, 8, false, true)));
        if (name == "pthread_cond_destroy" || name == "pthread_cond_signal" || name == "pthread_cond_broadcast")
            return call(static_cast<int*>(memory(0, CondSize, true)));
        if (name == "pthread_cond_wait")
            return call(static_cast<int*>(memory(0, CondSize, true)), memory(1, MutexSize, true));
        if (name == "pthread_cond_timedwait")
            return call(static_cast<int*>(memory(0, CondSize, true)), memory(1, MutexSize, true),
                        static_cast<const AndroidTimespec*>(memory(2, sizeof(AndroidTimespec), false)));
        if (name == "pthread_rwlock_init")
            return call(memory(0, RwLockSize, true), static_cast<const int64_t*>(memory(1, 8, false, true)));
        if (name == "pthread_rwlock_destroy" || name == "pthread_rwlock_rdlock" || name == "pthread_rwlock_wrlock" ||
            name == "pthread_rwlock_tryrdlock" || name == "pthread_rwlock_trywrlock" || name == "pthread_rwlock_unlock")
            return call(memory(0, RwLockSize, true));
        if (name == "pthread_attr_init" || name == "pthread_attr_destroy")
            return call(static_cast<AndroidThreadAttr*>(memory(0, AttrSize, true)));
        if (name == "pthread_attr_setdetachstate")
            return call(static_cast<AndroidThreadAttr*>(memory(0, AttrSize, true)), static_cast<int>(Get(1, 32)));
        if (name == "pthread_attr_setstacksize")
            return call(static_cast<AndroidThreadAttr*>(memory(0, AttrSize, true)), static_cast<size_t>(Get(1)));
        if (name == "pthread_attr_setschedpolicy")
            return call(static_cast<AndroidThreadAttr*>(memory(0, AttrSize, true)), static_cast<int>(Get(1, 32)));
        if (name == "pthread_attr_setschedparam")
            return call(static_cast<AndroidThreadAttr*>(memory(0, AttrSize, true)),
                        static_cast<const int*>(memory(1, 4, false)));
        if (name == "pthread_attr_getschedpolicy" || name == "pthread_attr_getschedparam")
            return call(static_cast<const AndroidThreadAttr*>(memory(0, AttrSize, false)),
                        static_cast<int*>(memory(1, 4, true)));
        if (name == "pthread_getattr_np") {
            if (Get(0) != GetCurrentThreadId())
                return 95; // Bionic ENOTSUP.
            AndroidThreadAttr attr{};
            attr.stackBase = reinterpret_cast<void*>(StackBase());
            attr.stackSize = StackSize;
            std::memcpy(memory(1, AttrSize, true), &attr, sizeof(attr));
            return 0;
        }
        if (name == "pthread_attr_getstack")
            return call(static_cast<const AndroidThreadAttr*>(memory(0, AttrSize, false)),
                        static_cast<void**>(memory(1, sizeof(void*), true)),
                        static_cast<size_t*>(memory(2, sizeof(size_t), true)));
        if (name == "pthread_attr_getstacksize" || name == "pthread_attr_getguardsize")
            return call(static_cast<const AndroidThreadAttr*>(memory(0, AttrSize, false)),
                        static_cast<size_t*>(memory(1, sizeof(size_t), true)));
        if (name == "__pthread_cleanup_push") {
            const auto address = Get(0), routine = Get(1);
            if (routine && !CodeImage(routine))
                Fail("invalid ARM64 pthread cleanup routine");
            const std::array entry{cleanupFrames.empty() ? uintptr_t{0} : cleanupFrames.back(), routine, Get(2)};
            auto* frame = Memory(address, sizeof(entry), true);
            cleanupFrames.push_back(address);
            std::memcpy(frame, entry.data(), sizeof(entry));
            return 0;
        }
        if (name == "__pthread_cleanup_pop") {
            const auto address = Get(0);
            if (cleanupFrames.empty() || cleanupFrames.back() != address)
                Fail("invalid ARM64 pthread cleanup frame");
            cleanupFrames.pop_back();
            if (!Get(1, 32))
                return 0;
            const auto routine = Load(address + sizeof(uintptr_t), sizeof(uintptr_t));
            if (!routine)
                return 0;
            const auto* owner = CodeImage(routine);
            if (!owner || callbackDepth >= 32)
                Fail("invalid ARM64 pthread cleanup callback");
            const std::array arguments{Load(address + 2 * sizeof(uintptr_t), sizeof(uintptr_t))};
            Interpreter callback(*owner, routine, importName, propertyGet, callbackDepth + 1, jni);
            callback.jniEnv = jniEnv;
            callback.RunCallback(arguments, {});
            return 0;
        }
        if (name == "pthread_create") {
            const auto code = Get(2);
            if (!CodeImage(code))
                Fail("invalid ARM64 pthread_create target");
            return call(static_cast<uint64_t*>(memory(0, 8, true)),
                        static_cast<const AndroidThreadAttr*>(memory(1, AttrSize, false, true)), code,
                        reinterpret_cast<void*>(Get(3)));
        }
        if (name == "pthread_join")
            return call(Get(0), static_cast<void**>(memory(1, 8, true, true)));
        if (name == "pthread_kill")
            return call(Get(0), static_cast<int>(Get(1, 32)));
        if (name == "sched_getscheduler")
            return call(static_cast<int>(Get(0, 32)));
        if (name == "sched_getparam")
            return call(static_cast<int>(Get(0, 32)), static_cast<int*>(memory(1, sizeof(int), true, true)));
        if (name == "sched_setscheduler")
            return call(static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32)),
                        static_cast<const int*>(memory(2, sizeof(int), false, true)));
        if (name == "sched_get_priority_min" || name == "sched_get_priority_max")
            return call(static_cast<int>(Get(0, 32)));
        if (name == "pthread_getschedparam")
            return call(Get(0), static_cast<int*>(memory(1, 4, true)), static_cast<int*>(memory(2, 4, true)));
        if (name == "pthread_setschedparam")
            return call(Get(0), static_cast<int>(Get(1, 32)), static_cast<const int*>(memory(2, 4, false)));
        if (name == "pthread_detach")
            return call(Get(0));
        if (name == "pthread_equal")
            return call(Get(0), Get(1));
        if (name == "pthread_self")
            return reinterpret_cast<uint64_t (*)()>(AndroidThreadImport(name, true))();
        if (name == "pthread_gettid_np")
            return uint32_t(reinterpret_cast<int (*)(uint64_t)>(AndroidThreadImport(name, true))(Get(0)));
        if (name == "pthread_setname_np") {
            const auto label = ReadString(Get(1), 256);
            return call(Get(0), label.c_str());
        }
        if (name == "pthread_getname_np") {
            // The host bridge writes a NUL-terminated name of at most 16 bytes.
            const auto capacity = Get(2);
            if (capacity < 16)
                return ERANGE;
            return call(Get(0), static_cast<char*>(memory(1, 16, true)), size_t{16});
        }
        if (name == "sem_init")
            return call(static_cast<unsigned*>(memory(0, 4, true)), static_cast<int>(Get(1, 32)),
                        static_cast<unsigned>(Get(2, 32)));
        if (name == "sem_destroy" || name == "sem_post" || name == "sem_wait" || name == "sem_trywait")
            return call(static_cast<unsigned*>(memory(0, 4, true)));
        if (name == "sem_timedwait")
            return call(static_cast<unsigned*>(memory(0, 4, true)),
                        static_cast<const AndroidTimespec*>(memory(1, sizeof(AndroidTimespec), false, true)));
        if (name == "sem_clockwait")
            return call(static_cast<unsigned*>(memory(0, 4, true)), static_cast<int>(Get(1, 32)),
                        static_cast<const AndroidTimespec*>(memory(2, sizeof(AndroidTimespec), false, true)));
        if (name == "sem_getvalue")
            return call(static_cast<unsigned*>(memory(0, 4, true)), static_cast<int*>(memory(1, 4, true)));
        if (name == "sigaction" || name == "sigaction64")
            return uint32_t(AndroidArm64Sigaction(
                static_cast<int>(Get(0, 32)),
                static_cast<const AndroidSignalAction*>(memory(1, sizeof(AndroidSignalAction), false, true)),
                static_cast<AndroidSignalAction*>(memory(2, sizeof(AndroidSignalAction), true, true))));
        if (name == "sigemptyset" || name == "sigfillset" || name == "sigemptyset64" || name == "sigfillset64")
            return call(static_cast<uint64_t*>(memory(0, 8, true)));
        if (name == "sigpending" || name == "sigpending64")
            return call(static_cast<uint64_t*>(memory(0, 8, true)));
        if (name == "sigwaitinfo" || name == "sigwaitinfo64")
            return call(static_cast<const uint64_t*>(memory(0, 8, false, true)), memory(1, 128, true, true));
        if (name == "sigsuspend" || name == "sigsuspend64")
            return uint32_t(AndroidArm64SuspendSignals(Load(Get(0), sizeof(uint64_t))));
        if (name == "raise")
            return call(static_cast<int>(Get(0, 32)));
        if (name == "sigaddset" || name == "sigdelset" || name == "sigaddset64" || name == "sigdelset64")
            return call(static_cast<uint64_t*>(memory(0, 8, true)), static_cast<int>(Get(1, 32)));
        if (name == "sigismember" || name == "sigismember64")
            return call(static_cast<const uint64_t*>(memory(0, 8, false)), static_cast<int>(Get(1, 32)));
        if (name == "pthread_sigmask" || name == "sigprocmask" || name == "pthread_sigmask64" ||
            name == "sigprocmask64")
            return call(static_cast<int>(Get(0, 32)), static_cast<const uint64_t*>(memory(1, 8, false, true)),
                        static_cast<uint64_t*>(memory(2, 8, true, true)));
        return std::nullopt;
    }

    std::optional<uint64_t> CallNativeLoader(std::string_view name)
    {
        const auto target = AndroidNativeLoaderImport(name, true);
        if (!target)
            return std::nullopt;
        const auto copyError = [](char* source, uint8_t* output) {
            const std::unique_ptr<char, decltype(&std::free)> cleanup(source, std::free);
            if (!output)
                return;
            uintptr_t error = 0;
            if (source) {
                const auto size = std::strlen(source) + 1;
                error = Arm64GuestHeap().Allocate(size, false);
                if (!error)
                    throw std::bad_alloc{};
                std::memcpy(reinterpret_cast<void*>(error), source, size);
            }
            std::memcpy(output, &error, sizeof(error));
        };
        if (name == "InitializeNativeLoader" || name == "ResetNativeLoader") {
            reinterpret_cast<void (*)()>(target)();
            return 0;
        }
        if (name == "OpenNativeLibrary") {
            const auto path = Get(2) ? std::optional{ReadString(Get(2), 32768)} : std::nullopt;
            const auto caller = Get(4) ? std::optional{ReadString(Get(4), 32768)} : std::nullopt;
            auto* bridgeOutput = Get(6) ? Memory(Get(6), sizeof(bool), true) : nullptr;
            auto* errorOutput = Get(7) ? Memory(Get(7), sizeof(uintptr_t), true) : nullptr;
            bool bridged = false;
            char* error = nullptr;
            using Open = void* (*)(JNIEnv*, int32_t, const char*, jobject, const char*, jstring, bool*, char**);
            const auto handle = reinterpret_cast<Open>(target)(
                reinterpret_cast<JNIEnv*>(Get(0)), static_cast<int32_t>(Get(1, 32)), path ? path->c_str() : nullptr,
                reinterpret_cast<jobject>(Get(3)), caller ? caller->c_str() : nullptr,
                reinterpret_cast<jstring>(Get(5)), &bridged, &error);
            if (bridgeOutput)
                std::memcpy(bridgeOutput, &bridged, sizeof(bridged));
            copyError(error, errorOutput);
            return reinterpret_cast<uintptr_t>(handle);
        }
        if (name == "CloseNativeLibrary") {
            auto* output = Get(2) ? Memory(Get(2), sizeof(uintptr_t), true) : nullptr;
            char* error = nullptr;
            const auto result = reinterpret_cast<bool (*)(void*, bool, char**)>(target)(reinterpret_cast<void*>(Get(0)),
                                                                                        Get(1) != 0, &error);
            copyError(error, output);
            return result;
        }
        if (name == "NativeLoaderFreeErrorMessage") {
            if (!Arm64GuestHeap().Free(Get(0)))
                Fail("invalid ARM64 native loader error allocation");
            return 0;
        }
        if (name == "FindNativeLoaderNamespaceByClassLoader")
            return reinterpret_cast<uintptr_t>(reinterpret_cast<void* (*)(JNIEnv*, jobject)>(target)(
                reinterpret_cast<JNIEnv*>(Get(0)), reinterpret_cast<jobject>(Get(1))));
        if (name == "IsNamespaceNativeBridged")
            return reinterpret_cast<bool (*)(const void*)>(target)(reinterpret_cast<const void*>(Get(0)));
        Fail("unsupported ARM64 native loader import");
    }

    bool CallMathImport(std::string_view name, uint64_t& integerResult)
    {
        const auto f0 = std::bit_cast<float>(uint32_t(GetVector(0)[0]));
        const auto f1 = std::bit_cast<float>(uint32_t(GetVector(1)[0]));
        const auto d0 = std::bit_cast<double>(GetVector(0)[0]);
        const auto d1 = std::bit_cast<double>(GetVector(1)[0]);
        if (name == "nanf") {
            const auto payload = ReadString(Get(0), 1024);
            importFloatResult = std::bit_cast<uint32_t>(std::nanf(payload.c_str()));
            return true;
        }
        if (name == "ldexp" || name == "ldexpf" || name == "scalbn" || name == "scalbnf") {
            const auto exponent = static_cast<int>(Get(0, 32));
            const bool scale = name.starts_with("scalbn");
            importFloatResult =
                name.ends_with('f')
                    ? uint64_t(std::bit_cast<uint32_t>(scale ? std::scalbn(f0, exponent) : std::ldexp(f0, exponent)))
                    : std::bit_cast<uint64_t>(scale ? std::scalbn(d0, exponent) : std::ldexp(d0, exponent));
            return true;
        }
        if (name == "frexp" || name == "frexpf") {
            int exponent = 0;
            importFloatResult = name == "frexpf" ? uint64_t(std::bit_cast<uint32_t>(std::frexp(f0, &exponent)))
                                                 : std::bit_cast<uint64_t>(std::frexp(d0, &exponent));
            Store(Get(0), sizeof(exponent), uint32_t(exponent));
            return true;
        }
        if (name == "modf" || name == "modff") {
            const auto split = [this]<typename Float>(Float value) {
                using Bits = std::conditional_t<sizeof(Float) == sizeof(uint64_t), uint64_t, uint32_t>;
                Float integral{};
                const auto fraction = std::modf(value, &integral);
                Store(Get(0), sizeof(Float), std::bit_cast<Bits>(integral));
                importFloatResult = std::bit_cast<Bits>(fraction);
            };
            if (name == "modff")
                split(f0);
            else
                split(d0);
            return true;
        }
        if (name == "sincos" || name == "sincosf") {
            if (name == "sincosf") {
                Store(Get(0), sizeof(float), std::bit_cast<uint32_t>(std::sin(f0)));
                Store(Get(1), sizeof(float), std::bit_cast<uint32_t>(std::cos(f0)));
            }
            else {
                Store(Get(0), sizeof(double), std::bit_cast<uint64_t>(std::sin(d0)));
                Store(Get(1), sizeof(double), std::bit_cast<uint64_t>(std::cos(d0)));
            }
            return true;
        }
#define UNARY(function)                                                                                                \
    if (name == #function "f") {                                                                                       \
        importFloatResult = std::bit_cast<uint32_t>(std::function(f0));                                                \
        return true;                                                                                                   \
    }                                                                                                                  \
    if (name == #function) {                                                                                           \
        importFloatResult = std::bit_cast<uint64_t>(std::function(d0));                                                \
        return true;                                                                                                   \
    }
#define BINARY(function)                                                                                               \
    if (name == #function "f") {                                                                                       \
        importFloatResult = std::bit_cast<uint32_t>(std::function(f0, f1));                                            \
        return true;                                                                                                   \
    }                                                                                                                  \
    if (name == #function) {                                                                                           \
        importFloatResult = std::bit_cast<uint64_t>(std::function(d0, d1));                                            \
        return true;                                                                                                   \
    }
        UNARY(sin)
        UNARY(cos)
        UNARY(tan)
        UNARY(asin)
        UNARY(acos)
        UNARY(atan)
        UNARY(sinh)
        UNARY(cosh)
        UNARY(tanh)
        UNARY(sqrt)
        UNARY(cbrt)
        UNARY(exp)
        UNARY(exp2)
        UNARY(log)
        UNARY(log1p)
        UNARY(log2)
        UNARY(log10)
        UNARY(floor)
        UNARY(ceil)
        UNARY(trunc)
        UNARY(round)
        UNARY(fabs)
        UNARY(logb)
        if (name == "nearbyintf" || name == "lrintf") {
            float value = 0;
            switch ((floatEnvironment.control >> 22) & 3) {
            case 0:
                value = std::nearbyint(f0);
                break;
            case 1:
                value = std::ceil(f0);
                break;
            case 2:
                value = std::floor(f0);
                break;
            case 3:
                value = std::trunc(f0);
                break;
            }
            if (name == "lrintf") {
                if (std::isnan(value) || value >= 0x1p63f || value < -0x1p63f)
                    floatEnvironment.status |= 1;
                else if (value != f0)
                    floatEnvironment.status |= 1u << 4;
                integerResult = ConvertFloatInteger(value, 64, true);
            }
            else {
                importFloatResult = std::bit_cast<uint32_t>(value);
            }
            return true;
        }
        BINARY(atan2)
        BINARY(pow)
        BINARY(fmod)
        BINARY(hypot)
        BINARY(remainder)
#undef BINARY
#undef UNARY
        return false;
    }

    void* FileHandle(unsigned reg, bool allowNull = false, bool close = false) const
    {
        const auto file = reinterpret_cast<void*>(Get(reg));
        if (!file && allowNull)
            return nullptr;
        auto& files = Files();
        {
            const std::lock_guard lock(files.mutex);
            if (close ? files.open.erase(file) != 0 : files.open.contains(file))
                return file;
        }
        if (!close) {
            for (const auto symbol : {"stdin", "stdout", "stderr"})
                if (file == *reinterpret_cast<void* const*>(AndroidLibcImport(symbol, true)))
                    return file;
        }
        Fail("invalid ARM64 stdio stream");
    }

    intptr_t MapMemory(uintptr_t address, size_t length, int access, int flags, int fd, int64_t offset)
    {
        const auto result = reinterpret_cast<intptr_t>(
            AndroidMapArm64(reinterpret_cast<void*>(address), length, access, flags, fd, offset));
        if (result == -1)
            mappingError = "mmap size " + std::to_string(length) + ", prot " + std::to_string(access) + ", flags " +
                           std::to_string(flags) + ", errno " + std::to_string(errno);
        return result;
    }

    int ProtectMemory(uintptr_t address, size_t length, int access)
    {
        std::shared_ptr<GuestStack> target;
        {
            auto& stacks = Stacks();
            const std::lock_guard lock(stacks.mutex);
            const auto next = stacks.stacks.upper_bound(address);
            if (next != stacks.stacks.begin()) {
                const auto& [base, candidate] = *std::prev(next);
                if (address - base < StackSize)
                    target = candidate;
            }
        }
        if (target)
            return target->Protect(address, length, access);
        if (access == 4 || access == 5) {
            if (image->ProtectArm64Code(address, length, access == 5))
                return 0;
            if (jni && jni->loadedImages)
                for (const auto& module : jni->loadedImages())
                    if (module.image->ProtectArm64Code(address, length, access == 5))
                        return 0;
        }
        if (access == 1 || access == 3) {
            if (image->ProtectData(address, length, access == 3))
                return 0;
            if (jni)
                for (const auto& module : jni->loadedImages())
                    if (module.image->ProtectData(address, length, access == 3))
                        return 0;
        }
        const auto result = AndroidProtectArm64(reinterpret_cast<void*>(address), length, access);
        if (result) {
            mappingError = "mprotect size " + std::to_string(length) + ", prot " + std::to_string(access) + ", errno " +
                           std::to_string(errno);
            char diagnostics[2]{};
            if (GetEnvironmentVariableA("STONE_ART_DIAGNOSTICS", diagnostics, sizeof(diagnostics)) == 1 &&
                diagnostics[0] == '1')
                std::fprintf(stderr, "[arm64-memory] mprotect rejected at 0x%llx, %s\n",
                             static_cast<unsigned long long>(address), mappingError.c_str());
        }
        return result;
    }

    int64_t Syscall(uint64_t number, unsigned argument)
    {
        // AArch64 Linux returns negative errno in x0. The libc wrapper does the errno translation.
        const auto checked = [](auto operation) {
            const auto previous = errno;
            const auto result = operation();
            const auto error = errno;
            errno = previous;
            return result == -1 ? -int64_t(error) : int64_t(result);
        };
        const auto arg = [&](unsigned n) {
            return Get(argument + n);
        };
        const auto integer = [&](unsigned n) {
            return static_cast<int>(Get(argument + n, 32));
        };
        const auto address = [&] {
            return reinterpret_cast<void*>(arg(0));
        };
        switch (number) {
        case 98: {
            if (arg(0) % std::atomic_ref<uint32_t>::required_alignment)
                return -EINVAL;
            const auto operation = static_cast<unsigned>(integer(1));
            const auto command = operation & ~(128u | 256u);
            if (command == 3 || command == 4) {
                if (operation & 256u)
                    return -38;
                if (!arg(4) || arg(4) % alignof(uint32_t) || arg(2) > INT_MAX || arg(3) > INT_MAX)
                    return -EINVAL;
                auto* source = reinterpret_cast<uint32_t*>(Memory(arg(0), sizeof(uint32_t), false));
                auto* destination = reinterpret_cast<uint32_t*>(Memory(arg(4), sizeof(uint32_t), false));
                return AndroidFutexRequeue(*source, operation, static_cast<uint32_t>(arg(2)),
                                           static_cast<uint32_t>(arg(3)), destination,
                                           static_cast<uint32_t>(integer(5)));
            }
            AndroidTimespec timeout{};
            const AndroidTimespec* limit = nullptr;
            if ((command == 0 || command == 9) && arg(3)) {
                std::memcpy(&timeout, ReadMemory(arg(3), sizeof(timeout)), sizeof(timeout));
                limit = &timeout;
            }
            auto* word = reinterpret_cast<uint32_t*>(Memory(arg(0), sizeof(uint32_t), false));
            return AndroidFutex(*word, operation, static_cast<uint32_t>(integer(2)), limit,
                                static_cast<uint32_t>(integer(5)));
        }
        case 172:
            return _getpid();
        case 174:
        case 175:
            return AndroidApplicationUid();
        case 178:
            return GetCurrentThreadId();
        case 215:
            return checked([&] {
                return AndroidUnmapArm64(address(), arg(1));
            });
        case 222:
            return checked([&] {
                return MapMemory(arg(0), arg(1), integer(2), integer(3), integer(4), std::bit_cast<int64_t>(arg(5)));
            });
        case 226:
            return checked([&] {
                return ProtectMemory(arg(0), arg(1), integer(2));
            });
        case 227:
            return checked([&] {
                return AndroidSyncArm64(address(), arg(1), integer(2));
            });
        case 233:
            return checked([&] {
                return AndroidAdviseArm64(address(), arg(1), integer(2));
            });
        case 278: {
            const auto size = arg(1);
            const auto flags = static_cast<unsigned>(integer(2));
            if (size > UINT_MAX || (flags & ~3u))
                return -EINVAL;
            return checked([&] {
                return AndroidGetRandom(size ? Memory(arg(0), static_cast<unsigned>(size), true) : nullptr, size,
                                        flags);
            });
        }
        case 279: {
            if (!arg(0))
                return -EFAULT;
            const auto name = ReadString(arg(0), 250, false);
            if (name.size() >= 250)
                return -EINVAL;
            return checked([&] {
                const auto create =
                    reinterpret_cast<int (*)(const char*, unsigned)>(AndroidLibcImport("memfd_create", true));
                return create(name.c_str(), static_cast<unsigned>(integer(1)));
            });
        }
        default:
            lastUnsupportedSyscall = number;
            return -38; // Bionic ENOSYS
        }
    }

    std::optional<uint64_t> CallCharacterImport(std::string_view name) const
    {
        const auto value = static_cast<uint32_t>(Get(0, 32));
#define CHAR_TEST(Symbol)                                                                                              \
    if (name == #Symbol)                                                                                               \
    return value <= UCHAR_MAX ? uint32_t(std::Symbol(int(value))) : 0
        CHAR_TEST(isalnum);
        CHAR_TEST(isalpha);
        CHAR_TEST(isblank);
        CHAR_TEST(iscntrl);
        CHAR_TEST(isdigit);
        CHAR_TEST(isgraph);
        CHAR_TEST(islower);
        CHAR_TEST(isprint);
        CHAR_TEST(ispunct);
        CHAR_TEST(isspace);
        CHAR_TEST(isupper);
        CHAR_TEST(isxdigit);
#undef CHAR_TEST
        if (name == "tolower")
            return value <= UCHAR_MAX ? uint32_t(std::tolower(int(value))) : value;
        if (name == "toupper")
            return value <= UCHAR_MAX ? uint32_t(std::toupper(int(value))) : value;
#define WIDE_TEST(Symbol)                                                                                              \
    if (name == #Symbol)                                                                                               \
    return value <= 0xffff ? uint32_t(std::Symbol(wint_t(value))) : 0
        WIDE_TEST(iswalnum);
        WIDE_TEST(iswalpha);
        WIDE_TEST(iswblank);
        WIDE_TEST(iswcntrl);
        WIDE_TEST(iswdigit);
        WIDE_TEST(iswgraph);
        WIDE_TEST(iswlower);
        WIDE_TEST(iswprint);
        WIDE_TEST(iswpunct);
        WIDE_TEST(iswspace);
        WIDE_TEST(iswupper);
        WIDE_TEST(iswxdigit);
#undef WIDE_TEST
        if (name == "towlower")
            return value <= 0xffff ? uint32_t(std::towlower(wint_t(value))) : value;
        if (name == "towupper")
            return value <= 0xffff ? uint32_t(std::towupper(wint_t(value))) : value;
        return std::nullopt;
    }

    size_t WideStringLength(uintptr_t address) const
    {
        for (size_t i = 0; i < (1u << 20); ++i)
            if (!Load(address + i * sizeof(uint32_t), sizeof(uint32_t)))
                return i;
        Fail("unterminated ARM64 wide string");
    }

    std::optional<uint64_t> CallWideMemoryImport(std::string_view name) const
    {
        if (name == "wcslen")
            return WideStringLength(Get(0));
        if (name == "wcscmp" || name == "wcscoll" || name == "wcscoll_l") {
            const auto left = Get(0), right = Get(1);
            for (size_t i = 0; i < (1u << 20); ++i) {
                const auto l = Load(left + i * sizeof(uint32_t), sizeof(uint32_t));
                const auto r = Load(right + i * sizeof(uint32_t), sizeof(uint32_t));
                if (l != r)
                    return uint32_t(l < r ? -1 : 1);
                if (!l)
                    return 0;
            }
            Fail("unterminated ARM64 wide string");
        }
        if (name == "wcsxfrm" || name == "wcsxfrm_l") {
            // In the C collation order the transformed key is the input string.
            const auto length = WideStringLength(Get(1));
            const auto count = static_cast<unsigned>(std::min<uint64_t>(Get(2), length + 1));
            if (count) {
                const auto bytes = count * static_cast<unsigned>(sizeof(uint32_t));
                std::memmove(Memory(Get(0), bytes, true), ReadMemory(Get(1), bytes), bytes);
            }
            return length;
        }
        if (name != "wmemcpy" && name != "wmemmove" && name != "wmemset" && name != "wmemchr" && name != "wmemcmp")
            return std::nullopt;
        const auto left = Get(0), count = Get(2);
        if (count > UINT_MAX / sizeof(uint32_t))
            Fail("ARM64 wide memory operation is too large");
        const auto bytes = static_cast<unsigned>(count * sizeof(uint32_t));
        if (!bytes)
            return name == "wmemchr" || name == "wmemcmp" ? 0 : left;
        if (name == "wmemcpy" || name == "wmemmove") {
            std::memmove(Memory(left, bytes, true), ReadMemory(Get(1), bytes), bytes);
            return left;
        }
        const auto value = static_cast<uint32_t>(Get(1, 32));
        if (name == "wmemset") {
            auto* output = Memory(left, bytes, true);
            for (unsigned i = 0; i < bytes; i += sizeof(value))
                std::memcpy(output + i, &value, sizeof(value));
            return left;
        }
        const auto* first = ReadMemory(left, bytes);
        const auto* second = name == "wmemcmp" ? ReadMemory(Get(1), bytes) : nullptr;
        for (unsigned i = 0; i < bytes; i += sizeof(uint32_t)) {
            uint32_t l, r = value;
            std::memcpy(&l, first + i, sizeof(l));
            if (second) {
                std::memcpy(&r, second + i, sizeof(r));
                if (l != r)
                    return uint32_t(l < r ? -1 : 1);
            }
            else if (l == r)
                return left + i;
        }
        return 0;
    }

    int CompareArrayElements(const ElfImage& owner, uintptr_t comparison, uintptr_t left, uintptr_t right)
    {
        const std::array arguments{left, right};
        Interpreter callback(owner, comparison, importName, propertyGet, callbackDepth + 1, jni, this);
        callback.jniEnv = jniEnv;
        return static_cast<int32_t>(callback.RunCallback(arguments, {}));
    }

    std::optional<uint64_t> CallLibcIo(std::string_view name)
    {
        const auto call = [this, name]<typename Result, typename... Args>(Result, Args... arguments) {
            const auto target = AndroidLibcImport(name, true);
            if (!target)
                Fail("missing ARM64 libc import");
            return reinterpret_cast<Result (*)(Args...)>(target)(arguments...);
        };
        if (name == "getpagesize")
            return uint32_t(call(0));
        if (name == "fchmod")
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32))));
        if (name == "readv" || name == "writev") {
            const auto count = static_cast<int>(Get(2, 32));
            if (count < 0 || count > 1024) {
                errno = EINVAL;
                return uint64_t(-1);
            }
            if (count && !Get(1)) {
                errno = EFAULT;
                return uint64_t(-1);
            }
            std::vector<AndroidIoVector> segments(static_cast<size_t>(count));
            if (count)
                std::memcpy(segments.data(), ReadMemory(Get(1), count * sizeof(AndroidIoVector)),
                            count * sizeof(AndroidIoVector));
            for (auto& vector : segments) {
                if (vector.size > UINT_MAX) {
                    errno = EINVAL;
                    return uint64_t(-1);
                }
                if (!vector.size) {
                    vector.data = nullptr;
                    continue;
                }
                if (!vector.data) {
                    errno = EFAULT;
                    return uint64_t(-1);
                }
                const auto address = reinterpret_cast<uintptr_t>(vector.data);
                vector.data = name == "readv" ? Memory(address, static_cast<unsigned>(vector.size), true)
                                              : ReadMemory(address, static_cast<unsigned>(vector.size));
            }
            return uint64_t(call(intptr_t{}, static_cast<int>(Get(0, 32)),
                                 static_cast<const AndroidIoVector*>(segments.data()), count));
        }
        if (name == "memfd_create") {
            if (!Get(0)) {
                errno = EFAULT;
                return uint32_t(-1);
            }
            const auto label = ReadString(Get(0), 250, false);
            if (label.size() >= 250) {
                errno = EINVAL;
                return uint32_t(-1);
            }
            return uint32_t(call(0, label.c_str(), static_cast<unsigned>(Get(1, 32))));
        }
        if (name == "android_mallopt") {
            errno = 95;
            return 0;
        }
        if (name == "mallopt")
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32))));
        if (name == "getpwuid_r" || name == "getpwnam_r" || name == "getgrgid_r" || name == "getgrnam_r") {
            const auto capacity = Get(3);
            if (capacity > UINT_MAX)
                Fail("ARM64 user database buffer is too large");
            const bool user = name == "getpwuid_r" || name == "getpwnam_r";
            const bool byName = name == "getpwnam_r" || name == "getgrnam_r";
            if (!Get(1) || !Get(4) || (capacity && !Get(2)) || (byName && !Get(0)))
                return uint32_t(EINVAL);
            auto* storage =
                Memory(Get(1), static_cast<unsigned>(user ? sizeof(AndroidPasswd) : sizeof(AndroidGroup)), true);
            auto* strings =
                capacity ? reinterpret_cast<char*>(Memory(Get(2), static_cast<unsigned>(capacity), true)) : nullptr;
            auto* output = Memory(Get(4), sizeof(uintptr_t), true);
            const auto key = byName ? ReadString(Get(0), 1024) : std::string{};
            int error;
            uintptr_t result = 0;
            if (user) {
                AndroidPasswd value{}, *found = nullptr;
                error = byName ? call(0, key.c_str(), &value, strings, size_t{capacity}, &found)
                               : call(0, static_cast<uint32_t>(Get(0, 32)), &value, strings, size_t{capacity}, &found);
                if (found) {
                    std::memcpy(storage, &value, sizeof(value));
                    result = Get(1);
                }
            }
            else {
                AndroidGroup value{}, *found = nullptr;
                error = byName ? call(0, key.c_str(), &value, strings, size_t{capacity}, &found)
                               : call(0, static_cast<uint32_t>(Get(0, 32)), &value, strings, size_t{capacity}, &found);
                if (found) {
                    std::memcpy(storage, &value, sizeof(value));
                    result = Get(1);
                }
            }
            std::memcpy(output, &result, sizeof(result));
            return uint32_t(error);
        }
        if (name == "getaddrinfo" || name == "android_getaddrinfofornet") {
            const bool network = name == "android_getaddrinfofornet";
            const auto destination = Get(network ? 5 : 3);
            if (!destination) {
                errno = EFAULT;
                return 11;
            } // EAI_SYSTEM
            auto* output = Memory(destination, sizeof(uintptr_t), true);
            const auto node = Get(0) ? std::optional{ReadString(Get(0), 1025)} : std::nullopt;
            const auto service = Get(1) ? std::optional{ReadString(Get(1), 1025)} : std::nullopt;
            AndroidAddrInfo hints{};
            if (Get(2))
                std::memcpy(&hints, ReadMemory(Get(2), sizeof(hints)), sizeof(hints));
            AndroidAddrInfo* address = nullptr;
            const auto* host = node ? node->c_str() : nullptr;
            const auto* port = service ? service->c_str() : nullptr;
            const auto* options = Get(2) ? &hints : nullptr;
            int result;
            if (network) {
                const auto resolve =
                    reinterpret_cast<int (*)(const char*, const char*, const AndroidAddrInfo*, unsigned, unsigned,
                                             AndroidAddrInfo**)>(AndroidSocketImport(name, true));
                result = resolve(host, port, options, static_cast<unsigned>(Get(3, 32)),
                                 static_cast<unsigned>(Get(4, 32)), &address);
            }
            else
                result = AndroidGetAddrInfo(host, port, options, &address);
            std::memcpy(output, &address, sizeof(address));
            return uint32_t(result);
        }
        if (name == "freeaddrinfo") {
            AndroidFreeAddrInfo(reinterpret_cast<AndroidAddrInfo*>(Get(0)));
            return 0;
        }
        if (name == "umask")
            return call(0u, static_cast<unsigned>(Get(0, 32)));
        if (name == "__cxa_guard_acquire" || name == "__cxa_guard_release" || name == "__cxa_guard_abort") {
            auto* guard = Memory(Get(0), sizeof(uint64_t), true);
            if (name == "__cxa_guard_acquire")
                return uint32_t(call(0, guard));
            const auto target = reinterpret_cast<void (*)(uint8_t*)>(AndroidLibcImport(name, true));
            target(guard);
            return 0;
        }
        if (name == "gettid")
            return uint32_t(call(DWORD{}));
        if (name == "sched_getcpu") {
            PROCESSOR_NUMBER cpu{};
            GetCurrentProcessorNumberEx(&cpu);
            return unsigned(cpu.Group) * 64 + cpu.Number;
        }
        if (name == "getgrgid_r") {
            const auto id = static_cast<uint32_t>(Get(0, 32));
            const auto result = Get(4);
            Store(result, sizeof(uintptr_t), 0);
            if (id != AndroidApplicationUid())
                return 0;
            const auto groupName = "u" + std::to_string(id / 100000) + "_a" + std::to_string(id % 100000 - 10000);
            const auto storage = Get(2);
            const auto memberListOffset = (8 - storage % 8) % 8;
            const auto nameOffset = memberListOffset + sizeof(uintptr_t);
            const auto bytes = nameOffset + groupName.size() + 2;
            if (Get(3) < bytes)
                return ERANGE;
            auto* output = Memory(storage, static_cast<unsigned>(bytes), true);
            std::memset(output, 0, static_cast<size_t>(bytes));
            std::memcpy(output + nameOffset, groupName.c_str(), groupName.size());
            const std::array<uint64_t, 4> fields{storage + nameOffset, storage + bytes - 1, id,
                                                 storage + memberListOffset};
            std::memcpy(Memory(Get(1), sizeof(fields), true), fields.data(), sizeof(fields));
            Store(result, sizeof(uintptr_t), Get(1));
            return 0;
        }
        if (name == "clock_getres") {
            const auto clock = Get(0, 32);
            int64_t resolution = 100; // CLOCK_REALTIME uses 100 ns Windows FILETIME ticks.
            if (clock != 0) {
                if (clock != 1 && clock != 4 && clock != 7) {
                    errno = EINVAL;
                    return uint32_t(-1);
                }
                LARGE_INTEGER frequency{};
                if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
                    errno = EINVAL;
                    return uint32_t(-1);
                }
                resolution = 1 + 999'999'999 / frequency.QuadPart;
            }
            if (Get(1)) {
                const AndroidTimespec value{resolution / 1'000'000'000, resolution % 1'000'000'000};
                std::memcpy(Memory(Get(1), sizeof(value), true), &value, sizeof(value));
            }
            return 0;
        }
        if (name == "__sched_cpucount") {
            const auto size = Get(0);
            if (size > UINT_MAX)
                Fail("ARM64 CPU set is too large");
            if (!size)
                return 0;
            const auto* bytes = ReadMemory(Get(1), static_cast<unsigned>(size));
            uint32_t count = 0;
            for (uint64_t i = 0; i < size; ++i)
                count += std::popcount(bytes[i]);
            return count;
        }
        if (name == "sched_yield") {
            SwitchToThread();
            return 0;
        }
        if (name == "ALooper_prepare") {
            const auto prepare = reinterpret_cast<void* (*)(int)>(AndroidLooperImport(name, true));
            return reinterpret_cast<uintptr_t>(prepare(static_cast<int>(Get(0, 32))));
        }
        if (name == "ALooper_forThread") {
            const auto current = reinterpret_cast<void* (*)()>(AndroidLooperImport(name, true));
            return reinterpret_cast<uintptr_t>(current());
        }
        if (name == "ALooper_pollOnce" || name == "ALooper_pollAll") {
            const auto poll = reinterpret_cast<int (*)(int, int*, int*, void**)>(AndroidLooperImport(name, true));
            auto* fd = Get(1) ? reinterpret_cast<int*>(Memory(Get(1), sizeof(int), true)) : nullptr;
            auto* events = Get(2) ? reinterpret_cast<int*>(Memory(Get(2), sizeof(int), true)) : nullptr;
            auto** data = Get(3) ? reinterpret_cast<void**>(Memory(Get(3), sizeof(void*), true)) : nullptr;
            return uint32_t(poll(static_cast<int>(Get(0, 32)), fd, events, data));
        }
        if (name == "ALooper_acquire" || name == "ALooper_addFd" || name == "ALooper_removeFd") {
            const auto current = reinterpret_cast<void* (*)()>(AndroidLooperImport("ALooper_forThread", true));
            auto* looper = current();
            if (!looper || reinterpret_cast<uintptr_t>(looper) != Get(0))
                Fail("ARM64 looper handle is not attached to the thread");
            if (name == "ALooper_addFd") {
                if (Get(4) && !CodeImage(Get(4)))
                    Fail("ARM64 looper callback is outside loaded code");
                const auto add =
                    reinterpret_cast<int (*)(void*, int, int, int, uintptr_t, void*)>(AndroidLooperImport(name, true));
                return uint32_t(add(looper, static_cast<int>(Get(1, 32)), static_cast<int>(Get(2, 32)),
                                    static_cast<int>(Get(3, 32)), Get(4), reinterpret_cast<void*>(Get(5))));
            }
            if (name == "ALooper_removeFd") {
                const auto remove = reinterpret_cast<int (*)(void*, int)>(AndroidLooperImport(name, true));
                return uint32_t(remove(looper, static_cast<int>(Get(1, 32))));
            }
            const auto acquire = reinterpret_cast<void (*)(void*)>(AndroidLooperImport(name, true));
            acquire(looper);
            return 0;
        }
        if (name == "eventfd")
            return uint32_t(call(0, static_cast<unsigned>(Get(0, 32)), static_cast<int>(Get(1, 32))));
        if (name == "setpriority")
            return uint32_t(
                call(0, static_cast<int>(Get(0, 32)), static_cast<unsigned>(Get(1, 32)), static_cast<int>(Get(2, 32))));
        if (name == "prctl")
            return uint32_t(AndroidUnsupportedOperation());
        if (name == "timerfd_create")
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32))));
        if (name == "timerfd_settime") {
            constexpr auto size = 2 * sizeof(AndroidTimespec);
            const auto* spec = Get(2) ? ReadMemory(Get(2), size) : nullptr;
            auto* previous = Get(3) ? Memory(Get(3), size, true) : nullptr;
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32)),
                                 static_cast<const void*>(spec), static_cast<void*>(previous)));
        }
        if (name == "epoll_create" || name == "epoll_create1")
            return uint32_t(call(0, static_cast<int>(Get(0, 32))));
        if (name == "epoll_ctl") {
            const auto operation = static_cast<int>(Get(1, 32));
            const auto* event = operation != 2 && Get(3) ? ReadMemory(Get(3), 16) : nullptr;
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), operation, static_cast<int>(Get(2, 32)),
                                 static_cast<const void*>(event)));
        }
        if (name == "epoll_wait") {
            const auto count = static_cast<int>(Get(2, 32));
            if (count <= 0 || count > 4096) {
                errno = EINVAL;
                return uint32_t(-1);
            }
            auto* events = Memory(Get(1), static_cast<unsigned>(count) * 16, true);
            return uint32_t(
                call(0, static_cast<int>(Get(0, 32)), static_cast<void*>(events), count, static_cast<int>(Get(3, 32))));
        }
        if (name == "poll") {
            constexpr unsigned PollFdSize = 8;
            const auto count = Get(1);
            if (count > UINT_MAX / PollFdSize) {
                errno = EINVAL;
                return uint32_t(-1);
            }
            auto* descriptors =
                count && Get(0) ? Memory(Get(0), static_cast<unsigned>(count) * PollFdSize, true) : nullptr;
            return uint32_t(call(0, static_cast<void*>(descriptors), count, static_cast<int>(Get(2, 32))));
        }
        if (name == "__FD_SET_chk" || name == "__FD_CLR_chk" || name == "__FD_ISSET_chk") {
            const auto fd = static_cast<int>(Get(0, 32));
            const auto size = Get(2);
            if (fd < 0 || fd >= AndroidSelectFdLimit || size < sizeof(AndroidFdSet))
                Fail("invalid ARM64 descriptor set");
            const bool test = name == "__FD_ISSET_chk";
            auto* set = reinterpret_cast<AndroidFdSet*>(Memory(Get(1), sizeof(AndroidFdSet), !test));
            if (test)
                return uint32_t(call(0, fd, static_cast<const AndroidFdSet*>(set), size));
            const auto update = reinterpret_cast<void (*)(int, AndroidFdSet*, size_t)>(AndroidLooperImport(name, true));
            update(fd, set, size);
            return 0;
        }
        if (name == "select") {
            const auto count = static_cast<int>(Get(0, 32));
            if (count < 0 || count > AndroidSelectFdLimit) {
                errno = EINVAL;
                return uint32_t(-1);
            }
            const auto set = [&](unsigned index) -> AndroidFdSet* {
                return Get(index) ? reinterpret_cast<AndroidFdSet*>(Memory(Get(index), sizeof(AndroidFdSet), true))
                                  : nullptr;
            };
            auto* timeout =
                Get(4) ? reinterpret_cast<AndroidTimeval*>(Memory(Get(4), sizeof(AndroidTimeval), true)) : nullptr;
            return uint32_t(call(0, count, set(1), set(2), set(3), timeout));
        }
        if (name == "pthread_attr_setstack")
            return 95; // ENOTSUP
        if (name == "fts_open" || name == "fts_read") {
            AndroidUnsupportedOperation();
            return 0;
        }
        if (name == "fts_close" || name == "fts_set")
            return uint32_t(AndroidUnsupportedOperation());
        if (name == "socket" || name == "socketpair") {
            const auto family = static_cast<int>(Get(0, 32)), type = static_cast<int>(Get(1, 32));
            const auto protocol = static_cast<int>(Get(2, 32));
            if (name == "socket")
                return uint32_t(call(0, family, type, protocol));
            int pair[2]{};
            auto* output = Memory(Get(3), sizeof(pair), true);
            const auto result = call(0, family, type, protocol, static_cast<int*>(pair));
            if (!result)
                std::memcpy(output, pair, sizeof(pair));
            return uint32_t(result);
        }
        if (name == "setsockopt" || name == "getsockopt") {
            const auto fd = static_cast<int>(Get(0, 32)), level = static_cast<int>(Get(1, 32));
            const auto option = static_cast<int>(Get(2, 32));
            const auto value = Get(3);
            if (name == "setsockopt") {
                const auto size = static_cast<int>(Get(4, 32));
                const auto* input = value && size > 0 ? ReadMemory(value, static_cast<unsigned>(size)) : nullptr;
                return uint32_t(call(0, fd, level, option, reinterpret_cast<const char*>(input), size));
            }
            const auto length = Get(4);
            auto* lengthOutput = length ? Memory(length, sizeof(int), true) : nullptr;
            int size = 0;
            if (lengthOutput)
                std::memcpy(&size, lengthOutput, sizeof(size));
            auto* output = value && size > 0 ? Memory(value, static_cast<unsigned>(size), true) : nullptr;
            const auto result =
                call(0, fd, level, option, reinterpret_cast<char*>(output), lengthOutput ? &size : nullptr);
            if (!result)
                std::memcpy(lengthOutput, &size, sizeof(size));
            return uint32_t(result);
        }
        if (name == "send" || name == "recv" || name == "sendto" || name == "recvfrom") {
            const auto bytes = Get(2);
            if (bytes > UINT_MAX)
                Fail("ARM64 socket buffer is too large");
            const bool receive = name == "recv" || name == "recvfrom";
            auto* buffer = bytes ? Memory(Get(1), static_cast<unsigned>(bytes), receive) : nullptr;
            const auto fd = static_cast<int>(Get(0, 32)), flags = static_cast<int>(Get(3, 32));
            const auto address = Get(4);
            uint8_t emptyAddress{};
            if (name == "sendto") {
                const auto length = static_cast<uint32_t>(Get(5, 32));
                const auto* input =
                    address ? (length ? ReadMemory(address, std::min(length, 128u)) : &emptyAddress) : nullptr;
                return uint64_t(call(intptr_t{}, fd, static_cast<const void*>(buffer), bytes, flags,
                                     static_cast<const void*>(input), length));
            }
            if (name == "recvfrom") {
                const auto length = address ? Get(5) : 0;
                auto* lengthOutput = length ? Memory(length, sizeof(uint32_t), true) : nullptr;
                uint32_t capacity = 0;
                if (lengthOutput)
                    std::memcpy(&capacity, lengthOutput, sizeof(capacity));
                auto* output =
                    address ? (capacity ? Memory(address, std::min(capacity, 128u), true) : &emptyAddress) : nullptr;
                const auto result = call(intptr_t{}, fd, static_cast<void*>(buffer), bytes, flags,
                                         static_cast<void*>(output), lengthOutput ? &capacity : nullptr);
                if (result >= 0 && lengthOutput)
                    std::memcpy(lengthOutput, &capacity, sizeof(capacity));
                return uint64_t(result);
            }
            return uint64_t(call(intptr_t{}, fd, static_cast<void*>(buffer), bytes, flags));
        }
        if (name == "connect" || name == "bind") {
            const auto length = static_cast<int>(Get(2, 32));
            if (length <= 0 || length > 128) {
                errno = EINVAL;
                return uint32_t(-1);
            }
            const auto* address = ReadMemory(Get(1), static_cast<unsigned>(length));
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<const void*>(address), length));
        }
        if (name == "getsockname" || name == "getpeername" || name == "accept" || name == "accept4") {
            const auto address = Get(1), length = Get(2);
            const bool accept = name == "accept" || name == "accept4";
            if ((!accept && !address) || (address && !length)) {
                errno = EINVAL;
                return uint32_t(-1);
            }
            auto* lengthOutput = address ? Memory(length, sizeof(int), true) : nullptr;
            int capacity = 0;
            if (lengthOutput)
                std::memcpy(&capacity, lengthOutput, sizeof(capacity));
            uint8_t emptyAddress{};
            auto* output = address
                               ? (capacity > 0 ? Memory(address, static_cast<unsigned>(std::min(capacity, 128)), true)
                                               : &emptyAddress)
                               : nullptr;
            const auto fd = static_cast<int>(Get(0, 32));
            auto* capacityArgument = lengthOutput ? &capacity : nullptr;
            const auto result = name == "accept4" ? call(0, fd, static_cast<void*>(output), capacityArgument,
                                                         static_cast<int>(Get(3, 32)))
                                                  : call(0, fd, static_cast<void*>(output), capacityArgument);
            if (result >= 0 && lengthOutput)
                std::memcpy(lengthOutput, &capacity, sizeof(capacity));
            return uint32_t(result);
        }
        if (name == "shutdown" || name == "listen") {
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32))));
        }
        if (name == "sendmmsg")
            return uint32_t(AndroidUnsupportedOperation());
        if (name == "if_indextoname") {
            errno = 19;
            return 0;
        } // ENODEV
        if (name == "mincore")
            return uint32_t(AndroidUnsupportedOperation());
        if (name == "waitpid") {
            errno = ECHILD;
            return uint32_t(-1);
        }
        if (name == "isdigit_l" || name == "islower_l" || name == "isupper_l" || name == "isxdigit_l") {
            const auto ch = Get(0, 32);
            const bool digit = ch >= '0' && ch <= '9';
            if (name == "isdigit_l")
                return digit;
            if (name == "islower_l")
                return ch >= 'a' && ch <= 'z';
            if (name == "isupper_l")
                return ch >= 'A' && ch <= 'Z';
            return digit || ((ch | 32) >= 'a' && (ch | 32) <= 'f');
        }
        if (name == "signal") {
            const auto target = AndroidThreadImport(name, true);
            return reinterpret_cast<uintptr_t (*)(int, uintptr_t)>(target)(static_cast<int>(Get(0, 32)), Get(1));
        }
        if (name == "sysconf")
            return uint64_t(call(int64_t{}, static_cast<int>(Get(0, 32))));
        if (name == "getpriority")
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<unsigned>(Get(1, 32))));
        if (name == "strerror") {
            return CopyGuestConstantString(call(static_cast<const char*>(nullptr), static_cast<int>(Get(0, 32))));
        }
        if (name == "perror") {
            const int saved = errno;
            const auto prefix = Get(0) ? ReadString(Get(0), 1u << 20) : std::string{};
            const auto errorText = reinterpret_cast<const char* (*)(int)>(AndroidLibcImport("strerror", true));
            const auto text = (prefix.empty() ? std::string{} : prefix + ": ") + errorText(saved) + '\n';
            const auto write =
                reinterpret_cast<intptr_t (*)(int, const void*, size_t)>(AndroidLibcImport("write", true));
            write(2, text.data(), text.size());
            errno = saved;
            return 0;
        }
        if (name == "openlog" || name == "closelog" || name == "setlogmask") {
            const auto tag = name == "openlog" ? Get(0) : 0;
            if (tag)
                ReadString(tag, 1u << 20);
            auto& state = SyslogState();
            const std::lock_guard lock(state.mutex);
            if (name == "setlogmask") {
                const auto previous = state.mask;
                if (Get(0, 32))
                    state.mask = static_cast<int>(Get(0, 32));
                return uint32_t(previous);
            }
            state.tag = tag;
            state.options = name == "openlog" ? static_cast<int>(Get(1, 32)) : 0;
            return 0;
        }
        if (name == "syslog" || name == "vsyslog") {
            Syslog(name == "vsyslog");
            return 0;
        }
        if (name == "strerror_r") {
            const auto bytes = Get(2);
            if (!bytes)
                return ERANGE;
            if (bytes > UINT_MAX)
                Fail("invalid ARM64 strerror_r buffer length");
            auto* buffer = reinterpret_cast<char*>(Memory(Get(1), static_cast<unsigned>(bytes), true));
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), buffer, static_cast<size_t>(bytes)));
        }
        if (name == "__gnu_strerror_r") {
            const auto bytes = Get(2);
            if (bytes > UINT_MAX)
                Fail("invalid ARM64 GNU strerror buffer length");
            auto* output =
                bytes ? reinterpret_cast<char*>(Memory(Get(1), static_cast<unsigned>(bytes), true)) : nullptr;
            const auto target = reinterpret_cast<char* (*)(int, char*, size_t)>(AndroidLibcImport(name, true));
            target(static_cast<int>(Get(0, 32)), output, bytes);
            // Bionic's GNU spelling returns the caller buffer even on truncation.
            return Get(1);
        }
        if (name == "open" || name == "__open_2" || name == "openat" || name == "__openat_2") {
            const unsigned index = name.ends_with("at") || name == "__openat_2" ? 1 : 0;
            const auto path = ReadString(Get(index), 32768);
            const auto flags = static_cast<int>(Get(index + 1, 32));
            const int mode = (flags & 0100) ? static_cast<int>(Get(index + 2, 32)) : 0;
            if (name == "__openat_2")
                return uint32_t(call(0, static_cast<int>(Get(0, 32)), path.c_str(), flags));
            if (name == "__open_2")
                return uint32_t(call(0, path.c_str(), flags));
            if (index)
                return uint32_t(call(0, static_cast<int>(Get(0, 32)), path.c_str(), flags, mode));
            return uint32_t(call(0, path.c_str(), flags, mode));
        }
        if (name == "pipe" || name == "pipe2") {
            if (!Get(0)) {
                errno = EFAULT;
                return uint32_t(-1);
            }
            int descriptors[2]{};
            auto* output = Memory(Get(0), sizeof(descriptors), true);
            const auto result = name == "pipe" ? call(0, static_cast<int*>(descriptors))
                                               : call(0, static_cast<int*>(descriptors), static_cast<int>(Get(1, 32)));
            if (!result)
                std::memcpy(output, descriptors, sizeof(descriptors));
            return uint32_t(result);
        }
        if (name == "android_fdsan_create_owner_tag")
            return call(uint64_t{}, static_cast<unsigned>(Get(0, 32)), Get(1));
        if (name == "android_fdsan_get_owner_tag")
            return call(uint64_t{}, static_cast<int>(Get(0, 32)));
        if (name == "android_fdsan_exchange_owner_tag") {
            const auto exchange = reinterpret_cast<void (*)(int, uint64_t, uint64_t)>(AndroidLibcImport(name, true));
            exchange(static_cast<int>(Get(0, 32)), Get(1), Get(2));
            return 0;
        }
        if (name == "android_fdsan_close_with_tag")
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), Get(1)));
        if (name == "android_fdsan_get_error_level")
            return uint32_t(call(0));
        if (name == "android_fdsan_set_error_level")
            return uint32_t(call(0, static_cast<int>(Get(0, 32))));
        const bool sequentialIo = name == "read" || name == "write" || name == "__read_chk" || name == "__write_chk";
        if (sequentialIo || name == "pread" || name == "pwrite" || name == "pread64" || name == "pwrite64" ||
            name == "__pread64_chk" || name == "__pwrite64_chk" || name == "__pread_chk" || name == "__pwrite_chk") {
            const auto size = Get(2);
            if (size > UINT_MAX)
                Fail("ARM64 file buffer is too large");
            const bool checked = name.starts_with("__");
            const bool write =
                name == "write" || name == "__write_chk" || name.starts_with("pwrite") || name.starts_with("__pwrite");
            if (checked && size > Get(sequentialIo ? 3 : 4))
                Fail("ARM64 file buffer overflow");
            auto* data = size ? Memory(Get(1), static_cast<unsigned>(size), !write) : nullptr;
            const int fd = static_cast<int>(Get(0, 32));
            if (sequentialIo) {
                return checked ? uint64_t(call(intptr_t{}, fd, data, size, Get(3)))
                               : uint64_t(call(intptr_t{}, fd, data, size));
            }
            if (checked)
                return uint64_t(call(intptr_t{}, fd, data, size, std::bit_cast<int64_t>(Get(3)), Get(4)));
            return uint64_t(call(intptr_t{}, fd, data, size, std::bit_cast<int64_t>(Get(3))));
        }
        if (name == "readlink" || name == "__readlink_chk") {
            const auto path = ReadString(Get(0), 32768);
            const auto size = Get(2);
            if (size > UINT_MAX || (name == "__readlink_chk" && size > Get(3)))
                Fail("ARM64 readlink buffer overflow");
            auto* output = size ? reinterpret_cast<char*>(Memory(Get(1), static_cast<unsigned>(size), true)) : nullptr;
            return name == "readlink" ? uint64_t(call(intptr_t{}, path.c_str(), output, size))
                                      : uint64_t(call(intptr_t{}, path.c_str(), output, size, Get(3)));
        }
        if (name == "setxattr" || name == "lsetxattr" || name == "fsetxattr") {
            const bool descriptor = name == "fsetxattr";
            const auto size = Get(3);
            if (!Get(1) || (size && !Get(2)) || (!descriptor && !Get(0))) {
                errno = EFAULT;
                return uint32_t(-1);
            }
            if (size > UINT_MAX)
                Fail("ARM64 extended attribute value is too large");
            const auto attribute = ReadString(Get(1), 32768);
            const auto* value = size ? ReadMemory(Get(2), static_cast<unsigned>(size)) : nullptr;
            const auto flags = static_cast<int>(Get(4, 32));
            if (descriptor)
                return uint32_t(call(0, static_cast<int>(Get(0, 32)), attribute.c_str(), value, size, flags));
            const auto path = ReadString(Get(0), 32768);
            return uint32_t(call(0, path.c_str(), attribute.c_str(), value, size, flags));
        }
        if (name == "ftruncate" || name == "ftruncate64")
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), std::bit_cast<int64_t>(Get(1))));
        if (name == "lseek" || name == "lseek64")
            return uint64_t(call(int64_t{}, static_cast<int>(Get(0, 32)), std::bit_cast<int64_t>(Get(1)),
                                 static_cast<int>(Get(2, 32))));
        if (name == "dup")
            return uint32_t(call(0, static_cast<int>(Get(0, 32))));
        if (name == "dup2")
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32))));
        if (name == "dup3")
            return uint32_t(
                call(0, static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32)), static_cast<int>(Get(2, 32))));
        if (name == "qsort") {
            const auto base = Get(0), count = Get(1), size = Get(2), comparison = Get(3);
            if (count < 2 || !size)
                return 0;
            const auto* owner = CodeImage(comparison);
            if (!owner || callbackDepth >= 32 || size > UINT_MAX / count || base > UINTPTR_MAX - count * size)
                Fail("invalid ARM64 qsort arguments");
            auto* elements = Memory(base, static_cast<unsigned>(count * size), true);

            struct SortContext {
                Interpreter& interpreter;
                const ElfImage& owner;
                uintptr_t comparison, guestBase, hostBase;
                std::exception_ptr failure;
            } context{*this, *owner, comparison, base, reinterpret_cast<uintptr_t>(elements), {}};

            qsort_s(
                elements, count, size,
                [](void* data, const void* left, const void* right) -> int {
                    auto& state = *static_cast<SortContext*>(data);
                    if (state.failure)
                        return 0;
                    try {
                        const auto guestLeft = state.guestBase + (reinterpret_cast<uintptr_t>(left) - state.hostBase);
                        const auto guestRight = state.guestBase + (reinterpret_cast<uintptr_t>(right) - state.hostBase);
                        return state.interpreter.CompareArrayElements(state.owner, state.comparison, guestLeft,
                                                                      guestRight);
                    }
                    catch (...) {
                        state.failure = std::current_exception();
                        return 0;
                    }
                },
                &context);
            if (context.failure)
                std::rethrow_exception(context.failure);
            return 0;
        }
        if (name == "bsearch") {
            const auto key = Get(0), base = Get(1), count = Get(2), size = Get(3), comparison = Get(4);
            if (!count || !size)
                return 0;
            const auto* owner = CodeImage(comparison);
            if (!owner || callbackDepth >= 32 || size > UINT64_MAX / count || base > UINTPTR_MAX - count * size)
                Fail("invalid ARM64 bsearch arguments");
            uint64_t first = 0, last = count;
            while (first < last) {
                const auto middle = first + (last - first) / 2;
                const auto element = base + middle * size;
                const auto order = CompareArrayElements(*owner, comparison, key, element);
                if (order == 0)
                    return element;
                if (order < 0)
                    last = middle;
                else
                    first = middle + 1;
            }
            return 0;
        }
        if (name == "close" || name == "fsync" || name == "fdatasync")
            return uint32_t(call(0, static_cast<int>(Get(0, 32))));
        if (name == "__ctype_get_mb_cur_max")
            return arm64ThreadMultibyteMax ? arm64ThreadMultibyteMax
                                           : arm64GlobalMultibyteMax.load(std::memory_order_relaxed);
        if (name == "tzset") {
            reinterpret_cast<void (*)()>(AndroidLibcImport(name, true))();
            return 0;
        }
        if (name == "time") {
            const auto seconds = std::time(nullptr);
            if (Get(0))
                Store(Get(0), sizeof(int64_t), uint64_t(seconds));
            return uint64_t(seconds);
        }
        if (name == "getrandom") {
            const auto size = Get(1);
            const auto flags = static_cast<unsigned>(Get(2, 32));
            if ((flags & ~3u) || size > ULONG_MAX) {
                errno = EINVAL;
                return UINT64_MAX;
            }
            if (size && !Get(0)) {
                errno = EFAULT;
                return UINT64_MAX;
            }
            return uint64_t(
                AndroidGetRandom(size ? Memory(Get(0), static_cast<unsigned>(size), true) : nullptr, size, flags));
        }
        if (name == "arc4random") {
            uint32_t value = 0;
            if (AndroidGetRandom(reinterpret_cast<unsigned char*>(&value), sizeof(value), 0) < 0)
                Fail("ARM64 arc4random could not obtain random bytes");
            return value;
        }
        if (name == "arc4random_buf") {
            const auto size = Get(1);
            if (size > UINT_MAX)
                Fail("ARM64 random buffer is too large");
            if (size && AndroidGetRandom(Memory(Get(0), static_cast<unsigned>(size), true), size, 0) < 0)
                Fail("ARM64 arc4random_buf could not obtain random bytes");
            return 0;
        }
        if (name == "isalnum") {
            const auto ch = Get(0, 32);
            return (ch >= '0' && ch <= '9') || ((ch | 32) >= 'a' && (ch | 32) <= 'z');
        }
        if (name == "getchar")
            return uint32_t(std::fgetc(stdin));
        if (name == "putchar")
            return uint32_t(std::fputc(static_cast<int>(Get(0, 32)), stdout));
        if (name == "fgetc") {
            const auto get = reinterpret_cast<int (*)(void*)>(AndroidLibcImport("getc", true));
            return uint32_t(get(FileHandle(0)));
        }
        if (name == "rewind") {
            const auto seek = reinterpret_cast<int (*)(void*, int64_t, int)>(AndroidLibcImport("fseek", true));
            const auto clear = reinterpret_cast<void (*)(void*)>(AndroidLibcImport("clearerr", true));
            const auto file = FileHandle(0);
            seek(file, 0, SEEK_SET);
            clear(file);
            return 0;
        }
        if (name == "timegm") {
            AndroidTm time{};
            std::memcpy(&time, ReadMemory(Get(0), sizeof(time)), sizeof(time));
            const auto seconds = _mkgmtime64(&time.fields);
            if (seconds == -1) {
                errno = EINVAL;
                return UINT64_MAX;
            }
            time.offset = 0;
            time.zone = reinterpret_cast<const char*>(CopyGuestConstantString("UTC"));
            std::memcpy(Memory(Get(0), sizeof(time), true), &time, sizeof(time));
            return uint64_t(seconds);
        }
        if (name == "mktime") {
            auto* output = Memory(Get(0), sizeof(AndroidTm), true);
            AndroidTm time{};
            std::memcpy(&time, output, sizeof(time));
            const auto seconds = call(int64_t{}, &time);
            if (seconds == -1)
                return UINT64_MAX;
            time.zone = reinterpret_cast<const char*>(CopyGuestConstantString(time.zone));
            std::memcpy(output, &time, sizeof(time));
            return uint64_t(seconds);
        }
        if (name == "srandom" || name == "random") {
            const std::lock_guard lock(arm64RandomMutex);
            if (name == "srandom") {
                arm64Random.seed(static_cast<unsigned>(Get(0, 32)));
                return 0;
            }
            return arm64Random();
        }
        if (name == "clock") {
            FILETIME creation{}, exit{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
                return UINT64_MAX;
            const auto ticks = [](FILETIME t) {
                return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
            };
            return (ticks(kernel) + ticks(user)) / 10;
        }
        if (name == "localtime_r" || name == "gmtime_r" || name == "localtime" || name == "gmtime") {
            const auto seconds = std::bit_cast<int64_t>(Load(Get(0), sizeof(int64_t)));
            AndroidTm value{};
            auto address = Get(1);
            if (name == "gmtime" || name == "localtime") {
                thread_local const GuestThreadBuffer storage(sizeof(AndroidTm));
                address = storage.address;
                const auto convert = reinterpret_cast<AndroidTm* (*)(const int64_t*, AndroidTm*)>(
                    AndroidLibcImport(name == "gmtime" ? "gmtime_r" : "localtime_r", true));
                if (!convert(&seconds, &value))
                    return 0;
            }
            else if (!call(static_cast<AndroidTm*>(nullptr), &seconds, &value)) {
                return 0;
            }
            value.zone = reinterpret_cast<const char*>(CopyGuestConstantString(value.zone));
            std::memcpy(Memory(address, sizeof(value), true), &value, sizeof(value));
            return address;
        }
        if (name == "asctime" || name == "asctime_r" || name == "ctime" || name == "ctime_r") {
            constexpr unsigned size = 26;
            static constexpr const char* days[]{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            static constexpr const char* months[]{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            std::tm time{};
            if (name.starts_with("ctime")) {
                const auto seconds = std::bit_cast<int64_t>(Load(Get(0), sizeof(int64_t)));
                const auto convert = reinterpret_cast<AndroidTm* (*)(const int64_t*, AndroidTm*)>(
                    AndroidLibcImport("localtime_r", true));
                AndroidTm value{};
                if (!convert(&seconds, &value))
                    return 0;
                time = value.fields;
            }
            else {
                std::memcpy(&time, ReadMemory(Get(0), sizeof(AndroidTm)), sizeof(time));
            }
            const auto year = int64_t(time.tm_year) + 1900;
            if (year < 0 || year > 9999 || time.tm_mday < 0 || time.tm_mday > 99 || time.tm_hour < 0 ||
                time.tm_hour > 99 || time.tm_min < 0 || time.tm_min > 99 || time.tm_sec < 0 || time.tm_sec > 99) {
                errno = 75; // EOVERFLOW
                return 0;
            }
            auto address = Get(1);
            if (!name.ends_with("_r")) {
                thread_local const GuestThreadBuffer storage(size);
                address = storage.address;
            }
            const auto day = unsigned(time.tm_wday) < std::size(days) ? days[time.tm_wday] : "???";
            const auto month = unsigned(time.tm_mon) < std::size(months) ? months[time.tm_mon] : "???";
            auto* output = reinterpret_cast<char*>(Memory(address, size, true));
            std::snprintf(output, size, "%s %s %2d %02d:%02d:%02d %04d\n", day, month, time.tm_mday, time.tm_hour,
                          time.tm_min, time.tm_sec, int(year));
            return address;
        }
        if (name == "inet_aton" || name == "inet_addr") {
            const auto input = ReadString(Get(0), 128);
            if (name == "inet_addr")
                return call(uint32_t{}, input.c_str());
            uint32_t address = 0;
            const auto result = call(0, input.c_str(), &address);
            if (result && Get(1))
                Store(Get(1), sizeof(address), address);
            return uint32_t(result);
        }
        if (name == "inet_ntoa") {
            constexpr unsigned size = 16;
            thread_local const GuestThreadBuffer storage(size);
            const auto address = static_cast<uint32_t>(Get(0, 32));
            const auto convert = reinterpret_cast<const char* (*)(int, const void*, char*, size_t)>(
                AndroidLibcImport("inet_ntop", true));
            auto* output = reinterpret_cast<char*>(Memory(storage.address, size, true));
            return convert(2, &address, output, size) ? storage.address : 0;
        }
        if (name == "inet_pton" || name == "inet_ntop") {
            const auto family = static_cast<int>(Get(0, 32));
            const unsigned length = family == 2 ? 4 : family == 10 ? 16 : 0;
            if (!length) {
                errno = 97;
                return name == "inet_pton" ? uint32_t(-1) : 0;
            }
            if (name == "inet_pton") {
                const auto input = ReadString(Get(1), 128);
                auto* target = Memory(Get(2), length, true);
                std::array<uint8_t, 16> address{};
                const auto result = call(0, family, input.c_str(), static_cast<void*>(address.data()));
                if (result == 1)
                    std::memcpy(target, address.data(), length);
                return uint32_t(result);
            }
            const auto size = Get(3, 32);
            const auto* address = ReadMemory(Get(1), length);
            auto* target = size ? reinterpret_cast<char*>(Memory(Get(2), static_cast<unsigned>(size), true)) : nullptr;
            if (!target) {
                errno = ENOSPC;
                return 0;
            }
            return reinterpret_cast<uintptr_t>(call(static_cast<const char*>(nullptr), family,
                                                    static_cast<const void*>(address), target,
                                                    static_cast<size_t>(size)));
        }
        if (name == "ioctl") {
            const auto request = static_cast<unsigned long>(Get(1, 32));
            if (request != 0x541b) {
                errno = ENOTTY;
                return uint32_t(-1);
            }
            auto* value = Get(2) ? reinterpret_cast<int*>(Memory(Get(2), sizeof(int), true)) : nullptr;
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), request, value));
        }
        if (name == "chown") {
            ReadString(Get(0), 32768);
            errno = EPERM;
            return uint32_t(-1);
        }
        if (name == "popen") {
            AndroidUnsupportedOperation();
            return 0;
        }
        if (name == "pclose")
            return uint32_t(AndroidUnsupportedOperation());
        if (name == "system")
            return uint32_t(AndroidUnsupportedOperation());
        if (name == "tmpfile" || name == "tmpnam" || name == "freopen") {
            AndroidUnsupportedOperation();
            return 0;
        }
        if (name == "recvmmsg")
            return uint32_t(AndroidUnsupportedOperation());
        if (name == "tcgetattr")
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<void*>(nullptr)));
        if (name == "tcsetattr")
            return uint32_t(
                call(0, static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32)), static_cast<const void*>(nullptr)));
        if (name == "strftime") {
            const auto size = Get(1);
            if (!size || size > UINT_MAX)
                return 0;
            const auto format = ReadString(Get(2), 65536);
            AndroidTm value{};
            std::memcpy(&value, ReadMemory(Get(3), sizeof(value)), sizeof(value));
            auto* output = reinterpret_cast<char*>(Memory(Get(0), static_cast<unsigned>(size), true));
            return call(size_t{}, output, size, format.c_str(), static_cast<const AndroidTm*>(&value));
        }
        if (name == "strptime") {
            const auto input = ReadString(Get(0), 1u << 20), format = ReadString(Get(1), 65536);
            AndroidTm value{};
            std::memcpy(&value, ReadMemory(Get(2), sizeof(value)), sizeof(value));
            const auto* end = ParseAndroidTime(input.c_str(), format.c_str(), &value);
            if (!end)
                return 0;
            std::memcpy(Memory(Get(2), sizeof(value), true), &value, sizeof(value));
            return Get(0) + static_cast<uint64_t>(end - input.c_str());
        }
        if (name == "gettimeofday") {
            auto* value =
                Get(0) ? reinterpret_cast<AndroidTimespec*>(Memory(Get(0), sizeof(AndroidTimespec), true)) : nullptr;
            auto* zone = Get(1) ? static_cast<void*>(Memory(Get(1), 2 * sizeof(int), true)) : nullptr;
            return uint32_t(call(0, value, zone));
        }
        if (name == "mbtowc") {
            const auto source = Get(1), available = Get(2);
            if (!source)
                return 0;
            uint32_t codePoint = 0;
            unsigned size = 0;
            if (available) {
                const auto first = Load(source, 1);
                if (first < 0x80) {
                    codePoint = static_cast<uint32_t>(first);
                    size = 1;
                }
                else {
                    size = first < 0xc2 ? 0 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : first < 0xf5 ? 4 : 0;
                    if (size && size <= available) {
                        char utf8[4]{static_cast<char>(first)};
                        for (unsigned i = 1; i < size; ++i) {
                            utf8[i] = static_cast<char>(Load(source + i, 1));
                            if ((utf8[i] & 0xc0) != 0x80) {
                                size = 0;
                                break;
                            }
                        }
                        wchar_t utf16[2]{};
                        if (size) {
                            const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, utf16, 2);
                            if (count == 1)
                                codePoint = utf16[0];
                            else if (count == 2)
                                codePoint = 0x10000u + ((utf16[0] - 0xd800u) << 10) + (utf16[1] - 0xdc00u);
                            else
                                size = 0;
                        }
                    }
                    else
                        size = 0;
                }
            }
            if (!size) {
                errno = 84;
                return uint32_t(-1);
            } // Bionic EILSEQ
            if (Get(0))
                Store(Get(0), sizeof(uint32_t), codePoint);
            return codePoint ? size : 0;
        }
        if (name == "wcrtomb") {
            if (!Get(0))
                return 1;
            const auto codePoint = static_cast<uint32_t>(Get(1, 32));
            if ((codePoint >= 0xd800 && codePoint <= 0xdfff) || codePoint > 0x10ffff) {
                errno = 84; // Bionic EILSEQ
                return uint64_t(-1);
            }
            wchar_t utf16[2]{};
            const int units = codePoint < 0x10000 ? 1 : 2;
            if (units == 1)
                utf16[0] = static_cast<wchar_t>(codePoint);
            else {
                const auto pair = codePoint - 0x10000;
                utf16[0] = static_cast<wchar_t>(0xd800 + (pair >> 10));
                utf16[1] = static_cast<wchar_t>(0xdc00 + (pair & 1023));
            }
            char utf8[4];
            const auto size =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16, units, utf8, sizeof(utf8), nullptr, nullptr);
            if (!size) {
                errno = 84;
                return uint64_t(-1);
            }
            std::memcpy(Memory(Get(0), size, true), utf8, size);
            return static_cast<unsigned>(size);
        }
        if (name == "srand") {
            std::srand(static_cast<unsigned>(Get(0, 32)));
            return 0;
        }
        if (name == "rand")
            return uint32_t(std::rand());
        if (name == "flock")
            return uint32_t(call(0, static_cast<int>(Get(0, 32)), static_cast<int>(Get(1, 32))));
        if (name == "fcntl") {
            const auto fd = static_cast<int>(Get(0, 32)), command = static_cast<int>(Get(1, 32));
            const auto argument = Get(2);
            if (command >= 5 && command <= 7) {
                if (!argument) {
                    errno = EFAULT;
                    return uint32_t(-1);
                }
                AndroidFileRange range{};
                std::memcpy(&range, ReadMemory(argument, sizeof(range)), sizeof(range));
                auto* output = command == 5 ? Memory(argument, sizeof(range), true) : nullptr;
                const auto result = call(0, fd, command, reinterpret_cast<uintptr_t>(&range));
                if (!result && output)
                    std::memcpy(output, &range, sizeof(range));
                return uint32_t(result);
            }
            return uint32_t(call(0, fd, command, static_cast<uintptr_t>(argument)));
        }
        if (name == "mmap" || name == "mmap64")
            return uint64_t(MapMemory(Get(0), Get(1), static_cast<int>(Get(2, 32)), static_cast<int>(Get(3, 32)),
                                      static_cast<int>(Get(4, 32)), std::bit_cast<int64_t>(Get(5))));
        if (name == "munmap")
            return uint32_t(AndroidUnmapArm64(reinterpret_cast<void*>(Get(0)), Get(1)));
        if (name == "mremap") {
            // Match the native Windows bridge: resizing Linux mappings is not
            // supported. Let callers use their mmap/copy fallback.
            errno = 38; // Bionic ENOSYS.
            return uint64_t(-1);
        }
        if (name == "mprotect" || name == "msync") {
            const auto flags = static_cast<int>(Get(2, 32));
            return uint32_t(name == "mprotect" ? ProtectMemory(Get(0), Get(1), flags)
                                               : AndroidSyncArm64(reinterpret_cast<void*>(Get(0)), Get(1), flags));
        }
        if (name == "madvise")
            return uint32_t(AndroidAdviseArm64(reinterpret_cast<void*>(Get(0)), Get(1), static_cast<int>(Get(2, 32))));
        if (name == "mkstemp" || name == "mkstemps" || name == "mkdtemp") {
            const auto path = ReadString(Get(0), 32768);
            auto* output = reinterpret_cast<char*>(Memory(Get(0), static_cast<unsigned>(path.size() + 1), true));
            if (name == "mkstemp")
                return uint32_t(call(0, output));
            if (name == "mkstemps")
                return uint32_t(call(0, output, static_cast<int>(Get(1, 32))));
            return reinterpret_cast<uintptr_t>(call(static_cast<char*>(nullptr), output));
        }
        if (name == "mkfifo" || name == "mknod") {
            const auto path = ReadString(Get(0), 32768);
            const auto mode = static_cast<unsigned>(Get(1, 32));
            return name == "mknod" ? uint32_t(call(0, path.c_str(), mode, Get(2)))
                                   : uint32_t(call(0, path.c_str(), mode));
        }
        if (name == "chmod" || name == "utime") {
            const auto path = ReadString(Get(0), 32768);
            if (name == "chmod")
                return uint32_t(call(0, path.c_str(), static_cast<int>(Get(1, 32))));
            AndroidUtime times{};
            if (Get(1))
                std::memcpy(&times, ReadMemory(Get(1), sizeof(times)), sizeof(times));
            return uint32_t(call(0, path.c_str(), Get(1) ? static_cast<const AndroidUtime*>(&times) : nullptr));
        }
        if (name == "setlocale") {
            // Bionic uses one process-wide locale for all LC_* categories.
            if (Get(0, 32) > 12) {
                errno = EINVAL;
                return 0;
            }
            if (Get(1)) {
                const auto locale = ReadString(Get(1), 256);
                unsigned width;
                if (locale == "C" || locale == "POSIX")
                    width = 1;
                else if (locale.empty() || locale == "C.UTF-8" || locale == "en_US.UTF-8")
                    width = 4;
                else {
                    errno = ENOENT;
                    return 0;
                }
                arm64GlobalMultibyteMax.store(width, std::memory_order_relaxed);
            }
            return CopyGuestConstantString(arm64GlobalMultibyteMax.load(std::memory_order_relaxed) == 4 ? "C.UTF-8"
                                                                                                        : "C");
        }
        if (name == "newlocale" || name == "freelocale" || name == "uselocale") {
            auto& locales = Locales();
            const bool create = name == "newlocale";
            const auto text = create ? ReadString(Get(1), 256) : std::string{};
            const auto handle = reinterpret_cast<_locale_t>(Get(create ? 2 : 0));
            const auto globalLocale = reinterpret_cast<_locale_t>(-1);
            const std::lock_guard lock(locales.mutex);
            if (handle && !(name == "uselocale" && handle == globalLocale) && !locales.open.contains(handle))
                Fail("invalid ARM64 locale handle");
            if (create) {
                const auto locale = call(_locale_t{}, static_cast<int>(Get(0, 32)), text.c_str(), handle);
                if (locale) {
                    if (handle)
                        locales.open.erase(handle);
                    const unsigned multibyteMax = text == "C" || text == "POSIX" ? 1 : 4;
                    try {
                        locales.open.emplace(locale, multibyteMax);
                    }
                    catch (...) {
                        _free_locale(locale);
                        throw;
                    }
                }
                return reinterpret_cast<uintptr_t>(locale);
            }
            if (name == "uselocale") {
                const auto previous = call(_locale_t{}, handle);
                if (handle)
                    arm64ThreadMultibyteMax = handle == globalLocale ? 0 : locales.open.at(handle);
                return reinterpret_cast<uintptr_t>(previous);
            }
            if (handle) {
                locales.open.erase(handle);
                _free_locale(handle);
            }
            return 0;
        }
        if (name == "fopen" || name == "fdopen") {
            const auto mode = ReadString(Get(1), 32);
            auto* file = name == "fdopen"
                             ? call(static_cast<FILE*>(nullptr), static_cast<int>(Get(0, 32)), mode.c_str())
                             : call(static_cast<FILE*>(nullptr), ReadString(Get(0), 32768).c_str(), mode.c_str());
            if (file) {
                auto& files = Files();
                const std::lock_guard lock(files.mutex);
                try {
                    files.open.insert(file);
                }
                catch (...) {
                    const auto close = reinterpret_cast<int (*)(void*)>(AndroidLibcImport("fclose", true));
                    close(file);
                    throw;
                }
            }
            return reinterpret_cast<uintptr_t>(file);
        }
        if (name == "fclose" || name == "ferror" || name == "feof" || name == "fflush" || name == "fileno" ||
            name == "getc")
            return uint32_t(call(0, FileHandle(0, name == "fflush", name == "fclose")));
        if (name == "putc" || name == "fputc") {
            const auto target = AndroidLibcImport("fputc", true);
            return uint32_t(reinterpret_cast<int (*)(int, void*)>(target)(static_cast<int>(Get(0, 32)), FileHandle(1)));
        }
        if (name == "setvbuf") {
            if (Get(1))
                return uint32_t(AndroidUnsupportedOperation());
            const int modes[]{_IOFBF, _IOLBF, _IONBF};
            const auto mode = Get(2, 32);
            if (mode >= std::size(modes)) {
                errno = EINVAL;
                return uint32_t(-1);
            }
            const auto target = AndroidLibcImport("setvbuf", true);
            return uint32_t(reinterpret_cast<int (*)(void*, int, size_t)>(target)(FileHandle(0), modes[mode], Get(3)));
        }
        if (name == "fgetpos" || name == "fsetpos") {
            if (name == "fsetpos") {
                const auto target = AndroidLibcImport("fseek", true);
                const auto position = std::bit_cast<int64_t>(Load(Get(1), sizeof(int64_t)));
                return uint32_t(
                    reinterpret_cast<int (*)(void*, int64_t, int)>(target)(FileHandle(0), position, SEEK_SET));
            }
            const auto target = AndroidLibcImport("ftell", true);
            const auto position = reinterpret_cast<int64_t (*)(void*)>(target)(FileHandle(0));
            if (position < 0)
                return uint32_t(-1);
            Store(Get(1), sizeof(position), static_cast<uint64_t>(position));
            return 0;
        }
        if (name == "fseeko64" || name == "ftello64") {
            const auto target = AndroidLibcImport(name == "fseeko64" ? "fseeko" : "ftello", true);
            if (name == "ftello64")
                return uint64_t(reinterpret_cast<int64_t (*)(void*)>(target)(FileHandle(0)));
            return uint32_t(reinterpret_cast<int (*)(void*, int64_t, int)>(target)(
                FileHandle(0), std::bit_cast<int64_t>(Get(1)), static_cast<int>(Get(2, 32))));
        }
        if (name == "fseek" || name == "fseeko")
            return uint32_t(call(0, FileHandle(0), std::bit_cast<int64_t>(Get(1)), static_cast<int>(Get(2, 32))));
        if (name == "ftell" || name == "ftello")
            return uint64_t(call(int64_t{}, FileHandle(0)));
        if (name == "fgets" || name == "__fgets_chk") {
            const auto size = static_cast<int>(Get(1, 32));
            if (name == "__fgets_chk" && (size < 0 || static_cast<uint64_t>(size) > Get(3)))
                Fail("ARM64 fgets destination overflow");
            if (size <= 0)
                return 0;
            auto* output = reinterpret_cast<char*>(Memory(Get(0), static_cast<unsigned>(size), true));
            const auto read = reinterpret_cast<char* (*)(char*, int, void*)>(AndroidLibcImport("fgets", true));
            return reinterpret_cast<uintptr_t>(read(output, size, FileHandle(2)));
        }
        if (name == "fread" || name == "fwrite" || name == "__fread_chk") {
            const auto size = Get(1), count = Get(2);
            if (!size || !count)
                return 0;
            if (count > UINT_MAX / size)
                Fail("ARM64 stdio buffer is too large");
            const auto bytes = static_cast<unsigned>(size * count);
            auto* file = FileHandle(3);
            if (name == "__fread_chk") {
                if (bytes > Get(4))
                    Fail("ARM64 fread destination overflow");
                const auto read =
                    reinterpret_cast<size_t (*)(void*, size_t, size_t, void*)>(AndroidLibcImport("fread", true));
                return read(Memory(Get(0), bytes, true), size, count, file);
            }
            if (name == "fread")
                return call(size_t{}, static_cast<void*>(Memory(Get(0), bytes, true)), size, count, file);
            return call(size_t{}, static_cast<const void*>(ReadMemory(Get(0), bytes)), size, count, file);
        }
        if (name == "fputs") {
            const auto text = ReadString(Get(0), 1u << 20);
            return uint32_t(call(0, text.c_str(), FileHandle(1)));
        }
        if (name == "fprintf" || name == "vfprintf" || name == "vprintf" || name == "printf") {
            const bool standardOutput = name == "printf" || name == "vprintf";
            const unsigned format = standardOutput ? 0 : 1;
            const auto file = standardOutput ? static_cast<void*>(stdout) : FileHandle(0);
            std::function<uint64_t(bool)> next;
            if (name == "fprintf" || name == "printf")
                next = RegisterArguments(format + 1);
            else
                next = VariadicArguments(Get(format + 1));
            auto measure = next;
            const auto size = Format(nullptr, 0, Get(format), measure);
            if (size < 0 || size > (1 << 20))
                return uint32_t(-1);
            std::vector<char> output(static_cast<size_t>(size) + 1);
            if (Format(output.data(), output.size(), Get(format), next) != size)
                return uint32_t(-1);
            const auto write =
                reinterpret_cast<size_t (*)(const void*, size_t, size_t, void*)>(AndroidLibcImport("fwrite", true));
            return write(output.data(), 1, size, file) == static_cast<size_t>(size) ? uint32_t(size) : uint32_t(-1);
        }
        if (name == "access") {
            const auto path = ReadString(Get(0), 32768);
            return uint32_t(call(0, path.c_str(), static_cast<int>(Get(1, 32))));
        }
        if (name == "pathconf") {
            const auto path = ReadString(Get(0), 32768);
            return uint64_t(call(int64_t{}, path.c_str(), static_cast<int>(Get(1, 32))));
        }
        if (name == "realpath") {
            const auto path = ReadString(Get(0), 32768);
            char resolved[4096];
            if (!call(static_cast<char*>(nullptr), path.c_str(), static_cast<char*>(resolved)))
                return 0;
            const auto guestPath = AndroidGuestFilePath(resolved);
            const auto size = guestPath.size() + 1;
            if (size > sizeof(resolved)) {
                errno = AndroidNameTooLong;
                return 0;
            }
            auto output = Get(1);
            if (!output) {
                output = Arm64GuestHeap().Allocate(size, false);
                if (!output) {
                    errno = ENOMEM;
                    return 0;
                }
            }
            std::memcpy(Memory(output, static_cast<unsigned>(size), true), guestPath.c_str(), size);
            return output;
        }
        if (name == "stat" || name == "lstat" || name == "stat64" || name == "lstat64" || name == "fstat" ||
            name == "fstat64" || name == "fstatat" || name == "fstatat64") {
            const bool relative = name.starts_with("fstatat"), fd = !relative && name.starts_with("fstat");
            auto* output = Memory(Get(relative ? 2 : 1), sizeof(AndroidArm64Stat), true);
            AndroidArm64Stat status{};
            int result;
            if (fd)
                result = call(0, static_cast<int>(Get(0, 32)), &status);
            else {
                const auto path = ReadString(Get(relative ? 1 : 0), 32768);
                if (relative)
                    result = call(0, static_cast<int>(Get(0, 32)), path.c_str(), &status, static_cast<int>(Get(3, 32)));
                else
                    result = call(0, path.c_str(), &status);
            }
            if (!result)
                std::memcpy(output, &status, sizeof(status));
            return uint32_t(result);
        }
        if (name == "statfs" || name == "statfs64" || name == "fstatfs" || name == "fstatfs64") {
            const bool fd = name.starts_with("fstatfs");
            if (!Get(1) || (!fd && !Get(0))) {
                errno = EFAULT;
                return uint32_t(-1);
            }
            auto* output = Memory(Get(1), sizeof(AndroidStatFs), true);
            AndroidStatFs status{};
            const int result = fd ? call(0, static_cast<int>(Get(0, 32)), &status)
                                  : call(0, ReadString(Get(0), 32768).c_str(), &status);
            if (!result)
                std::memcpy(output, &status, sizeof(status));
            return uint32_t(result);
        }
        if (name == "statvfs" || name == "statvfs64" || name == "fstatvfs" || name == "fstatvfs64") {
            const bool fd = name.starts_with("fstatvfs");
            if (!Get(1) || (!fd && !Get(0))) {
                errno = EFAULT;
                return uint32_t(-1);
            }
            auto* output = Memory(Get(1), sizeof(AndroidStatVfs), true);
            AndroidStatVfs status{};
            const int result = fd ? call(0, static_cast<int>(Get(0, 32)), &status)
                                  : call(0, ReadString(Get(0), 32768).c_str(), &status);
            if (!result)
                std::memcpy(output, &status, sizeof(status));
            return uint32_t(result);
        }
        if (name == "opendir") {
            if (!Get(0)) {
                errno = EFAULT;
                return 0;
            }
            const auto path = ReadString(Get(0), 32768);
            auto& directories = Directories();
            auto* directory = call(static_cast<void*>(nullptr), path.c_str());
            if (!directory)
                return 0;
            using Close = int (*)(void*);
            const auto close = reinterpret_cast<Close>(AndroidLibcImport("closedir", true));
            std::unique_ptr<void, Close> host(directory, close);
            // DIR is opaque. Keep the entry separate so each stream owns a
            // guest-visible buffer until the next read or close.
            uintptr_t buffer = 0;
            try {
                buffer = Arm64GuestHeap().Allocate(sizeof(AndroidDirent), false);
                if (!buffer)
                    throw std::bad_alloc();
                const std::lock_guard lock(directories.mutex);
                directories.open.emplace(directory, buffer);
                return reinterpret_cast<uintptr_t>(host.release());
            }
            catch (const std::bad_alloc&) {
                Arm64GuestHeap().Free(buffer);
                host.reset();
                errno = ENOMEM;
                return 0;
            }
        }
        if (name == "readdir" || name == "readdir64" || name == "closedir") {
            auto& directories = Directories();
            const std::lock_guard lock(directories.mutex);
            const auto handle = reinterpret_cast<void*>(Get(0));
            const auto entry = directories.open.find(handle);
            if (entry == directories.open.end()) {
                errno = EBADF;
                return name == "closedir" ? uint32_t(-1) : 0;
            }
            if (name == "closedir") {
                const auto result = call(0, handle);
                const int saved = errno;
                Arm64GuestHeap().Free(entry->second);
                directories.open.erase(entry);
                errno = saved;
                return uint32_t(result);
            }
            const auto* record = call(static_cast<AndroidDirent*>(nullptr), handle);
            if (!record)
                return 0;
            std::memcpy(Memory(entry->second, sizeof(*record), true), record, sizeof(*record));
            return entry->second;
        }
        if (name == "mkdir" || name == "mkdirat" || name == "remove" || name == "unlink" || name == "rmdir") {
            const auto path = ReadString(Get(name == "mkdirat" ? 1 : 0), 32768);
            if (name == "mkdirat")
                return uint32_t(call(0, static_cast<int>(Get(0, 32)), path.c_str(), static_cast<int>(Get(2, 32))));
            return uint32_t(call(0, path.c_str()));
        }
        if (name == "getcwd") {
            const auto bytes = Get(1);
            char resolved[32768];
            if (!call(static_cast<char*>(nullptr), static_cast<char*>(resolved), sizeof(resolved)))
                return 0;
            const auto guestPath = AndroidGuestFilePath(resolved);
            const auto size = guestPath.size() + 1;
            auto output = Get(0);
            if ((output || bytes) && bytes < size) {
                errno = ERANGE;
                return 0;
            }
            if (!output) {
                output = Arm64GuestHeap().Allocate(bytes ? bytes : size, false);
                if (!output) {
                    errno = ENOMEM;
                    return 0;
                }
            }
            std::memcpy(Memory(output, static_cast<unsigned>(size), true), guestPath.c_str(), size);
            return output;
        }
        if (name == "rename") {
            const auto from = ReadString(Get(0), 32768), to = ReadString(Get(1), 32768);
            return uint32_t(call(0, from.c_str(), to.c_str()));
        }
        if (name == "sleep")
            return call(0u, static_cast<unsigned>(Get(0, 32)));
        if (name == "usleep")
            return uint32_t(call(0, static_cast<unsigned>(Get(0, 32))));
        if (name == "nanosleep") {
            const auto* request = reinterpret_cast<const AndroidTimespec*>(ReadMemory(Get(0), sizeof(AndroidTimespec)));
            auto* remainder =
                Get(1) ? reinterpret_cast<AndroidTimespec*>(Memory(Get(1), sizeof(AndroidTimespec), true)) : nullptr;
            return uint32_t(call(0, request, remainder));
        }
        if (name == "clock_nanosleep") {
            const auto clock = static_cast<int>(Get(0, 32)), flags = static_cast<int>(Get(1, 32));
            const auto* request = reinterpret_cast<const AndroidTimespec*>(ReadMemory(Get(2), sizeof(AndroidTimespec)));
            auto* remainder = Get(3) && !(flags & 1)
                                  ? reinterpret_cast<AndroidTimespec*>(Memory(Get(3), sizeof(AndroidTimespec), true))
                                  : nullptr;
            return uint32_t(call(0, clock, flags, request, remainder));
        }
        return std::nullopt;
    }

    uint64_t CallSensorImport(std::string_view name)
    {
        const auto target = AndroidSensorImport(name, true);
        if (!target)
            Fail("unsupported ARM64 sensor import " + std::string(name));
        const auto call = [target]<typename Result, typename... Args>(Result, Args... args) {
            return reinterpret_cast<Result (*)(Args...)>(target)(args...);
        };
        auto* const first = reinterpret_cast<void*>(Get(0));
        auto* const second = reinterpret_cast<void*>(Get(1));
        if (name == "ASensorManager_getInstance")
            return reinterpret_cast<uintptr_t>(call(static_cast<void*>(nullptr)));
        if (name == "ASensorManager_getDefaultSensor")
            return reinterpret_cast<uintptr_t>(call(static_cast<void*>(nullptr), first, int32_t(Get(1, 32))));
        if (name == "ASensorManager_getSensorList") {
            auto* list =
                first && second ? reinterpret_cast<const void***>(Memory(Get(1), sizeof(uintptr_t), true)) : nullptr;
            return uint32_t(call(0, first, list));
        }
        if (name == "ASensorManager_createEventQueue")
            return reinterpret_cast<uintptr_t>(call(static_cast<void*>(nullptr), first, second, int32_t(Get(2, 32)),
                                                    reinterpret_cast<void*>(Get(3)), reinterpret_cast<void*>(Get(4))));
        if (name == "ASensorEventQueue_getEvents")
            return uint64_t(call(intptr_t{0}, first, second, size_t(Get(2))));
        if (name == "ASensorEventQueue_setEventRate")
            return uint32_t(call(0, first, static_cast<const void*>(second), int32_t(Get(2, 32))));
        if (name == "ASensorEventQueue_hasEvents")
            return uint32_t(call(0, first));
        if (name == "ASensor_getName" || name == "ASensor_getVendor")
            return reinterpret_cast<uintptr_t>(
                call(static_cast<const char*>(nullptr), static_cast<const void*>(first)));
        if (name == "ASensor_getResolution") {
            importFloatResult = std::bit_cast<uint32_t>(call(0.0f, static_cast<const void*>(first)));
            return 0;
        }
        if (name == "ASensor_getType" || name == "ASensor_getMinDelay")
            return uint32_t(call(0, static_cast<const void*>(first)));
        if (name == "ASensorManager_destroyEventQueue" || name == "ASensorEventQueue_disableSensor" ||
            name == "ASensorEventQueue_enableSensor")
            return uint32_t(call(0, first, static_cast<const void*>(second)));
        Fail("unsupported ARM64 sensor import " + std::string(name));
    }

    uint64_t CallConfigurationImport(std::string_view name)
    {
        if (!jni)
            Fail("ARM64 configuration import needs the Android runtime");
        if (name == "AConfiguration_fromAssetManager" && !jni->assets.configuration)
            Fail("ARM64 ART asset configuration is not implemented");
        auto& state = Assets();
        if (name == "AConfiguration_new") {
            try {
                auto configuration = std::make_unique<Arm64Configuration>();
                const auto handle = reinterpret_cast<uintptr_t>(configuration.get());
                const std::lock_guard lock(state.mutex);
                state.configurations.emplace(handle, std::move(configuration));
                return handle;
            }
            catch (const std::bad_alloc&) {
                return 0;
            }
        }
        const auto handle = Get(0);
        std::unique_lock lock(state.mutex);
        if (name == "AConfiguration_delete" && !handle)
            return 0;
        auto configuration = state.configurations.find(handle);
        if (configuration == state.configurations.end())
            Fail("invalid ARM64 configuration handle");
        if (name == "AConfiguration_delete") {
            state.configurations.erase(configuration);
            return 0;
        }
        if (name == "AConfiguration_fromAssetManager") {
            const auto manager = reinterpret_cast<jobject>(Get(1));
            if (!state.managers.contains(manager))
                Fail("invalid ARM64 configuration asset manager");
            lock.unlock();
            Arm64Configuration value{};
            if (!jni->assets.configuration(manager, value))
                Fail("could not read ARM64 asset configuration");
            lock.lock();
            configuration = state.configurations.find(handle);
            if (configuration == state.configurations.end())
                Fail("invalid ARM64 configuration handle");
            *configuration->second = value;
            return 0;
        }
        const auto& value = *configuration->second;
        if (name == "AConfiguration_getDensity")
            return uint32_t(value.density);
        if (name == "AConfiguration_getScreenWidthDp")
            return uint32_t(value.widthDp);
        if (name == "AConfiguration_getScreenHeightDp")
            return uint32_t(value.heightDp);
        Fail("unsupported ARM64 configuration import " + std::string(name));
    }

    uint64_t CallAssetImport(std::string_view name)
    {
        if (!jni || !jni->assets.manager)
            Fail("ARM64 ART asset imports are not implemented");
        auto& state = Assets();
        const auto& assets = jni->assets;
        if (name == "AAssetManager_fromJava") {
            const auto object = reinterpret_cast<jobject>(Get(1));
            if (!jniEnv || Get(0) != guestEnvAddress || !KnownReference(object))
                Fail("invalid ARM64 asset manager reference");
            const auto manager = assets.manager(jniEnv, object);
            if (manager) {
                const std::lock_guard lock(state.mutex);
                state.managers.insert(manager);
            }
            return reinterpret_cast<uintptr_t>(manager);
        }
        if (name == "AAssetManager_open" || name == "AAssetManager_openDir") {
            const auto manager = reinterpret_cast<jobject>(Get(0));
            {
                const std::lock_guard lock(state.mutex);
                if (!state.managers.contains(manager))
                    Fail("invalid ARM64 asset manager handle");
            }
            const auto path = ReadString(Get(1), 32768);
            if (name == "AAssetManager_openDir") {
                const auto directory = assets.openDirectory(manager, path.c_str());
                if (directory) {
                    const std::lock_guard lock(state.mutex);
                    try {
                        state.directories.try_emplace(directory, 0);
                    }
                    catch (...) {
                        assets.closeDirectory(directory);
                        throw;
                    }
                }
                return reinterpret_cast<uintptr_t>(directory);
            }
            const auto asset = assets.open(manager, path.c_str(), static_cast<int>(Get(2, 32)));
            if (asset) {
                const std::lock_guard lock(state.mutex);
                try {
                    state.open.try_emplace(asset, 0);
                }
                catch (...) {
                    assets.close(asset);
                    throw;
                }
            }
            return reinterpret_cast<uintptr_t>(asset);
        }
        const auto asset = reinterpret_cast<void*>(Get(0));
        const std::lock_guard lock(state.mutex);
        if (name.starts_with("AAssetDir_")) {
            if (!asset)
                return 0;
            const auto found = state.directories.find(asset);
            if (found == state.directories.end())
                Fail("invalid ARM64 asset directory handle");
            Arm64GuestHeap().Free(std::exchange(found->second, 0));
            if (name == "AAssetDir_close") {
                state.directories.erase(found);
                assets.closeDirectory(asset);
            }
            else if (name == "AAssetDir_rewind") {
                assets.rewindDirectory(asset);
            }
            else if (name == "AAssetDir_getNextFileName") {
                const auto* next = assets.nextFile(asset);
                if (!next)
                    return 0;
                const auto bytes = std::strlen(next) + 1;
                found->second = Arm64GuestHeap().Allocate(bytes, false);
                if (found->second)
                    std::memcpy(reinterpret_cast<void*>(found->second), next, bytes);
                return found->second;
            }
            else
                Fail("unsupported ARM64 asset directory import " + std::string(name));
            return 0;
        }
        const auto found = state.open.find(asset);
        if (found == state.open.end())
            Fail("invalid ARM64 asset handle");
        if (name == "AAsset_close") {
            Arm64GuestHeap().Free(found->second);
            state.open.erase(found);
            assets.close(asset);
            return 0;
        }
        if (name == "AAsset_getLength" || name == "AAsset_getLength64")
            return uint64_t(assets.length(asset));
        if (name == "AAsset_getRemainingLength" || name == "AAsset_getRemainingLength64")
            return uint64_t(assets.remaining(asset));
        if (name == "AAsset_seek" || name == "AAsset_seek64")
            return uint64_t(assets.seek(asset, std::bit_cast<int64_t>(Get(1)), static_cast<int>(Get(2, 32))));
        if (name == "AAsset_isAllocated")
            return 1;
        if (name == "AAsset_openFileDescriptor" || name == "AAsset_openFileDescriptor64") {
            if (!Get(1) || !Get(2))
                return uint32_t(-1);
            auto* startOutput = Memory(Get(1), sizeof(int64_t), true);
            auto* lengthOutput = Memory(Get(2), sizeof(int64_t), true);
            int64_t start = 0, length = 0;
            const auto fd = assets.openFileDescriptor(asset, &start, &length);
            if (fd >= 0) {
                std::memcpy(startOutput, &start, sizeof(start));
                std::memcpy(lengthOutput, &length, sizeof(length));
            }
            return uint32_t(fd);
        }
        if (name == "AAsset_read") {
            const auto bytes =
                static_cast<unsigned>(std::min({Get(2), uint64_t(assets.remaining(asset)), uint64_t(INT_MAX)}));
            return uint32_t(assets.read(asset, bytes ? Memory(Get(1), bytes, true) : nullptr, bytes));
        }
        if (name == "AAsset_getBuffer") {
            auto& buffer = found->second;
            if (!buffer) {
                const auto bytes = static_cast<size_t>(assets.length(asset));
                buffer = Arm64GuestHeap().Allocate(bytes, false);
                if (buffer && bytes)
                    std::memcpy(reinterpret_cast<void*>(buffer), assets.buffer(asset), bytes);
            }
            return buffer;
        }
        Fail("unsupported ARM64 asset import " + std::string(name));
    }

    uint64_t CallWindowImport(std::string_view name)
    {
        if (!jni)
            Fail("ARM64 window import needs the Android runtime");
        auto& state = Windows();
        const auto& bridge = jni->windows;
        if ((name == "ANativeWindow_fromSurface" && !bridge.fromSurface) ||
            (name == "ANativeWindow_toSurface" && !bridge.toSurface))
            Fail("ARM64 ART Surface conversion is not implemented");
        if (name == "ANativeWindow_fromSurface") {
            const auto object = reinterpret_cast<jobject>(Get(1));
            if (!jniEnv || Get(0) != guestEnvAddress || !KnownReference(object))
                Fail("invalid ARM64 native window surface");
            const auto window = bridge.fromSurface(jniEnv, object);
            if (window) {
                const std::lock_guard lock(state.mutex);
                try {
                    state.windows.try_emplace(window);
                }
                catch (...) {
                    bridge.release(window);
                    throw;
                }
            }
            return reinterpret_cast<uintptr_t>(window);
        }
        const bool toSurface = name == "ANativeWindow_toSurface";
        const auto window = reinterpret_cast<void*>(Get(toSurface ? 1 : 0));
        if (toSurface && !window)
            return 0;
        auto* output =
            name == "ANativeWindow_lock" && Get(1) ? Memory(Get(1), sizeof(AndroidNativeWindowBuffer), true) : nullptr;
        auto* rect = name == "ANativeWindow_lock" && Get(2) ? Memory(Get(2), sizeof(AndroidWindowRect), true) : nullptr;
        const std::lock_guard lock(state.mutex);
        const auto found = state.windows.find(window);
        if (found == state.windows.end())
            Fail("invalid ARM64 native window handle");
        if (toSurface) {
            if (!jniEnv || Get(0) != guestEnvAddress)
                Fail("invalid ARM64 JNIEnv handle");
            return ReturnReference(bridge.toSurface(jniEnv, window));
        }
        if (name == "ANativeWindow_acquire") {
            bridge.acquire(window);
            ++found->second.references;
            return 0;
        }
        if (name == "ANativeWindow_release") {
            if (!--found->second.references)
                state.windows.erase(found);
            bridge.release(window);
            return 0;
        }
        if (name == "ANativeWindow_getWidth" || name == "ANativeWindow_getHeight")
            return uint32_t(bridge.dimension(window, name == "ANativeWindow_getWidth"));
        if (name == "ANativeWindow_getFormat")
            return uint32_t(bridge.format(window));
        if (name == "ANativeWindow_setBuffersGeometry")
            return uint32_t(bridge.geometry(window, static_cast<int>(Get(1, 32)), static_cast<int>(Get(2, 32)),
                                            static_cast<int>(Get(3, 32))));
        if (name == "ANativeWindow_lock") {
            AndroidNativeWindowBuffer buffer{};
            AndroidWindowRect dirty{};
            const auto status = bridge.lock(window, output ? &buffer : nullptr, rect ? &dirty : nullptr);
            if (!status) {
                std::memcpy(output, &buffer, sizeof(buffer));
                if (rect)
                    std::memcpy(rect, &dirty, sizeof(dirty));
                found->second.pixels = reinterpret_cast<uintptr_t>(buffer.bits);
                found->second.pixelBytes = size_t(buffer.stride) * buffer.height * sizeof(uint32_t);
            }
            return uint32_t(status);
        }
        if (name == "ANativeWindow_unlockAndPost") {
            found->second.pixels = 0;
            found->second.pixelBytes = 0;
            return uint32_t(bridge.post(window));
        }
        Fail("unsupported ARM64 native window import " + std::string(name));
    }

    template <typename T> const T* EglAttributes(unsigned reg, std::array<T, 129>& values)
    {
        const auto address = Get(reg);
        if (!address)
            return nullptr;
        for (size_t i = 0; i < values.size(); i += 2) {
            values[i] = static_cast<T>(Load(address + i * sizeof(T), sizeof(T)));
            if (values[i] == EGL_NONE)
                return values.data();
            if (i + 1 < values.size())
                values[i + 1] = static_cast<T>(Load(address + (i + 1) * sizeof(T), sizeof(T)));
        }
        Fail("unterminated ARM64 EGL attributes");
    }

    uint64_t CallEgl(std::string_view name)
    {
        if (!jni)
            Fail("ARM64 EGL call needs the Android runtime");
        if (name == "eglGetProcAddress") {
            const auto symbol = ReadString(Get(0), 4096);
            return jni->graphicsSymbol(symbol, false);
        }
        const auto address = jni->graphicsSymbol(name, true);
        if (!address)
            Fail("unavailable ARM64 EGL import " + std::string(name));
        const auto call = [address]<typename F, typename... Args>(std::type_identity<F>, Args... args) {
            return reinterpret_cast<F>(address)(args...);
        };
        const auto display = reinterpret_cast<EGLDisplay>(Get(0));
        const auto integer = [&](unsigned reg) {
            return static_cast<EGLint>(Get(reg, 32));
        };
        const auto output = [&](unsigned reg) {
            return Get(reg) ? reinterpret_cast<EGLint*>(Memory(Get(reg), sizeof(EGLint), true)) : nullptr;
        };
#define EGL_CALL(function, ...) call(std::type_identity<decltype(&function)>{}, __VA_ARGS__)
        if (name == "eglReleaseThread")
            return call(std::type_identity<decltype(&eglReleaseThread)>{});
        if (name == "eglGetDisplay") {
            if (Get(0))
                Fail("unsupported ARM64 native display");
            return reinterpret_cast<uintptr_t>(EGL_CALL(eglGetDisplay, EGL_DEFAULT_DISPLAY));
        }
        if (name == "eglInitialize")
            return EGL_CALL(eglInitialize, display, output(1), output(2));
        if (name == "eglGetError")
            return uint32_t(call(std::type_identity<decltype(&eglGetError)>{}));
        if (name == "eglBindAPI")
            return EGL_CALL(eglBindAPI, static_cast<EGLenum>(Get(0, 32)));
        if (name == "eglQueryString")
            return CopyGuestConstantString(EGL_CALL(eglQueryString, display, integer(1)));
        if (name == "eglChooseConfig") {
            std::array<EGLint, 129> attributes{};
            const auto max = integer(3);
            if (max < 0 || static_cast<unsigned>(max) > UINT_MAX / sizeof(EGLConfig))
                Fail("invalid ARM64 EGL config count");
            auto* configs =
                Get(2) && max ? reinterpret_cast<EGLConfig*>(Memory(Get(2), max * sizeof(EGLConfig), true)) : nullptr;
            return EGL_CALL(eglChooseConfig, display, EglAttributes(1, attributes), configs, max, output(4));
        }
        if (name == "eglGetConfigAttrib")
            return EGL_CALL(eglGetConfigAttrib, display, reinterpret_cast<EGLConfig>(Get(1)), integer(2), output(3));
        if (name == "eglCreateContext") {
            std::array<EGLint, 129> attributes{};
            return reinterpret_cast<uintptr_t>(EGL_CALL(eglCreateContext, display, reinterpret_cast<EGLConfig>(Get(1)),
                                                        reinterpret_cast<EGLContext>(Get(2)),
                                                        EglAttributes(3, attributes)));
        }
        if (name == "eglCreateWindowSurface") {
            std::array<EGLint, 129> attributes{};
            const auto attrs = EglAttributes(3, attributes);
            const auto window = reinterpret_cast<void*>(Get(2));
            auto& windows = Windows();
            const std::lock_guard lock(windows.mutex);
            if (!windows.windows.contains(window))
                Fail("invalid ARM64 EGL native window");
            using CreateWindow = EGLSurface (*)(EGLDisplay, EGLConfig, void*, const EGLint*);
            return reinterpret_cast<uintptr_t>(
                call(std::type_identity<CreateWindow>{}, display, reinterpret_cast<EGLConfig>(Get(1)), window, attrs));
        }
        if (name == "eglCreatePbufferSurface") {
            std::array<EGLint, 129> attributes{};
            return reinterpret_cast<uintptr_t>(EGL_CALL(
                eglCreatePbufferSurface, display, reinterpret_cast<EGLConfig>(Get(1)), EglAttributes(2, attributes)));
        }
        const auto surface = reinterpret_cast<EGLSurface>(Get(1));
        if (name == "eglMakeCurrent")
            return EGL_CALL(eglMakeCurrent, display, surface, reinterpret_cast<EGLSurface>(Get(2)),
                            reinterpret_cast<EGLContext>(Get(3)));
        if (name == "eglQuerySurface")
            return EGL_CALL(eglQuerySurface, display, surface, integer(2), output(3));
        if (name == "eglQueryContext")
            return EGL_CALL(eglQueryContext, display, reinterpret_cast<EGLContext>(Get(1)), integer(2), output(3));
        if (name == "eglSurfaceAttrib")
            return EGL_CALL(eglSurfaceAttrib, display, surface, integer(2), integer(3));
        if (name == "eglSwapBuffers")
            return EGL_CALL(eglSwapBuffers, display, surface);
        if (name == "eglSwapInterval")
            return EGL_CALL(eglSwapInterval, display, integer(1));
        if (name == "eglDestroySurface")
            return EGL_CALL(eglDestroySurface, display, surface);
        if (name == "eglDestroyContext") {
            const auto context = reinterpret_cast<EGLContext>(Get(1));
            const auto result = EGL_CALL(eglDestroyContext, display, context);
            if (result)
                WithGraphicsState(context, [](GuestGraphicsState& state) {
                    state = {};
                });
            return result;
        }
        if (name == "eglGetCurrentContext")
            return reinterpret_cast<uintptr_t>(call(std::type_identity<decltype(&eglGetCurrentContext)>{}));
        if (name == "eglGetCurrentDisplay")
            return reinterpret_cast<uintptr_t>(call(std::type_identity<decltype(&eglGetCurrentDisplay)>{}));
        if (name == "eglGetCurrentSurface")
            return reinterpret_cast<uintptr_t>(EGL_CALL(eglGetCurrentSurface, integer(0)));
        if (name == "eglTerminate")
            return EGL_CALL(eglTerminate, display);
#undef EGL_CALL
        Fail("unsupported ARM64 EGL import " + std::string(name));
    }

    const void* GlPixels(uintptr_t pixels, GLsizei width, GLsizei height, GLenum format, GLenum type, bool write,
                         GuestPixelStore store, GLsizei depth = 1)
    {
        if (store.buffer)
            return reinterpret_cast<void*>(pixels);
        if (width <= 0 || height <= 0 || depth <= 0)
            return nullptr;
        if (!pixels && write)
            Fail("null ARM64 GLES readback pointer");
        if (!pixels)
            return nullptr;
        unsigned components;
        switch (format) {
        case GL_RGBA:
        case 0x80e1:
        case GL_RGBA_INTEGER:
            components = 4;
            break; // GL_BGRA_EXT
        case GL_RGB:
        case GL_RGB_INTEGER:
            components = 3;
            break;
        case GL_RG:
        case GL_RG_INTEGER:
        case GL_LUMINANCE_ALPHA:
            components = 2;
            break;
        case GL_RED:
        case GL_RED_INTEGER:
        case GL_ALPHA:
        case GL_LUMINANCE:
        case GL_DEPTH_COMPONENT:
        case GL_DEPTH_STENCIL:
            components = 1;
            break;
        default:
            Fail("unsupported ARM64 GLES pixel format");
        }
        unsigned pixelBytes;
        switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE:
            pixelBytes = components;
            break;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
        case GL_HALF_FLOAT:
        case 0x8d61:
            pixelBytes = 2 * components;
            break;
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_FLOAT:
            pixelBytes = 4 * components;
            break;
        case GL_UNSIGNED_SHORT_5_6_5:
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1:
            pixelBytes = 2;
            break;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
        case GL_UNSIGNED_INT_24_8:
        case GL_UNSIGNED_INT_10F_11F_11F_REV:
        case GL_UNSIGNED_INT_5_9_9_9_REV:
            pixelBytes = 4;
            break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
            pixelBytes = 8;
            break;
        default:
            Fail("unsupported ARM64 GLES pixel type");
        }
        const auto rowBytes = uint64_t(store.rowLength ? store.rowLength : width) * pixelBytes;
        const auto stride = (rowBytes + store.alignment - 1) & ~(uint64_t(store.alignment) - 1);
        const uint64_t slices = store.skipImages + uint64_t(depth) - 1;
        const uint64_t rows =
            slices * (store.imageHeight ? store.imageHeight : height) + store.skipRows + uint64_t(height) - 1;
        if (rows && stride > UINT_MAX / rows)
            Fail("ARM64 GLES pixel data is too large");
        const auto bytes = rows * stride + (static_cast<uint64_t>(store.skipPixels) + width) * pixelBytes;
        if (bytes > UINT_MAX)
            Fail("ARM64 GLES pixel data is too large");
        return write ? Memory(pixels, static_cast<unsigned>(bytes), true)
                     : ReadMemory(pixels, static_cast<unsigned>(bytes));
    }

    uint64_t CallGl(std::string_view name)
    {
        if (!jni)
            Fail("ARM64 GLES call needs the Android runtime");
        const auto address = jni->graphicsSymbol(name, true);
        if (!address)
            Fail("unavailable ARM64 GLES import " + std::string(name));
        const auto call = [address]<typename F, typename... Args>(std::type_identity<F>, Args... args) {
            return reinterpret_cast<F>(address)(args...);
        };
        const auto u = [&](unsigned reg) {
            return static_cast<uint32_t>(Get(reg, 32));
        };
        const auto s = [&](unsigned reg) {
            return static_cast<int32_t>(Get(reg, 32));
        };
        const auto f = [&](unsigned reg) {
            return std::bit_cast<float>(uint32_t(GetVector(reg)[0]));
        };
        const auto flag = [&](unsigned reg) {
            return static_cast<GLboolean>(Get(reg, 32));
        };
        const auto in = [&](unsigned reg, size_t bytes) {
            if (bytes > UINT_MAX)
                Fail("ARM64 GLES input is too large");
            return bytes ? ReadMemory(Get(reg), static_cast<unsigned>(bytes)) : nullptr;
        };
        const auto out = [&](unsigned reg, size_t bytes) {
            if (bytes > UINT_MAX)
                Fail("ARM64 GLES output is too large");
            return bytes ? Memory(Get(reg), static_cast<unsigned>(bytes), true) : nullptr;
        };
        const auto currentContext = [&] {
            const auto get =
                reinterpret_cast<decltype(&eglGetCurrentContext)>(jni->graphicsSymbol("eglGetCurrentContext", true));
            return get ? get() : EGL_NO_CONTEXT;
        };
        const auto integerState = [&](GLenum key) {
            GLint value = 0;
            const auto query = reinterpret_cast<decltype(&glGetIntegerv)>(jni->graphicsSymbol("glGetIntegerv", true));
            if (query)
                query(key, &value);
            return value;
        };
        const auto size = [&](unsigned reg) {
            const auto count = s(reg);
            if (count < 0)
                Fail("negative ARM64 GLES buffer size");
            return static_cast<unsigned>(count);
        };
#define GL_CALL(function, ...) call(std::type_identity<decltype(&function)>{}, __VA_ARGS__)
#define GL_VOID(function, ...)                                                                                         \
    if (name == #function) {                                                                                           \
        GL_CALL(function, __VA_ARGS__);                                                                                \
        return 0;                                                                                                      \
    }
#define GL_RETURN(function, ...)                                                                                       \
    if (name == #function)                                                                                             \
    return uint32_t(GL_CALL(function, __VA_ARGS__))
        if (name == "glGetError")
            return call(std::type_identity<decltype(&glGetError)>{});
        if (name == "glFinish") {
            call(std::type_identity<decltype(&glFinish)>{});
            return 0;
        }
        if (name == "glFlush") {
            call(std::type_identity<decltype(&glFlush)>{});
            return 0;
        }
        if (name == "glGetString") {
            const auto* value = reinterpret_cast<const char*>(GL_CALL(glGetString, u(0)));
            if (value && (u(0) == GL_VERSION || u(0) == GL_RENDERER)) {
                char trace[2]{};
                if (GetEnvironmentVariableA("STONE_TRACE_DEVICEINFO", trace, sizeof(trace)) == 1 && trace[0] == '1')
                    std::fprintf(stderr, "[android-jit] GLES %s: %s\n", u(0) == GL_VERSION ? "version" : "renderer",
                                 value);
            }
            return CopyGuestConstantString(value);
        }
        if (name == "glGetStringi")
            return CopyGuestConstantString(reinterpret_cast<const char*>(GL_CALL(glGetStringi, u(0), u(1))));
        GL_VOID(glEnable, u(0))
        GL_VOID(glDisable, u(0))
        GL_VOID(glClear, u(0))
        GL_VOID(glClearBufferfi, u(0), s(1), f(0), s(2))
        GL_VOID(glClearColor, f(0), f(1), f(2), f(3))
        GL_VOID(glColorMask, flag(0), flag(1), flag(2), flag(3))
        GL_VOID(glDepthFunc, u(0))
        GL_VOID(glDepthMask, flag(0))
        GL_VOID(glStencilFuncSeparate, u(0), u(1), s(2), u(3))
        GL_VOID(glStencilOpSeparate, u(0), u(1), u(2), u(3))
        GL_VOID(glStencilMask, u(0))
        GL_VOID(glCullFace, u(0))
        GL_VOID(glFrontFace, u(0))
        GL_VOID(glBlendColor, f(0), f(1), f(2), f(3))
        GL_VOID(glBlendEquation, u(0))
        GL_VOID(glBlendFunc, u(0), u(1))
        GL_VOID(glBlendEquationSeparate, u(0), u(1))
        GL_VOID(glBlendFuncSeparate, u(0), u(1), u(2), u(3))
        GL_VOID(glActiveTexture, u(0))
        GL_VOID(glBindTexture, u(0), u(1))
        GL_VOID(glBindSampler, u(0), u(1))
        GL_VOID(glBindBufferBase, u(0), u(1), u(2))
        if (name == "glBindVertexArray") {
            GL_CALL(glBindVertexArray, u(0));
            if (const auto context = currentContext(); context != EGL_NO_CONTEXT) {
                const auto bound = static_cast<GLuint>(integerState(GL_VERTEX_ARRAY_BINDING));
                WithGraphicsState(context, [&](GuestGraphicsState& state) {
                    state.vertexArray = bound;
                });
            }
            return 0;
        }
        GL_VOID(glCopyTexImage2D, u(0), s(1), u(2), s(3), s(4), s(5), s(6), s(7))
        GL_VOID(glCopyTexSubImage3D, u(0), s(1), s(2), s(3), s(4), s(5), s(6), s(7), static_cast<GLsizei>(Load(sp, 4)))
        GL_VOID(glPolygonOffset, f(0), f(1))
        GL_VOID(glSampleCoverage, f(0), flag(0))
        GL_VOID(glSamplerParameterf, u(0), u(1), f(0))
        GL_VOID(glVertexAttrib2f, u(0), f(0), f(1))
        GL_VOID(glVertexAttrib3f, u(0), f(0), f(1), f(2))
        GL_VOID(glVertexAttrib4f, u(0), f(0), f(1), f(2), f(3))
        if (name == "glInvalidateSubFramebuffer") {
            const auto attachments = reinterpret_cast<const GLenum*>(in(2, size_t(size(1)) * sizeof(GLenum)));
            GL_CALL(glInvalidateSubFramebuffer, u(0), s(1), attachments, s(3), s(4), s(5), s(6));
            return 0;
        }
        if (name == "glGetTransformFeedbackVarying") {
            auto* length = Get(3) ? reinterpret_cast<GLsizei*>(out(3, sizeof(GLsizei))) : nullptr;
            auto* count = reinterpret_cast<GLsizei*>(out(4, sizeof(GLsizei)));
            auto* type = reinterpret_cast<GLenum*>(out(5, sizeof(GLenum)));
            auto* text = reinterpret_cast<GLchar*>(out(6, size(2)));
            GL_CALL(glGetTransformFeedbackVarying, u(0), u(1), s(2), length, count, type, text);
            return 0;
        }
        if (name == "glBindBuffer") {
            GL_CALL(glBindBuffer, u(0), u(1));
            if (u(0) == GL_PIXEL_UNPACK_BUFFER || u(0) == GL_PIXEL_PACK_BUFFER) {
                const bool pack = u(0) == GL_PIXEL_PACK_BUFFER;
                const auto buffer = integerState(pack ? GL_PIXEL_PACK_BUFFER_BINDING : GL_PIXEL_UNPACK_BUFFER_BINDING);
                WithGraphicsState(currentContext(), [&](GuestGraphicsState& state) {
                    (pack ? state.pack : state.unpack).buffer = buffer;
                });
            }
            return 0;
        }
        GL_VOID(glBindFramebuffer, u(0), u(1))
        GL_VOID(glBindRenderbuffer, u(0), u(1))
        GL_VOID(glRenderbufferStorage, u(0), u(1), s(2), s(3))
        GL_VOID(glRenderbufferStorageMultisample, u(0), s(1), u(2), s(3), s(4))
        if (name == "glPixelStorei") {
            if (s(1) >= 0) {
                WithGraphicsState(currentContext(), [&](GuestGraphicsState& state) {
                    const auto value = u(1);
                    switch (u(0)) {
                    case GL_PACK_ALIGNMENT:
                        if (value && value <= 8 && std::has_single_bit(value))
                            state.pack.alignment = value;
                        break;
                    case GL_UNPACK_ALIGNMENT:
                        if (value && value <= 8 && std::has_single_bit(value))
                            state.unpack.alignment = value;
                        break;
                    case GL_PACK_ROW_LENGTH:
                        state.pack.rowLength = value;
                        break;
                    case GL_UNPACK_ROW_LENGTH:
                        state.unpack.rowLength = value;
                        break;
                    case GL_PACK_SKIP_ROWS:
                        state.pack.skipRows = value;
                        break;
                    case GL_UNPACK_SKIP_ROWS:
                        state.unpack.skipRows = value;
                        break;
                    case GL_PACK_SKIP_PIXELS:
                        state.pack.skipPixels = value;
                        break;
                    case GL_UNPACK_SKIP_PIXELS:
                        state.unpack.skipPixels = value;
                        break;
                    case GL_UNPACK_IMAGE_HEIGHT:
                        state.unpack.imageHeight = value;
                        break;
                    case GL_UNPACK_SKIP_IMAGES:
                        state.unpack.skipImages = value;
                        break;
                    }
                });
            }
            GL_CALL(glPixelStorei, u(0), s(1));
            return 0;
        }
        GL_VOID(glTexParameteri, u(0), u(1), s(2))
        GL_VOID(glTexParameterf, u(0), u(1), f(0))
        // The EXT entry points use the core signatures, with ANGLE resolving
        // the exact name so extension-specific formats keep their semantics.
        if (name == "glTexStorage2D" || name == "glTexStorage2DEXT") {
            GL_CALL(glTexStorage2D, u(0), s(1), u(2), s(3), s(4));
            return 0;
        }
        if (name == "glTexStorage3D" || name == "glTexStorage3DEXT") {
            GL_CALL(glTexStorage3D, u(0), s(1), u(2), s(3), s(4), s(5));
            return 0;
        }
        GL_VOID(glTexStorage2DMultisample, u(0), s(1), u(2), s(3), s(4), flag(5))
        GL_VOID(glTexStorage3DMultisample, u(0), s(1), u(2), s(3), s(4), s(5), flag(6))
        GL_VOID(glGenerateMipmap, u(0))
        if (name == "glGetTexParameteriv") {
            constexpr GLenum TextureCropRectOes = 0x8b9d;
            const auto count = u(1) == GL_TEXTURE_BORDER_COLOR || u(1) == TextureCropRectOes ? 4 : 1;
            GL_CALL(glGetTexParameteriv, u(0), u(1), reinterpret_cast<GLint*>(out(2, count * sizeof(GLint))));
            return 0;
        }
        GL_VOID(glGetInternalformativ, u(0), u(1), u(2), s(3),
                reinterpret_cast<GLint*>(out(4, size_t(size(3)) * sizeof(GLint))))
        // The KHR and EXT variants use the same signatures as the core
        // calls. Use the selected export for each variant's object enums.
        if (name == "glGetObjectLabel" || name == "glGetObjectLabelKHR" || name == "glGetObjectLabelEXT") {
            auto* length = Get(3) ? reinterpret_cast<GLsizei*>(out(3, sizeof(GLsizei))) : nullptr;
            auto* label = Get(4) ? reinterpret_cast<GLchar*>(out(4, size(2))) : nullptr;
            GL_CALL(glGetObjectLabel, u(0), u(1), s(2), length, label);
            return 0;
        }
        if (name == "glObjectLabel" || name == "glObjectLabelKHR" || name == "glLabelObjectEXT") {
            const auto length = s(2);
            const bool terminated = name == "glLabelObjectEXT" ? length == 0 : length < 0;
            std::string text;
            const GLchar* label = nullptr;
            if (Get(3) && terminated) {
                text = ReadString(Get(3), 1u << 20);
                label = text.c_str();
            }
            else if (Get(3) && length > 0) {
                label = reinterpret_cast<const GLchar*>(in(3, length));
            }
            GL_CALL(glObjectLabel, u(0), u(1), length, label);
            return 0;
        }
        GL_VOID(glDispatchCompute, u(0), u(1), u(2))
        GL_VOID(glMemoryBarrierByRegion, u(0))
        GL_VOID(glViewport, s(0), s(1), s(2), s(3))
        GL_VOID(glScissor, s(0), s(1), s(2), s(3))
        if (name == "glEnableVertexAttribArray" || name == "glDisableVertexAttribArray") {
            const bool enable = name == "glEnableVertexAttribArray";
            WithGraphicsState(currentContext(), [&](GuestGraphicsState& state) {
                if (u(0) >= state.attributes.size())
                    Fail("unsupported ARM64 GLES attribute index");
                if (!state.vertexArray)
                    state.attributes[u(0)].enabled = enable;
            });
            GL_VOID(glEnableVertexAttribArray, u(0))
            GL_VOID(glDisableVertexAttribArray, u(0))
        }
        if (name == "glVertexAttribPointer") {
            if (s(1) <= 0 || s(1) > 4 || s(4) < 0)
                Fail("invalid ARM64 GLES vertex layout");
            unsigned componentBytes;
            switch (u(2)) {
            case GL_BYTE:
            case GL_UNSIGNED_BYTE:
                componentBytes = 1;
                break;
            case GL_SHORT:
            case GL_UNSIGNED_SHORT:
            case GL_HALF_FLOAT:
            case 0x8d61:
                componentBytes = 2;
                break;
            case GL_INT:
            case GL_UNSIGNED_INT:
            case GL_FLOAT:
            case GL_FIXED:
                componentBytes = 4;
                break;
            default:
                Fail("unsupported ARM64 GLES vertex component type");
            }
            const unsigned width = componentBytes * u(1);
            const bool buffered = integerState(GL_ARRAY_BUFFER_BINDING) != 0;
            auto* pointer = buffered ? reinterpret_cast<const void*>(Get(5)) : ReadMemory(Get(5), width);
            WithGraphicsState(currentContext(), [&](GuestGraphicsState& state) {
                if (u(0) >= state.attributes.size())
                    Fail("unsupported ARM64 GLES attribute index");
                if (state.vertexArray)
                    return;
                auto& attribute = state.attributes[u(0)];
                attribute.pixels = buffered ? 0 : Get(5);
                attribute.width = width;
                attribute.stride = s(4) ? u(4) : width;
            });
            GL_CALL(glVertexAttribPointer, u(0), s(1), u(2), flag(3), s(4), pointer);
            return 0;
        }
        if (name == "glDrawArrays") {
            if (s(1) < 0 || s(2) < 0)
                Fail("invalid ARM64 GLES draw range");
            if (s(2)) {
                const auto last = uint64_t(u(1)) + u(2) - 1;
                WithGraphicsState(currentContext(), [&](GuestGraphicsState& state) {
                    // Nondefault vertex arrays use buffer objects for their data.
                    if (state.vertexArray)
                        return;
                    for (const auto& attribute : state.attributes) {
                        if (!attribute.enabled || !attribute.pixels)
                            continue;
                        const auto bytes = last * attribute.stride + attribute.width;
                        if (bytes > UINT_MAX)
                            Fail("ARM64 GLES vertex data is too large");
                        ReadMemory(attribute.pixels, static_cast<unsigned>(bytes));
                    }
                });
            }
            GL_CALL(glDrawArrays, u(0), s(1), s(2));
            return 0;
        }
        GL_VOID(glCompileShader, u(0))
        GL_VOID(glLinkProgram, u(0))
        GL_VOID(glUseProgram, u(0))
        GL_VOID(glAttachShader, u(0), u(1))
        GL_VOID(glDetachShader, u(0), u(1))
        GL_VOID(glDeleteShader, u(0))
        GL_VOID(glDeleteProgram, u(0))
        GL_VOID(glUniform1i, s(0), s(1))
        GL_VOID(glUniform1f, s(0), f(0))
        GL_VOID(glUniform2f, s(0), f(0), f(1))
        GL_VOID(glUniform3f, s(0), f(0), f(1), f(2))
        GL_VOID(glUniform4f, s(0), f(0), f(1), f(2), f(3))
        GL_VOID(glFramebufferRenderbuffer, u(0), u(1), u(2), u(3))
        GL_VOID(glFramebufferTexture2D, u(0), u(1), u(2), u(3), s(4))
        GL_VOID(glFramebufferTextureLayer, u(0), u(1), u(2), s(3), s(4))
        GL_RETURN(glCreateShader, u(0));
        GL_RETURN(glCheckFramebufferStatus, u(0));
        if (name == "glCreateProgram")
            return call(std::type_identity<decltype(&glCreateProgram)>{});
        if (name == "glBindAttribLocation" || name == "glGetAttribLocation" || name == "glGetUniformLocation") {
            const auto label = ReadString(Get(name == "glBindAttribLocation" ? 2 : 1), 4096);
            GL_VOID(glBindAttribLocation, u(0), u(1), label.c_str())
            GL_RETURN(glGetAttribLocation, u(0), label.c_str());
            GL_RETURN(glGetUniformLocation, u(0), label.c_str());
        }
        const auto ids = [&](unsigned reg, bool write) {
            const auto count = size(0);
            if (count > UINT_MAX / sizeof(GLuint))
                Fail("ARM64 GLES object list is too large");
            return write ? reinterpret_cast<GLuint*>(out(reg, count * sizeof(GLuint)))
                         : const_cast<GLuint*>(reinterpret_cast<const GLuint*>(in(reg, count * sizeof(GLuint))));
        };
        GL_VOID(glGenTextures, s(0), ids(1, true))
        GL_VOID(glGenFramebuffers, s(0), ids(1, true))
        GL_VOID(glGenRenderbuffers, s(0), ids(1, true))
        GL_VOID(glGenBuffers, s(0), ids(1, true))
        GL_VOID(glDeleteTextures, s(0), ids(1, false))
        GL_VOID(glDeleteFramebuffers, s(0), ids(1, false))
        GL_VOID(glDeleteRenderbuffers, s(0), ids(1, false))
        GL_VOID(glDeleteBuffers, s(0), ids(1, false))
        GL_VOID(glGetFramebufferAttachmentParameteriv, u(0), u(1), u(2),
                reinterpret_cast<GLint*>(out(3, sizeof(GLint))))
        GL_VOID(glGetRenderbufferParameteriv, u(0), u(1), reinterpret_cast<GLint*>(out(2, sizeof(GLint))))
        GL_VOID(glGetProgramiv, u(0), u(1), reinterpret_cast<GLint*>(out(2, sizeof(GLint))))
        GL_VOID(glGetShaderiv, u(0), u(1), reinterpret_cast<GLint*>(out(2, sizeof(GLint))))
        GL_VOID(glGetShaderPrecisionFormat, u(0), u(1), reinterpret_cast<GLint*>(out(2, 2 * sizeof(GLint))),
                reinterpret_cast<GLint*>(out(3, sizeof(GLint))))
        if (name == "glGetIntegerv") {
            unsigned count = 1;
            switch (u(0)) {
            case GL_MAX_VIEWPORT_DIMS:
            case GL_ALIASED_LINE_WIDTH_RANGE:
            case GL_ALIASED_POINT_SIZE_RANGE:
            case GL_DEPTH_RANGE:
                count = 2;
                break;
            case GL_VIEWPORT:
            case GL_SCISSOR_BOX:
            case GL_COLOR_CLEAR_VALUE:
            case GL_BLEND_COLOR:
            case GL_COLOR_WRITEMASK:
                count = 4;
                break;
            case GL_COMPRESSED_TEXTURE_FORMATS:
            case GL_SHADER_BINARY_FORMATS:
            case GL_PROGRAM_BINARY_FORMATS: {
                const auto pname = u(0) == GL_COMPRESSED_TEXTURE_FORMATS ? GL_NUM_COMPRESSED_TEXTURE_FORMATS
                                   : u(0) == GL_SHADER_BINARY_FORMATS    ? GL_NUM_SHADER_BINARY_FORMATS
                                                                         : GL_NUM_PROGRAM_BINARY_FORMATS;
                GLint formats = 0;
                GL_CALL(glGetIntegerv, pname, &formats);
                if (formats <= 0)
                    return 0;
                count = static_cast<unsigned>(formats);
                break;
            }
            }
            const size_t bytes = count * sizeof(GLint);
            auto* guest = out(1, bytes);
            std::vector<GLint> values(std::max(count, 16u));
            std::memcpy(values.data(), guest, bytes);
            GL_CALL(glGetIntegerv, u(0), values.data());
            std::memcpy(guest, values.data(), bytes);
            return 0;
        }
        if (name == "glGetShaderInfoLog" || name == "glGetProgramInfoLog") {
            auto* length = Get(2) ? reinterpret_cast<GLsizei*>(out(2, sizeof(GLsizei))) : nullptr;
            auto* data = reinterpret_cast<GLchar*>(out(3, size(1)));
            GL_VOID(glGetShaderInfoLog, u(0), s(1), length, data)
            GL_VOID(glGetProgramInfoLog, u(0), s(1), length, data)
        }
        if (name == "glShaderSource") {
            const auto count = size(1);
            if (count > 4096)
                Fail("too many ARM64 GLES shader strings");
            std::vector<std::string> sources;
            sources.reserve(count);
            for (unsigned i = 0; i < count; ++i) {
                const auto text = Load(Get(2) + i * sizeof(uintptr_t), sizeof(uintptr_t));
                const auto length = Get(3) ? static_cast<GLint>(Load(Get(3) + i * sizeof(GLint), sizeof(GLint))) : -1;
                if (length < 0)
                    sources.push_back(ReadString(text, 1u << 20));
                else if (!length)
                    sources.emplace_back();
                else
                    sources.emplace_back(reinterpret_cast<const char*>(ReadMemory(text, length)), length);
            }
            std::vector<const GLchar*> strings;
            std::vector<GLint> lengths;
            for (const auto& source : sources) {
                strings.push_back(source.data());
                lengths.push_back(static_cast<GLint>(source.size()));
            }
            GL_CALL(glShaderSource, u(0), s(1), strings.data(), lengths.data());
            return 0;
        }
        if (name == "glBufferData" || name == "glBufferSubData") {
            const auto bytes = Get(name == "glBufferData" ? 1 : 2);
            if (bytes > UINT_MAX)
                Fail("ARM64 GLES buffer exceeds the addressable range");
            GL_VOID(glBufferData, u(0), static_cast<GLsizeiptr>(bytes), Get(2) ? in(2, bytes) : nullptr, u(3))
            GL_VOID(glBufferSubData, u(0), static_cast<GLintptr>(Get(1)), static_cast<GLsizeiptr>(bytes), in(3, bytes))
        }
        if (name == "glTexImage2D" || name == "glTexSubImage2D" || name == "glReadPixels") {
            GuestPixelStore store;
            const auto readback = name == "glReadPixels";
            WithGraphicsState(currentContext(), [&](GuestGraphicsState& state) {
                store = readback ? state.pack : state.unpack;
            });
            if (store.buffer)
                store.buffer = integerState(readback ? GL_PIXEL_PACK_BUFFER_BINDING : GL_PIXEL_UNPACK_BUFFER_BINDING);
            if (readback) {
                auto* pixels = const_cast<void*>(GlPixels(Get(6), s(2), s(3), u(4), u(5), true, store));
                GL_CALL(glReadPixels, s(0), s(1), s(2), s(3), u(4), u(5), pixels);
                return 0;
            }
            const auto data = Load(sp, sizeof(uintptr_t));
            const bool allocate = name == "glTexImage2D";
            store.imageHeight = store.skipImages = 0;
            const auto* pixels = GlPixels(data, s(allocate ? 3 : 4), s(allocate ? 4 : 5), u(6), u(7), false, store);
            GL_VOID(glTexImage2D, u(0), s(1), s(2), s(3), s(4), s(5), u(6), u(7), pixels)
            GL_VOID(glTexSubImage2D, u(0), s(1), s(2), s(3), s(4), s(5), u(6), u(7), pixels)
        }
        if (name == "glTexImage3D" || name == "glTexSubImage3D") {
            GuestPixelStore store;
            WithGraphicsState(currentContext(), [&](GuestGraphicsState& state) {
                store = state.unpack;
            });
            if (store.buffer)
                store.buffer = integerState(GL_PIXEL_UNPACK_BUFFER_BINDING);
            const bool allocate = name == "glTexImage3D";
            const auto format = allocate ? u(7) : static_cast<GLenum>(Load(sp, 4));
            const auto type = static_cast<GLenum>(Load(sp + (allocate ? 0 : 8), 4));
            const auto data = Load(sp + (allocate ? 8 : 16), sizeof(uintptr_t));
            const auto* pixels = GlPixels(data, s(allocate ? 3 : 5), s(allocate ? 4 : 6), format, type, false, store,
                                          s(allocate ? 5 : 7));
            GL_VOID(glTexImage3D, u(0), s(1), s(2), s(3), s(4), s(5), s(6), format, type, pixels)
            GL_VOID(glTexSubImage3D, u(0), s(1), s(2), s(3), s(4), s(5), s(6), s(7), format, type, pixels)
        }
        if (name == "glCompressedTexImage2D" || name == "glCompressedTexSubImage2D") {
            const bool allocate = name == "glCompressedTexImage2D";
            const auto bytes = s(allocate ? 6 : 7);
            const auto data = allocate ? Get(7) : Load(sp, sizeof(uintptr_t));
            const auto* pixels = integerState(GL_PIXEL_UNPACK_BUFFER_BINDING) ? reinterpret_cast<const void*>(data)
                                 : bytes > 0 && data ? ReadMemory(data, static_cast<unsigned>(bytes))
                                                     : nullptr;
            GL_VOID(glCompressedTexImage2D, u(0), s(1), u(2), s(3), s(4), s(5), bytes, pixels)
            GL_VOID(glCompressedTexSubImage2D, u(0), s(1), s(2), s(3), s(4), s(5), u(6), bytes, pixels)
        }
        if (name == "glCompressedTexImage3D" || name == "glCompressedTexSubImage3D") {
            const bool allocate = name == "glCompressedTexImage3D";
            const auto bytes = allocate ? s(7) : static_cast<GLsizei>(Load(sp + 8, 4));
            const auto data = Load(sp + (allocate ? 0 : 16), sizeof(uintptr_t));
            const auto* pixels = integerState(GL_PIXEL_UNPACK_BUFFER_BINDING) ? reinterpret_cast<void*>(data)
                                 : bytes > 0 && data ? ReadMemory(data, static_cast<unsigned>(bytes))
                                                     : nullptr;
            const auto format = allocate ? u(2) : static_cast<GLenum>(Load(sp, 4));
            GL_VOID(glCompressedTexImage3D, u(0), s(1), format, s(3), s(4), s(5), s(6), bytes, pixels)
            GL_VOID(glCompressedTexSubImage3D, u(0), s(1), s(2), s(3), s(4), s(5), s(6), s(7), format, bytes, pixels)
        }
        if (name == "glUniformMatrix3fv" || name == "glUniformMatrix4fv") {
            const auto fields = name == "glUniformMatrix3fv" ? 9u : 16u;
            const auto count = size(1);
            if (count > UINT_MAX / (fields * sizeof(GLfloat)))
                Fail("ARM64 GLES matrix data is too large");
            const auto* values = reinterpret_cast<const GLfloat*>(in(3, count * fields * sizeof(GLfloat)));
            GL_VOID(glUniformMatrix3fv, s(0), s(1), flag(2), values)
            GL_VOID(glUniformMatrix4fv, s(0), s(1), flag(2), values)
        }
#undef GL_RETURN
#undef GL_VOID
#undef GL_CALL
        Fail("unsupported ARM64 GLES import " + std::string(name));
    }

    uint64_t CallAudio(std::string_view name)
    {
        const auto target = AndroidAudioImport(name, true);
        if (!target)
            Fail("unsupported ARM64 audio import " + std::string(name));
        const auto call = [target]<typename Result, typename... Args>(std::type_identity<Result>, Args... args) {
            return reinterpret_cast<Result (*)(Args...)>(target)(args...);
        };
        const auto i = [&](unsigned reg) {
            return static_cast<int>(Get(reg, 32));
        };
        auto& state = Audio();
        if (name == "AAudio_createStreamBuilder") {
            auto* output = Memory(Get(0), sizeof(void*), true);
            void* builder = nullptr;
            const auto status = call(std::type_identity<int>{}, &builder);
            if (builder) {
                const std::lock_guard lock(state.mutex);
                try {
                    state.builders.insert(builder);
                }
                catch (...) {
                    reinterpret_cast<int (*)(void*)>(AndroidAudioImport("AAudioStreamBuilder_delete", true))(builder);
                    throw;
                }
            }
            std::memcpy(output, &builder, sizeof(builder));
            return uint32_t(status);
        }
        if (name == "AAudio_convertResultToText" || name == "AAudio_convertStreamStateToText")
            return CopyGuestConstantString(call(std::type_identity<const char*>{}, i(0)));
        const auto handle = reinterpret_cast<void*>(Get(0));
        const bool builder = name.starts_with("AAudioStreamBuilder_");
        {
            const std::lock_guard lock(state.mutex);
            if (!(builder ? state.builders : state.streams).contains(handle))
                Fail("invalid ARM64 AAudio handle");
        }
        if (name == "AAudioStreamBuilder_openStream") {
            auto* output = Memory(Get(1), sizeof(void*), true);
            void* stream = nullptr;
            const auto status = call(std::type_identity<int>{}, static_cast<const void*>(handle), &stream);
            if (stream) {
                const std::lock_guard lock(state.mutex);
                try {
                    state.streams.insert(stream);
                }
                catch (...) {
                    reinterpret_cast<int (*)(void*)>(AndroidAudioImport("AAudioStream_close", true))(stream);
                    throw;
                }
            }
            std::memcpy(output, &stream, sizeof(stream));
            return uint32_t(status);
        }
        if (name == "AAudioStreamBuilder_delete" || name == "AAudioStream_close") {
            const auto status = call(std::type_identity<int>{}, handle);
            if (!status) {
                const std::lock_guard lock(state.mutex);
                (builder ? state.builders : state.streams).erase(handle);
            }
            return uint32_t(status);
        }
        if (name == "AAudioStreamBuilder_setDataCallback" || name == "AAudioStreamBuilder_setErrorCallback") {
            const auto code = Get(1);
            if (code && !CodeImage(code))
                Fail("invalid ARM64 audio callback target");
            call(std::type_identity<void>{}, handle, code, reinterpret_cast<void*>(Get(2)));
            return 0;
        }
        if (name.starts_with("AAudioStreamBuilder_set")) {
            call(std::type_identity<void>{}, handle, i(1));
            return 0;
        }
        if (name == "AAudioStream_getTimestamp")
            return uint32_t(call(std::type_identity<int>{}, handle, i(1),
                                 reinterpret_cast<int64_t*>(Memory(Get(2), 8, true)),
                                 reinterpret_cast<int64_t*>(Memory(Get(3), 8, true))));
        if (name == "AAudioStream_waitForStateChange")
            return uint32_t(call(std::type_identity<int>{}, handle, i(1),
                                 Get(2) ? reinterpret_cast<int*>(Memory(Get(2), sizeof(int), true)) : nullptr,
                                 std::bit_cast<int64_t>(Get(3))));
        if (name == "AAudioStream_setBufferSizeInFrames")
            return uint32_t(call(std::type_identity<int>{}, handle, i(1)));
        if (name == "AAudioStream_getFramesWritten" || name == "AAudioStream_getFramesRead")
            return uint64_t(call(std::type_identity<int64_t>{}, handle));
        if (name.starts_with("AAudioStream_get") || name.starts_with("AAudioStream_request"))
            return uint32_t(call(std::type_identity<int>{}, handle));
        Fail("unsupported ARM64 audio import " + std::string(name));
    }

    const ElfImage* CodeImage(uintptr_t target) const
    {
        if (!target || target % 4)
            return nullptr;
        if ((!executableMapped && InExecutableRange(target)) || image->ContainsCode(target, 4))
            return image;
        return jni ? jni->findCode(target) : nullptr;
    }

    uint64_t IterateProgramHeaders()
    {
        if (!jni || callbackDepth >= 32)
            Fail("cannot enumerate ARM64 program headers");
        const auto entry = Get(0), data = Get(1);
        const auto* owner = CodeImage(entry);
        if (!owner)
            Fail("program header callback is outside loaded ARM64 code");

        struct HeaderInfo {
            uintptr_t bias, name, headers;
            uint16_t headerCount;
            uint64_t loads, unloads;
            uintptr_t tlsModule, tlsData;
        };

        static_assert(sizeof(HeaderInfo) == 64 && offsetof(HeaderInfo, loads) == 32);
        const auto images = jni->loadedImages();
        for (const auto& loaded : images) {
            const auto headers = loaded.image->ProgramHeaders();
            const size_t nameOffset = sizeof(HeaderInfo) + headers.size_bytes();
            std::vector<uint8_t> memory(nameOffset + loaded.name.size() + 1);
            const auto base = reinterpret_cast<uintptr_t>(memory.data());
            const HeaderInfo info{loaded.image->LoadBias(),
                                  base + nameOffset,
                                  base + sizeof(HeaderInfo),
                                  static_cast<uint16_t>(headers.size()),
                                  images.size(),
                                  0,
                                  loaded.image->TlsModule(),
                                  reinterpret_cast<uintptr_t>(loaded.image->ThreadLocalAddress(0, false))};
            std::memcpy(memory.data(), &info, sizeof(info));
            std::memcpy(memory.data() + sizeof(info), headers.data(), headers.size_bytes());
            std::memcpy(memory.data() + nameOffset, loaded.name.data(), loaded.name.size());
            Interpreter callback(*owner, entry, importName, propertyGet, callbackDepth + 1, jni);
            callback.jniEnv = jniEnv;
            const uint64_t arguments[]{base, sizeof(HeaderInfo), data};
            const auto result = static_cast<uint32_t>(callback.RunCallback(arguments, memory));
            if (result)
                return result;
        }
        return 0;
    }

    struct JumpTarget {
        uintptr_t pc, sp;
        std::array<uint64_t, 11> preservedRegisters;
        std::array<uint64_t, 8> preservedVectors;
        std::optional<uint64_t> signalMask;
    };

    void SetJumpTarget(bool saveMask)
    {
        const auto buffer = Get(0);
        std::array<uint64_t, 24> saved{};
        auto* destination = Memory(buffer, sizeof(saved), true);
        if (jumpTargets.size() >= 256 && !jumpTargets.contains(buffer))
            Fail("too many ARM64 jump targets");
        JumpTarget continuation{};
        continuation.pc = Get(Link);
        continuation.sp = sp;
        std::copy_n(registers.begin() + 19, continuation.preservedRegisters.size(),
                    continuation.preservedRegisters.begin());
        for (size_t i = 0; i < continuation.preservedVectors.size(); ++i)
            continuation.preservedVectors[i] = vectors[i + 8][0];
        if (saveMask) {
            const auto mask = reinterpret_cast<int (*)(int, const uint64_t*, uint64_t*)>(
                AndroidThreadImport("pthread_sigmask", true));
            uint64_t savedMask = 0;
            if (mask(0, nullptr, &savedMask) == 0)
                continuation.signalMask = savedMask;
        }
        jumpTargets.insert_or_assign(buffer, continuation);
        // The ARM64 Bionic buffer has dedicated slots for the callee-saved
        // registers. Stack scanners can see guest pointers stored there.
        // longjmp still restores the continuation from the interpreter's table.
        saved = {continuation.signalMask.has_value(),
                 continuation.signalMask.value_or(0),
                 continuation.pc,
                 sp,
                 Get(28),
                 Get(29),
                 Get(26),
                 Get(27),
                 Get(24),
                 Get(25),
                 Get(22),
                 Get(23),
                 Get(20),
                 Get(21),
                 0,
                 Get(19),
                 vectors[14][0],
                 vectors[15][0],
                 vectors[12][0],
                 vectors[13][0],
                 vectors[10][0],
                 vectors[11][0],
                 vectors[8][0],
                 vectors[9][0]};
        std::memcpy(destination, saved.data(), sizeof(saved));
    }

    void LongJump()
    {
        const auto found = jumpTargets.find(Get(0));
        if (found == jumpTargets.end())
            Fail("ARM64 longjmp has no target in the current invocation");
        const auto& continuation = found->second;
        if (continuation.sp < sp)
            Fail("ARM64 longjmp target frame has returned");
        if (continuation.signalMask) {
            const auto mask = reinterpret_cast<int (*)(int, const uint64_t*, uint64_t*)>(
                AndroidThreadImport("pthread_sigmask", true));
            mask(2, &*continuation.signalMask, nullptr);
        }
        const auto value = uint32_t(Get(1, 32));
        SetSp(continuation.sp);
        std::fill_n(registers.begin(), 19, 0);
        std::copy(continuation.preservedRegisters.begin(), continuation.preservedRegisters.end(),
                  registers.begin() + 19);
        for (auto& vector : vectors)
            vector = {};
        for (size_t i = 0; i < continuation.preservedVectors.size(); ++i)
            vectors[i + 8][0] = continuation.preservedVectors[i];
        Set(Link, continuation.pc);
        flagsValid = false;
        Set(0, value ? value : 1, 32);
        BranchTo(continuation.pc, true);
    }

    uint64_t ChangeEnvironment(bool remove)
    {
        try {
            const auto key = Get(0) ? ReadString(Get(0), 32768) : std::string{};
            if (key.empty() || key.find('=') != key.npos || (!remove && !Get(1))) {
                errno = EINVAL;
                return uint32_t(-1);
            }
            const auto value = remove ? std::string{} : ReadString(Get(1), 1u << 20);
            const std::lock_guard lock(arm64EnvironmentMutex);
            auto* slot = Arm64EnvironmentSlot();
            std::vector<uintptr_t> entries;
            size_t replacement = SIZE_MAX;
            for (auto cursor = *slot; cursor; cursor += sizeof(uintptr_t)) {
                const auto entry = Load(cursor, sizeof(uintptr_t));
                if (!entry)
                    break;
                const auto text = ReadString(entry, 1u << 20);
                const bool matches = text.size() > key.size() && text[key.size()] == '=' && text.starts_with(key);
                if (matches) {
                    if (!remove && !Get(2, 32))
                        return 0;
                    if (replacement == SIZE_MAX)
                        replacement = entries.size();
                    if (remove)
                        continue;
                }
                entries.push_back(entry);
            }
            if (remove && replacement == SIZE_MAX)
                return 0;
            if (!remove) {
                if (replacement == SIZE_MAX) {
                    replacement = entries.size();
                    entries.push_back(0);
                }
                entries[replacement] = CopyGuestConstantString((key + '=' + value).c_str());
                if (!entries[replacement])
                    throw std::bad_alloc();
            }
            entries.push_back(0);
            const auto bytes = entries.size() * sizeof(uintptr_t);
            const auto array = Arm64GuestHeap().Allocate(bytes, false);
            if (!array)
                throw std::bad_alloc();
            std::memcpy(reinterpret_cast<void*>(array), entries.data(), bytes);
            *slot = array;
            Arm64GuestHeap().Free(std::exchange(arm64EnvironmentArray, array));
            return 0;
        }
        catch (const std::bad_alloc&) {
            errno = ENOMEM;
            return uint32_t(-1);
        }
    }

    void BranchTo(uintptr_t target, bool isReturn = false)
    {
        if (!target) {
            if (isReturn) {
                pc = 0;
                return;
            }
            Fail("null branch target");
        }
        if (!(target & 3) && ((!executableMapped && InExecutableRange(target)) || image->ContainsCode(target, 4))) {
            pc = target;
            return;
        }
        if (executableMapped && InExecutableRange(target)) {
            pc = target;
            return;
        }
        // Only registered host imports have a name. Resolve those before a
        // full search of every loaded ELF; this is especially frequent in
        // ART's low-address msync probes. Current-image/mapped branches above
        // retain their normal execution and permission checks.
        const auto name = importName ? importName(target) : std::string_view{};
        if (name.empty()) {
            if (const auto* owner = CodeImage(target)) {
                if (image != owner)
                    executableSize = 0;
                image = owner;
                pc = target;
                return;
            }
            if (!AndroidArm64CodeAt(target).empty()) {
                if (!InExecutableRange(target))
                    executableSize = 0;
                pc = target;
                return;
            }
        }
        if (isReturn) {
            Fail("return outside executable ARM64 memory");
        }
        if (target == Arm64TlsDescriptorResolver()) {
            // TLSDESC has its own convention: preserve every register except
            // x0. A normal imported call would destroy live argument registers.
            const auto argument = Load(Get(0) + sizeof(uintptr_t), sizeof(uintptr_t));
            uintptr_t address = 0;
            bool resolved = image->ResolveTlsDescriptor(argument, address);
            if (!resolved && jni && jni->loadedImages) {
                for (const auto& loaded : jni->loadedImages()) {
                    if (loaded.image->ResolveTlsDescriptor(argument, address)) {
                        resolved = true;
                        break;
                    }
                }
            }
            if (!resolved)
                Fail("unknown ARM64 ELF TLS descriptor");
            Set(0, address - reinterpret_cast<uintptr_t>(ElfImage::GuestArm64ThreadPointer()));
            BranchTo(Get(Link), true);
            return;
        }
        if (profile)
            RecordArm64ProfileImport(*profile, name.empty() ? "<jni-or-callback>" : name);
        uint64_t result = 0;
        uint64_t secondResult = 0;
        importFloatResult = 0;
        const auto jniBase = JniToken(static_cast<JniEntry>(0));
        if (target >= jniBase && target - jniBase < jniTokens.size()) {
            result = CallJni(static_cast<JniEntry>(target - jniBase));
        }
        else if (const auto host = FindArm64HostJniTarget(target)) {
            AndroidScalarArguments arguments{};
            std::vector<uint64_t> spills;
            size_t gp = 0, fp = 0, hostGp = 0, hostFp = 0, stackSlot = 0;
            for (const auto type : host->native.prototype.parameters) {
                const bool floating = type == 'F' || type == 'D';
                auto& bank = floating ? fp : gp;
                const uint64_t bits = bank < 8 ? (floating ? vectors[bank++][0] : Get(static_cast<unsigned>(bank++)))
                                               : Load(sp + 8 * stackSlot++, 8);
                auto& hostBank = floating ? hostFp : hostGp;
                if (hostBank < (floating ? 8u : 6u)) {
                    if (floating)
                        arguments.floats[hostBank++] = bits;
                    else
                        arguments.integers[hostBank++] = bits;
                }
                else
                    spills.push_back(bits);
            }
            arguments.stack = spills.data();
            auto* environment = host->native.prototype.jniEnvironment ? reinterpret_cast<JNIEnv*>(Get(0)) : nullptr;
            if (host->native.prototype.jniEnvironment) {
                try {
                    Arm64ArtJniEntry(environment, offsetof(JNINativeInterface_, ExceptionCheck));
                }
                catch (const std::exception& error) {
                    std::ostringstream reason;
                    reason << "host JNI target 0x" << std::hex << host->native.address << ": " << error.what();
                    Fail(reason.str());
                }
            }
            try {
                result = host->native.invoke(arguments, host->native.address);
            }
            catch (const std::exception& error) {
                if (!environment)
                    throw;
                if (!ArtJni<&JNINativeInterface_::ExceptionCheck>(environment)) {
                    const auto type =
                        ArtJni<&JNINativeInterface_::FindClass>(environment, "java/lang/RuntimeException");
                    if (type) {
                        ArtJni<&JNINativeInterface_::ThrowNew>(environment, type, error.what());
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(environment, type);
                    }
                }
                result = 0;
            }
            switch (host->result) {
            case 'F':
            case 'D':
                importFloatResult = result;
                result = 0;
                break;
            case 'V':
                result = 0;
                break;
            case 'Z':
                result = result != 0;
                break;
            case 'B':
                result = static_cast<int8_t>(result);
                break;
            case 'C':
                result = static_cast<uint16_t>(result);
                break;
            case 'S':
                result = static_cast<int16_t>(result);
                break;
            case 'I':
                result = static_cast<uint32_t>(result);
                break;
            }
        }
        else if (name == "__cfi_slowpath" || name == "__cfi_slowpath_diag") {
            const auto address = Get(1);
            const auto contains = [&](const ElfImage& module) {
                return module.ContainsCode(address, 1) || module.DataAt(address, 1, false);
            };
            const ElfImage* owner = contains(*image) ? image : nullptr;
            if (!owner && jni) {
                for (const auto& module : jni->loadedImages())
                    if (contains(*module.image)) {
                        owner = module.image;
                        break;
                    }
            }
            if (!owner)
                Fail("ARM64 CFI target is outside loaded ELF memory");
            if (const auto check = owner->Export("__cfi_check", true)) {
                if (!owner->ContainsCode(check, 4))
                    Fail("ARM64 CFI checker is outside executable ELF memory");
                const std::array<Arm64Argument, 3> arguments{
                    {{'J', Get(0)}, {'L', address}, {'L', name == "__cfi_slowpath_diag" ? Get(2) : 0}}};
                RunArm64Function(*owner, check, 'V', importName, propertyGet, jni, arguments);
            }
            // As in Bionic, a loaded ELF without __cfi_check is uninstrumented.
        }
        else if (name.empty()) {
            Fail("unknown indirect branch target");
        }
        else if (name == "sigsetjmp" || name == "setjmp" || name == "_setjmp") {
            SetJumpTarget(name == "setjmp" || (name == "sigsetjmp" && Get(1, 32) != 0));
        }
        else if (name == "siglongjmp" || name == "longjmp" || name == "_longjmp") {
            LongJump();
            return;
        }
        else if (name == "getauxval") {
            switch (Get(0)) {
            case 6: { // AT_PAGESZ
                SYSTEM_INFO info{};
                GetSystemInfo(&info);
                result = info.dwPageSize;
                break;
            }
            case 11:
            case 12:
            case 13:
            case 14: // AT_UID, AT_EUID, AT_GID, AT_EGID
                result = AndroidApplicationUid();
                break;
            case 15: // AT_PLATFORM
                result = CopyGuestConstantString("aarch64");
                break;
            case 16:
            case 26: // AT_HWCAP, AT_HWCAP2
            case 23: // AT_SECURE
                break;
            default:
                errno = ENOENT;
                break;
            }
        }
        else if (name == "__system_property_get") {
            const auto key = ReadString(Get(0), 256);
            std::array<char, PropertyValueSize> value{};
            const int length = propertyGet(key.c_str(), value.data());
            if (length < 0 || static_cast<unsigned>(length) >= PropertyValueSize)
                Fail("invalid Android system property length");
            const auto bytes = static_cast<unsigned>(length + 1);
            std::memcpy(Memory(Get(1), bytes, true), value.data(), bytes);
            result = static_cast<unsigned>(length);
        }
        else if (name == "android_get_application_target_sdk_version") {
            result = static_cast<uint32_t>(AndroidTargetSdk());
        }
        else if (name == "android_get_device_api_level") {
            std::array<char, PropertyValueSize> value{};
            result =
                propertyGet("ro.build.version.sdk", value.data()) ? uint32_t(std::atoi(value.data())) : uint32_t(-1);
        }
        else if (name == "__system_property_find") {
            result = FindAndroidSystemProperty(ReadString(Get(0), 256));
        }
        else if (name == "__system_property_serial" || name == "__system_property_read" ||
                 name == "__system_property_read_callback") {
            AndroidSystemPropertyValue property{};
            const auto handle = Get(0);
            const bool published = handle && ReadAndroidSystemProperty(handle, property);
            if (handle && !published && name != "__system_property_serial") {
                std::ostringstream reason;
                reason << "unknown ARM64 system property handle 0x" << std::hex << handle;
                Fail(reason.str());
            }
            if (name == "__system_property_serial") {
                // libcutils owns a zero prop_info before finding the real
                // tracing property. Its serial is the first uint32_t in the
                // public Bionic layout; read it only through guest bounds.
                if (!handle)
                    result = UINT32_MAX;
                else if (published)
                    result = property.serial;
                else {
                    if (handle & 3)
                        Fail("unaligned ARM64 property serial");
                    const auto* serial = reinterpret_cast<const uint32_t*>(ReadMemory(handle, sizeof(uint32_t)));
                    result = std::atomic_ref<const uint32_t>(*serial).load(std::memory_order_acquire);
                }
            }
            else if (name == "__system_property_read") {
                if (handle && Get(1)) {
                    const auto count = static_cast<unsigned>(std::min<size_t>(property.name.size(), 31));
                    auto* output = Memory(Get(1), count + 1, true);
                    std::memcpy(output, property.name.data(), count);
                    output[count] = 0;
                }
                const auto value = property.value.size() > 91
                                       ? std::string_view{"Must use __system_property_read_callback() to read"}
                                       : property.value;
                const auto count = static_cast<unsigned>(value.size());
                auto* output = Memory(Get(2), count + 1, true);
                if (count)
                    std::memcpy(output, value.data(), count);
                output[count] = 0;
                result = count;
            }
            else if (handle && Get(1)) {
                const auto code = Get(1);
                const auto* owner = CodeImage(code);
                if (!owner || callbackDepth >= 32)
                    Fail("invalid ARM64 property read callback");
                const auto size = property.name.size() + property.value.size() + 2;
                const auto storage = Arm64GuestHeap().Allocate(size, false);
                if (!storage)
                    throw std::bad_alloc{};

                struct Release {
                    uintptr_t address;

                    ~Release()
                    {
                        Arm64GuestHeap().Free(address);
                    }
                } release{storage};

                auto* bytes = reinterpret_cast<char*>(storage);
                std::memcpy(bytes, property.name.data(), property.name.size());
                bytes[property.name.size()] = 0;
                auto* value = bytes + property.name.size() + 1;
                std::memcpy(value, property.value.data(), property.value.size());
                value[property.value.size()] = 0;
                const std::array<uint64_t, 4> args{Get(2), storage, reinterpret_cast<uintptr_t>(value),
                                                   property.serial};
                Interpreter callback(*owner, code, importName, propertyGet, callbackDepth + 1, jni, this);
                callback.RunCallback(args, {});
            }
        }
        else if (name == "__system_property_area_serial") {
            result = AndroidSystemPropertyAreaSerial();
        }
        else if (name == "__system_properties_init" || name == "__system_properties_zygote_reload") {
            result = AndroidSystemPropertyAreaSerial() == UINT32_MAX ? uint32_t(-1) : 0;
        }
        else if (name == "feholdexcept") {
            std::memcpy(Memory(Get(0), sizeof(FloatEnvironment), true), &floatEnvironment, sizeof(floatEnvironment));
            floatEnvironment.status &= ~uint32_t{31};
        }
        else if (name == "fesetenv") {
            std::memcpy(&floatEnvironment, ReadMemory(Get(0), sizeof(FloatEnvironment)), sizeof(floatEnvironment));
        }
        else if (name == "feclearexcept" || name == "fetestexcept") {
            const auto mask = static_cast<uint32_t>(Get(0, 32)) & 31;
            if (name == "feclearexcept")
                floatEnvironment.status &= ~mask;
            else
                result = floatEnvironment.status & mask;
        }
        else if (name == "fegetround" || name == "fesetround") {
            constexpr uint32_t mask = 3u << 22;
            const auto mode = static_cast<uint32_t>(Get(0, 32));
            if (name == "fegetround")
                result = floatEnvironment.control & mask;
            else if (mode & ~mask)
                result = uint32_t(-1);
            else
                floatEnvironment.control = (floatEnvironment.control & ~mask) | mode;
        }
        else if (name == "fegetexcept" || name == "fedisableexcept" || name == "feenableexcept") {
            if (name == "feenableexcept" && Get(0, 32))
                result = uint32_t(-1);
        }
        else if (name == "difftime") {
            importFloatResult =
                std::bit_cast<uint64_t>(std::difftime(std::bit_cast<int64_t>(Get(0)), std::bit_cast<int64_t>(Get(1))));
        }
        else if (name == "__cxa_atexit") {
            const auto code = Get(0);
            const auto* owner = CodeImage(code);
            if (!owner)
                Fail("destructor outside executable ARM64 memory");
            const std::lock_guard lock(exitMutex);
            try {
                ExitCallbacks().push_back({owner, code, Get(1), Get(2)});
            }
            catch (const std::bad_alloc&) {
                result = uint32_t(-1);
            }
        }
        else if (name == "__cxa_thread_atexit_impl" || name == "__cxa_thread_atexit") {
            const auto code = Get(0);
            const auto* owner = CodeImage(code);
            if (!owner)
                Fail("thread destructor outside executable ARM64 memory");
            auto callback = std::make_unique<ThreadExitCallback>(
                ThreadExitCallback{owner, Get(1), importName, propertyGet, jni ? std::optional{*jni} : std::nullopt});
            const auto invoke = +[](uintptr_t entry, void* argument) {
                const std::unique_ptr<ThreadExitCallback> task(static_cast<ThreadExitCallback*>(argument));
                try {
                    Interpreter(*task->image, entry, task->importName, task->propertyGet, 0,
                                task->jni ? &*task->jni : nullptr)
                        .RunInitializer(0, &task->argument);
                }
                catch (const std::exception& error) {
                    std::fprintf(stderr, "[android-jit] ARM64 thread destructor failed: %s\n", error.what());
                }
            };
            if (ElfImage::RegisterThreadDestructor(invoke, code, callback.get()))
                callback.release();
            else
                result = uint32_t(-1);
        }
        else if (name == "__cxa_finalize") {
            FinalizeArm64(Get(0), importName, propertyGet, callbackDepth + 1, jni);
        }
        else if (name == "asprintf" || name == "vasprintf") {
            std::function<uint64_t(bool)> next;
            if (name == "asprintf")
                next = RegisterArguments(2);
            else
                next = VariadicArguments(Get(2));
            auto measure = next;
            const int length = Format(nullptr, 0, Get(1), measure);
            auto* output = Memory(Get(0), sizeof(uintptr_t), true);
            uintptr_t buffer = 0;
            if (length >= 0 && length < (1 << 20)) {
                buffer = Arm64GuestHeap().Allocate(static_cast<size_t>(length) + 1, false);
                if (buffer &&
                    Format(reinterpret_cast<char*>(buffer), static_cast<size_t>(length) + 1, Get(1), next) != length) {
                    Arm64GuestHeap().Free(buffer);
                    buffer = 0;
                }
            }
            std::memcpy(output, &buffer, sizeof(buffer));
            result = buffer ? uint32_t(length) : uint32_t(-1);
        }
        else if (name == "sprintf" || name == "__vsprintf_chk") {
            const bool checked = name == "__vsprintf_chk";
            const auto format = Get(checked ? 3 : 1);
            std::function<uint64_t(bool)> next;
            if (checked)
                next = VariadicArguments(Get(4));
            else
                next = RegisterArguments(2);
            auto measure = next;
            const auto length = Format(nullptr, 0, format, measure);
            if (length < 0)
                result = uint32_t(-1);
            else {
                const auto size = static_cast<unsigned>(length) + 1;
                if (checked && size > Get(2))
                    Fail("ARM64 sprintf destination overflow");
                auto* output = reinterpret_cast<char*>(Memory(Get(0), size, true));
                result = uint32_t(Format(output, size, format, next));
            }
        }
        else if (name == "vsnprintf" || name == "__vsnprintf_chk" || name == "snprintf") {
            const auto size = Get(1);
            const bool checked = name == "__vsnprintf_chk";
            if (size > UINT_MAX || (checked && size > Get(3)))
                Fail("ARM64 snprintf destination overflow");
            auto* output = size ? reinterpret_cast<char*>(Memory(Get(0), static_cast<unsigned>(size), true)) : nullptr;
            const unsigned format = checked ? 4 : 2;
            std::function<uint64_t(bool)> next;
            if (name == "snprintf")
                next = RegisterArguments(format + 1);
            else
                next = VariadicArguments(Get(format + 1));
            result = uint32_t(Format(output, size, Get(format), next));
        }
        else if (name == "vsscanf" || name == "sscanf") {
            const auto input = ReadString(Get(0), 1u << 20), format = ReadString(Get(1), 65536);
            unsigned integer = 2;
            auto spilled = sp;
            std::function<uint64_t(bool)> fromVaList;
            if (name == "vsscanf")
                fromVaList = VariadicArguments(Get(2));
            result = uint32_t(ScanAndroidArguments(input.c_str(), format.c_str(), [&](size_t bytes) {
                if (bytes > UINT_MAX)
                    Fail("ARM64 scanf output is too large");
                uintptr_t address;
                if (fromVaList)
                    address = fromVaList(false);
                else if (integer < 8)
                    address = Get(integer++);
                else {
                    address = Load(spilled, 8);
                    spilled += 8;
                }
                return reinterpret_cast<uintptr_t>(Memory(address, static_cast<unsigned>(bytes), true));
            }));
        }
        else if (name == "__android_log_print" || name == "__android_log_buf_print" || name == "__android_log_vprint") {
            result = uint32_t(LogPrint(name));
        }
        else if (name == "__android_log_write") {
            const auto tag = Get(1) ? ReadString(Get(1), 1024) : std::string{};
            const auto message = ReadString(Get(2), 1u << 20);
            result = uint32_t(
                WriteAndroidLog(static_cast<int>(Get(0, 32)), Get(1) ? tag.c_str() : nullptr, message.c_str()));
        }
        else if (name == "__android_log_set_default_tag") {
            const auto tag = Get(0) ? ReadString(Get(0), 1024) : std::string{};
            SetAndroidLogDefaultTag(tag.c_str());
        }
        else if (name == "__android_log_is_loggable") {
            const auto tag = Get(1) ? ReadString(Get(1), 1024) : std::string{};
            result = uint32_t(IsAndroidLogEnabled(static_cast<int>(Get(0, 32)), Get(1) ? tag.c_str() : nullptr,
                                                  static_cast<int>(Get(2, 32))));
        }
        else if (name == "__android_log_write_log_message" || name == "__android_log_logd_logger" ||
                 name == "__android_log_stderr_logger") {
            if (Load(Get(0), sizeof(size_t)) >= sizeof(AndroidLogMessage)) {
                AndroidLogMessage message;
                std::memcpy(&message, ReadMemory(Get(0), sizeof(message)), sizeof(message));
                const auto tag =
                    message.tag ? ReadString(reinterpret_cast<uintptr_t>(message.tag), 4068) : std::string{};
                const auto file =
                    message.file ? ReadString(reinterpret_cast<uintptr_t>(message.file), 32768) : std::string{};
                const auto text = ReadString(reinterpret_cast<uintptr_t>(message.text), 1u << 20);
                message.size = sizeof(message);
                message.tag = message.tag ? tag.c_str() : nullptr;
                message.file = message.file ? file.c_str() : nullptr;
                message.text = text.c_str();
                if (name == "__android_log_write_log_message")
                    WriteAndroidLogMessage(&message);
                else
                    WriteAndroidLogToStderr(&message);
            }
        }
        else if (name == "__android_log_set_logger") {
            const auto code = Get(0);
            const auto targetName = importName(code);
            if (!code || targetName == "__android_log_logd_logger" || targetName == "__android_log_stderr_logger")
                SetTranslatedAndroidLogger({});
            else {
                const auto* owner = CodeImage(code);
                if (!owner)
                    Fail("invalid ARM64 logger callback");
                SetTranslatedAndroidLogger(
                    [owner, code, imports = importName, property = propertyGet,
                     bridge = jni ? std::optional{*jni} : std::nullopt](const AndroidLogMessage* message) {
                        const auto tag = message->tag ? std::string_view{message->tag} : std::string_view{};
                        const auto file = message->file ? std::string_view{message->file} : std::string_view{};
                        const auto text = message->text ? std::string_view{message->text} : std::string_view{};
                        std::vector<uint8_t> memory(sizeof(*message) + tag.size() + file.size() + text.size() + 3);
                        auto* entry = reinterpret_cast<AndroidLogMessage*>(memory.data());
                        *entry = *message;
                        entry->size = sizeof(*entry);
                        auto* at = reinterpret_cast<char*>(entry + 1);
                        const auto copy = [&](std::string_view value) {
                            auto* result = at;
                            if (!value.empty())
                                std::memcpy(at, value.data(), value.size());
                            at[value.size()] = 0;
                            at += value.size() + 1;
                            return result;
                        };
                        entry->tag = copy(tag);
                        entry->file = copy(file);
                        entry->text = copy(text);
                        if (!message->tag)
                            entry->tag = nullptr;
                        if (!message->file)
                            entry->file = nullptr;
                        const auto depth = activeArm64Interpreter ? activeArm64Interpreter->callbackDepth + 1 : 0;
                        if (depth >= 32)
                            throw std::runtime_error("ARM64 logger callback recursion limit exceeded");
                        const std::array<uint64_t, 1> args{reinterpret_cast<uintptr_t>(entry)};
                        Interpreter callback(*owner, code, imports, property, depth, bridge ? &*bridge : nullptr);
                        callback.RunCallback(args, memory);
                    });
            }
        }
        else if (name == "__android_log_set_aborter") {
            const auto code = Get(0);
            if (!code)
                SetTranslatedAndroidAborter({});
            else {
                const auto* owner = CodeImage(code);
                if (!owner)
                    Fail("invalid ARM64 aborter callback");
                SetTranslatedAndroidAborter([owner, code, imports = importName, property = propertyGet,
                                             bridge = jni ? std::optional{*jni} : std::nullopt](const char* message) {
                    const auto length = std::strlen(message) + 1;
                    std::vector<uint8_t> memory(length);
                    std::memcpy(memory.data(), message, length);
                    const auto depth = activeArm64Interpreter ? activeArm64Interpreter->callbackDepth + 1 : 0;
                    if (depth >= 32)
                        throw std::runtime_error("ARM64 aborter callback recursion limit exceeded");
                    const std::array<uint64_t, 1> args{reinterpret_cast<uintptr_t>(memory.data())};
                    Interpreter callback(*owner, code, imports, property, depth, bridge ? &*bridge : nullptr);
                    callback.RunCallback(args, memory);
                });
            }
        }
        else if (name == "__android_log_call_aborter") {
            const auto message = ReadString(Get(0), 1u << 20);
            CallAndroidLogAborter(message.c_str());
        }
        else if (name == "pthread_once") {
            result = uint32_t(Once());
        }
        else if (name == "pthread_key_create") {
            const auto slot = Get(0), code = Get(1);
            if (code && !CodeImage(code) && importName(code) != "free")
                Fail("invalid ARM64 TLS destructor: " + std::string(importName(code)));
            auto* keySlot = Memory(slot, sizeof(int), true);
            int key = 0;
            result = uint32_t(CreateAndroidKey(key, code));
            if (!result)
                std::memcpy(keySlot, &key, sizeof(key));
        }
        else if (name == "pthread_key_delete") {
            result = uint32_t(DeleteAndroidKey(static_cast<int>(Get(0, 32))));
        }
        else if (name == "pthread_getspecific") {
            result = reinterpret_cast<uintptr_t>(GetAndroidThreadValue(static_cast<int>(Get(0, 32))));
        }
        else if (name == "pthread_setspecific") {
            result = uint32_t(SetAndroidThreadValue(static_cast<int>(Get(0, 32)), reinterpret_cast<void*>(Get(1))));
        }
        else if (name == "dl_iterate_phdr") {
            result = IterateProgramHeaders();
        }
        else if (name == "android_get_LD_LIBRARY_PATH") {
            const auto path = AndroidLibrarySearchPath();
            if (Get(1) <= path.size())
                Fail("ARM64 library search path buffer is too small");
            auto* output = Memory(Get(0), static_cast<unsigned>(path.size() + 1), true);
            std::memcpy(output, path.data(), path.size());
            output[path.size()] = 0;
        }
        else if (jni && name == "android_get_exported_namespace") {
            const auto exported = Get(0) ? ReadString(Get(0), 1024) : std::string{};
            result = reinterpret_cast<uintptr_t>(AndroidExportedNamespace(Get(0) ? exported.c_str() : nullptr));
        }
        else if (jni && name == "android_dlopen_ext") {
            const auto file = Get(0) ? ReadString(Get(0), 32768) : std::string{};
            std::array<uint64_t, 6> extension{};
            if (Get(2)) {
                extension[0] = Load(Get(2), sizeof(uint64_t));
                if (extension[0] == 0x200 || extension[0] == 0x240)
                    std::memcpy(extension.data(), ReadMemory(Get(2), sizeof(extension)), sizeof(extension));
            }
            result = reinterpret_cast<uintptr_t>(AndroidDlOpenExt(
                Get(0) ? file.c_str() : nullptr, static_cast<int>(Get(1, 32)), Get(2) ? extension.data() : nullptr));
        }
        else if (jni && name == "dlopen") {
            const auto file = Get(0) ? ReadString(Get(0), 32768) : std::string{};
            result =
                reinterpret_cast<uintptr_t>(jni->dlopen(Get(0) ? file.c_str() : nullptr, static_cast<int>(Get(1, 32))));
            if (result)
                loaderError.clear();
            else
                loaderError = "dlopen failed for \"" + file + '"';
        }
        else if (jni && name == "dlsym") {
            const auto symbol = ReadString(Get(1), 4096);
            result = reinterpret_cast<uintptr_t>(jni->dlsym(reinterpret_cast<void*>(Get(0)), symbol.c_str()));
            if (result)
                loaderError.clear();
            else
                loaderError = "dlsym failed for \"" + symbol + '"';
        }
        else if (jni && name == "dlclose") {
            result = uint32_t(jni->dlclose(reinterpret_cast<void*>(Get(0))));
        }
        else if (jni && name == "dladdr") {
            AndroidDlInfo info{};
            if (Get(1) && jni->dladdr(reinterpret_cast<const void*>(Get(0)), &info)) {
                info.file = reinterpret_cast<const char*>(CopyGuestConstantString(info.file));
                info.symbol = reinterpret_cast<const char*>(CopyGuestConstantString(info.symbol));
                std::memcpy(Memory(Get(1), sizeof(info), true), &info, sizeof(info));
                result = 1;
            }
        }
        else if (jni && name == "dlerror") {
            if (const auto* error = jni->dlerror()) {
                const auto bytes = std::strlen(error) + 1;
                if (bytes <= dlErrorBuffer.size()) {
                    std::memcpy(dlErrorBuffer.data(), error, bytes);
                    result = reinterpret_cast<uintptr_t>(dlErrorBuffer.data());
                }
            }
        }
        else if (name == "malloc") {
            result = Arm64GuestHeap().AllocateLibc(Get(0), false);
        }
        else if (name == "malloc_usable_size") {
            result = Arm64GuestHeap().AllocationSize(Get(0));
        }
        else if (name == "calloc") {
            const auto count = Get(0), size = Get(1);
            if (!count || size <= SIZE_MAX / count)
                result = Arm64GuestHeap().AllocateLibc(count * size, true);
        }
        else if (name == "memalign" || name == "aligned_alloc") {
            auto alignment = Get(0);
            const auto size = Get(1);
            if (name == "memalign" && alignment <= PTRDIFF_MAX)
                alignment = std::bit_ceil(alignment);
            if (alignment > PTRDIFF_MAX) {
                errno = ENOMEM;
            }
            else if (!std::has_single_bit(alignment) || (name == "aligned_alloc" && size % alignment)) {
                errno = EINVAL;
            }
            else {
                result = Arm64GuestHeap().AllocateLibc(size, false, std::max<uint64_t>(alignment, sizeof(void*)));
                if (!result)
                    errno = ENOMEM;
            }
        }
        else if (name == "posix_memalign") {
            const auto alignment = Get(1);
            if (alignment < sizeof(void*) || !std::has_single_bit(alignment)) {
                result = EINVAL;
            }
            else {
                auto* output = Memory(Get(0), sizeof(uintptr_t), true);
                const auto address = Arm64GuestHeap().AllocateLibc(Get(2), false, alignment);
                if (address)
                    std::memcpy(output, &address, sizeof(address));
                else
                    result = ENOMEM;
            }
        }
        else if (name == "free") {
            if (!Arm64GuestHeap().Free(Get(0)))
                Fail("invalid ARM64 free address");
        }
        else if (name == "realloc") {
            const auto address = Get(0), size = Get(1);
            if (!address) {
                result = Arm64GuestHeap().AllocateLibc(size, false);
            }
            else {
                const auto oldSize = Arm64GuestHeap().AllocationSize(address);
                if (!oldSize)
                    Fail("invalid ARM64 realloc address");
                if (size) {
                    result = Arm64GuestHeap().AllocateLibc(size, false);
                    if (result)
                        std::memcpy(reinterpret_cast<void*>(result), reinterpret_cast<const void*>(address),
                                    std::min(oldSize, size));
                }
                if (result || !size)
                    Arm64GuestHeap().Free(address);
            }
        }
        else if (name == "memset" || name == "memcpy" || name == "memmove" || name == "__memset_chk" ||
                 name == "__memcpy_chk" || name == "__memmove_chk") {
            const auto size = Get(2);
            result = Get(0);
            if (size > UINT_MAX)
                Fail("ARM64 memory operation is too large");
            if (name.starts_with("__") && size > Get(3))
                Fail("ARM64 fortified memory destination overflow");
            if (size) {
                auto* dest = Memory(result, static_cast<unsigned>(size), true);
                if (name == "memset" || name == "__memset_chk")
                    std::memset(dest, static_cast<int>(Get(1, 32)), static_cast<size_t>(size));
                else
                    std::memmove(dest, ReadMemory(Get(1), static_cast<unsigned>(size)), static_cast<size_t>(size));
            }
        }
        else if (name == "strnlen") {
            const auto length = Get(1);
            const auto limit = static_cast<unsigned>(std::min<uint64_t>(length, 1u << 20));
            result = ReadString(Get(0), limit, false).size();
            if (result == limit && length > limit)
                Fail("ARM64 strnlen exceeded the interpreter string limit");
        }
        else if (name == "memmem") {
            const auto haystackSize = Get(1), needleSize = Get(3);
            if (!needleSize)
                result = Get(0);
            else if (needleSize <= haystackSize) {
                if (haystackSize > UINT_MAX)
                    Fail("ARM64 memmem input is too large");
                const auto* haystack = ReadMemory(Get(0), static_cast<unsigned>(haystackSize));
                const auto* needle = ReadMemory(Get(2), static_cast<unsigned>(needleSize));
                const auto* end = haystack + haystackSize;
                const auto* match = std::search(haystack, end, needle, needle + needleSize);
                if (match != end)
                    result = reinterpret_cast<uintptr_t>(match);
            }
        }
        else if (name == "__strchr_chk" || name == "__strrchr_chk") {
            const auto address = Get(0), bound = std::min<uint64_t>(Get(2), 1u << 20);
            const auto wanted = static_cast<unsigned char>(Get(1, 32));
            for (uint64_t i = 0;; ++i) {
                if (i >= bound)
                    Fail("ARM64 strchr ran past its source bound");
                const auto byte = Load(address + i, 1);
                if (byte == wanted) {
                    result = address + i;
                    if (name == "__strchr_chk")
                        break;
                }
                if (!byte)
                    break;
            }
        }
        else if (name == "strlen" || name == "__strlen_chk" || name == "strchr" || name == "strrchr") {
            const auto address = Get(0);
            const bool checked = name == "__strlen_chk";
            const auto bound = checked ? static_cast<unsigned>(std::min<uint64_t>(Get(1), 1u << 20)) : 1u << 20;
            const auto str = ReadString(address, bound);
            if (name == "strlen" || checked) {
                result = str.size();
            }
            else {
                const auto byte = static_cast<char>(Get(1, 32));
                const auto found = !byte ? str.size() : name == "strchr" ? str.find(byte) : str.rfind(byte);
                if (found != str.npos)
                    result = address + found;
            }
        }
        else if (name == "strcpy" || name == "__strcpy_chk") {
            const auto bound =
                name == "__strcpy_chk" ? static_cast<unsigned>(std::min<uint64_t>(Get(2), 1u << 20)) : 1u << 20;
            const auto value = ReadString(Get(1), bound);
            const auto bytes = static_cast<unsigned>(value.size() + 1);
            result = Get(0);
            std::memcpy(Memory(result, bytes, true), value.c_str(), bytes);
        }
        else if (name == "strcat" || name == "strncat" || name == "__strcat_chk" || name == "__strncat_chk") {
            const auto address = Get(0);
            const bool checked = name == "__strcat_chk" || name == "__strncat_chk";
            const bool limited = name == "strncat" || name == "__strncat_chk";
            const auto bound =
                checked ? static_cast<unsigned>(std::min<uint64_t>(Get(limited ? 3 : 2), 1u << 20)) : 1u << 20;
            const auto prefix = ReadString(address, bound).size();
            const auto capacity = static_cast<unsigned>(bound - prefix);
            const auto suffix =
                limited ? ReadString(Get(1), static_cast<unsigned>(std::min<uint64_t>(capacity, Get(2))), false)
                        : ReadString(Get(1), capacity);
            if (suffix.size() >= capacity)
                Fail("ARM64 string concatenation destination overflow");
            auto* output = Memory(address + prefix, static_cast<unsigned>(suffix.size() + 1), true);
            std::memcpy(output, suffix.c_str(), suffix.size() + 1);
            result = address;
        }
        else if (name == "__strncpy_chk" || name == "strncpy") {
            const auto address = Get(0);
            const auto count = Get(2);
            if (count > (1u << 20) || (name == "__strncpy_chk" && count > Get(3)))
                Fail("ARM64 strncpy destination overflow");
            if (count) {
                const auto input = ReadString(Get(1), static_cast<unsigned>(count), false);
                auto* output = Memory(address, static_cast<unsigned>(count), true);
                std::memcpy(output, input.data(), input.size());
                std::memset(output + input.size(), 0, static_cast<size_t>(count) - input.size());
            }
            result = address;
        }
        else if (name == "strlcpy") {
            const auto value = ReadString(Get(1), 1u << 20);
            const auto size = Get(2);
            if (size) {
                const auto count = static_cast<unsigned>(std::min<uint64_t>(value.size(), size - 1));
                auto* output = Memory(Get(0), count + 1, true);
                std::memcpy(output, value.data(), count);
                output[count] = 0;
            }
            result = value.size();
        }
        else if (name == "mallinfo" || name == "mallinfo2") {
            // AAPCS64 returns this 80-byte LP64 structure indirectly through x8.
            const auto statistics = Arm64GuestHeap().Statistics();
            std::memcpy(Memory(Get(8), sizeof(statistics), true), statistics.data(), sizeof(statistics));
        }
        else if (name == "__b64_ntop") {
            const auto size = Get(1), capacity = Get(3);
            if (size > UINT_MAX || capacity > UINT_MAX)
                Fail("ARM64 base64 buffer exceeds input limit");
            if (!Get(2) || capacity == 0) {
                errno = ENOSPC;
                result = uint32_t(-1);
            }
            else
                result = uint32_t(AndroidEncodeBase64(
                    size ? ReadMemory(Get(0), static_cast<unsigned>(size)) : nullptr, size,
                    reinterpret_cast<char*>(Memory(Get(2), static_cast<unsigned>(capacity), true)), capacity));
        }
        else if (name == "res_mkquery") {
            const auto operation = static_cast<int>(Get(0, 32));
            if (operation != 0)
                result = uint32_t(AndroidMakeDnsQuery(operation, nullptr, 0, 0, nullptr, 0, nullptr, nullptr, 0));
            else if (!Get(1) || !Get(7)) {
                errno = EINVAL;
                result = uint32_t(-1);
            }
            else {
                const auto capacity = static_cast<int32_t>(Load(sp, sizeof(uint64_t)));
                if (capacity <= 0) {
                    errno = ENOSPC;
                    result = uint32_t(-1);
                }
                else {
                    const auto domain = ReadString(Get(1), 4096);
                    auto* output = Memory(Get(7), static_cast<unsigned>(capacity), true);
                    result = uint32_t(AndroidMakeDnsQuery(operation, domain.c_str(), static_cast<int>(Get(2, 32)),
                                                          static_cast<int>(Get(3, 32)), nullptr, 0, nullptr, output,
                                                          capacity));
                }
            }
        }
        else if (name == "strtok" || name == "strtok_r") {
            const bool reentrant = name == "strtok_r";
            const auto state = reentrant ? Get(2) : 0;
            if (reentrant)
                Memory(state, sizeof(uintptr_t), true);
            const auto previous = reentrant ? (Get(0) ? 0 : Load(state, sizeof(uintptr_t))) : arm64Tokenizer;
            const auto address = Get(0) ? Get(0) : previous;
            uintptr_t next = 0;
            if (address) {
                const auto delimiters = ReadString(Get(1), 256);
                const auto text = ReadString(address, 1u << 20);
                const auto start = text.find_first_not_of(delimiters);
                if (start != text.npos) {
                    result = address + start;
                    const auto end = text.find_first_of(delimiters, start);
                    if (end != text.npos) {
                        Store(address + end, 1, 0);
                        next = address + end + 1;
                    }
                }
            }
            if (reentrant)
                Store(state, sizeof(uintptr_t), next);
            else
                arm64Tokenizer = next;
        }
        else if (name == "strstr" || name == "strpbrk" || name == "strcspn" || name == "strspn") {
            const auto address = Get(0);
            const auto text = ReadString(address, 1u << 20), key = ReadString(Get(1), 1u << 20);
            const auto found = name == "strstr"   ? text.find(key)
                               : name == "strspn" ? text.find_first_not_of(key)
                                                  : text.find_first_of(key);
            if (name == "strcspn" || name == "strspn")
                result = found == text.npos ? text.size() : found;
            else if (found != text.npos)
                result = address + found;
        }
        else if (name == "strdup") {
            const auto text = ReadString(Get(0), 1u << 20);
            result = Arm64GuestHeap().Allocate(text.size() + 1, false);
            if (result)
                std::memcpy(reinterpret_cast<void*>(result), text.c_str(), text.size() + 1);
        }
        else if (name == "memchr") {
            const auto size = Get(2);
            if (size > UINT_MAX)
                Fail("ARM64 memory search is too large");
            if (size)
                result =
                    reinterpret_cast<uintptr_t>(std::memchr(ReadMemory(Get(0), static_cast<unsigned>(size)),
                                                            static_cast<int>(Get(1, 32)), static_cast<size_t>(size)));
        }
        else if (name == "strtof" || name == "strtod") {
            const auto address = Get(0);
            auto* output = Get(1) ? Memory(Get(1), sizeof(uintptr_t), true) : nullptr;
            const auto value = ReadString(address, 1u << 20);
            char* end = nullptr;
            if (name == "strtof")
                importFloatResult = std::bit_cast<uint32_t>(std::strtof(value.c_str(), &end));
            else
                importFloatResult = std::bit_cast<uint64_t>(std::strtod(value.c_str(), &end));
            if (output) {
                const auto guestEnd = address + (end - value.c_str());
                std::memcpy(output, &guestEnd, sizeof(guestEnd));
            }
        }
        else if (name == "atoi" || name == "strtol" || name == "strtoul" || name == "strtoll" || name == "strtoull" ||
                 name == "strtoumax" || name == "strtoll_l" || name == "strtoull_l") {
            const auto address = Get(0);
            const auto value = ReadString(address, 1u << 20);
            char* end = nullptr;
            const auto base = name == "atoi" ? 10 : static_cast<int>(Get(2, 32));
            const bool unsignedResult = name.starts_with("strtou");
            if (name.ends_with("_l")) {
                const auto locale = reinterpret_cast<_locale_t>(Get(3));
                auto& locales = Locales();
                const std::lock_guard lock(locales.mutex);
                if (!locales.open.contains(locale))
                    Fail("invalid ARM64 locale handle");
                result = unsignedResult ? _strtoui64_l(value.c_str(), &end, base, locale)
                                        : uint64_t(_strtoi64_l(value.c_str(), &end, base, locale));
            }
            else {
                result = unsignedResult ? std::strtoull(value.c_str(), &end, base)
                                        : uint64_t(std::strtoll(value.c_str(), &end, base));
            }
            if (name == "atoi")
                result = uint32_t(result);
            else if (Get(1))
                Store(Get(1), sizeof(uintptr_t), address + (end ? end - value.c_str() : 0));
        }
        else if (name == "getenv") {
            const auto variable = ReadString(Get(0), 32768);
            const std::lock_guard lock(arm64EnvironmentMutex);
            auto entries = *Arm64EnvironmentSlot();
            if (!variable.empty() && variable.find('=') == variable.npos) {
                while (entries) {
                    const auto entry = Load(entries, sizeof(uintptr_t));
                    if (!entry)
                        break;
                    const auto value = ReadString(entry, 1u << 20);
                    if (value.starts_with(variable) && value.size() > variable.size() &&
                        value[variable.size()] == '=') {
                        result = entry + variable.size() + 1;
                        break;
                    }
                    entries += sizeof(uintptr_t);
                }
            }
        }
        else if (name == "setenv" || name == "unsetenv") {
            result = ChangeEnvironment(name == "unsetenv");
        }
        else if (name == "__errno") {
            result = reinterpret_cast<uintptr_t>(_errno());
        }
        else if (name == "getpid") {
            result = uint32_t(_getpid());
        }
        else if (name == "getuid" || name == "geteuid" || name == "getgid" || name == "getegid") {
            result = AndroidApplicationUid();
        }
        else if (name == "exit" || name == "_Exit") {
            Fail("guest requested process exit (" + std::to_string(Get(0, 32)) + ")");
        }
        else if (name.starts_with("AMediaCodec_")) {
            // There is no NDK codec backend. Let native clients choose a software codec.
            if (name.starts_with("AMediaCodec_create") || name == "AMediaCodec_getInputBuffer" ||
                name == "AMediaCodec_getOutputBuffer" || name == "AMediaCodec_getOutputFormat") {
                errno = 38;
            }
            else {
                result = uint64_t(int64_t{-10002}); // AMEDIA_ERROR_UNSUPPORTED
            }
        }
        else if (name.starts_with("AMediaExtractor_") || name == "AMediaFormat_getString") {
            // No extractor or track format is created without an NDK media backend.
            if (name == "AMediaExtractor_new")
                errno = ENOSYS;
            else if (name == "AMediaExtractor_readSampleData" || name == "AMediaExtractor_getSampleTime")
                result = UINT64_MAX;
            else if (name == "AMediaExtractor_delete" || name == "AMediaExtractor_seekTo" ||
                     name == "AMediaExtractor_selectTrack" || name == "AMediaExtractor_setDataSourceFd")
                result = uint64_t(int64_t{-10002}); // AMEDIA_ERROR_UNSUPPORTED
        }
        else if (name == "__android_log_assert") {
            const auto condition = Get(0) ? ReadString(Get(0), 8192) : std::string{};
            const auto tag = Get(1) ? ReadString(Get(1), 4096) : std::string{};
            std::array<char, 1024> output{};
            if (!Get(2) || Format(output.data(), output.size(), Get(2), RegisterArguments(3)) < 0) {
                const auto text = Get(0) ? "Assertion failed: " + condition : std::string{"Assertion failed"};
                const auto size = std::min(text.size(), output.size() - 1);
                std::memcpy(output.data(), text.data(), size);
                output[size] = 0;
            }
            const AndroidLogMessage message{sizeof(message), 0, 7, Get(1) ? tag.c_str() : nullptr, nullptr, 0,
                                            output.data()};
            WriteAndroidLogToStderr(&message);
            WriteAndroidLogMessage(&message);
            CallAndroidLogAborterIfRegistered(output.data());
            // A guest assertion cannot return, even if an installed aborter
            // does. Diagnose it like guest abort(), without a host CRT dialog.
            Fail("guest log assertion: " + std::string{output.data()});
        }
        else if (name == "android_set_abort_message") {
            abortMessage = Get(0) ? ReadString(Get(0), 8192) : std::string{};
        }
        else if (name == "abort" || name == "__stack_chk_fail") {
            std::ostringstream reason;
            reason << "guest called " << name << "()";
            const auto link = Get(Link);
            if (const auto* caller = link >= 4 ? CodeImage(link - 4) : nullptr)
                reason << " from ELF offset 0x" << std::hex << link - 4 - caller->LoadBias();
            if (!abortMessage.empty())
                reason << ": " << abortMessage;
            if (lastUnsupportedSyscall)
                reason << " (last unsupported AArch64 syscall " << std::dec << *lastUnsupportedSyscall << ')';
            if (!loaderError.empty())
                reason << " (last loader error: " << loaderError << ')';
            if (!mappingError.empty())
                reason << " (last mapping error: " << mappingError << ')';
            Fail(reason.str());
        }
        else if (name == "syscall") {
            const auto value = Syscall(Get(0), 1);
            if (value < 0) {
                errno = static_cast<int>(-value);
                result = uint64_t(-1);
            }
            else
                result = uint64_t(value);
        }
        else if (name == "getprogname") {
            result = reinterpret_cast<uintptr_t>(programName);
        }
        else if (name == "basename" || name == "dirname") {
            const auto path = Get(0) ? ReadString(Get(0), 1u << 20) : std::string{};
            result = reinterpret_cast<uintptr_t>(AndroidPathComponent(path.c_str(), name == "dirname"));
        }
        else if (name == "uname") {
            auto* output =
                Get(0) ? reinterpret_cast<AndroidUtsname*>(Memory(Get(0), sizeof(AndroidUtsname), true)) : nullptr;
            result = uint32_t(AndroidUname(output, "aarch64"));
        }
        else if (name == "gethostname") {
            const auto size = Get(1);
            auto* output = size >= sizeof(AndroidNodename) && Get(0)
                               ? reinterpret_cast<char*>(Memory(Get(0), sizeof(AndroidNodename), true))
                               : nullptr;
            result = uint32_t(AndroidGetHostname(output, size));
        }
        else if (name == "div") {
            const auto numerator = static_cast<int32_t>(Get(0, 32));
            const auto denominator = static_cast<int32_t>(Get(1, 32));
            if (!denominator || (numerator == INT32_MIN && denominator == -1))
                Fail("invalid ARM64 div operands");
            // AArch64 returns the two 32-bit fields of div_t together in x0.
            result = uint32_t(numerator / denominator) | (uint64_t(uint32_t(numerator % denominator)) << 32);
        }
        else if (name == "lldiv") {
            const auto numerator = static_cast<int64_t>(Get(0));
            const auto denominator = static_cast<int64_t>(Get(1));
            if (!denominator || (numerator == INT64_MIN && denominator == -1))
                Fail("invalid ARM64 lldiv operands");
            result = static_cast<uint64_t>(numerator / denominator);
            secondResult = static_cast<uint64_t>(numerator % denominator);
        }
        else if (name == "ptrace" || name == "process_vm_readv" || name == "execl" || name == "signalfd") {
            result = uint64_t(AndroidUnsupportedOperation());
        }
        else if (name == "sigaltstack") {
            if (Get(0))
                result = uint32_t(AndroidUnsupportedOperation());
            else {
                // Guest handlers currently use the regular guest stack. Report
                // that real disabled state; registration remains unsupported.
                struct Stack {
                    uint64_t address;
                    uint32_t flags, padding;
                    uint64_t size;
                };

                static_assert(sizeof(Stack) == 24);
                const Stack disabled{0, 2, 0, 0};
                if (Get(1))
                    std::memcpy(Memory(Get(1), sizeof(disabled), true), &disabled, sizeof(disabled));
                result = 0;
            }
        }
        else if (name == "strxfrm") {
            const auto value = ReadString(Get(1), 1u << 20);
            const auto count = static_cast<unsigned>(std::min<uint64_t>(Get(2), value.size() + 1));
            if (count)
                std::memcpy(Memory(Get(0), count, true), value.c_str(), count);
            result = value.size();
        }
        else if (name == "memcmp") {
            const auto size = Get(2);
            if (size > UINT_MAX)
                Fail("ARM64 memory comparison is too large");
            if (size)
                result =
                    uint32_t(std::memcmp(ReadMemory(Get(0), static_cast<unsigned>(size)),
                                         ReadMemory(Get(1), static_cast<unsigned>(size)), static_cast<size_t>(size)));
        }
        else if (name == "strcoll" || name == "strcmp" || name == "strncmp" || name == "strcasecmp" ||
                 name == "strncasecmp") {
            const bool ignoreCase = name == "strcasecmp" || name == "strncasecmp";
            const auto size = name == "strcmp" || name == "strcoll" || name == "strcasecmp" ? 1u << 20 : Get(2);
            result = uint32_t(CompareGuestStrings(Get(0), Get(1), size, ignoreCase));
        }
        else if (name == "clock_gettime") {
            AndroidTimespec value{};
            const auto ret = AndroidClock(static_cast<int>(Get(0, 32)), &value);
            result = uint32_t(ret);
            if (!ret)
                std::memcpy(Memory(Get(1), sizeof(value), true), &value, sizeof(value));
        }
        else if (name.starts_with("AAsset")) {
            result = CallAssetImport(name);
        }
        else if (name.starts_with("ASensor")) {
            result = CallSensorImport(name);
        }
        else if (name.starts_with("AConfiguration_")) {
            result = CallConfigurationImport(name);
        }
        else if (name.starts_with("ANativeWindow_")) {
            result = CallWindowImport(name);
        }
        else if (name.starts_with("egl")) {
            result = CallEgl(name);
        }
        else if (name.starts_with("gl")) {
            result = CallGl(name);
        }
        else if (name.starts_with("inflate") || name.starts_with("deflate") || name == "crc32") {
            result = CallArm64Zlib(
                name, std::span<const uint64_t, 8>{registers.data(), 8},
                [&](uintptr_t address, unsigned size, bool write) {
                    return Memory(address, size, write);
                },
                [&](uintptr_t address, unsigned size) {
                    return ReadString(address, size);
                });
        }
        else if (name.starts_with("vk")) {
            result = CallArm64Vulkan(
                name, std::span<const uint64_t, 8>{registers.data(), 8}, sp,
                [&](uintptr_t address, unsigned size, bool write) {
                    return Memory(address, size, write);
                },
                [&](uintptr_t address, unsigned size) {
                    return ReadString(address, size);
                },
                [&](unsigned index) {
                    return std::bit_cast<float>(static_cast<uint32_t>(GetVector(index)[0]));
                },
                [&](uintptr_t address) -> std::shared_ptr<NativeWindow> {
                    if (!jni)
                        return {};
                    auto* window = reinterpret_cast<NativeWindow*>(address);
                    auto& state = Windows();
                    const std::lock_guard lock(state.mutex);
                    if (!state.windows.contains(window))
                        return {};
                    jni->windows.acquire(window);
                    return {window, jni->windows.release};
                });
        }
        else if (name.starts_with("AAudio")) {
            result = CallAudio(name);
        }
        else if (name == "slCreateEngine" || name.starts_with("OpenSLES.")) {
            result = CallArm64OpenSl(name, std::span<const uint64_t, 8>{registers.data(), 8},
                                     [&](uintptr_t address, unsigned size, bool write) {
                                         return Memory(address, size, write);
                                     });
        }
        else if (name == "isnanf") {
            result = std::isnan(std::bit_cast<float>(uint32_t(GetVector(0)[0])));
        }
        else if (CallMathImport(name, result)) {
        }
        else if (const auto io = CallLibcIo(name)) {
            result = *io;
        }
        else if (const auto character = CallCharacterImport(name)) {
            result = *character;
        }
        else if (const auto wide = CallWideMemoryImport(name)) {
            result = *wide;
        }
        else if (const auto thread = CallThreadImport(name)) {
            result = *thread;
        }
        else if (const auto loader = CallNativeLoader(name)) {
            result = *loader;
        }
        else {
            Fail("unsupported ARM64 import call " + std::string(name));
        }
        // Imports obey AAPCS64. A procedure-call veneer does not preserve NZCV.
        std::fill_n(registers.begin(), 19, 0);
        for (size_t i = 0; i < vectors.size(); ++i) {
            if (i < 8 || i >= 16)
                vectors[i][0] = 0;
            vectors[i][1] = 0;
        }
        flagsValid = false;
        Set(0, result);
        Set(1, secondResult);
        SetVector(0, {importFloatResult, 0});
        BranchTo(Get(Link), true);
    }

    bool Condition(unsigned code) const
    {
        if (code >= 14)
            return true;
        if (!flagsValid)
            Fail("read of unavailable condition flags");
        const bool condition[] = {z, c, n, v, c && !z, n == v, !z && n == v};
        return condition[code >> 1] != bool(code & 1);
    }

    void SetConditionFlags(unsigned nzcv)
    {
        n = nzcv & 8;
        z = nzcv & 4;
        c = nzcv & 2;
        v = nzcv & 1;
        flagsValid = true;
    }

    void Arithmetic(uint32_t word, unsigned width, uint64_t left, uint64_t right)
    {
        const bool subtract = (word & 0x40000000) != 0;
        const uint64_t mask = width == 32 ? UINT32_MAX : UINT64_MAX;
        const uint64_t sign = (mask >> 1) + 1;
        const auto result = (subtract ? left - right : left + right) & mask;
        const unsigned dest = word & 31;
        if (word & 0x20000000) {
            n = (result & sign) != 0;
            z = result == 0;
            c = subtract ? left >= right : result < left;
            v = ((left ^ result) & (subtract ? left ^ right : ~(left ^ right)) & sign) != 0;
            flagsValid = true;
        }
        else if (dest == Zero && ((word & 0x1f000000) == 0x11000000 || (word & 0x1f200000) == 0x0b200000)) {
            if (width != 64)
                Fail("unsupported 32-bit stack pointer write");
            SetSp(result);
            return;
        }
        Set(dest, result, width);
    }

    void CarryArithmetic(uint32_t word, unsigned width)
    {
        if (!flagsValid)
            Fail("read of unavailable carry flag");
        const uint64_t mask = width == 32 ? UINT32_MAX : UINT64_MAX;
        const uint64_t sign = (mask >> 1) + 1;
        const auto left = Get((word >> 5) & 31, width);
        auto right = Get((word >> 16) & 31, width);
        if (word & 0x40000000)
            right ^= mask; // SBC adds the complement and carry
        const auto sum = (left + right) & mask;
        const auto result = (sum + c) & mask;
        if (word & 0x20000000) {
            n = (result & sign) != 0;
            z = result == 0;
            c = sum < left || result < sum;
            v = ((left ^ result) & ~(left ^ right) & sign) != 0;
        }
        Set(word & 31, result, width);
    }

    void Pair(uint32_t word, unsigned bytes, bool vector = false)
    {
        const unsigned mode = (word >> 23) & 3, base = (word >> 5) & 31;
        const unsigned first = word & 31, second = (word >> 10) & 31;
        const bool load = (word & 0x00400000) != 0;
        const bool writeback = mode == 1 || mode == 3;
        if ((load && first == second) || (writeback && !vector && base != Zero && (base == first || base == second)))
            Fail("overlapping register pair");
        const auto delta = SignExtend((word >> 15) & 127, 7) * bytes;
        const auto original = base == Zero ? sp : Get(base);
        const auto address = original + (mode == 1 ? 0 : delta);
        // Validate both elements before changing registers or writing either.
        if (load) {
            ReadMemory(address, bytes);
            ReadMemory(address + bytes, bytes);
        }
        else {
            Memory(address, bytes, true);
            Memory(address + bytes, bytes, true);
        }
        if (mode == 3 && base == Zero)
            SetSp(address);
        if (vector) {
            const auto transfer = [&](unsigned reg, uintptr_t slot) {
                if (load) {
                    Vector value{};
                    std::memcpy(value.data(), ReadMemory(slot, bytes), bytes);
                    SetVector(reg, value);
                }
                else {
                    const auto value = GetVector(reg);
                    std::memcpy(Memory(slot, bytes, true), value.data(), bytes);
                }
            };
            transfer(first, address);
            transfer(second, address + bytes);
        }
        else if (load) {
            const auto firstValue = Load(address, bytes), secondValue = Load(address + bytes, bytes);
            const bool signedWord = (word & 0xfe000000) == 0x68000000;
            Set(first, signedWord ? SignExtend(firstValue, 32) : firstValue, signedWord ? 64 : bytes * 8);
            Set(second, signedWord ? SignExtend(secondValue, 32) : secondValue, signedWord ? 64 : bytes * 8);
        }
        else {
            const auto firstValue = Get(first, bytes * 8), secondValue = Get(second, bytes * 8);
            Store(address, bytes, firstValue);
            Store(address + bytes, bytes, secondValue);
        }
        if (mode == 1 && base == Zero)
            SetSp(original + delta);
        if (writeback && base != Zero)
            Set(base, original + delta);
    }

    void Transfer(uint32_t word, bool vector)
    {
        const unsigned reg = word & 31, base = (word >> 5) & 31;
        const unsigned bytes =
            vector && (word & 0x00800000) ? static_cast<unsigned>(sizeof(Vector)) : 1u << (word >> 30);
        const unsigned op = (word >> 22) & 3;
        const auto original = base == Zero ? sp : Get(base);
        auto address = original;
        unsigned mode = 0;
        uint64_t offset;
        if (word & 0x01000000) {
            offset = ((word >> 10) & 4095) * bytes;
        }
        else if (word & 0x00200000) {
            const unsigned option = (word >> 13) & 7, index = (word >> 16) & 31;
            if (!(option & 2))
                Fail("invalid ARM64 indexed transfer extension");
            offset = Get(index, option & 1 ? 64 : 32);
            if (option == 6)
                offset = SignExtend(offset, 32);
            if (word & 0x1000)
                offset *= bytes;
        }
        else {
            mode = (word >> 10) & 3;
            offset = SignExtend((word >> 12) & 511, 9);
        }
        const bool writeback = mode == 1 || mode == 3;
        if (writeback && !vector && base == reg && base != Zero)
            Fail("overlapping ARM64 transfer registers");
        if (mode != 1)
            address += offset;
        if (vector) {
            if (op & 1) {
                Vector value{};
                std::memcpy(value.data(), ReadMemory(address, bytes), bytes);
                SetVector(reg, value);
            }
            else {
                const auto value = GetVector(reg);
                std::memcpy(Memory(address, bytes, true), value.data(), bytes);
            }
        }
        else if (!op) {
            Store(address, bytes, Get(reg));
        }
        else {
            const auto value = Load(address, bytes);
            Set(reg, op >= 2 ? SignExtend(value, bytes * 8) : value, op == 2 || bytes == 8 ? 64 : 32);
        }
        if (writeback && base != Zero)
            Set(base, original + offset);
        if (writeback && base == Zero)
            SetSp(original + offset);
    }

    template <typename T> void OrderedAccess(uintptr_t address, unsigned reg, bool load)
    {
        if (address % std::atomic_ref<T>::required_alignment) {
            std::ostringstream reason;
            reason << "unaligned ARM64 ordered memory access (low bits " << (address & 15);
            if (const auto* caller = CodeImage(Get(Link)))
                reason << ", caller ELF offset 0x" << std::hex << Get(Link) - caller->LoadBias();
            reason << ')';
            Fail(reason.str());
        }
        if (load) {
            const auto* pointer = reinterpret_cast<const T*>(ReadMemory(address, sizeof(T)));
            const auto value = std::atomic_ref<const T>(*pointer).load(std::memory_order_acquire);
            Set(reg, value, sizeof(T) * 8);
        }
        else {
            auto* pointer = reinterpret_cast<T*>(Memory(address, sizeof(T), true));
            std::atomic_ref<T>(*pointer).store(static_cast<T>(Get(reg, sizeof(T) * 8)), std::memory_order_release);
        }
    }

    template <typename T> void ExclusiveAccess(uint32_t word, uintptr_t address, unsigned reg, bool load)
    {
        if (address % std::atomic_ref<T>::required_alignment)
            Fail("unaligned ARM64 exclusive memory access");
        if (load) {
            const auto* memory = reinterpret_cast<const T*>(ReadMemory(address, sizeof(T)));
            const auto order = word & 0x8000 ? std::memory_order_acquire : std::memory_order_relaxed;
            const auto value = std::atomic_ref<const T>(*memory).load(order);
            exclusiveAddress = address;
            exclusiveValue = value;
            exclusiveSize = sizeof(T);
            Set(reg, value, sizeof(T) == 8 ? 64 : 32);
            return;
        }
        auto* memory = reinterpret_cast<T*>(Memory(address, sizeof(T), true));
        auto expected = static_cast<T>(exclusiveValue);
        const auto order = word & 0x8000 ? std::memory_order_release : std::memory_order_relaxed;
        const bool stored =
            exclusiveAddress == address && exclusiveSize == sizeof(T) &&
            std::atomic_ref<T>(*memory).compare_exchange_strong(expected, static_cast<T>(Get(reg)), order);
        exclusiveSize = 0;
        Set((word >> 16) & 31, stored ? 0 : 1, 32);
    }

    void ExclusivePairAccess(uint32_t word, uintptr_t address, unsigned first, bool load)
    {
        const bool wide = word & 0x40000000;
        const unsigned bytes = wide ? 16 : 8;
        const unsigned second = (word >> 10) & 31;
        if (address % bytes)
            Fail("unaligned ARM64 exclusive pair access");
        if (load) {
            const auto* memory = reinterpret_cast<const uint64_t*>(ReadMemory(address, bytes));
            const auto order = word & 0x8000 ? std::memory_order_acquire : std::memory_order_relaxed;
            exclusiveAddress = address;
            exclusiveSize = bytes;
            exclusiveValue = std::atomic_ref<const uint64_t>(memory[0]).load(order);
            exclusiveHigh = wide ? std::atomic_ref<const uint64_t>(memory[1]).load(order) : exclusiveValue >> 32;
            const unsigned width = wide ? 64 : 32;
            Set(first, exclusiveValue, width);
            Set(second, exclusiveHigh, width);
            return;
        }
        auto* memory = reinterpret_cast<uint64_t*>(Memory(address, bytes, true));
        bool stored = false;
        if (exclusiveAddress == address && exclusiveSize == bytes) {
            if (wide) {
                alignas(16) long long expected[2]{std::bit_cast<long long>(exclusiveValue),
                                                  std::bit_cast<long long>(exclusiveHigh)};
                stored = _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(memory),
                                                        std::bit_cast<long long>(Get(second)),
                                                        std::bit_cast<long long>(Get(first)), expected) != 0;
            }
            else {
                auto expected = exclusiveValue;
                const auto value = Get(first, 32) | (Get(second, 32) << 32);
                const auto order = word & 0x8000 ? std::memory_order_release : std::memory_order_relaxed;
                stored = std::atomic_ref<uint64_t>(*memory).compare_exchange_strong(expected, value, order);
            }
        }
        exclusiveSize = 0;
        Set((word >> 16) & 31, stored ? 0 : 1, 32);
    }

    void Logical(uint32_t word, unsigned width, uint64_t right)
    {
        const auto left = Get((word >> 5) & 31, width);
        const unsigned op = (word >> 29) & 3;
        const auto value = op == 1 ? left | right : op == 2 ? left ^ right : left & right;
        if (op == 3) {
            n = (value >> (width - 1)) != 0;
            z = value == 0;
            c = v = false;
            flagsValid = true;
        }
        const unsigned destination = word & 31;
        if (destination == Zero && op != 3 && (word & 0x1f800000) == 0x12000000) {
            // The immediate form can write SP, such as AND SP, Xn, #-16.
            SetSp(width == 32 ? uint32_t(value) : value);
        }
        else {
            Set(destination, value, width);
        }
    }

    uint64_t LogicalImmediate(uint32_t word, unsigned width) const
    {
        const unsigned nBit = (word >> 22) & 1, r = (word >> 16) & 63, s = (word >> 10) & 63;
        const unsigned length = std::bit_width((nBit << 6) | (s ^ 63));
        if (length < 2 || (nBit && width == 32))
            Fail("invalid logical immediate");
        const unsigned size = 1u << (length - 1), levels = size - 1;
        const unsigned ones = (s & levels) + 1, rotation = r & levels;
        if (ones == size)
            Fail("reserved logical immediate");
        const auto element = (uint64_t{1} << ones) - 1;
        const auto mask = UINT64_MAX >> (64 - size);
        const auto rotated = ((element >> rotation) | (element << ((size - rotation) & levels))) & mask;
        uint64_t result = 0;
        for (unsigned bit = 0; bit < width; bit += size)
            result |= rotated << bit;
        return result;
    }

    uint64_t Shift(uint64_t value, unsigned kind, unsigned amount, unsigned width) const
    {
        if (amount >= width)
            Fail("invalid register shift");
        switch (kind) {
        case 0:
            return width == 32 ? uint32_t(value << amount) : value << amount;
        case 1:
            return value >> amount;
        case 2:
            if (width == 32)
                return uint32_t(std::bit_cast<int32_t>(uint32_t(value)) >> amount);
            return uint64_t(std::bit_cast<int64_t>(value) >> amount);
        default:
            return width == 32 ? std::rotr(uint32_t(value), static_cast<int>(amount))
                               : std::rotr(value, static_cast<int>(amount));
        }
    }

    void Bitfield(uint32_t word, unsigned width)
    {
        const unsigned first = (word >> 16) & 63, last = (word >> 10) & 63;
        if (first >= width || last >= width || ((word >> 22) & 1) != (width == 64))
            Fail("invalid bitfield");
        const auto input = Get((word >> 5) & 31, width);
        const unsigned shift = first > last ? width - first : 0;
        const unsigned count = last + 1 - (shift ? 0 : first);
        const auto mask = (UINT64_MAX >> (64 - count)) << shift;
        auto value = ((shift ? input << shift : input >> first) & mask);
        const unsigned dest = word & 31;
        const unsigned op = (word >> 29) & 3;
        if (op == 0)
            value = SignExtend(value, shift + count);
        else if (op == 1)
            value |= Get(dest, width) & ~mask;
        Set(dest, value, width);
    }

    using Vector = std::array<uint64_t, 2>;

    Vector GetVector(unsigned reg) const
    {
        return vectors[reg];
    }

    void SetVector(unsigned reg, Vector value)
    {
        vectors[reg] = value;
    }

    static uint64_t Lane(const Vector& value, unsigned lane, unsigned width)
    {
        const unsigned bit = lane * width;
        return (value[bit / 64] >> (bit % 64)) & (UINT64_MAX >> (64 - width));
    }

    static void SetLane(Vector& value, unsigned lane, unsigned width, uint64_t bits)
    {
        const unsigned bit = lane * width, shift = bit % 64;
        const auto mask = UINT64_MAX >> (64 - width);
        value[bit / 64] = (value[bit / 64] & ~(mask << shift)) | ((bits & mask) << shift);
    }

    static uint64_t ConvertFloatInteger(double value, unsigned width, bool sign)
    {
        const auto limit = std::ldexp(1.0, width - unsigned(sign));
        if (value >= limit)
            return (UINT64_MAX >> (64 - width)) >> unsigned(sign);
        if (sign && value <= -limit)
            return uint64_t{1} << (width - 1);
        if (sign && !std::isnan(value))
            return uint64_t(static_cast<int64_t>(value));
        return value > 0 ? static_cast<uint64_t>(value) : 0;
    }

    static double ReciprocalEstimate(double value, bool squareRoot)
    {
        if (value == 0) {
            floatEnvironment.status |= 1u << 1;
            return std::copysign(HUGE_VAL, value);
        }
        if (squareRoot && value < 0) {
            floatEnvironment.status |= 1u;
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (!std::isfinite(value))
            return 1.0 / value;
        int exponent;
        double mantissa = std::frexp(std::abs(value), &exponent);
        if (squareRoot && exponent % 2) {
            mantissa *= 0.5;
            ++exponent;
        }
        const double scale = squareRoot && mantissa >= 0.5 ? 256 : 512;
        const double midpoint = (std::floor(mantissa * scale) + 0.5) / scale;
        const auto estimate = std::round(256.0 / (squareRoot ? std::sqrt(midpoint) : midpoint));
        return std::copysign(std::ldexp(estimate, -8 - (squareRoot ? exponent / 2 : exponent)), value);
    }

    bool VectorLaneTransfer(uint32_t word)
    {
        const unsigned opcode = (word >> 13) & 7, size = (word >> 10) & 3;
        const unsigned kind = opcode & ~1u;
        const unsigned count = 1 + 2 * (opcode & 1) + ((word >> 21) & 1);
        const bool q = word & 0x40000000, load = word & 0x00400000, post = word & 0x00800000;
        const unsigned reg = word & 31, base = (word >> 5) & 31, index = (word >> 16) & 31, s = (word >> 12) & 1;
        if (!post && index)
            return false;
        const auto address = base == Zero ? sp : Get(base);
        unsigned width = 8u << (kind >> 1);
        unsigned lane = 0;
        bool replicate = false;
        switch (kind) {
        case 0:
            lane = (q << 3) | (s << 2) | size;
            break;
        case 2:
            if (size & 1)
                return false;
            lane = (q << 2) | (s << 1) | (size >> 1);
            break;
        case 4:
            if (size == 0)
                lane = (q << 1) | s;
            else if (size == 1 && s == 0) {
                width = 64;
                lane = q;
            }
            else
                return false;
            break;
        case 6:
            if (!load || s)
                return false;
            replicate = true;
            width = 8u << size;
            break;
        }
        const unsigned bytes = width / 8;
        const auto* input = load ? ReadMemory(address, count * bytes) : nullptr;
        auto* output = load ? nullptr : Memory(address, count * bytes, true);
        for (unsigned i = 0; i < count; ++i) {
            const auto target = (reg + i) & 31;
            if (load) {
                uint64_t bits = 0;
                std::memcpy(&bits, input + i * bytes, bytes);
                auto value = replicate ? Vector{} : GetVector(target);
                if (replicate) {
                    for (unsigned j = 0; j < (q ? 128u : 64u) / width; ++j)
                        SetLane(value, j, width, bits);
                }
                else {
                    SetLane(value, lane, width, bits);
                }
                SetVector(target, value);
            }
            else {
                const auto bits = Lane(GetVector(target), lane, width);
                std::memcpy(output + i * bytes, &bits, bytes);
            }
        }
        if (post) {
            const auto next = address + (index == Zero ? count * bytes : Get(index));
            if (base == Zero)
                SetSp(next);
            else
                Set(base, next);
        }
        return true;
    }

    bool VectorStructure(uint32_t word)
    {
        const auto opcode = (word >> 12) & 15;
        unsigned registerCount, interleave;
        switch (opcode) {
        case 0:
            registerCount = interleave = 4;
            break;
        case 4:
            registerCount = interleave = 3;
            break;
        case 8:
            registerCount = interleave = 2;
            break;
        case 2:
            registerCount = 4;
            interleave = 1;
            break;
        case 6:
            registerCount = 3;
            interleave = 1;
            break;
        case 10:
            registerCount = 2;
            interleave = 1;
            break;
        case 7:
            registerCount = interleave = 1;
            break;
        default:
            return false;
        }
        const unsigned vectorSize = word & 0x40000000 ? 16 : 8, laneSize = 1u << ((word >> 10) & 3);
        const unsigned reg = word & 31, base = (word >> 5) & 31, index = (word >> 16) & 31;
        const bool load = word & 0x00400000, post = word & 0x00800000;
        if (!post && index)
            return false;
        const auto address = base == Zero ? sp : Get(base);
        const unsigned bytes = registerCount * vectorSize;
        const auto* input = load ? ReadMemory(address, bytes) : nullptr;
        auto* output = load ? nullptr : Memory(address, bytes, true);
        std::array<Vector, 4> data{};
        for (unsigned i = 0; i < registerCount; ++i) {
            if (!load)
                data[i] = GetVector((reg + i) & 31);
            auto* lanes = reinterpret_cast<uint8_t*>(data[i].data());
            for (unsigned lane = 0; lane < vectorSize / laneSize; ++lane) {
                const auto offset =
                    interleave == 1 ? i * vectorSize + lane * laneSize : (lane * interleave + i) * laneSize;
                if (load)
                    std::memcpy(lanes + lane * laneSize, input + offset, laneSize);
                else
                    std::memcpy(output + offset, lanes + lane * laneSize, laneSize);
            }
            if (load)
                SetVector((reg + i) & 31, data[i]);
        }
        if (post) {
            const auto next = address + (index == Zero ? bytes : Get(index));
            if (base == Zero)
                SetSp(next);
            else
                Set(base, next);
        }
        return true;
    }

    bool Simd(uint32_t word)
    {
        const unsigned dest = word & 31, source = (word >> 5) & 31, second = (word >> 16) & 31;
        const bool wide = (word & 0x00400000) != 0;
        if ((word & 0xbf200000) == 0x0c000000)
            return VectorStructure(word);
        if ((word & 0xbf000000) == 0x0d000000)
            return VectorLaneTransfer(word);
        if ((word & 0xdf20f400) == 0x5e203400) { // CMGT / CMGE / CMHI / CMHS Dd, Dn, Dm
            if (((word >> 22) & 3) != 3)
                Fail("reserved ARM64 scalar compare arrangement");
            const auto sign = word & 0x20000000 ? uint64_t{0} : uint64_t{1} << 63;
            const auto left = GetVector(source)[0] ^ sign, right = GetVector(second)[0] ^ sign;
            const bool matches = left > right || ((word & 0x800) && left == right);
            SetVector(dest, {matches ? UINT64_MAX : 0, 0});
            return true;
        }
        if ((word & 0xdfe0fc00) == 0x5ee08c00) { // CMEQ / CMTST Dd, Dn, Dm
            const auto left = GetVector(source)[0], right = GetVector(second)[0];
            const bool matches = word & 0x20000000 ? left == right : (left & right) != 0;
            SetVector(dest, {matches ? UINT64_MAX : 0, 0});
            return true;
        }
        const auto compareIntegerZero = word & 0xbf3ffc00;
        if (compareIntegerZero == 0x0e208800 || compareIntegerZero == 0x0e209800 || compareIntegerZero == 0x0e20a800 ||
            compareIntegerZero == 0x2e208800 ||
            compareIntegerZero == 0x2e209800) { // CMGT / CMEQ / CMLT / CMGE / CMLE with immediate zero
            const unsigned width = 8u << ((word >> 22) & 3);
            const unsigned bits = word & 0x40000000 ? 128 : 64;
            if (width == 64 && bits == 64)
                Fail("invalid ARM64 vector compare arrangement");
            const auto input = GetVector(source);
            Vector result{};
            for (unsigned lane = 0; lane < bits / width; ++lane) {
                const auto value = std::bit_cast<int64_t>(SignExtend(Lane(input, lane, width), width));
                bool matches;
                switch (compareIntegerZero) {
                case 0x0e208800:
                    matches = value > 0;
                    break;
                case 0x0e209800:
                    matches = value == 0;
                    break;
                case 0x0e20a800:
                    matches = value < 0;
                    break;
                case 0x2e208800:
                    matches = value >= 0;
                    break;
                default:
                    matches = value <= 0;
                    break;
                }
                if (matches)
                    SetLane(result, lane, width, UINT64_MAX);
            }
            SetVector(dest, result);
            return true;
        }
        const auto extrema = word & 0x9f20f400;
        if (extrema == 0x0e206400 || extrema == 0x0e20a400) { // SMAX, SMIN, UMAX, UMIN and pairwise variants
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width == 64)
                Fail("invalid ARM64 vector min or max arrangement");
            const unsigned bits = word & 0x40000000 ? 128 : 64;
            const unsigned lanes = bits / width;
            const bool pairwise = extrema == 0x0e20a400;
            const bool minimum = (word & 0x800) != 0;
            const bool isUnsigned = (word & 0x20000000) != 0;
            const auto left = GetVector(source), right = GetVector(second);
            Vector result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const auto& input = lane < lanes / 2 ? left : right;
                const auto pair = 2 * (lane % (lanes / 2));
                const auto a = pairwise ? Lane(input, pair, width) : Lane(left, lane, width);
                const auto b = pairwise ? Lane(input, pair + 1, width) : Lane(right, lane, width);
                const bool less = isUnsigned ? a < b
                                             : std::bit_cast<int64_t>(SignExtend(a, width)) <
                                                   std::bit_cast<int64_t>(SignExtend(b, width));
                SetLane(result, lane, width, less == minimum ? a : b);
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbffffc00) == 0x0e205800) { // CNT counts bits in each byte
            const unsigned count = word & 0x40000000 ? 16 : 8;
            const auto input = GetVector(source);
            Vector value{};
            for (unsigned i = 0; i < count; ++i)
                SetLane(value, i, 8, std::popcount(Lane(input, i, 8)));
            SetVector(dest, value);
            return true;
        }
        if ((word & 0xbf3ffc00) == 0x2e303800) { // UADDLV widens the sum of all lanes
            const unsigned width = 8u << ((word >> 22) & 3);
            const unsigned bits = word & 0x40000000 ? 128 : 64;
            if (width == 64 || (width == 32 && bits == 64))
                Fail("invalid ARM64 UADDLV arrangement");
            const auto input = GetVector(source);
            uint64_t sum = 0;
            for (unsigned i = 0; i < bits / width; ++i)
                sum += Lane(input, i, width);
            SetVector(dest, {sum, 0});
            return true;
        }
        if ((word & 0x9f80fc00) == 0x0f00fc00 && ((word >> 21) & 3)) { // fixed-point FCVTZS / FCVTZU
            const unsigned immediate = (word >> 16) & 127, width = std::bit_floor(immediate);
            const unsigned lanes = (word & 0x40000000 ? 128 : 64) / width;
            const bool sign = !(word & 0x20000000);
            const auto old = GetVector(source);
            Vector result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const auto bits = Lane(old, lane, width);
                const auto value = width == 64 ? std::bit_cast<double>(bits) : std::bit_cast<float>(uint32_t(bits));
                SetLane(result, lane, width,
                        ConvertFloatInteger(std::ldexp(value, 2 * width - immediate), width, sign));
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0x9f80fc00) == 0x0f00e400 && ((word >> 21) & 3)) { // fixed-point SCVTF / UCVTF
            const unsigned immediate = (word >> 16) & 127, width = std::bit_floor(immediate);
            const unsigned lanes = (word & 0x40000000 ? 128 : 64) / width;
            const int exponent = static_cast<int>(immediate) - 2 * static_cast<int>(width);
            const bool sign = !(word & 0x20000000);
            const auto old = GetVector(source);
            Vector result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const auto bits = Lane(old, lane, width);
                const auto value = sign ? static_cast<double>(std::bit_cast<int64_t>(SignExtend(bits, width)))
                                        : static_cast<double>(bits);
                const auto scaled = std::ldexp(value, exponent);
                SetLane(result, lane, width,
                        width == 64 ? std::bit_cast<uint64_t>(scaled)
                                    : std::bit_cast<uint32_t>(static_cast<float>(scaled)));
            }
            SetVector(dest, result);
            return true;
        }
        const auto vectorAlu = word & 0xbf20fc00;
        const bool q = word & 0x40000000;
        if ((word & 0xbfe08c00) == 0x0e000000) { // TBL / TBX
            const unsigned count = ((word >> 13) & 3) + 1;
            std::array<Vector, 4> table;
            for (unsigned i = 0; i < count; ++i)
                table[i] = GetVector((source + i) & 31);
            const auto indices = GetVector(second);
            Vector result = word & 0x1000 ? GetVector(dest) : Vector{};
            if (!q)
                result[1] = 0;
            for (unsigned lane = 0; lane < (q ? 16u : 8u); ++lane) {
                const auto index = Lane(indices, lane, 8);
                if (index < count * 16)
                    SetLane(result, lane, 8, Lane(table[index / 16], index % 16, 8));
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbf3ffc00) == 0x0e200800 || (word & 0xbf3ffc00) == 0x2e200800 ||
            (word & 0xbf3ffc00) == 0x0e201800) { // REV64 / REV32 / REV16
            const unsigned width = 8u << ((word >> 22) & 3);
            const unsigned block = word & 0x1000 ? 16 : word & 0x20000000 ? 32 : 64;
            if (width >= block)
                Fail("invalid ARM64 vector byte reversal");
            const auto lanesPerBlock = block / width;
            const auto input = GetVector(source);
            Vector result{};
            for (unsigned lane = 0; lane < (q ? 128u : 64u) / width; ++lane)
                SetLane(result, lane, width, Lane(input, lane ^ (lanesPerBlock - 1), width));
            SetVector(dest, result);
            return true;
        }
        const auto shiftImmediate = word & 0x9f80fc00;
        const unsigned immediateBits = (word >> 16) & 127;
        const auto shiftInsert = word & 0xaf80fc00;
        if ((shiftInsert == 0x2f005400 || shiftInsert == 0x2f004400) && immediateBits >= 8) { // SLI / SRI
            const auto width = std::bit_floor(immediateBits);
            const bool scalar = word & 0x10000000;
            if ((scalar && (!q || width != 64)) || (!scalar && !q && width == 64))
                Fail("reserved ARM64 shift-and-insert arrangement");
            const bool left = shiftInsert == 0x2f005400;
            const unsigned shift = left ? immediateBits - width : 2 * width - immediateBits;
            const auto lowMask = (uint64_t{1} << (left ? shift : width - shift)) - 1;
            const auto input = GetVector(source), old = GetVector(dest);
            const unsigned lanes = scalar ? 1 : (q ? 128 : 64) / width;
            Vector result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const auto in = Lane(input, lane, width), previous = Lane(old, lane, width);
                const auto value = left ? (previous & lowMask) | (in << shift)
                                        : (previous & ~lowMask) | (shift == width ? 0 : in >> shift);
                SetLane(result, lane, width, value);
            }
            SetVector(dest, result);
            return true;
        }
        const bool shiftLongByWidth = (word & 0xbf3ffc00) == 0x2e213800;                // SHLL / SHLL2
        if (shiftLongByWidth || (shiftImmediate == 0x0f00a400 && immediateBits >= 8)) { // SSHLL / USHLL
            const auto width = shiftLongByWidth ? 8u << ((word >> 22) & 3) : std::bit_floor(immediateBits);
            if (width >= 64)
                Fail("reserved ARM64 widening shift");
            const auto shift = shiftLongByWidth ? width : immediateBits - width;
            const auto old = GetVector(source);
            const auto start = q ? 64 / width : 0;
            Vector result{};
            for (unsigned i = 0; i < 64 / width; ++i) {
                auto lane = Lane(old, i + start, width);
                if (!(word & 0x20000000))
                    lane = SignExtend(lane, width);
                SetLane(result, i, width * 2, lane << shift);
            }
            SetVector(dest, result);
            return true;
        }
        const auto rightShift = shiftImmediate & ~0x3000u;
        const bool scalarRightShift = q && rightShift == 0x1f000400 && immediateBits >= 64;
        if ((rightShift == 0x0f000400 && immediateBits >= 8) || scalarRightShift) {
            // SSHR / USHR, SSRA / USRA, and their rounded forms.
            const auto width = std::bit_floor(immediateBits), shift = 2 * width - immediateBits;
            const auto old = GetVector(source), accum = GetVector(dest);
            const bool sign = !(word & 0x20000000);
            const bool round = word & 0x2000, accumulate = word & 0x1000;
            Vector result{};
            const auto lanes = scalarRightShift ? 1u : (q ? 128u : 64u) / width;
            for (unsigned i = 0; i < lanes; ++i) {
                const auto lane = Lane(old, i, width);
                auto value = shift < width ? lane >> shift : 0;
                if (sign)
                    value = uint64_t(std::bit_cast<int64_t>(SignExtend(lane, width)) >> std::min(shift, 63u));
                if (round)
                    value += (lane >> (shift - 1)) & 1;
                if (accumulate)
                    value += Lane(accum, i, width);
                SetLane(result, i, width, value);
            }
            SetVector(dest, result);
            return true;
        }
        const auto narrowShift = word & 0xbf80f400;
        if ((narrowShift == 0x0f008400 || narrowShift == 0x2f008400) && immediateBits >= 8) {
            // SHRN / RSHRN and SQSHRUN / SQRSHRUN, including upper halves.
            const auto width = std::bit_floor(immediateBits), shift = 2 * width - immediateBits;
            if (width >= 64)
                Fail("reserved ARM64 narrowing shift");
            const auto input = GetVector(source);
            auto result = q ? GetVector(dest) : Vector{};
            const auto start = q ? 64 / width : 0;
            const auto maximum = (int64_t{1} << width) - 1;
            for (unsigned i = 0; i < 64 / width; ++i) {
                const auto lane = Lane(input, i, width * 2);
                const auto rounding = (word & 0x800) ? (lane >> (shift - 1)) & 1 : 0;
                auto value = (lane >> shift) + rounding;
                if (narrowShift == 0x2f008400) {
                    const auto signedLane = std::bit_cast<int64_t>(SignExtend(lane, width * 2));
                    const auto shifted = (signedLane >> shift) + static_cast<int64_t>(rounding);
                    const auto saturated = std::clamp(shifted, int64_t{0}, maximum);
                    if (saturated != shifted)
                        floatEnvironment.status |= 1u << 27; // FPSR.QC
                    value = uint64_t(saturated);
                }
                SetLane(result, i + start, width, value);
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0x9f20dc00) == 0x0e204000) { // ADDHN/SUBHN, RADDHN/RSUBHN, including *2.
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width >= 64)
                Fail("reserved ARM64 high-narrow arithmetic arrangement");
            const auto left = GetVector(source), right = GetVector(second);
            auto result = q ? GetVector(dest) : Vector{};
            const auto first = q ? 64 / width : 0;
            const uint64_t rounding = word & 0x20000000 ? uint64_t{1} << (width - 1) : 0;
            for (unsigned lane = 0; lane < 64 / width; ++lane) {
                const auto a = Lane(left, lane, 2 * width), b = Lane(right, lane, 2 * width);
                const auto value = (word & 0x2000 ? a - b : a + b) + rounding;
                SetLane(result, lane + first, width, value >> width);
            }
            SetVector(dest, result);
            return true;
        }
        const auto halfAdd = vectorAlu & ~0x20000000u;
        if (halfAdd == 0x0e200400 || halfAdd == 0x0e201400) { // SHADD / UHADD / SRHADD / URHADD
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width >= 64)
                Fail("reserved ARM64 halving add width");
            const bool signedOperands = !(word & 0x20000000);
            const unsigned round = (word >> 12) & 1;
            const auto left = GetVector(source), right = GetVector(second);
            Vector result{};
            for (unsigned i = 0; i < (q ? 128u : 64u) / width; ++i) {
                auto a = Lane(left, i, width), b = Lane(right, i, width);
                if (signedOperands) {
                    a = SignExtend(a, width);
                    b = SignExtend(b, width);
                }
                const auto sum = a + b + round;
                const auto value = signedOperands ? uint64_t(std::bit_cast<int64_t>(sum) >> 1) : sum >> 1;
                SetLane(result, i, width, value);
            }
            SetVector(dest, result);
            return true;
        }
        const auto pairwiseLong = word & 0x9f3ffc00;
        if (pairwiseLong == 0x0e202800 || pairwiseLong == 0x0e206800) { // SADDLP/UADDLP, SADALP/UADALP
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width >= 64)
                Fail("reserved ARM64 pairwise widening width");
            const bool signedOperands = !(word & 0x20000000);
            const auto input = GetVector(source);
            const auto accumulator = pairwiseLong == 0x0e206800 ? GetVector(dest) : Vector{};
            Vector result{};
            for (unsigned i = 0; i < (q ? 128u : 64u) / (width * 2); ++i) {
                auto left = Lane(input, i * 2, width), right = Lane(input, i * 2 + 1, width);
                if (signedOperands) {
                    left = SignExtend(left, width);
                    right = SignExtend(right, width);
                }
                SetLane(result, i, width * 2, Lane(accumulator, i, width * 2) + left + right);
            }
            SetVector(dest, result);
            return true;
        }
        const auto widening = vectorAlu & ~0x20000000u;
        if ((widening & ~0x3000u) == 0x0e200000) { // SADDL/UADDL, SSUBL/USUBL and wide forms
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width >= 64)
                Fail("reserved ARM64 widening add or subtract");
            const auto leftInput = GetVector(source), rightInput = GetVector(second);
            const auto start = q ? 64 / width : 0;
            const bool signedOperands = !(word & 0x20000000);
            const bool wideLeft = widening & 0x1000;
            const bool subtract = widening & 0x2000;
            Vector result{};
            for (unsigned lane = 0; lane < 64 / width; ++lane) {
                auto left = wideLeft ? Lane(leftInput, lane, 2 * width) : Lane(leftInput, lane + start, width);
                auto right = Lane(rightInput, lane + start, width);
                if (signedOperands && !wideLeft)
                    left = SignExtend(left, width);
                if (signedOperands)
                    right = SignExtend(right, width);
                SetLane(result, lane, 2 * width, subtract ? left - right : left + right);
            }
            SetVector(dest, result);
            return true;
        }
        const auto indexedLane = [&](unsigned width) {
            const auto highIndex = (word >> 11) & 1, lowIndex = (word >> 21) & 1;
            const auto index =
                width == 16 ? 4 * highIndex + 2 * lowIndex + ((word >> 20) & 1) : 2 * highIndex + lowIndex;
            return Lane(GetVector(width == 16 ? second & 15 : second), index, width);
        };
        const auto wideningIndexed = word & 0x9f00f400;
        const bool multiplyByElement =
            wideningIndexed == 0x0f002000 || wideningIndexed == 0x0f006000 || wideningIndexed == 0x0f00a000;
        if (widening == 0x0e208000 || widening == 0x0e20a000 || widening == 0x0e20c000 || multiplyByElement) {
            // SMLAL / UMLAL, SMLSL / UMLSL, and SMULL / UMULL, including their upper-half forms.
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width >= 64 || (multiplyByElement && width < 16))
                Fail("reserved ARM64 widening multiply");
            const auto left = GetVector(source), right = GetVector(second), accumulator = GetVector(dest);
            const auto start = q ? 64 / width : 0;
            const bool signedOperands = !(word & 0x20000000);
            const auto element = multiplyByElement ? indexedLane(width) : 0;
            const bool add = multiplyByElement ? wideningIndexed == 0x0f002000 : widening == 0x0e208000;
            const bool subtract = multiplyByElement ? wideningIndexed == 0x0f006000 : widening == 0x0e20a000;
            Vector result{};
            for (unsigned i = 0; i < 64 / width; ++i) {
                auto l = Lane(left, i + start, width), r = multiplyByElement ? element : Lane(right, i + start, width);
                if (signedOperands) {
                    l = SignExtend(l, width);
                    r = SignExtend(r, width);
                }
                const auto product = l * r;
                const auto accumulated = Lane(accumulator, i, width * 2);
                const auto value = add ? accumulated + product : subtract ? accumulated - product : product;
                SetLane(result, i, width * 2, value);
            }
            SetVector(dest, result);
            return true;
        }
        const auto narrow = word & 0xaf3ffc00;
        if ((!(word & 0x10000000) || q) &&
            (narrow == 0x0e212800 || narrow == 0x2e212800 || narrow == 0x0e214800 || narrow == 0x2e214800)) {
            // XTN, SQXTUN, SQXTN and UQXTN, including scalar and upper-half forms.
            const bool scalar = (word & 0x10000000) != 0;
            if (scalar && narrow == 0x0e212800)
                Fail("reserved ARM64 scalar narrowing arrangement");
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width >= 64)
                Fail("reserved ARM64 narrowing width");
            const auto input = GetVector(source);
            auto result = !scalar && q ? GetVector(dest) : Vector{};
            const auto start = !scalar && q ? 64 / width : 0;
            const auto maximum = (int64_t{1} << width) - 1;
            for (unsigned i = 0; i < (scalar ? 1u : 64 / width); ++i) {
                auto value = Lane(input, i, width * 2);
                if (narrow == 0x2e214800) { // Unsigned source and result.
                    if (value > uint64_t(maximum)) {
                        value = uint64_t(maximum);
                        floatEnvironment.status |= 1u << 27;
                    }
                }
                else if (narrow != 0x0e212800) {
                    const auto inputValue = std::bit_cast<int64_t>(SignExtend(value, width * 2));
                    const auto minimum = narrow == 0x2e212800 ? int64_t{0} : -(int64_t{1} << (width - 1));
                    const auto limit = narrow == 0x2e212800 ? maximum : (int64_t{1} << (width - 1)) - 1;
                    const auto saturated = std::clamp(inputValue, minimum, limit);
                    if (saturated != inputValue)
                        floatEnvironment.status |= 1u << 27; // FPSR.QC
                    value = uint64_t(saturated);
                }
                SetLane(result, i + start, width, value);
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbfe0fc00) == 0x0e000c00) { // DUP from a general register
            const auto selector = (word >> 16) & 31;
            if (!std::has_single_bit(selector) || selector > 8)
                Fail("invalid ARM64 SIMD duplication width");
            const auto width = 8u * selector;
            const auto bits = Get(source);
            Vector result{};
            for (unsigned lane = 0; lane < (q ? 128u : 64u) / width; ++lane)
                SetLane(result, lane, width, bits);
            SetVector(dest, result);
            return true;
        }
        const auto permutation = word & 0xbf208c00;
        if (permutation == 0x0e000800 && ((word >> 12) & 3)) { // UZP / TRN / ZIP
            const auto left = GetVector(source), right = GetVector(second);
            const unsigned width = 8u << ((word >> 22) & 3);
            const unsigned lanes = (word & 0x40000000 ? 128 : 64) / width, secondHalf = (word >> 14) & 1;
            if (lanes < 2)
                Fail("invalid ARM64 vector permutation");
            Vector result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                switch ((word >> 12) & 3) {
                case 1:
                    SetLane(result, lane, width,
                            Lane(lane < lanes / 2 ? left : right, (lane % (lanes / 2)) * 2 + secondHalf, width));
                    break;
                case 2:
                    SetLane(result, lane, width, Lane(lane % 2 ? right : left, (lane / 2) * 2 + secondHalf, width));
                    break;
                case 3:
                    SetLane(result, lane, width,
                            Lane(lane % 2 ? right : left, lane / 2 + secondHalf * lanes / 2, width));
                    break;
                }
            }
            SetVector(dest, result);
            return true;
        }
        const auto moveLane = word & 0xbfe0fc00;
        if (moveLane == 0x0e003c00 || moveLane == 0x0e002c00) { // UMOV / SMOV
            const auto imm = (word >> 16) & 31;
            if (!imm)
                Fail("invalid ARM64 lane selector");
            const auto scale = std::countr_zero(imm);
            const auto size = 8u << scale;
            const bool x = word & 0x40000000;
            const unsigned width = x ? 64 : 32;
            const bool sign = moveLane == 0x0e002c00;
            if (size > 64 || (sign ? size >= width : x != (size == 64)))
                Fail("invalid ARM64 lane width");
            const auto bits = Lane(GetVector(source), imm >> (scale + 1), size);
            Set(dest, sign ? SignExtend(bits, size) : bits, width);
            return true;
        }
        const bool insertLane = (word & 0xffe08400) == 0x6e000400;
        if (insertLane || (word & 0xffe0fc00) == 0x4e001c00) {
            const unsigned imm = (word >> 16) & 31;
            if (!imm)
                Fail("invalid ARM64 lane selector");
            const auto scale = std::countr_zero(imm);
            const auto size = 8u << scale;
            if (size > 64)
                Fail("invalid ARM64 lane width");
            const auto lane = imm >> (scale + 1);
            const auto bits = insertLane ? Lane(GetVector(source), ((word >> 11) & 15) >> scale, size) : Get(source);
            auto value = GetVector(dest);
            SetLane(value, lane, size, bits);
            SetVector(dest, value);
            return true;
        }
        const bool scalarDup = (word & 0xffe0fc00) == 0x5e000400;
        if (scalarDup || (word & 0xbfe0fc00) == 0x0e000400) { // DUP from a vector lane
            const unsigned imm = (word >> 16) & 31;
            if (!imm)
                Fail("invalid ARM64 lane selector");
            const auto scale = std::countr_zero(imm);
            const auto width = 8u << scale;
            if (width > 64)
                Fail("invalid ARM64 lane width");
            const auto bits = Lane(GetVector(source), imm >> (scale + 1), width);
            Vector result{};
            const unsigned lanes = scalarDup ? 1 : (q ? 128u : 64u) / width;
            for (unsigned lane = 0; lane < lanes; ++lane)
                SetLane(result, lane, width, bits);
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbffffc00) == 0x0e617800) { // FCVTL / FCVTL2 from singles to doubles
            const auto input = GetVector(source);
            Vector result{};
            for (unsigned lane = 0; lane < 2; ++lane) {
                const auto bits = static_cast<uint32_t>(Lane(input, lane + (q ? 2 : 0), 32));
                result[lane] = std::bit_cast<uint64_t>(static_cast<double>(std::bit_cast<float>(bits)));
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbffffc00) == 0x0e616800) { // FCVTN / FCVTN2 from doubles to singles
            const auto input = GetVector(source);
            Vector result = q ? GetVector(dest) : Vector{};
            for (unsigned lane = 0; lane < 2; ++lane) {
                const auto value = static_cast<float>(std::bit_cast<double>(input[lane]));
                SetLane(result, lane + (q ? 2 : 0), 32, std::bit_cast<uint32_t>(value));
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0x9fbffc00) == 0x0e21d800) { // SCVTF / UCVTF on vector lanes
            const bool sign = !(word & 0x20000000);
            const auto input = GetVector(source);
            const unsigned halves = word & 0x40000000 ? 2 : 1;
            Vector result{};
            for (unsigned i = 0; i < halves; ++i) {
                if (wide) {
                    const auto bits = input[i];
                    result[i] = std::bit_cast<uint64_t>(sign ? static_cast<double>(std::bit_cast<int64_t>(bits))
                                                             : static_cast<double>(bits));
                    continue;
                }
                for (unsigned shift : {0u, 32u}) {
                    const auto bits = static_cast<uint32_t>(input[i] >> shift);
                    const auto value =
                        sign ? static_cast<float>(std::bit_cast<int32_t>(bits)) : static_cast<float>(bits);
                    result[i] |= uint64_t(std::bit_cast<uint32_t>(value)) << shift;
                }
            }
            SetVector(dest, result);
            return true;
        }
        const auto unaryVectorFloat = word & 0xbfbffc00;
        if (unaryVectorFloat == 0x0ea0f800) { // vector FABS
            if (wide && !q)
                return false;
            const auto input = GetVector(source);
            const uint64_t magnitude = wide ? 0x7fffffffffffffffULL : 0x7fffffff7fffffffULL;
            SetVector(dest, {input[0] & magnitude, q ? input[1] & magnitude : 0});
            return true;
        }
        if (unaryVectorFloat == 0x2ea1d800 || unaryVectorFloat == 0x0ea1d800) { // FRSQRTE / FRECPE
            if (wide && !q)
                return false;
            const unsigned width = wide ? 64 : 32;
            const bool squareRoot = unaryVectorFloat == 0x2ea1d800;
            const auto input = GetVector(source);
            Vector result{};
            for (unsigned lane = 0; lane < (q ? 128u : 64u) / width; ++lane) {
                const auto element = Lane(input, lane, width);
                const auto estimate = wide ? ReciprocalEstimate(std::bit_cast<double>(element), squareRoot)
                                           : ReciprocalEstimate(std::bit_cast<float>(uint32_t(element)), squareRoot);
                SetLane(result, lane, width,
                        wide ? std::bit_cast<uint64_t>(estimate)
                             : std::bit_cast<uint32_t>(static_cast<float>(estimate)));
            }
            SetVector(dest, result);
            return true;
        }
        const auto vectorFloat = word & 0xbfa0fc00;
        if ((word & 0xffa0fc00) == 0x7ea0d400) { // FABD Sd, Sn, Sm / Dd, Dn, Dm
            const auto left = GetVector(source)[0], right = GetVector(second)[0];
            const auto result =
                wide ? std::bit_cast<uint64_t>(std::abs(std::bit_cast<double>(left) - std::bit_cast<double>(right)))
                     : uint64_t(std::bit_cast<uint32_t>(
                           std::abs(std::bit_cast<float>(uint32_t(left)) - std::bit_cast<float>(uint32_t(right)))));
            SetVector(dest, {result, 0});
            return true;
        }
        if ((word & 0xffbffc00) == 0x7e30d800) { // FADDP Sd, Vn.2S / Dd, Vn.2D
            const auto input = GetVector(source);
            const auto result =
                wide ? std::bit_cast<uint64_t>(std::bit_cast<double>(input[0]) + std::bit_cast<double>(input[1]))
                     : uint64_t(std::bit_cast<uint32_t>(std::bit_cast<float>(uint32_t(input[0])) +
                                                        std::bit_cast<float>(uint32_t(input[0] >> 32))));
            SetVector(dest, {result, 0});
            return true;
        }
        if ((word & 0xbfbffc00) == 0x2ea0f800) { // FNEG (vector)
            if (wide && !q)
                return false;
            const auto input = GetVector(source);
            const uint64_t sign = wide ? 0x8000000000000000ull : 0x8000000080000000ull;
            SetVector(dest, {input[0] ^ sign, q ? input[1] ^ sign : 0});
            return true;
        }
        if (unaryVectorFloat == 0x0e219800 || unaryVectorFloat == 0x0e218800 ||
            unaryVectorFloat == 0x2e218800) { // FRINTM / FRINTN / FRINTA (vector)
            if (wide && !q)
                Fail("reserved ARM64 vector round-down arrangement");
            const auto input = GetVector(source);
            const unsigned width = wide ? 64 : 32;
            const auto rounding = unaryVectorFloat == 0x0e219800   ? FloatIntegralRounding::Floor
                                  : unaryVectorFloat == 0x2e218800 ? FloatIntegralRounding::NearestAway
                                                                   : FloatIntegralRounding::NearestEven;
            Vector result{};
            for (unsigned lane = 0; lane < (q ? 128u : 64u) / width; ++lane)
                SetLane(result, lane, width, RoundFloatIntegralBits(Lane(input, lane, width), width, rounding));
            SetVector(dest, result);
            return true;
        }
        if (unaryVectorFloat == 0x0ea18800 || unaryVectorFloat == 0x2ea1f800) { // FRINTP / FSQRT
            if (wide && !q)
                return false;
            const auto input = GetVector(source);
            const unsigned width = wide ? 64 : 32;
            const auto operation = [unaryVectorFloat](auto value) {
                return unaryVectorFloat == 0x0ea18800 ? std::ceil(value) : std::sqrt(value);
            };
            Vector result{};
            for (unsigned lane = 0; lane < (q ? 128u : 64u) / width; ++lane) {
                const auto element = Lane(input, lane, width);
                const auto value = wide ? std::bit_cast<uint64_t>(operation(std::bit_cast<double>(element)))
                                        : std::bit_cast<uint32_t>(operation(std::bit_cast<float>(uint32_t(element))));
                SetLane(result, lane, width, value);
            }
            SetVector(dest, result);
            return true;
        }
        const auto zeroComparison = word & 0xbfbffc00;
        const bool compareZero = zeroComparison == 0x0ea0c800 || zeroComparison == 0x2ea0c800 ||
                                 zeroComparison == 0x0ea0d800 || zeroComparison == 0x2ea0d800 ||
                                 zeroComparison == 0x0ea0e800;
        if (vectorFloat == 0x2ea0e400 || vectorFloat == 0x2e20e400 || vectorFloat == 0x0e20e400 ||
            compareZero) { // FCMGT / FCMGE / FCMEQ / FCMLE / FCMLT
            if (wide && !q)
                Fail("reserved ARM64 vector floating-point comparison arrangement");
            const auto left = GetVector(source), right = compareZero ? Vector{} : GetVector(second);
            const unsigned width = wide ? 64 : 32, lanes = (q ? 128u : 64u) / width;
            const auto comparison = compareZero ? zeroComparison : vectorFloat;
            Vector result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const auto l = Lane(left, lane, width), r = Lane(right, lane, width);
                const auto x = wide ? std::bit_cast<double>(l) : double(std::bit_cast<float>(uint32_t(l)));
                const auto y = wide ? std::bit_cast<double>(r) : double(std::bit_cast<float>(uint32_t(r)));
                bool selected;
                switch (comparison) {
                case 0x2ea0e400:
                case 0x0ea0c800:
                    selected = x > y;
                    break;
                case 0x2e20e400:
                case 0x2ea0c800:
                    selected = x >= y;
                    break;
                case 0x2ea0d800:
                    selected = x <= y;
                    break;
                case 0x0ea0e800:
                    selected = x < y;
                    break;
                default:
                    selected = x == y;
                    break;
                }
                SetLane(result, lane, width, selected ? UINT64_MAX : 0);
            }
            SetVector(dest, result);
            return true;
        }
        if (vectorFloat == 0x2e20fc00 || vectorFloat == 0x2e20dc00 || vectorFloat == 0x2ea0d400 ||
            vectorFloat == 0x0e20d400 || vectorFloat == 0x0ea0d400 || vectorFloat == 0x0e20cc00 ||
            vectorFloat == 0x0ea0cc00 || vectorFloat == 0x2e20d400 || vectorFloat == 0x0e20fc00 ||
            vectorFloat == 0x0ea0fc00) {
            if ((vectorFloat == 0x2ea0d400 || vectorFloat == 0x0ea0fc00) && wide && !q)
                return false;
            const auto left = GetVector(source), right = GetVector(second), old = GetVector(dest);
            const unsigned halves = word & 0x40000000 ? 2 : 1;
            const auto operation = [vectorFloat](auto x, auto y, auto addend) {
                if (vectorFloat == 0x2e20fc00)
                    return x / y;
                if (vectorFloat == 0x2e20dc00)
                    return x * y;
                if (vectorFloat == 0x2ea0d400)
                    return std::abs(x - y);      // FABD
                if (vectorFloat == 0x0e20fc00) { // FRECPS
                    using Float = decltype(x);
                    if ((x == 0 && std::isinf(y)) || (y == 0 && std::isinf(x)))
                        return Float{2};
                    return std::fma(-x, y, Float{2});
                }
                if (vectorFloat == 0x0ea0fc00) { // FRSQRTS
                    using Float = decltype(x);
                    if ((x == 0 && std::isinf(y)) || (y == 0 && std::isinf(x)))
                        return Float{1.5};
                    // Halve the larger operand to avoid losing subnormal bits before the fused step.
                    if (std::abs(x) >= std::abs(y))
                        x *= Float{0.5};
                    else
                        y *= Float{0.5};
                    return std::fma(-x, y, Float{1.5});
                }
                if (vectorFloat == 0x0e20cc00 || vectorFloat == 0x0ea0cc00)
                    return std::fma(vectorFloat == 0x0ea0cc00 ? -x : x, y, addend);
                return vectorFloat == 0x0ea0d400 ? x - y : x + y;
            };
            const unsigned width = wide ? 64 : 32, lanes = halves * 64 / width;
            const bool pair = vectorFloat == 0x2e20d400;
            if (pair && lanes < 2)
                Fail("invalid ARM64 floating-point pair");
            Vector result{};
            for (unsigned i = 0; i < lanes; ++i) {
                const auto& input = i < lanes / 2 ? left : right;
                const auto lane = pair ? (i % (lanes / 2)) * 2 : i;
                const auto l = Lane(pair ? input : left, lane, width),
                           r = Lane(pair ? input : right, pair ? lane + 1 : i, width);
                const auto addend = Lane(old, i, width);
                if (wide) {
                    SetLane(result, i, width,
                            std::bit_cast<uint64_t>(operation(std::bit_cast<double>(l), std::bit_cast<double>(r),
                                                              std::bit_cast<double>(addend))));
                }
                else {
                    SetLane(result, i, width,
                            std::bit_cast<uint32_t>(operation(std::bit_cast<float>(uint32_t(l)),
                                                              std::bit_cast<float>(uint32_t(r)),
                                                              std::bit_cast<float>(uint32_t(addend)))));
                }
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbf3ffc00) == 0x2e204800) { // CLZ (vector)
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width == 64)
                Fail("reserved ARM64 vector leading zero count arrangement");
            const auto input = GetVector(source);
            const unsigned laneCount = (q ? 128u : 64u) / width;
            Vector result{};
            for (unsigned lane = 0; lane < laneCount; ++lane) {
                const auto zeros = std::countl_zero(Lane(input, lane, width)) - (64 - width);
                SetLane(result, lane, width, zeros);
            }
            SetVector(dest, result);
            return true;
        }
        if (vectorAlu == 0x0e20bc00) { // ADDP (vector)
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width == 64 && !q)
                Fail("reserved ARM64 integer pairwise addition arrangement");
            const auto left = GetVector(source), right = GetVector(second);
            const unsigned pairs = (q ? 64u : 32u) / width;
            Vector result{};
            for (unsigned pair = 0; pair < pairs; ++pair) {
                const auto lane = pair * 2;
                SetLane(result, pair, width, Lane(left, lane, width) + Lane(left, lane + 1, width));
                SetLane(result, pair + pairs, width, Lane(right, lane, width) + Lane(right, lane + 1, width));
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbf3ffc00) == 0x0e31b800) { // ADDV
            const unsigned size = 8u << ((word >> 22) & 3);
            const unsigned vectorSize = word & 0x40000000 ? 128 : 64;
            if (size == 64 || (size == 32 && vectorSize == 64))
                return false;
            const auto input = GetVector(source);
            uint64_t sum = 0;
            for (unsigned lane = 0; lane < vectorSize / size; ++lane)
                sum += Lane(input, lane, size);
            SetVector(dest, {sum & (UINT64_MAX >> (64 - size)), 0});
            return true;
        }
        if ((word & 0x9f3efc00) == 0x0e30a800) { // SMAXV / SMINV / UMAXV / UMINV
            const unsigned size = 8u << ((word >> 22) & 3);
            if (size == 64 || (size == 32 && !q))
                Fail("reserved ARM64 integer min/max reduction arrangement");
            const auto input = GetVector(source);
            const unsigned count = (q ? 128u : 64u) / size;
            const bool minimum = word & 0x00010000;
            const auto sign = word & 0x20000000 ? uint64_t{0} : uint64_t{1} << (size - 1);
            auto result = Lane(input, 0, size) ^ sign;
            for (unsigned i = 1; i < count; ++i) {
                const auto lane = Lane(input, i, size) ^ sign;
                result = minimum ? std::min(result, lane) : std::max(result, lane);
            }
            SetVector(dest, {result ^ sign, 0});
            return true;
        }
        const bool shiftLeft = (word & 0xbf80fc00) == 0x0f005400 && ((word >> 19) & 15);
        const bool multiplyAccumulate = (word & 0x9f20fc00) == 0x0e209400 && ((word >> 22) & 3) != 3;
        const auto orderedComparison = vectorAlu & ~0x20000000u;
        const bool compareOrdered = orderedComparison == 0x0e203400 || orderedComparison == 0x0e203c00;
        const bool saturatingShift = orderedComparison == 0x0e204c00; // SQSHL / UQSHL (register)
        const bool variableShift = saturatingShift || orderedComparison == 0x0e204400;
        const bool saturatingArithmetic = (orderedComparison & ~0x2000u) == 0x0e200c00; // SQ/UQ ADD/SUB
        const bool absoluteDifference = orderedComparison == 0x0e207400;                // SABD / UABD
        if (shiftLeft || vectorAlu == 0x0e208400 || vectorAlu == 0x2e208400 || compareOrdered ||
            vectorAlu == 0x2e208c00 || vectorAlu == 0x0e209c00 || saturatingArithmetic || absoluteDifference ||
            vectorAlu == 0x0e208c00 || variableShift || multiplyAccumulate) {
            const auto left = GetVector(source), right = GetVector(second);
            const auto old = multiplyAccumulate ? GetVector(dest) : Vector{};
            const unsigned count = word & 0x40000000 ? 2 : 1;
            const unsigned immediate = (word >> 16) & 127;
            const unsigned size = shiftLeft ? std::bit_floor(immediate) : 8u << ((word >> 22) & 3);
            if (saturatingShift && size == 64 && !q)
                Fail("reserved ARM64 saturating shift arrangement");
            if ((saturatingArithmetic && size == 64 && !q) || (absoluteDifference && size == 64))
                Fail("reserved ARM64 vector arithmetic arrangement");
            const auto mask = UINT64_MAX >> (64 - size);
            // Flipping the sign bit puts signed lanes in unsigned order.
            const auto sign = word & 0x20000000 ? uint64_t{0} : uint64_t{1} << (size - 1);
            Vector result{};
            for (unsigned i = 0; i < count; ++i) {
                for (unsigned shift = 0; shift < 64; shift += size) {
                    const auto l = (left[i] >> shift) & mask, r = (right[i] >> shift) & mask;
                    uint64_t value;
                    if (shiftLeft)
                        value = l << (immediate - size);
                    else if (vectorAlu == 0x0e208400)
                        value = l + r;
                    else if (vectorAlu == 0x2e208400)
                        value = l - r;
                    else if (saturatingArithmetic) {
                        const bool subtract = word & 0x2000;
                        value = subtract ? l - r : l + r;
                        const bool overflow = sign       ? ((l ^ value) & (subtract ? l ^ r : ~(l ^ r)) & sign) != 0
                                              : subtract ? l < r
                                                         : l > mask - r;
                        if (overflow) {
                            floatEnvironment.status |= 1u << 27; // FPSR.QC
                            value = sign ? (l & sign ? sign : sign - 1) : subtract ? 0 : mask;
                        }
                    }
                    else if (absoluteDifference) {
                        const auto x = l ^ sign, y = r ^ sign;
                        value = x >= y ? x - y : y - x;
                    }
                    else if (compareOrdered) { // CMGT / CMGE / CMHI / CMHS
                        const auto x = l ^ sign, y = r ^ sign;
                        value = (x > y || (orderedComparison == 0x0e203c00 && x == y)) ? mask : 0;
                    }
                    else if (vectorAlu == 0x0e208c00)
                        value = (l & r) ? mask : 0; // CMTST
                    else if (vectorAlu == 0x0e209c00)
                        value = l * r;
                    else if (multiplyAccumulate) {
                        const auto lane = (old[i] >> shift) & mask;
                        value = word & 0x20000000 ? lane - l * r : lane + l * r;
                    }
                    else if (variableShift) {
                        const auto amount = std::bit_cast<int8_t>(uint8_t(r));
                        const auto signedValue = std::bit_cast<int64_t>(SignExtend(l, size));
                        if (amount < 0) {
                            value = sign ? uint64_t(signedValue >> std::min(-amount, 63))
                                         : (-amount < int(size) ? l >> -amount : 0);
                        }
                        else if (!saturatingShift || !l) {
                            value = amount < int(size) ? l << amount : 0;
                        }
                        else {
                            const auto maximum = static_cast<int64_t>(mask >> 1);
                            const auto minimum = -maximum - 1;
                            const bool overflow =
                                amount >= int(size) ||
                                (sign ? (signedValue > (maximum >> amount) || signedValue < (minimum >> amount))
                                      : l > (mask >> amount));
                            if (overflow) {
                                value = sign ? (signedValue < 0 ? sign : sign - 1) : mask;
                                floatEnvironment.status |= 1u << 27; // FPSR.QC
                            }
                            else
                                value = l << amount;
                        }
                    }
                    else
                        value = l == r ? mask : 0;
                    result[i] |= (value & mask) << shift;
                }
            }
            SetVector(dest, result);
            return true;
        }
        if (vectorAlu == 0x2e201c00) { // EOR, BSL, BIT, BIF
            const auto left = GetVector(source), right = GetVector(second), old = GetVector(dest);
            const unsigned count = word & 0x40000000 ? 2 : 1;
            Vector result{};
            for (unsigned i = 0; i < count; ++i) {
                switch ((word >> 22) & 3) {
                case 0:
                    result[i] = left[i] ^ right[i];
                    break;
                case 1:
                    result[i] = (left[i] & old[i]) | (right[i] & ~old[i]);
                    break;
                case 2:
                    result[i] = (left[i] & right[i]) | (old[i] & ~right[i]);
                    break;
                case 3:
                    result[i] = (left[i] & ~right[i]) | (old[i] & right[i]);
                    break;
                }
            }
            SetVector(dest, result);
            return true;
        }
        const auto scalar = [&](unsigned reg) {
            const auto bits = GetVector(reg)[0];
            return wide ? std::bit_cast<double>(bits) : static_cast<double>(std::bit_cast<float>(uint32_t(bits)));
        };
        const auto convert = word & 0x7fbffc00;
        if ((word & 0xfffffc00) == 0x1e624000) { // FCVT Sd, Dn
            SetVector(dest, {std::bit_cast<uint32_t>(static_cast<float>(scalar(source))), 0});
            return true;
        }
        if ((word & 0xfffffc00) == 0x1e22c000) { // FCVT Dd, Sn
            SetVector(dest, {std::bit_cast<uint64_t>(scalar(source)), 0});
            return true;
        }
        const bool scalarNearestInteger = (word & 0xdfbffc00) == 0x5e21a800;
        if (scalarNearestInteger || (word & 0x9fbffc00) == 0x0e21a800) { // FCVTNS / FCVTNU, ties to even.
            const unsigned width = wide ? 64 : 32, bits = q ? 128 : 64;
            if (!scalarNearestInteger && wide && !q)
                Fail("reserved ARM64 nearest integer arrangement");
            const auto input = GetVector(source);
            Vector result{};
            for (unsigned lane = 0; lane < (scalarNearestInteger ? 1 : bits / width); ++lane)
                SetLane(result, lane, width,
                        FloatToNearestIntegerBits(Lane(input, lane, width), width, !(word & 0x20000000)));
            SetVector(dest, result);
            return true;
        }
        const bool scalarFloatToInt = (word & 0xdfbffc00) == 0x5ea1b800;
        if (scalarFloatToInt || (word & 0x9fbffc00) == 0x0ea1b800) { // FCVTZS / FCVTZU
            const unsigned width = wide ? 64 : 32;
            const unsigned bits = word & 0x40000000 ? 128 : 64;
            if (!scalarFloatToInt && wide && bits == 64)
                return false;
            const unsigned lanes = scalarFloatToInt ? 1 : bits / width;
            const auto input = GetVector(source);
            Vector result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const auto element = Lane(input, lane, width);
                const auto value =
                    wide ? std::bit_cast<double>(element) : double(std::bit_cast<float>(uint32_t(element)));
                SetLane(result, lane, width, ConvertFloatInteger(std::trunc(value), width, !(word & 0x20000000)));
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xdfbffc00) == 0x5e21d800) { // SCVTF / UCVTF with scalar SIMD operands
            const auto bits = GetVector(source)[0];
            const bool sign = (word & 0x20000000) == 0;
            const auto result =
                wide ? std::bit_cast<uint64_t>(sign ? static_cast<double>(std::bit_cast<int64_t>(bits))
                                                    : static_cast<double>(bits))
                     : std::bit_cast<uint32_t>(sign ? static_cast<float>(std::bit_cast<int32_t>(uint32_t(bits)))
                                                    : static_cast<float>(uint32_t(bits)));
            SetVector(dest, {result, 0});
            return true;
        }
        const auto fixedConvert = word & 0x7fbf0000;
        if (fixedConvert == 0x1e180000 || fixedConvert == 0x1e190000) { // FCVTZS / FCVTZU, fixed-point.
            const unsigned width = word >> 31 ? 64 : 32;
            const unsigned fractionalBits = 64 - ((word >> 10) & 63);
            if (fractionalBits > width)
                Fail("invalid ARM64 fixed-point conversion size");
            const bool sign = (word & 0x10000) == 0;
            Set(dest, ConvertFloatInteger(std::ldexp(scalar(source), fractionalBits), width, sign), width);
            return true;
        }
        if (convert == 0x1e220000 || convert == 0x1e230000 || fixedConvert == 0x1e020000 ||
            fixedConvert == 0x1e030000) {
            const auto width = word >> 31 ? 64u : 32u;
            const auto bits = Get(source, width);
            const auto integer = std::bit_cast<int64_t>(SignExtend(bits, width));
            const bool sign = (word & 0x10000) == 0;
            const int exponent = (word & 0x00200000) ? 0 : static_cast<int>((word >> 10) & 63) - 64;
            if (-exponent > static_cast<int>(width))
                Fail("invalid ARM64 fixed-point conversion size");
            const auto result =
                wide ? std::bit_cast<uint64_t>(
                           std::ldexp(sign ? static_cast<double>(integer) : static_cast<double>(bits), exponent))
                     : std::bit_cast<uint32_t>(
                           std::ldexp(sign ? static_cast<float>(integer) : static_cast<float>(bits), exponent));
            SetVector(dest, {result, 0});
            return true;
        }
        const auto rounding = convert & ~0x10000u;
        if (rounding == 0x1e380000 || rounding == 0x1e240000 || rounding == 0x1e280000 || rounding == 0x1e300000) {
            const auto width = word >> 31 ? 64u : 32u;
            const auto original = scalar(source);
            const auto value = rounding == 0x1e380000   ? std::trunc(original)
                               : rounding == 0x1e240000 ? std::round(original)
                               : rounding == 0x1e280000 ? std::ceil(original)
                                                        : std::floor(original);
            const bool sign = (convert & 0x10000) == 0;
            Set(dest, ConvertFloatInteger(value, width, sign), width);
            return true;
        }
        const auto unary = word & 0xffbffc00;
        if (unary == 0x1e244000 || unary == 0x1e264000) { // FRINTN / FRINTA (scalar).
            const unsigned width = wide ? 64 : 32;
            SetVector(dest, {RoundFloatIntegralBits(Lane(GetVector(source), 0, width), width,
                                                    unary == 0x1e264000 ? FloatIntegralRounding::NearestAway
                                                                        : FloatIntegralRounding::NearestEven),
                             0});
            return true;
        }
        if (unary == 0x1e20c000 || unary == 0x1e214000 || unary == 0x1e21c000 || unary == 0x1e254000 ||
            unary == 0x1e25c000 || unary == 0x1e24c000 || unary == 0x1e274000) {
            const auto value = scalar(source);
            double result;
            switch (unary) {
            case 0x1e20c000:
                result = std::abs(value);
                break;
            case 0x1e214000:
                result = -value;
                break;
            case 0x1e21c000:
                result = std::sqrt(value);
                break;
            case 0x1e254000:
                result = std::floor(value);
                break;
            case 0x1e25c000:
                result = std::trunc(value);
                break;
            case 0x1e274000: // FRINTX uses the current FPCR rounding mode.
                switch ((floatEnvironment.control >> 22) & 3) {
                case 0:
                    result = std::nearbyint(value);
                    break;
                case 1:
                    result = std::ceil(value);
                    break;
                case 2:
                    result = std::floor(value);
                    break;
                default:
                    result = std::trunc(value);
                    break;
                }
                if (result != value)
                    floatEnvironment.status |= 1u << 4;
                break;
            default:
                result = std::ceil(value);
                break;
            }
            SetVector(
                dest,
                {wide ? std::bit_cast<uint64_t>(result) : std::bit_cast<uint32_t>(static_cast<float>(result)), 0});
            return true;
        }
        const unsigned arithmetic = (word >> 12) & 15;
        if ((word & 0xffa00c00) == 0x1e200800 && arithmetic <= 8) {
            const auto operation = [arithmetic](auto left, auto right) {
                switch (arithmetic) {
                case 0:
                    return left * right;
                case 1:
                    return left / right;
                case 2:
                    return left + right;
                case 3:
                    return left - right;
                case 4:
                case 5:
                case 6:
                case 7:
                    return FloatMinMax(left, right, (arithmetic & 1) != 0, arithmetic >= 6);
                default:
                    return -(left * right);
                }
            };
            const auto left = GetVector(source)[0], right = GetVector(second)[0];
            const auto result =
                wide ? std::bit_cast<uint64_t>(operation(std::bit_cast<double>(left), std::bit_cast<double>(right)))
                     : std::bit_cast<uint32_t>(
                           operation(std::bit_cast<float>(uint32_t(left)), std::bit_cast<float>(uint32_t(right))));
            SetVector(dest, {result, 0});
            return true;
        }
        if ((word & 0xff800000) == 0x1f000000) { // FMADD, FMSUB, FNMADD, FNMSUB
            const auto accumulate = [word](auto left, auto right, auto addend) {
                const bool negative = word & 0x00200000;
                const bool subtract = word & 0x00008000;
                return std::fma(negative != subtract ? -left : left, right, negative ? -addend : addend);
            };
            const auto left = GetVector(source)[0], right = GetVector(second)[0],
                       addend = GetVector((word >> 10) & 31)[0];
            const auto result =
                wide ? std::bit_cast<uint64_t>(accumulate(std::bit_cast<double>(left), std::bit_cast<double>(right),
                                                          std::bit_cast<double>(addend)))
                     : std::bit_cast<uint32_t>(accumulate(std::bit_cast<float>(uint32_t(left)),
                                                          std::bit_cast<float>(uint32_t(right)),
                                                          std::bit_cast<float>(uint32_t(addend))));
            SetVector(dest, {result, 0});
            return true;
        }
        if ((word & 0xbf00f400) == 0x0f008000) { // Vector MUL by element
            const auto width = 8u << ((word >> 22) & 3);
            if (width != 16 && width != 32)
                Fail("reserved ARM64 indexed multiply width");
            const auto left = GetVector(source);
            const auto factor = indexedLane(width);
            Vector result{};
            for (unsigned lane = 0; lane < (q ? 128u : 64u) / width; ++lane)
                SetLane(result, lane, width, Lane(left, lane, width) * factor);
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbf00f400) == 0x0f00d000) { // Vector SQRDMULH by element
            const auto width = 8u << ((word >> 22) & 3);
            if (width != 16 && width != 32)
                Fail("reserved ARM64 indexed multiply width");
            const auto left = GetVector(source);
            const auto factor = std::bit_cast<int64_t>(SignExtend(indexedLane(width), width));
            const auto half = int64_t{1} << (width - 2);
            const auto maximum = 2 * half - 1;
            Vector result{};
            for (unsigned lane = 0; lane < (q ? 128u : 64u) / width; ++lane) {
                const auto value = std::bit_cast<int64_t>(SignExtend(Lane(left, lane, width), width));
                const auto rounded = (value * factor + half) >> (width - 1);
                if (rounded > maximum)
                    floatEnvironment.status |= 1u << 27; // FPSR.QC
                SetLane(result, lane, width, uint64_t(std::min(rounded, maximum)));
            }
            SetVector(dest, result);
            return true;
        }
        const auto indexed = word & 0xaf00f400;
        if (indexed == 0x0f009000 || indexed == 0x0f001000 || indexed == 0x0f005000) {
            if (!(word & 0x00800000))
                Fail("unsupported ARM64 half-precision indexed arithmetic");
            const bool scalarResult = word & 0x10000000;
            const bool lowIndex = word & 0x00200000;
            if ((scalarResult && !q) || (wide && (!q || lowIndex)))
                Fail("reserved ARM64 floating-point indexed arithmetic arrangement");
            const auto left = GetVector(source), old = GetVector(dest);
            const unsigned width = wide ? 64 : 32;
            const unsigned lanes = scalarResult ? 1 : (q ? 128u : 64u) / width;
            const unsigned highIndex = (word >> 11) & 1;
            const unsigned index = wide ? highIndex : 2 * highIndex + lowIndex;
            const auto right = Lane(GetVector(second), index, width);
            const auto operation = [indexed](auto x, auto y, auto previous) {
                return indexed == 0x0f009000 ? x * y : std::fma(indexed == 0x0f005000 ? -x : x, y, previous);
            };
            Vector result{};
            for (unsigned i = 0; i < lanes; ++i) {
                const auto x = Lane(left, i, width), previous = Lane(old, i, width);
                const auto bits =
                    wide ? std::bit_cast<uint64_t>(operation(std::bit_cast<double>(x), std::bit_cast<double>(right),
                                                             std::bit_cast<double>(previous)))
                         : std::bit_cast<uint32_t>(operation(std::bit_cast<float>(uint32_t(x)),
                                                             std::bit_cast<float>(uint32_t(right)),
                                                             std::bit_cast<float>(uint32_t(previous))));
                SetLane(result, i, width, bits);
            }
            SetVector(dest, result);
            return true;
        }
        const bool conditionalCompare = (word & 0xffa00c00) == 0x1e200400; // FCCMP and FCCMPE
        if (conditionalCompare || (word & 0xffa0fc07) == 0x1e202000) {     // FCMP and FCMPE
            if (conditionalCompare && !Condition((word >> 12) & 15)) {
                SetConditionFlags(word);
                return true;
            }
            const auto left = scalar(source), right = !conditionalCompare && (word & 8) ? 0.0 : scalar(second);
            const bool unordered = std::isunordered(left, right);
            n = !unordered && left < right;
            z = !unordered && left == right;
            c = !n;
            v = unordered;
            flagsValid = true;
            return true;
        }
        if ((word & 0xffa00c00) == 0x1e200c00) { // FCSEL
            const auto bits = GetVector(Condition((word >> 12) & 15) ? source : second)[0];
            SetVector(dest, {wide ? bits : uint32_t(bits), 0});
            return true;
        }
        if ((word & 0xffa01fe0) == 0x1e201000) { // FMOV immediate
            SetVector(dest, {FloatImmediate((word >> 13) & 255, wide), 0});
            return true;
        }
        const auto vectorMinMax = word & 0x9f20fc00;
        if (vectorMinMax == 0x0e20c400 || vectorMinMax == 0x0e20f400) { // FMAX(NM) / FMIN(NM), including pairwise
            if (wide && !q)
                Fail("reserved ARM64 vector min/max arrangement");
            const auto left = GetVector(source), right = GetVector(second);
            const unsigned width = wide ? 64 : 32;
            const unsigned lanes = (q ? 128 : 64) / width;
            const bool minimum = word & 0x00800000;
            const bool number = vectorMinMax == 0x0e20c400;
            const bool pairwise = word & 0x20000000;
            Vector result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const auto& input = lane < lanes / 2 ? left : right;
                const auto pair = 2 * (lane % (lanes / 2));
                const auto l = pairwise ? Lane(input, pair, width) : Lane(left, lane, width);
                const auto r = pairwise ? Lane(input, pair + 1, width) : Lane(right, lane, width);
                const auto bits =
                    wide ? std::bit_cast<uint64_t>(
                               FloatMinMax(std::bit_cast<double>(l), std::bit_cast<double>(r), minimum, number))
                         : std::bit_cast<uint32_t>(FloatMinMax(std::bit_cast<float>(uint32_t(l)),
                                                               std::bit_cast<float>(uint32_t(r)), minimum, number));
                SetLane(result, lane, width, bits);
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0x9ff80c00) == 0x0f000400) {
            const unsigned imm = ((word >> 11) & 0xe0) | ((word >> 5) & 31);
            const unsigned mode = (word >> 12) & 15;
            const bool op = word & 0x20000000;
            uint64_t bits = 0;
            if (mode == 15) {
                bits = FloatImmediate(imm, op);
                if (!op)
                    bits |= bits << 32;
            }
            else if (mode == 14) {
                if (op) {
                    for (unsigned i = 0; i < 8; ++i)
                        if ((imm >> i) & 1)
                            bits |= uint64_t{255} << (i * 8);
                }
                else {
                    bits = uint64_t(imm) * 0x0101010101010101ull;
                }
            }
            else if (mode >= 12) {
                const unsigned shift = mode == 12 ? 8 : 16;
                const auto value = (imm << shift) | ((1u << shift) - 1);
                bits = uint64_t(value) * 0x0000000100000001ull;
            }
            else if (mode >= 8) {
                bits = uint64_t(imm << ((mode & 2) * 4)) * 0x0001000100010001ull;
            }
            else {
                bits = uint64_t(imm << ((mode & 6) * 4)) * 0x0000000100000001ull;
            }
            const unsigned lanes = word & 0x40000000 ? 2 : 1;
            Vector result{};
            const auto old = GetVector(dest);
            for (unsigned i = 0; i < lanes; ++i) {
                if (mode < 12 && (mode & 1))
                    result[i] = op ? old[i] & ~bits : old[i] | bits;
                else
                    result[i] = op && mode < 14 ? ~bits : bits;
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbf20fc00) == 0x0e201c00) { // AND, BIC, ORR, ORN
            const auto left = GetVector(source), right = GetVector(second);
            Vector result{};
            const unsigned lanes = (word & 0x40000000) ? 2 : 1;
            for (unsigned i = 0; i < lanes; ++i) {
                const auto value = word & 0x00400000 ? ~right[i] : right[i];
                result[i] = word & 0x00800000 ? left[i] | value : left[i] & value;
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbffffc00) == 0x2e205800) { // NOT
            const auto value = GetVector(source);
            SetVector(dest, {~value[0], q ? ~value[1] : 0});
            return true;
        }
        if ((word & 0xbffffc00) == 0x2e605800) { // RBIT (vector)
            const auto value = GetVector(source);
            SetVector(dest, {ReverseBitsInBytes(value[0]), q ? ReverseBitsInBytes(value[1]) : 0});
            return true;
        }
        const auto integerUnary = word & 0xbf3ffc00;
        if (integerUnary == 0x2e20b800 || integerUnary == 0x0e20b800) { // NEG / ABS (vector)
            const unsigned width = 8u << ((word >> 22) & 3);
            if (width == 64 && !q)
                Fail("reserved ARM64 vector negate or absolute value");
            const auto value = GetVector(source);
            const auto sign = uint64_t{1} << (width - 1);
            Vector result{};
            for (unsigned lane = 0; lane < (q ? 128u : 64u) / width; ++lane) {
                const auto bits = Lane(value, lane, width);
                SetLane(result, lane, width, integerUnary == 0x2e20b800 || (bits & sign) ? uint64_t{0} - bits : bits);
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xfffffc00) == 0x5ef1b800) { // ADDP Dd, Vn.2D
            const auto value = GetVector(source);
            SetVector(dest, {value[0] + value[1], 0});
            return true;
        }
        if (vectorAlu == 0x0e200c00 || vectorAlu == 0x2e200c00) { // SQADD / UQADD
            const auto left = GetVector(source), right = GetVector(second);
            const unsigned width = 8u << ((word >> 22) & 3);
            const auto max = UINT64_MAX >> (64 - width), sign = (max >> 1) + 1;
            Vector result{};
            for (unsigned lane = 0; lane < (q ? 128u : 64u) / width; ++lane) {
                const auto l = Lane(left, lane, width), r = Lane(right, lane, width);
                auto value = (l + r) & max;
                if (vectorAlu == 0x0e200c00) {
                    if ((~(l ^ r) & (l ^ value) & sign) != 0)
                        value = l & sign ? sign : sign - 1;
                }
                else if (value < l) {
                    value = max;
                }
                SetLane(result, lane, width, value);
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xffe0fc00) == 0x6ee04400) { // USHL .2D
            const auto value = GetVector(source), shifts = GetVector(second);
            Vector result{};
            for (unsigned i = 0; i < 2; ++i) {
                const auto shift = std::bit_cast<int8_t>(uint8_t(shifts[i]));
                if (shift >= 0 && shift < 64)
                    result[i] = value[i] << shift;
                if (shift < 0 && shift > -64)
                    result[i] = value[i] >> -shift;
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbfe0fc00) == 0x0e20e000) { // PMULL / PMULL2 .8H, .8B/.16B
            const auto left = GetVector(source), right = GetVector(second);
            Vector result{};
            for (unsigned lane = 0; lane < 8; ++lane) {
                const auto a = Lane(left, lane + (q ? 8 : 0), 8);
                const auto b = Lane(right, lane + (q ? 8 : 0), 8);
                uint16_t product = 0;
                for (unsigned bit = 0; bit < 8; ++bit)
                    if (b & (uint64_t{1} << bit))
                        product ^= static_cast<uint16_t>(a << bit);
                SetLane(result, lane, 16, product);
            }
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xbfe08400) == 0x2e000000) { // EXT .8B / .16B
            const auto left = GetVector(source), right = GetVector(second);
            const auto offset = (word >> 11) & 15;
            const unsigned width = q ? 16 : 8;
            if (offset >= width)
                Fail("reserved ARM64 vector extract offset");
            std::array<Vector, 2> bytes{left, right};
            if (!q)
                bytes[0][1] = right[0];
            Vector result{};
            std::memcpy(result.data(), reinterpret_cast<const uint8_t*>(bytes.data()) + offset, width);
            SetVector(dest, result);
            return true;
        }
        if ((word & 0xfffffc00) == 0x9e660000) {
            Set(dest, GetVector(source)[0]);
            return true;
        }
        if ((word & 0xfffffc00) == 0x1e260000) {
            Set(dest, GetVector(source)[0], 32);
            return true;
        }
        if ((word & 0xfffffc00) == 0x9e670000) {
            SetVector(dest, {Get(source), 0});
            return true;
        }
        if ((word & 0xfffffc00) == 0x1e270000) {
            SetVector(dest, {Get(source, 32), 0});
            return true;
        }
        if ((word & 0xffbffc00) == 0x1e204000) { // FMOV Dd,Dn / Sd,Sn
            const auto bits = GetVector(source)[0];
            SetVector(dest, {word & 0x00400000 ? bits : static_cast<uint32_t>(bits), 0});
            return true;
        }
        return false;
    }

    void Step(uint32_t word)
    {
        const unsigned dest = word & 31, source = (word >> 5) & 31;
        const unsigned width = (word >> 31) ? 64 : 32;
        if (word == 0xd4000001) {
            Set(0, uint64_t(Syscall(Get(8), 0)));
            return;
        } // SVC #0
        if (word == 0xd503201f || word == 0xd503241f || word == 0xd503245f || word == 0xd503249f ||
            word == 0xd50324df || word == 0xd503233f || word == 0xd50323bf || word == 0xd50320ff ||
            word == 0xd503219f || word == 0xd50321df) {
            // BTI and pointer authentication hints have no effect on a guest
            // that advertises neither BTI nor pointer authentication.
            return;
        }
        if (word == 0xd503203f || word == 0xd503205f) { // YIELD / WFE
            // WFE may return without an event. Let the guest recheck its wait condition.
            std::this_thread::yield();
            return;
        }
        if ((word & 0xfffff0ff) == 0xd503305f) {
            exclusiveSize = 0;
            return;
        }
        if (((word & 0xfffff0ff) == 0xd50330bf || (word & 0xfffff0ff) == 0xd503309f) && (word & 0x300)) { // DMB / DSB
            // Use a full host fence for every guest shareability domain and access type.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return;
        }
        if (word == 0xd5033fdf) { // ISB SY: subsequent instructions are fetched from current memory.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return;
        }
        if ((word & 0xffffffe0) == 0xd50b7b20 || (word & 0xffffffe0) == 0xd50b7520) { // DC CVAU / IC IVAU
            // This interpreter has no instruction-byte cache or host ARM I-cache.
            // Validate the addressed guest storage and order publication.
            ReadMemory(Get(dest), 1);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return;
        }
        if ((word & 0xfffffc1f) == 0xd65f0000) {
            BranchTo(Get(source), true);
            return;
        }
        if ((word & 0xffdffc1f) == 0xd61f0000) {
            const auto target = Get(source);
            if (word & 0x00200000)
                Set(Link, pc);
            BranchTo(target);
            return;
        }
        if ((word & 0x7c000000) == 0x14000000) {
            if (word >> 31)
                Set(Link, pc);
            BranchTo(instruction + SignExtend(word & 0x03ffffff, 26) * 4);
            return;
        }
        if ((word & 0xff000010) == 0x54000000) {
            if (Condition(word & 15))
                BranchTo(instruction + SignExtend((word >> 5) & 0x7ffff, 19) * 4);
            return;
        }
        if ((word & 0x7e000000) == 0x34000000) {
            if ((Get(dest, width) != 0) == bool(word & 0x01000000))
                BranchTo(instruction + SignExtend((word >> 5) & 0x7ffff, 19) * 4);
            return;
        }
        if ((word & 0x7e000000) == 0x36000000) {
            const auto bit = ((word >> 26) & 32) | ((word >> 19) & 31);
            if (((Get(dest, width) >> bit) & 1) == bool(word & 0x01000000))
                BranchTo(instruction + SignExtend((word >> 5) & 0x3fff, 14) * 4);
            return;
        }
        if ((word & 0x1f000000) == 0x10000000) {
            const auto delta = SignExtend((((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3), 21);
            const auto base = instruction - image->LoadBias();
            const bool pageAddress = word >> 31;
            Set(dest, image->LoadBias() + (pageAddress ? (base & ~uint64_t{4095}) + delta * 4096 : base + delta));
            return;
        }
        if ((word & 0x3b000000) == 0x18000000) { // LDR/LDRSW (literal), PRFM (literal)
            const auto address = instruction + SignExtend((word >> 5) & 0x7ffff, 19) * 4;
            const auto op = word >> 30;
            if (word & 0x04000000) {
                if (op == 3)
                    Fail("reserved ARM64 vector literal load");
                const unsigned bytes = 4u << op;
                Vector value{};
                std::memcpy(value.data(), ReadMemory(address, bytes), bytes);
                SetVector(dest, value);
            }
            else if (op == 0) {
                Set(dest, Load(address, 4), 32);
            }
            else if (op == 1) {
                Set(dest, Load(address, 8));
            }
            else if (op == 2) {
                Set(dest, SignExtend(Load(address, 4), 32));
            }
            return;
        }
        if ((word & 0x1f800000) == 0x12800000) {
            const unsigned kind = (word >> 29) & 3, shift = ((word >> 21) & 3) * 16;
            if (kind == 1 || shift >= width)
                Fail("invalid wide move");
            const auto value = uint64_t((word >> 5) & 0xffff) << shift;
            Set(dest,
                kind == 0   ? ~value
                : kind == 2 ? value
                            : (Get(dest, width) & ~(uint64_t{0xffff} << shift)) | value,
                width);
            return;
        }
        if ((word & 0x1f800000) == 0x11000000) {
            const auto left = source == Zero ? sp : Get(source, width);
            Arithmetic(word, width, width == 32 ? uint32_t(left) : left,
                       ((word >> 10) & 4095) << ((word & 0x00400000) ? 12 : 0));
            return;
        }
        if ((word & 0x1f200000) == 0x0b000000) {
            const unsigned kind = (word >> 22) & 3;
            if (kind == 3)
                Fail("reserved arithmetic shift");
            Arithmetic(word, width, Get(source, width),
                       Shift(Get((word >> 16) & 31, width), kind, (word >> 10) & 63, width));
            return;
        }
        if ((word & 0x1f200000) == 0x0b200000) {
            const unsigned option = (word >> 13) & 7, shift = (word >> 10) & 7;
            const unsigned bits = 8u << (option & 3);
            if (bits > width || shift > 4)
                Fail("invalid arithmetic extension");
            auto right = Get((word >> 16) & 31) & (UINT64_MAX >> (64 - bits));
            if (option & 4)
                right = SignExtend(right, bits);
            const auto left = source == Zero ? sp : Get(source);
            // Unlike shifted-register arithmetic, the extended form permits SP.
            Arithmetic(word, width, width == 32 ? uint32_t(left) : left,
                       (right << shift) & (UINT64_MAX >> (64 - width)));
            return;
        }
        const auto multiply = word & 0x7fe00000;
        const auto highMultiply = word & 0xffe0fc00;
        if (highMultiply == 0x9bc07c00 || highMultiply == 0x9b407c00) {
            const auto left = Get(source), right = Get((word >> 16) & 31);
            Set(dest, highMultiply == 0x9bc07c00
                          ? __umulh(left, right)
                          : uint64_t(__mulh(std::bit_cast<int64_t>(left), std::bit_cast<int64_t>(right))));
            return;
        }
        if (multiply == 0x1b000000 || (width == 64 && (multiply == 0x1b200000 || multiply == 0x1ba00000))) {
            const auto right = (word >> 16) & 31, accumulator = (word >> 10) & 31;
            uint64_t product;
            if (multiply == 0x1b200000)
                product = uint64_t(int64_t(std::bit_cast<int32_t>(uint32_t(Get(source)))) *
                                   std::bit_cast<int32_t>(uint32_t(Get(right))));
            else if (multiply == 0x1ba00000)
                product = Get(source, 32) * Get(right, 32);
            else
                product = Get(source, width) * Get(right, width);
            Set(dest, (word & 0x8000) ? Get(accumulator) - product : Get(accumulator) + product, width);
            return;
        }
        if ((word & 0x1f800000) == 0x12000000) {
            Logical(word, width, LogicalImmediate(word, width));
            return;
        }
        if ((word & 0x1f000000) == 0x0a000000) {
            auto right = Shift(Get((word >> 16) & 31, width), (word >> 22) & 3, (word >> 10) & 63, width);
            if (word & 0x00200000)
                right = ~right & (UINT64_MAX >> (64 - width));
            Logical(word, width, right);
            return;
        }
        if ((word & 0x1f800000) == 0x13000000 && ((word >> 29) & 3) != 3) {
            Bitfield(word, width);
            return;
        }
        if ((word & 0x7fa00000) == 0x13800000) { // EXTR (including ROR immediate)
            const unsigned lsb = (word >> 10) & 63;
            if (lsb >= width || ((word >> 22) & 1) != (width == 64))
                Fail("invalid EXTR");
            const auto low = Get((word >> 16) & 31, width);
            const auto high = Get(source, width);
            Set(dest, lsb ? (low >> lsb) | (high << (width - lsb)) : low, width);
            return;
        }
        if ((word & 0x1fe0fc00) == 0x1a000000) {
            CarryArithmetic(word, width);
            return;
        }
        if ((word & 0x3fe00800) == 0x1a800000) {
            const bool condition = Condition((word >> 12) & 15);
            auto value = Get(condition ? source : (word >> 16) & 31, width);
            if (!condition) {
                if (word & 0x40000000)
                    value = ~value;
                if (word & 0x00000400)
                    ++value;
            }
            Set(dest, value, width);
            return;
        }
        if ((word & 0x3fe00410) == 0x3a400000) { // CCMP / CCMN
            if (Condition((word >> 12) & 15)) {
                const auto right = (word >> 16) & 31;
                Arithmetic((word & 0x40000000) | 0x2000001f, width, Get(source, width),
                           word & 0x800 ? right : Get(right, width));
            }
            else {
                SetConditionFlags(word);
            }
            return;
        }
        const auto twoSource = word & 0x7fe0fc00;
        if (twoSource == 0x1ac00800 || twoSource == 0x1ac00c00) { // UDIV / SDIV
            const auto left = Get(source, width), right = Get((word >> 16) & 31, width);
            uint64_t result = 0;
            if (right) {
                if (twoSource == 0x1ac00800) {
                    result = left / right;
                }
                else {
                    const auto numerator = std::bit_cast<int64_t>(SignExtend(left, width));
                    const auto divisor = std::bit_cast<int64_t>(SignExtend(right, width));
                    result = uint64_t(divisor == -1 && numerator == INT64_MIN ? numerator : numerator / divisor);
                }
            }
            Set(dest, result, width);
            return;
        }
        if ((word & 0x7fe0f000) == 0x1ac02000) {
            const auto count = Get((word >> 16) & 31) & (width - 1);
            Set(dest, Shift(Get(source, width), (word >> 10) & 3, static_cast<unsigned>(count), width), width);
            return;
        }
        const auto oneSource = word & 0x7ffffc00;
        if (oneSource == 0x5ac00000) { // RBIT
            const auto value = _byteswap_uint64(ReverseBitsInBytes(Get(source, width)));
            Set(dest, width == 64 ? value : value >> 32, width);
            return;
        }
        if (oneSource == 0x5ac00400 || oneSource == 0x5ac00800 || oneSource == 0x5ac00c00) { // REV16 / REV32 / REV
            const auto value = Get(source, width);
            if (oneSource == 0x5ac00400) {
                constexpr uint64_t lower = 0x00ff00ff00ff00ffULL;
                Set(dest, ((value & lower) << 8) | ((value >> 8) & lower), width);
            }
            else if (oneSource == 0x5ac00800) {
                const uint64_t low = _byteswap_ulong(static_cast<uint32_t>(value));
                const uint64_t high = _byteswap_ulong(static_cast<uint32_t>(value >> 32));
                Set(dest, low | (high << 32), width);
            }
            else {
                if (width != 64)
                    Fail("invalid ARM64 REV width");
                Set(dest, _byteswap_uint64(value));
            }
            return;
        }
        if (oneSource == 0x5ac01000 || oneSource == 0x5ac01400) { // CLZ / CLS
            auto value = Get(source, width);
            const bool sign = oneSource == 0x5ac01400;
            if (sign && ((value >> (width - 1)) != 0))
                value = ~value;
            const auto count = width == 64 ? std::countl_zero(value) : std::countl_zero(static_cast<uint32_t>(value));
            Set(dest, count - unsigned(sign), width);
            return;
        }
        const auto ordered = word & 0x3ffffc00;
        if (ordered == 0x08dffc00 || ordered == 0x089ffc00) { // LDAR / STLR (byte, halfword, word, doubleword)
            const auto address = source == Zero ? sp : Get(source);
            const bool load = ordered == 0x08dffc00;
            switch (word >> 30) {
            case 0:
                OrderedAccess<uint8_t>(address, dest, load);
                break;
            case 1:
                OrderedAccess<uint16_t>(address, dest, load);
                break;
            case 2:
                OrderedAccess<uint32_t>(address, dest, load);
                break;
            case 3:
                OrderedAccess<uint64_t>(address, dest, load);
                break;
            }
            return;
        }
        const bool loadExclusive = (word & 0x3fff7c00) == 0x085f7c00;
        const bool loadExclusivePair = (word & 0xbfff0000) == 0x887f0000;
        if (loadExclusivePair || (word & 0xbfe00000) == 0x88200000) {
            ExclusivePairAccess(word, source == Zero ? sp : Get(source), dest, loadExclusivePair);
            return;
        }
        if (loadExclusive || (word & 0x3fe07c00) == 0x08007c00) {
            const auto address = source == Zero ? sp : Get(source);
            switch (word >> 30) {
            case 0:
                ExclusiveAccess<uint8_t>(word, address, dest, loadExclusive);
                break;
            case 1:
                ExclusiveAccess<uint16_t>(word, address, dest, loadExclusive);
                break;
            case 2:
                ExclusiveAccess<uint32_t>(word, address, dest, loadExclusive);
                break;
            case 3:
                ExclusiveAccess<uint64_t>(word, address, dest, loadExclusive);
                break;
            }
            return;
        }
        if ((word & 0x3b000000) == 0x39000000 || (word & 0x3b200000) == 0x38000000 ||
            (word & 0x3b200c00) == 0x38200800) {
            const bool vector = word & 0x04000000;
            if (!vector && (word >> 30) == 3 && (word & 0x00800000))
                return; // PRFM/PRFUM
            if (!vector && (word >> 30) == 2 && (word & 0x00c00000) == 0x00c00000)
                Fail("reserved ARM64 load encoding");
            Transfer(word, vector);
            return;
        }
        if ((word & 0xfe000000) == 0x28000000) {
            Pair(word, 4);
            return;
        }
        if ((word & 0xfe400000) == 0x68400000) {
            Pair(word, 4);
            return;
        } // LDPSW
        if ((word & 0xfe000000) == 0xa8000000) {
            Pair(word, 8);
            return;
        }
        if ((word & 0xfe000000) == 0x2c000000) {
            Pair(word, 4, true);
            return;
        }
        if ((word & 0xfe000000) == 0x6c000000) {
            Pair(word, 8, true);
            return;
        }
        if ((word & 0xfe000000) == 0xac000000) {
            Pair(word, 16, true);
            return;
        }
        if (Simd(word))
            return;
        switch (word & 0xffffffe0) {
        case 0xd53b0020: // CTR_EL0: coherent virtual I/D storage, 64-byte lines, PIPT.
            Set(dest, uint64_t{0xb004c004});
            return;
        case 0xd53b4200: // MRS Xt, NZCV
            if (!flagsValid)
                Fail("read of unavailable condition flags");
            Set(dest, uint32_t(n) << 31 | uint32_t(z) << 30 | uint32_t(c) << 29 | uint32_t(v) << 28);
            return;
        case 0xd51b4200:
            SetConditionFlags(static_cast<unsigned>(Get(dest) >> 28));
            return; // MSR NZCV, Xt
        case 0xd53b4400:
            Set(dest, floatEnvironment.control);
            return;
        case 0xd53b4420:
            Set(dest, floatEnvironment.status);
            return;
        case 0xd51b4400:
            floatEnvironment.control = static_cast<uint32_t>(Get(dest));
            return;
        case 0xd51b4420:
            floatEnvironment.status = static_cast<uint32_t>(Get(dest));
            return;
        case 0xd53be000:   // CNTFRQ_EL0
        case 0xd53be040: { // CNTVCT_EL0
            LARGE_INTEGER value{};
            if ((word & 0x40) ? !QueryPerformanceCounter(&value) : !QueryPerformanceFrequency(&value))
                Fail("could not query ARM64 virtual counter");
            Set(dest, static_cast<uint64_t>(value.QuadPart));
            return;
        }
        case 0xd53bd040: { // MRS Xt, TPIDR_EL0
            auto* tls = ElfImage::GuestArm64ThreadPointer();
            static const auto self = reinterpret_cast<uint64_t (*)()>(AndroidThreadImport("pthread_self", true));
            tls[ThreadIdSlot] = self();
            Set(dest, reinterpret_cast<uintptr_t>(tls));
            return;
        }
        }
        std::ostringstream reason;
        reason << "unsupported instruction 0x" << std::hex << word;
        Fail(reason.str());
    }

    const ElfImage* image;
    Arm64ExecutionProfile* profile = CurrentArm64ExecutionProfile();
    const bool profileInstructions = profile && profile->instructionProfiling;
    const bool blockJit = Arm64BlockJitEnabled();
    const bool stackStoreTrace = Arm64StackStoreTraceEnabled();
    const bool mappingFirst = [] {
        static const bool enabled = [] {
            const auto saved = GetLastError();
            wchar_t value[2]{};
            const bool disabled =
                GetEnvironmentVariableW(L"STONE_ARM64_MAPPING_FIRST", value, 2) == 1 && value[0] == L'0';
            SetLastError(saved);
            return !disabled;
        }();
        return enabled;
    }();
    const bool chainBlocks = [] {
        static const bool enabled = [] {
            const auto saved = GetLastError();
            wchar_t value[2]{};
            const bool enabled = GetEnvironmentVariableW(L"STONE_ARM64_BLOCK_CHAIN", value, 2) == 1 && value[0] == L'1';
            SetLastError(saved);
            return enabled;
        }();
        return enabled;
    }();
    // A same-binary diagnostic control preserves the old direct mapping.
    // No emitter or persistent-artifact format changes between the policies.
    const bool directBlocks = [] {
        static const bool direct = [] {
            const auto saved = GetLastError();
            wchar_t value[2]{};
            const bool enabled =
                GetEnvironmentVariableW(L"STONE_ARM64_LOCAL_DIRECT", value, 2) == 1 && value[0] == L'1';
            SetLastError(saved);
            return enabled;
        }();
        return direct;
    }();

    struct alignas(16) LocalBlockBucket {
        std::array<uint32_t, 4> tags{};
        std::array<std::shared_ptr<const Arm64Block>, 4> entries;
    };

    // Still 256 owners, plus a bounded 1 KiB tag index. Tags never bypass code
    // identity, source-byte or mapping-generation verification.
    std::array<LocalBlockBucket, 64> blocks;
    std::optional<std::exception_ptr> blockFailure;
    Arm64BlockContext* activeBlockMemory = nullptr;
    uint64_t blockCalls = 0, blockInstructions = 0, blockWarmupSkips = 0, blockDispatches = 0;
    const Arm64ImportName& importName;
    Arm64PropertyGet propertyGet;
    std::shared_ptr<GuestStack> stack;
    mutable std::vector<std::shared_ptr<GuestStack>> stackPins;
    std::array<uint64_t, 31> registers{};
    std::array<Vector, 32> vectors{};
    std::unordered_map<uintptr_t, JumpTarget> jumpTargets;
    std::vector<uintptr_t> cleanupFrames;
    uintptr_t sp = 0, entrySp = 0;
    const uintptr_t frameTop;
    uintptr_t pc, instruction = 0;
    uintptr_t executableStart = 0;
    size_t executableSize = 0;
    bool executableMapped = false;
    uint64_t executableGeneration = 0;
    unsigned callbackDepth;
    bool n = false, z = false, c = false, v = false, flagsValid = false;
    uintptr_t exclusiveAddress = 0;
    uint64_t exclusiveValue = 0;
    uint64_t exclusiveHigh = 0;
    unsigned exclusiveSize = 0;
    const Arm64Jni* jni;
    const uintptr_t guestEnvAddress = jni ? reinterpret_cast<uintptr_t>(&guestJniEnv) : 0;
    JNIEnv* jniEnv = nullptr;
    uint64_t importFloatResult = 0;
    std::optional<uint64_t> lastUnsupportedSyscall;
    std::string loaderError;
    std::string mappingError;
    std::vector<size_t> localFrames;
    std::vector<jobject> references;
    std::vector<CriticalArray> criticalArrays;
    mutable std::vector<std::pair<jobject, DirectBufferRange>> pinnedDirectBuffers;
    std::span<uint8_t> callbackMemory;
    std::string abortMessage;
};

void FinalizeArm64(uintptr_t module, const Arm64ImportName& importName, Arm64PropertyGet propertyGet, unsigned depth,
                   const Arm64Jni* jni)
{
    if (depth > 32)
        throw std::runtime_error("ARM64 destructor recursion limit exceeded");
    auto& callbacks = ExitCallbacks();
    for (;;) {
        ExitCallback callback;
        {
            const std::lock_guard lock(exitMutex);
            const auto last = std::find_if(callbacks.rbegin(), callbacks.rend(), [module](const ExitCallback& entry) {
                return !module || entry.module == module;
            });
            if (last == callbacks.rend())
                return;
            callback = *last;
            callbacks.erase(std::next(last).base());
        }
        Interpreter(*callback.image, callback.code, importName, propertyGet, depth, jni)
            .RunInitializer(0, &callback.argument);
    }
}

} // namespace

uintptr_t Arm64TlsDescriptorResolver()
{
    return reinterpret_cast<uintptr_t>(&tlsDescriptorToken);
}

uintptr_t RunArm64IfuncResolver(const ElfImage& image, uintptr_t entry, const Arm64ImportName& importName,
                                Arm64PropertyGet propertyGet)
{
    // Android API 30+ supplies __ifunc_arg_t. Keep optional capabilities at
    // zero, exactly as the translated getauxval(AT_HWCAP/AT_HWCAP2) path does.
    std::array<uint64_t, 3> info{3 * sizeof(uint64_t), 0, 0};
    const std::array<uint64_t, 2> args{uint64_t{1} << 62, reinterpret_cast<uintptr_t>(info.data())};
    return Interpreter(image, entry, importName, propertyGet)
        .RunCallback(args, {reinterpret_cast<uint8_t*>(info.data()), sizeof(info)}, true);
}

uintptr_t CopyArm64GuestConstantString(const char* value)
{
    return CopyGuestConstantString(value);
}

uint64_t RunArm64Initializer(const ElfImage& image, uintptr_t entry, unsigned resultWidth,
                             const Arm64ImportName& importName, Arm64PropertyGet propertyGet, const Arm64Jni* jni)
{
    return Interpreter(image, entry, importName, propertyGet, 0, jni).RunInitializer(resultWidth);
}

uint64_t RunArm64Native(const ElfImage& image, uintptr_t entry, char result, const Arm64ImportName& importName,
                        Arm64PropertyGet propertyGet, const Arm64Jni& jni, JNIEnv* env, jobject receiver,
                        std::span<const Arm64Argument> arguments)
{
    return Interpreter(image, entry, importName, propertyGet, 0, &jni).RunNative(result, env, receiver, arguments);
}

uint64_t RunArm64Function(const ElfImage& image, uintptr_t entry, char result, const Arm64ImportName& importName,
                          Arm64PropertyGet propertyGet, const Arm64Jni* jni, std::span<const Arm64Argument> arguments)
{
    return Interpreter(image, entry, importName, propertyGet, 0, jni).RunFunction(result, arguments);
}

uint64_t RunArm64Callback(const ElfImage& image, uintptr_t entry, const Arm64ImportName& importName,
                          Arm64PropertyGet propertyGet, const Arm64Jni& jni, std::span<const uint64_t> arguments,
                          std::span<uint8_t> writable)
{
    return Interpreter(image, entry, importName, propertyGet, 0, &jni).RunCallback(arguments, writable);
}

bool RunArm64Signal(uintptr_t handler, int signal, uint64_t previousMask, bool withInfo)
{
    if (!activeArm64Interpreter)
        return false;
    activeArm64Interpreter->RunSignal(handler, signal, previousMask, withInfo);
    return true;
}

bool RunArm64MemoryFault(uintptr_t handler, uintptr_t address, int code, uint64_t& mask, bool withInfo)
{
    if (!activeArm64Interpreter)
        return false;
    activeArm64Interpreter->RunSignal(handler, 11, mask, withInfo, std::pair{address, code}, &mask);
    return true;
}

void RunArm64Destructors(const Arm64ImportName& importName, Arm64PropertyGet propertyGet, const Arm64Jni& jni)
{
    FinalizeArm64(0, importName, propertyGet, 0, &jni);
}

} // namespace stone::windows
