#include "stub-libz.h"
#include "abi/AndroidLibraryImports.h"
#include "loader/AndroidSystemElf.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "stub-libc.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace stone::windows {
namespace {
constexpr int Ok = 0;
constexpr int StreamError = -2;
constexpr int VersionError = -6;
constexpr unsigned MessageBytes = 256;
constexpr std::string_view operations[]{
    "inflateInit_",  "inflateInit2_", "inflate", "inflateEnd", "inflateReset",
    "inflateReset2", "deflateInit2_", "deflate", "deflateEnd", "crc32",
};

// The two Android ABIs use the LP64 z_stream layout. Windows unsigned long
// has a different size, so use fixed-width fields at the ABI boundary.
struct ZStream {
    uintptr_t nextIn;
    uint32_t availIn;
    uint64_t totalIn;
    uintptr_t nextOut;
    uint32_t availOut;
    uint64_t totalOut;
    uintptr_t message;
    uintptr_t state;
    uintptr_t allocate;
    uintptr_t release;
    uintptr_t opaque;
    int32_t dataType;
    uint64_t adler;
    uint64_t reserved;
};

static_assert(sizeof(ZStream) == 112 && offsetof(ZStream, nextOut) == 24 && offsetof(ZStream, adler) == 96);

using Memory = std::function<uint8_t*(uintptr_t, unsigned, bool)>;

struct Stream {
    std::recursive_mutex mutex;
    ZStream host{};
    uintptr_t guestAllocate = 0;
    uintptr_t guestRelease = 0;
    uintptr_t guestOpaque = 0;
    uintptr_t guestMessage = 0;
    const Memory* memory = nullptr;
    std::exception_ptr callbackError;
    bool initialized = false;
    bool deflating = false;

    ~Stream()
    {
        if (guestMessage)
            FreeArm64GuestMemory(guestMessage);
    }
};

std::mutex streamsMutex;
std::unordered_map<uintptr_t, std::shared_ptr<Stream>> streams;

void* Allocate(Stream* bridge, unsigned items, unsigned size) noexcept
{
    try {
        if (!bridge->memory || (items && size > UINT_MAX / items))
            return nullptr;
        const auto result = CallAndroidCallback(bridge->guestAllocate, reinterpret_cast<void*>(bridge->guestOpaque),
                                                reinterpret_cast<void*>(static_cast<uintptr_t>(items)),
                                                reinterpret_cast<void*>(static_cast<uintptr_t>(size)), 0);
        return result ? (*bridge->memory)(result, items * size, true) : nullptr;
    }
    catch (...) {
        bridge->callbackError = std::current_exception();
        return nullptr;
    }
}

void Release(Stream* bridge, void* address) noexcept
{
    try {
        CallAndroidCallback(bridge->guestRelease, reinterpret_cast<void*>(bridge->guestOpaque), address, nullptr, 0);
    }
    catch (...) {
        bridge->callbackError = std::current_exception();
    }
}

template <typename Call> int WithStreamMemory(Stream& bridge, const Memory& memory, Call call)
{
    struct Restore {
        Stream& stream;
        const Memory* previous;

        ~Restore()
        {
            stream.memory = previous;
        }
    } restore{bridge, std::exchange(bridge.memory, &memory)};

    return call();
}

int Run(Stream& bridge, std::string_view name, const Memory& memory, uintptr_t second = 0, uintptr_t third = 0,
        uintptr_t fourth = 0)
{
    return WithStreamMemory(bridge, memory, [&] {
        return CallNativeImageZlib(name, {reinterpret_cast<uintptr_t>(&bridge.host), second, third, fourth});
    });
}

void CopyState(Stream& bridge, ZStream& guest, uintptr_t address)
{
    guest.totalIn = bridge.host.totalIn;
    guest.totalOut = bridge.host.totalOut;
    guest.dataType = bridge.host.dataType;
    guest.adler = bridge.host.adler;
    guest.reserved = bridge.host.reserved;
    guest.state = bridge.host.state ? address : 0;
    guest.message = 0;
    if (bridge.host.message) {
        if (!bridge.guestMessage)
            bridge.guestMessage = AllocateArm64GuestMemory(MessageBytes, true);
        if (!bridge.guestMessage)
            throw std::bad_alloc();
        const auto* message = reinterpret_cast<const char*>(bridge.host.message);
        const auto size = strnlen_s(message, MessageBytes - 1);
        auto* output = reinterpret_cast<char*>(bridge.guestMessage);
        std::memcpy(output, message, size);
        output[size] = '\0';
        guest.message = bridge.guestMessage;
    }
}
}

uintptr_t Libz(std::string_view name)
{
    if (std::ranges::find(operations, name) == std::end(operations) || !NativeImageZlibExport(name))
        return 0;
    return Import(
        name, +[] {
        }); // ARM64 enters CallArm64Zlib through the import token.
}

uint64_t CallArm64Zlib(std::string_view name, std::span<const uint64_t, 8> args, const Memory& memory,
                       const std::function<std::string(uintptr_t, unsigned)>& string)
{
    if (name == "crc32") {
        using Checksum = uint64_t (*)(uint64_t, const uint8_t*, unsigned);
        static const auto checksum = reinterpret_cast<Checksum>(NativeSystemElfThunk("libz.so", "crc32", "(JLI)J"));
        const auto size = static_cast<unsigned>(args[2]);
        const uint8_t empty = 0;
        const auto* input = args[1] ? (size ? memory(args[1], size, false) : &empty) : nullptr;
        return checksum(args[0], input, size);
    }
    if (!args[0])
        return static_cast<uint32_t>(StreamError);
    const bool deflating = name.starts_with("deflate");
    const bool extended = name == "inflateInit2_";
    const bool initialize = extended || name == "inflateInit_" || name == "deflateInit2_";
    std::string version;
    if (initialize) {
        const unsigned versionIndex = deflating ? 6 : (extended ? 2 : 1);
        if (static_cast<unsigned>(args[versionIndex + 1]) != sizeof(ZStream) || !args[versionIndex])
            return static_cast<uint32_t>(VersionError);
        version = string(args[versionIndex], 32);
    }
    auto* output = memory(args[0], sizeof(ZStream), true);
    ZStream guest{};
    std::memcpy(&guest, output, sizeof(guest));
    if (initialize && (!!guest.allocate != !!guest.release))
        return static_cast<uint32_t>(StreamError);
    std::shared_ptr<Stream> bridge;
    {
        const std::lock_guard lock(streamsMutex);
        if (initialize) {
            if (streams.contains(args[0]))
                return static_cast<uint32_t>(StreamError);
            bridge = std::make_shared<Stream>();
            streams.emplace(args[0], bridge);
        }
        else {
            const auto found = streams.find(args[0]);
            if (found == streams.end())
                return static_cast<uint32_t>(StreamError);
            bridge = found->second;
        }
    }
    const std::lock_guard lock(bridge->mutex);
    if (!initialize && (!bridge->initialized || bridge->deflating != deflating))
        return static_cast<uint32_t>(StreamError);
    const auto forgetStream = [&] {
        const std::lock_guard mapLock(streamsMutex);
        streams.erase(args[0]);
    };
    bridge->callbackError = nullptr;
    int result;
    if (initialize) {
        try {
            if (guest.allocate) {
                bridge->guestAllocate = guest.allocate;
                bridge->guestRelease = guest.release;
                bridge->guestOpaque = guest.opaque;
                const std::lock_guard imports(AndroidLoaderMutex());
                bridge->host.allocate = Import("native libz guest allocate", Allocate);
                bridge->host.release = Import("native libz guest release", Release);
                bridge->host.opaque = reinterpret_cast<uintptr_t>(bridge.get());
            }
            if (deflating) {
                using Init = int (*)(ZStream*, int, int, int, int, int, const char*, int);
                static const auto init =
                    reinterpret_cast<Init>(NativeSystemElfThunk("libz.so", "deflateInit2_", "(LIIIIILI)I"));
                result = WithStreamMemory(*bridge, memory, [&] {
                    return init(&bridge->host, static_cast<int>(args[1]), static_cast<int>(args[2]),
                                static_cast<int>(args[3]), static_cast<int>(args[4]), static_cast<int>(args[5]),
                                version.c_str(), sizeof(ZStream));
                });
            }
            else {
                const auto pointer = reinterpret_cast<uintptr_t>(version.c_str());
                result = extended ? Run(*bridge, name, memory, args[1], pointer, sizeof(ZStream))
                                  : Run(*bridge, name, memory, pointer, sizeof(ZStream));
            }
        }
        catch (...) {
            forgetStream();
            throw;
        }
        bridge->initialized = result == Ok;
        bridge->deflating = deflating;
    }
    else if (name == "inflate" || name == "deflate") {
        if ((guest.availIn && !guest.nextIn) || !guest.nextOut)
            return static_cast<uint32_t>(StreamError);
        uint8_t unused = 0;
        bridge->host.nextIn =
            guest.availIn ? reinterpret_cast<uintptr_t>(memory(guest.nextIn, guest.availIn, false)) : 0;
        bridge->host.nextOut =
            reinterpret_cast<uintptr_t>(guest.availOut ? memory(guest.nextOut, guest.availOut, true) : &unused);
        bridge->host.availIn = guest.availIn;
        bridge->host.availOut = guest.availOut;
        result = Run(*bridge, name, memory, args[1]);
        if (bridge->host.availIn > guest.availIn || bridge->host.availOut > guest.availOut)
            throw std::runtime_error("invalid Android libz stream result");
        guest.nextIn += guest.availIn - bridge->host.availIn;
        guest.nextOut += guest.availOut - bridge->host.availOut;
        guest.availIn = bridge->host.availIn;
        guest.availOut = bridge->host.availOut;
        bridge->host.nextIn = 0;
        bridge->host.nextOut = 0;
    }
    else {
        result = Run(*bridge, name, memory, args[1]);
        if (name.ends_with("End") && !bridge->host.state)
            bridge->initialized = false;
    }
    if (bridge->initialized) {
        CopyState(*bridge, guest, args[0]);
    }
    else {
        forgetStream();
        guest.message = 0;
        guest.state = 0;
    }
    std::memcpy(output, &guest, sizeof(guest));
    if (bridge->callbackError)
        std::rethrow_exception(bridge->callbackError);
    return static_cast<uint32_t>(result);
}
}
