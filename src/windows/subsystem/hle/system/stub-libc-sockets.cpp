#include "stub-libc-sockets.h"
#include "stub-libc-inotify.h"
#include "stub-libc-interfaces.h"
#include "../../../client/BrokerClient.h"
#include "../../../host/WindowsPath.h"
#include "abi/AndroidLibraryImports.h"
#include "abi/TypedAndroidImport.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "stub-libandroid-hardware-buffer.h"
#include "stub-libc.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace stone::windows {
namespace {
constexpr int Nonblocking = 0x800, CloseOnExec = 0x80000;
constexpr int UnixFamily = 1, Seqpacket = 5, SocketLevel = 1, PassCredentials = 16;
constexpr int Peek = 2, ControlTruncated = 8, Truncated = 0x20, DontWait = 0x40, NoSignal = 0x4000;
constexpr int ControlCloseOnExec = 0x40000000, Rights = 1; // MSG_CMSG_CLOEXEC and SCM_RIGHTS
constexpr uint32_t Input = 1, Priority = 2, Output = 4, Error = 8, Hangup = 16, ReadHangup = 0x2000, Edge = 0x80000000;

struct AddressInfoNode {
    AndroidAddrInfo info{};
    sockaddr_storage address{};
};

struct AddressInfoRegistry {
    std::mutex mutex;
    std::unordered_set<AndroidAddrInfo*> nodes;
};

AddressInfoRegistry& ResolvedAddresses()
{
    static auto* registry = new AddressInfoRegistry;
    return *registry;
}

void FreeAddressInfoNode(void* node)
{
    if (IsArm64TranslationEnabled())
        FreeArm64GuestMemory(reinterpret_cast<uintptr_t>(node));
    else
        std::free(node);
}

int ResolveError(int error)
{
    switch (error) {
    case WSATRY_AGAIN:
        return 2; // EAI_AGAIN
    case WSAEINVAL:
        return 3; // EAI_BADFLAGS
    case WSAEAFNOSUPPORT:
        return 5; // EAI_FAMILY
    case WSA_NOT_ENOUGH_MEMORY:
        return 6; // EAI_MEMORY
    case WSANO_DATA:
        return 7; // EAI_NODATA
    case WSAHOST_NOT_FOUND:
        return 8; // EAI_NONAME
    case WSATYPE_NOT_FOUND:
        return 9; // EAI_SERVICE
    case WSAESOCKTNOSUPPORT:
        return 10; // EAI_SOCKTYPE
    default:
        return 4; // EAI_FAIL
    }
}

struct GuestIoVector {
    void* data;
    size_t length;
};

struct GuestMessage {
    void* name;
    uint32_t nameLength;
    GuestIoVector* vectors;
    size_t vectorCount;
    void* control;
    size_t controlLength;
    int flags;
};

struct GuestControl {
    size_t length;
    int level, type;
};

struct GuestCredentials {
    int32_t pid;
    uint32_t uid, gid;
};

static_assert(sizeof(GuestMessage) == 56 && offsetof(GuestMessage, control) == 32);
static_assert(sizeof(GuestControl) == 16 && sizeof(GuestCredentials) == 12);

using BufferReference = std::unique_ptr<const AndroidHardwareBuffer, decltype(&ReleaseAndroidHardwareBuffer)>;

template <bool OwnSocket> class ScopedGuestDescriptor {
public:
    explicit ScopedGuestDescriptor(int fd, bool socket = OwnSocket) : fd_(fd), socket_(socket)
    {
    }

    ScopedGuestDescriptor(ScopedGuestDescriptor&& other) noexcept : fd_(other.release()), socket_(other.socket_)
    {
    }

    ScopedGuestDescriptor& operator=(ScopedGuestDescriptor&& other) noexcept
    {
        if (this != &other) {
            reset();
            fd_ = other.release();
            socket_ = other.socket_;
        }
        return *this;
    }

    ~ScopedGuestDescriptor()
    {
        reset();
    }

    int get() const
    {
        return fd_;
    }

    bool socket() const
    {
        return socket_;
    }

    int release()
    {
        return std::exchange(fd_, -1);
    }

private:
    void reset()
    {
        if (fd_ >= 0) {
            const int saved = errno;
            if (socket_)
                CloseAndroidFd(fd_);
            else
                CloseQueuedAndroidFd(fd_);
            errno = saved;
            fd_ = -1;
        }
    }

    int fd_;
    bool socket_;
};

using QueuedDescriptor = ScopedGuestDescriptor<false>;
using SocketReference = ScopedGuestDescriptor<true>;
using PipeHandle = std::unique_ptr<void, decltype(&CloseHandle)>;
constexpr size_t MaxRights = 253; // Linux SCM_MAX_FD.

struct SocketFileReference {
    PipeHandle file{nullptr, &CloseHandle};
    PipeHandle section{nullptr, &CloseHandle};
    unsigned mode = 0, seals = 0;
    SocketFileReference() = default;

    explicit SocketFileReference(const AndroidSocketFile& grant)
        : file(reinterpret_cast<HANDLE>(grant.file), &CloseHandle),
          section(reinterpret_cast<HANDLE>(grant.section), &CloseHandle), mode(grant.mode), seals(grant.seals)
    {
    }
};

struct LocalSocketPair {
    static constexpr size_t MaxPacket = 212960;
    static constexpr size_t QueueBytes = 256 * 1024;
    static constexpr size_t QueuePackets = 256;

    struct Record {
        std::vector<char> data;
        // The hardware buffer travels with the first byte of its record.
        BufferReference attachment{nullptr, &ReleaseAndroidHardwareBuffer};
        std::vector<QueuedDescriptor> descriptors;
        std::optional<GuestCredentials> credentials;
        std::vector<SocketFileReference> grants;
    };

    struct Endpoint {
        std::deque<Record> records;
        size_t offset = 0;
        size_t bytes = 0;
        bool receiving = true;
        bool sending = true;
        bool passCredentials = false;
        unsigned relays = 0;
        unsigned brokerRelays = 0;
        uint64_t inputGeneration = 0, outputGeneration = 0;

        bool hasRoom(size_t length) const
        {
            return records.size() < QueuePackets && length <= QueueBytes - bytes;
        }
    };

    explicit LocalSocketPair(int type) : type(type)
    {
    }

    const int type;
    std::mutex mutex;
    std::condition_variable changed;
    std::array<Endpoint, 2> ends;
};

struct UnixBinding {
    PipeHandle identity{nullptr, &CloseHandle};
    std::filesystem::path path;

    ~UnixBinding()
    {
        if (!path.empty())
            DeleteFileW(path.c_str());
    }
};

struct Socket {
    Socket(SOCKET handle, int type) : handle(handle), type(type)
    {
    }

    ~Socket()
    {
        if (handle != INVALID_SOCKET)
            closesocket(handle);
    }

    const SOCKET handle;
    const int type;
    std::atomic<bool> nonblocking = false;
    std::atomic<bool> listening = false;
    bool unixSocket = false;
    std::mutex addressMutex;
    std::unique_ptr<UnixBinding> binding;
    std::vector<char> localAddress, peerAddress;
    std::atomic<uint64_t> reads = 0, writes = 0;
    std::atomic<uint64_t> multicastV4Interface = 0; // Address, or bit 32 plus the selected interface index.
    std::shared_ptr<LocalSocketPair> pair;
    unsigned side = 0;
    unsigned descriptors = 1; // Protected by socketMutex.
};

struct SocketDescriptor {
    std::shared_ptr<Socket> socket;
    uint64_t id;
    bool closeOnExec;
};

struct SocketRegistry {
    std::mutex mutex;
    std::unordered_map<int, SocketDescriptor> descriptors;
    uint64_t nextId = 1;
};

SocketRegistry& SocketDescriptors()
{
    // Native image services can retain daemon threads after DestroyJavaVM.
    // Their fd callbacks must remain valid throughout CRT/FLS teardown. Open
    // descriptors still release normally on close; the OS reclaims any left
    // at process exit, after all native threads have stopped.
    static auto* registry = new SocketRegistry;
    return *registry;
}

std::mutex& socketMutex = SocketDescriptors().mutex;
std::unordered_map<int, SocketDescriptor>& sockets = SocketDescriptors().descriptors;
uint64_t& nextSocketId = SocketDescriptors().nextId;

std::shared_ptr<Socket> FindSocket(int fd, uint64_t id = 0)
{
    const std::lock_guard lock(socketMutex);
    const auto it = sockets.find(fd);
    return it != sockets.end() && (!id || it->second.id == id) ? it->second.socket : nullptr;
}

int GuestError(int error)
{
    switch (error) {
    case 0:
        return 0;
    case WSAEINTR:
        return EINTR;
    case WSAEACCES:
        return EACCES;
    case WSAEFAULT:
        return EFAULT;
    case WSAEINVAL:
        return EINVAL;
    case WSAEMFILE:
        return EMFILE;
    case WSAEWOULDBLOCK:
        return EAGAIN;
    case WSAEINPROGRESS:
        return 115;
    case WSAEALREADY:
        return 114;
    case WSAENOTSOCK:
        return 88;
    case WSAEDESTADDRREQ:
        return 89;
    case WSAEMSGSIZE:
        return 90;
    case WSAEPROTONOSUPPORT:
        return 93;
    case WSAENOPROTOOPT:
        return 92;
    case WSAEOPNOTSUPP:
        return 95;
    case WSAEAFNOSUPPORT:
        return 97;
    case WSAEADDRINUSE:
        return 98;
    case WSAEADDRNOTAVAIL:
        return 99;
    case WSAENETDOWN:
        return 100;
    case WSAENETUNREACH:
        return 101;
    case WSAENETRESET:
        return 102;
    case WSAECONNABORTED:
        return 103;
    case WSAECONNRESET:
        return 104;
    case WSAENOBUFS:
        return 105;
    case WSAEISCONN:
        return 106;
    case WSAENOTCONN:
        return 107;
    case WSAESHUTDOWN:
        return EPIPE;
    case WSAETIMEDOUT:
        return 110;
    case WSAECONNREFUSED:
        return 111;
    case WSAEHOSTUNREACH:
        return 113;
    default:
        return EIO;
    }
}

int SocketError()
{
    errno = GuestError(WSAGetLastError());
    return -1;
}

bool SetNonblocking(Socket& socket, bool enabled)
{
    if (socket.pair) {
        const std::lock_guard lock(socket.pair->mutex);
        socket.nonblocking = enabled;
        socket.pair->changed.notify_all();
        return true;
    }
    u_long mode = enabled;
    if (ioctlsocket(socket.handle, FIONBIO, &mode) == SOCKET_ERROR) {
        SocketError();
        return false;
    }
    socket.nonblocking = enabled;
    return true;
}

int RegisterSocket(SOCKET handle, int type, bool nonblocking, bool closeOnExec,
                   std::shared_ptr<LocalSocketPair> pair = {}, unsigned side = 0, bool unixSocket = false)
{
    std::shared_ptr<Socket> socket;
    int fd = -1;
    try {
        socket = std::make_shared<Socket>(handle, type);
        socket->pair = std::move(pair);
        socket->side = side;
        socket->unixSocket = unixSocket;
        if (!SetNonblocking(*socket, nonblocking))
            return -1;
        _sopen_s(&fd, "NUL", _O_RDONLY | _O_BINARY | _O_NOINHERIT, _SH_DENYNO, 0);
        if (fd < 0)
            return -1;
        const std::lock_guard lock(socketMutex);
        sockets.emplace(fd, SocketDescriptor{socket, nextSocketId++, closeOnExec});
        return fd;
    }
    catch (const std::bad_alloc&) {
        if (!socket && handle != INVALID_SOCKET)
            closesocket(handle);
        if (fd >= 0)
            _close(fd);
        errno = ENOMEM;
        return -1;
    }
}

int CreateSocket(int family, int type, int protocol)
{
    const int hostFamily = family == UnixFamily ? AF_UNIX : family == 2 ? AF_INET : family == 10 ? AF_INET6 : AF_UNSPEC;
    if (hostFamily == AF_UNSPEC) {
        errno = 97;
        return -1;
    }
    const int socketType = type & ~(Nonblocking | CloseOnExec);
    const int expectedProtocol = hostFamily == AF_UNIX       ? 0
                                 : socketType == SOCK_STREAM ? IPPROTO_TCP
                                 : socketType == SOCK_DGRAM  ? IPPROTO_UDP
                                                             : 0;
    if (hostFamily == AF_UNIX ? socketType != SOCK_STREAM || protocol != 0
                              : !expectedProtocol || (protocol && protocol != expectedProtocol)) {
        errno = 93;
        return -1;
    }
    if (!InitializeAndroidWinsock()) {
        errno = EIO;
        return -1;
    }
    // A pending read must not hold the Windows file object lock across a write
    // from another thread, as used by TLS and HTTP/2.
    const SOCKET handle = WSASocketW(hostFamily, socketType, expectedProtocol, nullptr, 0,
                                     WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (handle == INVALID_SOCKET)
        return SocketError();
    if (hostFamily == AF_INET6) {
        // Android Java sockets connect to IPv4 peers through IPv4-mapped IPv6 addresses.
        const DWORD ipv6Only = 0;
        if (setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&ipv6Only), sizeof(ipv6Only)) ==
            SOCKET_ERROR) {
            SocketError();
            closesocket(handle);
            return -1;
        }
    }
    return RegisterSocket(handle, socketType, (type & Nonblocking) != 0, (type & CloseOnExec) != 0, {}, 0,
                          family == UnixFamily);
}

// Windows accepts abstract sockaddr_un values but does not give them Linux
// binding/connection semantics. Reserve the exact guest name with a kernel
// object, and publish a private, randomly named native AF_UNIX filesystem node.
// The named object disappears even when its owning process crashes.
struct PublishedUnixAddress {
    LONG ready;
    wchar_t path[32768];
};

std::wstring UnixIdentity(const sockaddr* address, int length)
{
    std::array<unsigned char, 32> hash{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<sockaddr*>(address)),
                   length, hash.data(), static_cast<ULONG>(hash.size())) < 0)
        return {};
    std::wstring name = L"Local\\Stone.Unix.";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (auto byte : hash) {
        name += hex[byte >> 4];
        name += hex[byte & 15];
    }
    return name;
}

int UseUnixAddress(Socket& socket, const sockaddr* address, int length, bool connecting)
{
    if (!address || length < 3 || length > 110) {
        errno = EINVAL;
        return -1;
    }
    const auto* bytes = reinterpret_cast<const char*>(address);
    uint16_t family;
    std::memcpy(&family, bytes, sizeof(family));
    if (family != UnixFamily) {
        errno = 97;
        return -1;
    }
    // Pathname sockets require Android filesystem mapping, not host paths.
    if (bytes[2] != '\0') {
        errno = 95;
        return -1;
    }
    const std::lock_guard lock(socket.addressMutex);
    if (!connecting && socket.binding) {
        errno = EINVAL;
        return -1;
    }
    if (connecting && socket.binding) {
        errno = 95;
        return -1;
    } // Bound-client peer naming is not implemented.
    try {
        const auto identity = UnixIdentity(address, length);
        if (identity.empty()) {
            errno = EIO;
            return -1;
        }
        auto binding = std::make_unique<UnixBinding>();
        if (connecting) {
            binding->identity.reset(OpenFileMappingW(FILE_MAP_READ, FALSE, identity.c_str()));
            if (!binding->identity) {
                errno = 111;
                return -1;
            }
        }
        else {
            binding->identity.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                       sizeof(PublishedUnixAddress), identity.c_str()));
            const auto error = GetLastError();
            if (!binding->identity) {
                errno = EACCES;
                return -1;
            }
            if (error == ERROR_ALREADY_EXISTS) {
                errno = 98;
                return -1;
            }
        }
        auto* published = static_cast<PublishedUnixAddress*>(MapViewOfFile(
            binding->identity.get(), connecting ? FILE_MAP_READ : FILE_MAP_WRITE, 0, 0, sizeof(PublishedUnixAddress)));
        if (!published) {
            errno = EIO;
            return -1;
        }
        const std::unique_ptr<void, decltype(&UnmapViewOfFile)> view(published, &UnmapViewOfFile);
        std::filesystem::path path;
        if (connecting) {
            if (!std::atomic_ref<LONG>(published->ready).load(std::memory_order_acquire) ||
                published->path[32767] != L'\0') {
                errno = 111;
                return -1;
            }
            path = published->path;
        }
        else {
            std::array<wchar_t, 32768> temporary{};
            const auto size = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
            if (!size || size >= temporary.size()) {
                errno = AndroidNameTooLong;
                return -1;
            }
            std::array<unsigned char, 16> nonce{};
            if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
                errno = EIO;
                return -1;
            }
            std::wstring file = L"stone-unix-";
            constexpr wchar_t hex[] = L"0123456789abcdef";
            for (auto byte : nonce) {
                file += hex[byte >> 4];
                file += hex[byte & 15];
            }
            path = std::filesystem::absolute(std::filesystem::path(temporary.data()) / file);
            if (path.native().size() >= 32768) {
                errno = AndroidNameTooLong;
                return -1;
            }
        }
        auto native = path.u8string();
        const auto relative = path.lexically_relative(std::filesystem::current_path()).u8string();
        if (!relative.empty() && relative.size() < native.size())
            native = relative;
        if (native.size() >= 108) {
            errno = AndroidNameTooLong;
            return -1;
        }

        struct {
            uint16_t family = AF_UNIX;
            char path[108]{};
        } host;

        std::memcpy(host.path, native.data(), native.size());
        std::vector<char> guest(bytes, bytes + length);
        const auto* raw = reinterpret_cast<const sockaddr*>(&host);
        if (connecting) {
            const auto result = connect(socket.handle, raw, sizeof(host));
            if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
                return SocketError();
            socket.peerAddress = std::move(guest);
            if (result == SOCKET_ERROR) {
                errno = 115;
                return -1;
            }
        }
        else {
            auto cleanupPath = ExtendedWindowsPath(path);
            if (bind(socket.handle, raw, sizeof(host)) == SOCKET_ERROR)
                return SocketError();
            binding->path = std::move(cleanupPath);
            std::copy(path.native().begin(), path.native().end(), published->path);
            std::atomic_ref<LONG>(published->ready).store(1, std::memory_order_release);
            socket.localAddress = std::move(guest);
            socket.binding = std::move(binding);
        }
        return 0;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
    catch (const std::filesystem::filesystem_error&) {
        errno = EIO;
        return -1;
    }
}

int CreateSocketPair(int family, int type, int protocol, int* pair)
{
    if (family != UnixFamily) {
        errno = 97;
        return -1;
    }
    const int socketType = type & ~(Nonblocking | CloseOnExec);
    if ((socketType != SOCK_STREAM && socketType != Seqpacket) || protocol) {
        errno = 93;
        return -1;
    }
    if (!pair) {
        errno = EFAULT;
        return -1;
    }
    const bool nonblocking = (type & Nonblocking) != 0;
    try {
        auto state = std::make_shared<LocalSocketPair>(socketType);
        const bool closeOnExec = (type & CloseOnExec) != 0;
        const int first = RegisterSocket(INVALID_SOCKET, socketType, nonblocking, closeOnExec, state, 0);
        if (first < 0)
            return -1;
        const int second = RegisterSocket(INVALID_SOCKET, socketType, nonblocking, closeOnExec, state, 1);
        if (second < 0) {
            const int saved = errno;
            CloseAndroidFd(first);
            errno = saved;
            return -1;
        }
        pair[0] = first;
        pair[1] = second;
        return 0;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

template <bool Send> intptr_t TransferSocketBytes(Socket& socket, void* buffer, size_t bytes, int flags)
{
    if ((flags & ~(DontWait | (Send ? NoSignal : Peek))) || ((flags & DontWait) && !socket.nonblocking)) {
        errno = 95;
        return -1;
    }
    if (!bytes)
        return 0;
    const auto size = static_cast<int>(std::min<size_t>(bytes, INT_MAX));
    const int result = Send ? send(socket.handle, static_cast<const char*>(buffer), size, 0)
                            : recv(socket.handle, static_cast<char*>(buffer), size, (flags & Peek) ? MSG_PEEK : 0);
    if (result == SOCKET_ERROR) {
        SocketError();
        if (errno == EAGAIN) {
            if constexpr (Send)
                ++socket.writes;
            else
                ++socket.reads;
        }
        return -1;
    }
    if constexpr (!Send) {
        if (!(flags & Peek) && result > 0)
            ++socket.reads;
    }
    return result;
}

bool VectorLength(std::span<const GuestIoVector> vectors, size_t& length)
{
    length = 0;
    for (const auto& vector : vectors) {
        if (vector.length > PTRDIFF_MAX - length) {
            errno = EINVAL;
            return false;
        }
        if (vector.length && !vector.data) {
            errno = EFAULT;
            return false;
        }
        length += vector.length;
    }
    return true;
}

int ReadInetAddress(const sockaddr* address, uint32_t length, sockaddr_storage& host)
{
    if (!address || length < sizeof(address->sa_family)) {
        errno = EINVAL;
        return -1;
    }
    uint16_t family;
    std::memcpy(&family, address, sizeof(family));
    const int size = family == 2 ? sizeof(sockaddr_in) : family == 10 ? sizeof(sockaddr_in6) : 0;
    if (!size) {
        errno = 97;
        return -1;
    }
    if (length < static_cast<uint32_t>(size)) {
        errno = EINVAL;
        return -1;
    }
    std::memcpy(&host, address, size);
    host.ss_family = family == 10 ? AF_INET6 : AF_INET;
    return size;
}

void WriteInetAddress(void* address, uint32_t& length, sockaddr_storage& host, int size)
{
    host.ss_family = host.ss_family == AF_INET6 ? 10 : 2;
    std::memcpy(address, &host, std::min(length, static_cast<uint32_t>(size)));
    length = size;
}

// Winsock scatter/gather preserves packet boundaries without concatenating the
// payload. When Linux requests the original packet length, reserve the rest of
// the largest UDP packet in a discard buffer to get that length in the same read.
template <bool Send>
intptr_t TransferDatagram(Socket& socket, GuestMessage& message, std::span<const GuestIoVector> vectors, int flags)
{
    const int supported = DontWait | (Send ? NoSignal : Peek | Truncated | ControlCloseOnExec);
    if ((flags & ~supported) || ((flags & DontWait) && !socket.nonblocking)) {
        errno = 95;
        return -1;
    }
    if (Send && message.controlLength) {
        errno = 95;
        return -1;
    }
    size_t length;
    if (!VectorLength(vectors, length))
        return -1;
    constexpr size_t MaxUdpBytes = 65536;
    if (Send && length > MaxUdpBytes) {
        errno = 90;
        return -1;
    }

    sockaddr_storage host{};
    int addressLength = sizeof(host);
    auto* address = message.name ? reinterpret_cast<sockaddr*>(&host) : nullptr;
    if constexpr (Send) {
        addressLength =
            address ? ReadInetAddress(static_cast<const sockaddr*>(message.name), message.nameLength, host) : 0;
        if (addressLength < 0)
            return -1;
    }

    try {
        std::array<WSABUF, 8> local;
        std::vector<WSABUF> extended;
        WSABUF* buffers = local.data();
        if (vectors.size() >= local.size()) {
            extended.resize(vectors.size() + 1);
            buffers = extended.data();
        }
        size_t remaining = MaxUdpBytes;
        DWORD count = 0;
        for (const auto& vector : vectors) {
            const auto size = std::min(vector.length, remaining);
            if (size)
                buffers[count++] = {static_cast<ULONG>(size), static_cast<char*>(vector.data)};
            remaining -= size;
        }
        std::vector<char> discard;
        if (!Send && (flags & Truncated) && remaining) {
            discard.resize(remaining);
            buffers[count++] = {static_cast<ULONG>(discard.size()), discard.data()};
        }
        char empty;
        if (!count)
            buffers[count++] = {0, &empty};

        DWORD transferred = 0;
        DWORD hostFlags = !Send && (flags & Peek) ? MSG_PEEK : 0;
        const int result = Send ? WSASendTo(socket.handle, buffers, count, &transferred, hostFlags, address,
                                            addressLength, nullptr, nullptr)
                                : WSARecvFrom(socket.handle, buffers, count, &transferred, &hostFlags, address,
                                              address ? &addressLength : nullptr, nullptr, nullptr);
        const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
        if (error && (Send || error != WSAEMSGSIZE)) {
            // Winsock reports an ICMP port-unreachable on a UDP socket this way.
            errno = error == WSAECONNRESET ? 111 : GuestError(error);
            if (errno == EAGAIN) {
                if constexpr (Send)
                    ++socket.writes;
                else
                    ++socket.reads;
            }
            return -1;
        }
        if constexpr (Send) {
            return transferred;
        }
        else {
            if (!(flags & Peek))
                ++socket.reads;
            if (address)
                WriteInetAddress(message.name, message.nameLength, host, addressLength);
            else
                message.nameLength = 0;
            message.controlLength = 0;
            message.flags = error == WSAEMSGSIZE || transferred > length ? Truncated : 0;
            if (error == WSAEMSGSIZE)
                return static_cast<intptr_t>(length);
            return static_cast<intptr_t>(transferred);
        }
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

intptr_t SendLocalSocket(Socket& socket, std::span<const GuestIoVector> vectors, int flags,
                         BufferReference attachment = {nullptr, &ReleaseAndroidHardwareBuffer},
                         std::vector<QueuedDescriptor> descriptors = {},
                         std::optional<GuestCredentials> credentials = {}, HANDLE cancelled = nullptr)
{
    constexpr int EndOfRecord = 0x80;
    if (flags & ~(DontWait | NoSignal | EndOfRecord)) {
        errno = 95;
        return -1;
    }
    size_t length;
    if (!VectorLength(vectors, length))
        return -1;
    auto& pair = *socket.pair;
    const bool stream = pair.type == SOCK_STREAM;
    if (stream && !length)
        return 0;
    if (!stream && length > LocalSocketPair::MaxPacket) {
        errno = 90;
        return -1;
    } // EMSGSIZE
    auto& local = pair.ends[socket.side];
    auto& peer = pair.ends[1 - socket.side];
    std::unique_lock lock(pair.mutex);
    for (;;) {
        if (peer.relays && (attachment || (!descriptors.empty() && peer.relays != 1))) {
            errno = 95;
            return -1;
        }
        if (cancelled && WaitForSingleObject(cancelled, 0) != WAIT_TIMEOUT) {
            errno = EPIPE;
            return -1;
        }
        if (!local.sending || !peer.receiving) {
            errno = EPIPE;
            return -1;
        }
        if (peer.hasRoom(stream ? 1 : length))
            break;
        if (socket.nonblocking || (flags & DontWait)) {
            errno = EAGAIN;
            return -1;
        }
        pair.changed.wait(lock);
    }
    if (stream)
        length = std::min(length, LocalSocketPair::QueueBytes - peer.bytes);
    const bool wasEmpty = peer.records.empty();
    try {
        std::vector<SocketFileReference> grants;
        if (peer.relays && !descriptors.empty()) {
            grants.reserve(descriptors.size());
            for (const auto& descriptor : descriptors) {
                if (descriptor.socket()) {
                    const auto channel = FindSocket(descriptor.get());
                    if (!channel || !channel->pair || channel->pair == socket.pair) {
                        errno = 95;
                        return -1;
                    }
                    grants.emplace_back();
                    continue;
                }
                AndroidSocketFile grant;
                if (!ExportAndroidSocketFile(descriptor.get(), grant, peer.brokerRelays == 1))
                    return -1;
                grants.emplace_back(grant);
            }
        }
        else if (!credentials && std::ranges::any_of(descriptors, &QueuedDescriptor::socket)) {
            // A local queue owning an endpoint could keep itself alive.
            errno = 95;
            return -1;
        }
        std::vector<char> data(length);
        size_t offset = 0;
        for (const auto& vector : vectors) {
            if (offset == length)
                break;
            const auto count = std::min(vector.length, length - offset);
            if (count)
                std::memcpy(data.data() + offset, vector.data, count);
            offset += count;
        }
        peer.records.push_back(
            {std::move(data), std::move(attachment), std::move(descriptors), credentials, std::move(grants)});
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
    peer.bytes += length;
    if (wasEmpty)
        ++peer.inputGeneration;
    pair.changed.notify_all();
    return static_cast<intptr_t>(length);
}

bool ReadSentDescriptors(const GuestMessage& message, std::vector<QueuedDescriptor>& descriptors)
{
    const auto* control = static_cast<const char*>(message.control);
    if (!control) {
        errno = EFAULT;
        return false;
    }
    size_t remaining = message.controlLength;
    while (remaining >= sizeof(GuestControl)) {
        GuestControl header;
        std::memcpy(&header, control, sizeof(header));
        if (header.length < sizeof(header) || header.length > remaining || header.length > SIZE_MAX - 7) {
            errno = EINVAL;
            return false;
        }
        if (header.level != SocketLevel || header.type != Rights) {
            errno = 95;
            return false;
        }
        const size_t bytes = header.length - sizeof(header);
        const size_t count = bytes / sizeof(int);
        if (bytes % sizeof(int) || count > MaxRights - descriptors.size()) {
            errno = EINVAL;
            return false;
        }
        descriptors.reserve(descriptors.size() + count);
        for (size_t i = 0; i < count; ++i) {
            int fd;
            std::memcpy(&fd, control + sizeof(header) + i * sizeof(fd), sizeof(fd));
            int copy = -1;
            const bool isSocket = DuplicateAndroidSocket(fd, 0, true, copy);
            if (!isSocket)
                copy = DuplicateAndroidFileFd(fd, true);
            if (copy < 0)
                return false;
            descriptors.emplace_back(copy, isSocket);
        }
        const size_t advance = std::min(remaining, (header.length + 7) & ~size_t{7});
        remaining -= advance;
        control += advance;
    }
    return true;
}

GuestCredentials LocalCredentials()
{
    const auto uid = AndroidApplicationUid();
    return {static_cast<int32_t>(GetCurrentProcessId()), uid, uid};
}

void ReceiveCredentials(GuestMessage& message, size_t capacity, const GuestCredentials& credentials)
{
    constexpr int Credentials = 2; // SCM_CREDENTIALS
    constexpr size_t length = sizeof(GuestControl) + sizeof(GuestCredentials);
    constexpr size_t aligned = (length + 7) & ~size_t{7};
    if (!message.control || capacity < length) {
        message.flags |= ControlTruncated;
        return;
    }
    const GuestControl header{length, SocketLevel, Credentials};
    auto* control = static_cast<char*>(message.control);
    message.controlLength = std::min(capacity, aligned);
    std::memcpy(control, &header, sizeof(header));
    std::memcpy(control + sizeof(header), &credentials, sizeof(credentials));
    std::memset(control + length, 0, message.controlLength - length);
}

void ReceiveDescriptors(GuestMessage& message, size_t capacity, std::vector<QueuedDescriptor>& descriptors, int flags)
{
    const size_t available =
        message.control && !(message.flags & ControlTruncated) ? capacity - message.controlLength : 0;
    const size_t count = available < sizeof(GuestControl)
                             ? 0
                             : std::min(descriptors.size(), (available - sizeof(GuestControl)) / sizeof(int));
    const bool closeOnExec = (flags & ControlCloseOnExec) != 0;
    size_t delivered = 0;
    if (count) {
        auto* control = static_cast<char*>(message.control) + message.controlLength;
        const int saved = errno;
        for (; delivered < count; ++delivered) {
            auto& descriptor = descriptors[delivered];
            int fd;
            if (flags & Peek) {
                if (descriptor.socket()) {
                    fd = -1;
                    DuplicateAndroidSocket(descriptor.get(), 0, closeOnExec, fd);
                }
                else
                    fd = DuplicateAndroidFileFd(descriptor.get(), closeOnExec);
                if (fd < 0)
                    break;
            }
            else {
                const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor.get()));
                if (!closeOnExec) {
                    if (descriptor.socket()) {
                        int result;
                        if (!AndroidSocketFlags(descriptor.get(), 2, 0, result) || result < 0)
                            break;
                    }
                    else if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                        break;
                }
                fd = descriptor.release();
            }
            std::memcpy(control + sizeof(GuestControl) + delivered * sizeof(fd), &fd, sizeof(fd));
        }
        errno = saved;
        if (delivered) {
            const size_t length = sizeof(GuestControl) + delivered * sizeof(int);
            const GuestControl header{length, SocketLevel, Rights};
            const size_t used = std::min(available, (length + 7) & ~size_t{7});
            std::memcpy(control, &header, sizeof(header));
            std::memset(control + length, 0, used - length);
            message.controlLength += used;
        }
    }
    if (delivered < descriptors.size())
        message.flags |= ControlTruncated;
}

intptr_t ReceiveLocalSocket(Socket& socket, std::span<const GuestIoVector> vectors, int flags,
                            GuestMessage* message = nullptr, AndroidHardwareBuffer** attachment = nullptr)
{
    if (flags & ~(DontWait | Peek | Truncated | ControlCloseOnExec)) {
        errno = 95;
        return -1;
    }
    size_t capacity;
    if (!VectorLength(vectors, capacity))
        return -1;
    const size_t controlCapacity = message ? message->controlLength : 0;
    auto& pair = *socket.pair;
    const bool stream = pair.type == SOCK_STREAM;
    auto& local = pair.ends[socket.side];
    auto& peer = pair.ends[1 - socket.side];
    std::vector<QueuedDescriptor> discarded;
    std::unique_lock lock(pair.mutex);
    while ((!stream || capacity) && local.records.empty() && local.receiving && peer.sending) {
        if (socket.nonblocking || (flags & DontWait)) {
            errno = EAGAIN;
            return -1;
        }
        pair.changed.wait(lock);
    }
    if (message) {
        message->flags = 0;
        message->nameLength = 0; // socketpair endpoints are unnamed
        message->controlLength = 0;
    }
    if (local.records.empty() || (stream && !capacity))
        return 0;
    auto* controlRecord = &local.records.front();
    size_t length = local.records.front().data.size();
    size_t rightsOffset = 0;
    if (stream) {
        length = 0;
        size_t offset = local.offset;
        controlRecord = nullptr;
        for (auto& queued : local.records) {
            if (length >= capacity)
                break;
            if (!queued.descriptors.empty()) {
                controlRecord = &queued;
                rightsOffset = length;
                length += queued.data.size() - offset;
                break;
            }
            length += queued.data.size() - offset;
            offset = 0;
        }
    }
    const size_t copied = std::min(capacity, length);
    auto record = local.records.begin();
    size_t sourceOffset = local.offset, remaining = copied;
    for (const auto& vector : vectors) {
        const size_t output = std::min(vector.length, remaining);
        auto* destination = static_cast<char*>(vector.data);
        for (size_t written = 0; written < output;) {
            const size_t count = std::min(output - written, record->data.size() - sourceOffset);
            std::memcpy(destination + written, record->data.data() + sourceOffset, count);
            written += count;
            sourceOffset += count;
            if (sourceOffset == record->data.size()) {
                ++record;
                sourceOffset = 0;
            }
        }
        remaining -= output;
        if (!remaining)
            break;
    }
    if (message) {
        if (!stream && copied < length)
            message->flags |= Truncated;
        if (local.passCredentials) {
            ReceiveCredentials(*message, controlCapacity,
                               local.records.front().credentials.value_or(LocalCredentials()));
        }
        if (controlRecord && (!stream || copied > rightsOffset)) {
            ReceiveDescriptors(*message, controlCapacity, controlRecord->descriptors, flags);
        }
    }
    if (!(flags & Peek)) {
        const bool wasFull = !local.hasRoom(1);
        if (controlRecord && (!stream || copied > rightsOffset)) {
            discarded = std::move(controlRecord->descriptors);
            controlRecord->grants.clear();
        }
        if (attachment && !local.offset)
            *attachment = const_cast<AndroidHardwareBuffer*>(local.records.front().attachment.release());
        if (stream) {
            local.records.erase(local.records.begin(), record);
            local.offset = sourceOffset;
            local.bytes -= copied;
        }
        else {
            local.bytes -= length;
            local.records.pop_front();
        }
        if (wasFull && local.hasRoom(1))
            ++peer.outputGeneration;
        pair.changed.notify_all();
    }
    return static_cast<intptr_t>((!stream && (flags & Truncated)) ? length : copied);
}

void ShutdownLocalSocket(Socket& socket, int how)
{
    auto& pair = *socket.pair;
    // Closing a queued socket can acquire the mutex of a different pair.
    std::deque<LocalSocketPair::Record> discarded;
    const std::lock_guard lock(pair.mutex);
    auto& local = pair.ends[socket.side];
    auto& peer = pair.ends[1 - socket.side];
    if (how != SD_SEND && local.receiving) {
        local.receiving = false;
        discarded.swap(local.records);
        local.offset = 0;
        local.bytes = 0;
        ++local.inputGeneration;
    }
    if (how != SD_RECEIVE && local.sending) {
        local.sending = false;
        ++peer.inputGeneration;
    }
    pair.changed.notify_all();
}

// These pipes are opened by the broker before transferring the client handle.
// The AppContainer never opens a named endpoint or needs a network capability.
bool CreateRelayPipe(PipeHandle& server, PipeHandle& client, bool local)
{
    std::array<uint64_t, 2> nonce{};
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(nonce.data()), sizeof(nonce),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return false;
    const auto prefix = local ? L"\\\\.\\pipe\\LOCAL\\Stone-stream-" : L"\\\\.\\pipe\\Stone-stream-";
    const auto name = prefix + std::to_wstring(nonce[0]) + L'-' + std::to_wstring(nonce[1]);
    const auto first = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
    if (first == INVALID_HANDLE_VALUE)
        return false;
    server.reset(first);
    const auto second = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
    if (second == INVALID_HANDLE_VALUE)
        return false;
    client.reset(second);
    ULONG connected = 0;
    return GetNamedPipeClientProcessId(first, &connected) && connected == GetCurrentProcessId();
}

bool RelayBytes(HANDLE pipe, HANDLE event, HANDLE cancelled, bool write, void* bytes, DWORD size)
{
    auto* next = static_cast<char*>(bytes);
    while (size) {
        OVERLAPPED operation{};
        operation.hEvent = event;
        ResetEvent(event);
        DWORD done = 0;
        if (WaitForSingleObject(cancelled, 0) != WAIT_TIMEOUT)
            return false;
        if (!(write ? WriteFile(pipe, next, size, &done, &operation) : ReadFile(pipe, next, size, &done, &operation))) {
            if (GetLastError() != ERROR_IO_PENDING)
                return false;
            const HANDLE waiting[] = {event, cancelled};
            if (WaitForMultipleObjects(2, waiting, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation);
                GetOverlappedResult(pipe, &operation, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &operation, &done, FALSE))
                return false;
        }
        if (!done)
            return false;
        next += done;
        size -= done;
    }
    return true;
}

// Preserve the existing guest socket queues, descriptor lifetime and poll
// generations. Only the endpoint held by this relay moves data over the pipe.
uintptr_t OpenSocketRelay(int fd, GuestCredentials peer, PipeHandle target, unsigned& flags);
int AdoptSocketRelay(PipeHandle pipe, unsigned flags, GuestCredentials peer, PipeHandle target);

PipeHandle ForkTargetHandle(HANDLE target)
{
    const auto process = GetCurrentProcess();
    HANDLE duplicate = nullptr;
    if (target)
        DuplicateHandle(process, target, process, &duplicate, PROCESS_DUP_HANDLE, FALSE, 0);
    return {duplicate, &CloseHandle};
}

class UnixSocketRelay {
public:
    UnixSocketRelay(PipeHandle pipe, SocketReference fd, const Socket& local, GuestCredentials peer, PipeHandle target,
                    bool exporting)
        : pipe_(std::move(pipe)), retained_(std::move(fd)), socket_(INVALID_SOCKET, local.type), peer_(peer),
          target_(std::move(target)), exporting_(exporting)
    {
        socket_.pair = local.pair;
        socket_.side = local.side;
    }

    bool reserve()
    {
        auto& pair = *socket_.pair;
        const std::lock_guard lock(pair.mutex);
        auto& local = pair.ends[socket_.side];
        if (!local.receiving || !local.sending) {
            errno = EPIPE;
            return false;
        }
        if (std::ranges::any_of(local.records, [](const auto& item) {
                return item.attachment || !item.descriptors.empty();
            })) {
            errno = 95;
            return false;
        } // EOPNOTSUPP
        ++local.relays;
        if (target_)
            ++local.brokerRelays;
        reserved_ = true;
        return true;
    }

    ~UnixSocketRelay()
    {
        if (reserved_) {
            const std::lock_guard lock(socket_.pair->mutex);
            auto& local = socket_.pair->ends[socket_.side];
            --local.relays;
            if (target_)
                --local.brokerRelays;
        }
    }

    void run() noexcept
    {
        PipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle);
        if (!cancelled_ || !event)
            return;
        uint32_t hello = exporting_ ? 0 : Protocol;
        // A revoked Binder grant has no effect on the local socket until the
        // recipient adopts the transferred pipe and acknowledges it.
        if (!RelayBytes(pipe_.get(), event.get(), cancelled_.get(), !exporting_, &hello, sizeof(hello)) ||
            hello != Protocol)
            return;
        std::thread outbound;
        try {
            outbound = std::thread([this] {
                try {
                    write();
                }
                catch (const std::exception&) {
                    stop();
                }
            });
            read(event.get());
        }
        catch (const std::exception&) {
        }
        stop();
        if (outbound.joinable())
            outbound.join();
    }

private:
    static constexpr uint32_t Protocol = 0x53545234, Closed = UINT32_MAX, Finished = UINT32_MAX - 1;
    static constexpr unsigned UnixEndpoint = 0x80000000;

    struct Header {
        uint32_t length, grants;
    };

    static_assert(sizeof(AndroidSocketFile) == 24);
    PipeHandle pipe_;
    PipeHandle cancelled_{CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle};
    SocketReference retained_;
    Socket socket_;
    const GuestCredentials peer_;
    const PipeHandle target_;
    const bool exporting_;
    bool reserved_ = false;
    std::atomic<bool> remoteFinished_ = false;

    void stop()
    {
        SetEvent(cancelled_.get());
        if (exporting_) {
            // The original descriptor may still be shared with other workers.
            // Dropping this relay closes only its retained descriptor.
            const std::lock_guard lock(socket_.pair->mutex);
            socket_.pair->changed.notify_all();
        }
        else {
            ShutdownLocalSocket(socket_, SD_BOTH);
        }
    }

    bool transfer(HANDLE event, bool write, void* data, DWORD size)
    {
        return RelayBytes(pipe_.get(), event, cancelled_.get(), write, data, size);
    }

    bool peerClosed()
    {
        const std::lock_guard lock(socket_.pair->mutex);
        const auto& peer = socket_.pair->ends[1 - socket_.side];
        return !peer.receiving && !peer.sending;
    }

    void finishWrite(HANDLE event, uint32_t length)
    {
        if (length != Closed) {
            auto& pair = *socket_.pair;
            const auto& peer = pair.ends[1 - socket_.side];
            {
                std::unique_lock lock(pair.mutex);
                pair.changed.wait(lock, [&] {
                    return remoteFinished_ || !peer.receiving ||
                           WaitForSingleObject(cancelled_.get(), 0) != WAIT_TIMEOUT;
                });
            }
            if (!remoteFinished_) {
                Header close{Closed, 0};
                transfer(event, true, &close, sizeof(close));
            }
        }
        stop();
    }

    // The writer takes whole records to keep packet boundaries on seqpacket
    // sockets. A stream record may already have been partly read by a local dup.
    std::optional<LocalSocketPair::Record> nextRecord(size_t& offset)
    {
        auto& pair = *socket_.pair;
        auto& local = pair.ends[socket_.side];
        auto& peer = pair.ends[1 - socket_.side];
        std::unique_lock lock(pair.mutex);
        pair.changed.wait(lock, [&] {
            return !local.records.empty() || !local.receiving || !peer.sending ||
                   WaitForSingleObject(cancelled_.get(), 0) != WAIT_TIMEOUT;
        });
        if (WaitForSingleObject(cancelled_.get(), 0) != WAIT_TIMEOUT)
            return std::nullopt;
        if (local.records.empty())
            return std::nullopt;
        const bool wasFull = !local.hasRoom(1);
        auto record = std::move(local.records.front());
        local.records.pop_front();
        offset = std::exchange(local.offset, 0);
        local.bytes -= record.data.size() - offset;
        if (wasFull)
            ++peer.outputGeneration;
        pair.changed.notify_all();
        return record;
    }

    bool writeGrants(HANDLE event, LocalSocketPair::Record& record)
    {
        const auto process = GetCurrentProcess();
        for (size_t i = 0; i < record.grants.size(); ++i) {
            auto& local = record.grants[i];
            const auto& descriptor = record.descriptors[i];
            if (descriptor.socket()) {
                auto target = ForkTargetHandle(target_.get());
                if (target_ && !target)
                    return false;
                unsigned flags = 0;
                const auto pipe = OpenSocketRelay(descriptor.get(), peer_, std::move(target), flags);
                if (!pipe)
                    return false;
                local.file.reset(reinterpret_cast<HANDLE>(pipe));
                local.mode = UnixEndpoint;
                local.seals = flags;
            }
            HANDLE file = nullptr, section = nullptr;
            if (target_) {
                if (!DuplicateHandle(process, local.file.get(), target_.get(), &file, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    return false;
                if (local.section && !DuplicateHandle(process, local.section.get(), target_.get(), &section, 0, FALSE,
                                                      DUPLICATE_SAME_ACCESS)) {
                    DuplicateHandle(target_.get(), file, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
                    return false;
                }
            }
            else {
                // Only the trusted broker has a process handle for cross-process duplication.
                file = local.file.release();
                section = local.section.release();
            }
            AndroidSocketFile entry{reinterpret_cast<uintptr_t>(file), reinterpret_cast<uintptr_t>(section), local.mode,
                                    local.seals};
            // Once exposed, IDs belong to the receiver or are closed with the worker process.
            if (!transfer(event, true, &entry, sizeof(entry)))
                return false;
        }
        return true;
    }

    int importGrant(AndroidSocketFile grant)
    {
        const auto validHandle = [](uintptr_t handle) {
            return handle >= 4 && handle <= UINT32_MAX && !(handle & 3);
        };
        const auto copy = [&](uintptr_t handle, DWORD access, DWORD flags = 0) {
            HANDLE duplicate = nullptr;
            DuplicateHandle(target_.get(), reinterpret_cast<HANDLE>(handle), GetCurrentProcess(), &duplicate, access,
                            FALSE, DUPLICATE_CLOSE_SOURCE | flags);
            return reinterpret_cast<uintptr_t>(duplicate);
        };
        if (grant.mode == UnixEndpoint) {
            if (!validHandle(grant.file) || grant.section)
                return -1;
            const auto handle = target_ ? copy(grant.file, 0, DUPLICATE_SAME_ACCESS) : grant.file;
            PipeHandle pipe(reinterpret_cast<HANDLE>(handle), &CloseHandle);
            auto target = ForkTargetHandle(target_.get());
            if (!pipe || (target_ && !target))
                return -1;
            return AdoptSocketRelay(std::move(pipe), grant.seals, peer_, std::move(target));
        }
        if (!target_)
            return ImportAndroidSocketFile(grant);
        if (!validHandle(grant.file) || !validHandle(grant.section) || grant.file == grant.section)
            return -1;
        grant.file = copy(grant.file, FILE_GENERIC_READ);
        constexpr unsigned writeSeals = 0x18;
        grant.section = copy(grant.section, SECTION_MAP_READ | ((grant.seals & writeSeals) ? 0 : SECTION_MAP_WRITE));
        if (!grant.section) {
            if (grant.file)
                CloseHandle(reinterpret_cast<HANDLE>(grant.file));
            return -1;
        }
        return ImportAndroidSocketFile(grant);
    }

    void write()
    {
        PipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle);
        if (!event) {
            stop();
            return;
        }
        for (;;) {
            size_t offset = 0;
            auto record = nextRecord(offset);
            Header header{record         ? static_cast<uint32_t>(record->data.size() - offset)
                          : peerClosed() ? Closed
                                         : Finished,
                          record ? static_cast<uint32_t>(record->grants.size()) : 0};
            if (!transfer(event.get(), true, &header, sizeof(header)) ||
                (header.grants && !writeGrants(event.get(), *record)) ||
                (record && header.length &&
                 !transfer(event.get(), true, record->data.data() + offset, header.length))) {
                stop();
                return;
            }
            if (!record) {
                finishWrite(event.get(), header.length);
                return;
            }
        }
    }

    void read(HANDLE event)
    {
        bool eof = false;
        std::vector<char> bytes;
        const bool packet = socket_.type == Seqpacket;
        for (;;) {
            Header header;
            if (!transfer(event, false, &header, sizeof(header)))
                return;
            const auto length = header.length;
            if (header.grants > MaxRights || (header.grants && length >= Finished))
                return;
            if (length == Closed)
                return;
            if (length == Finished && !eof) {
                remoteFinished_ = true;
                ShutdownLocalSocket(socket_, SD_SEND);
                eof = true;
                continue;
            }
            if (eof || length > (packet ? LocalSocketPair::MaxPacket : LocalSocketPair::QueueBytes) ||
                (!packet && !length))
                return;
            std::vector<QueuedDescriptor> descriptors;
            if (header.grants) {
                descriptors.reserve(header.grants);
                bool valid = true;
                for (uint32_t i = 0; i < header.grants; ++i) {
                    AndroidSocketFile grant;
                    if (!transfer(event, false, &grant, sizeof(grant)))
                        return;
                    const int fd = importGrant(grant);
                    if (fd < 0)
                        valid = false;
                    else
                        descriptors.emplace_back(fd, grant.mode == UnixEndpoint);
                }
                if (!valid)
                    return;
            }
            bytes.resize(length);
            if (!transfer(event, false, bytes.data(), length))
                return;
            if (packet) {
                const GuestIoVector vector{bytes.data(), length};
                if (SendLocalSocket(socket_, std::span{&vector, 1}, 0, {nullptr, &ReleaseAndroidHardwareBuffer},
                                    std::move(descriptors), peer_, cancelled_.get()) < 0)
                    return;
                continue;
            }
            size_t offset = 0;
            while (offset < length) {
                const GuestIoVector vector{bytes.data() + offset, length - offset};
                const auto sent =
                    SendLocalSocket(socket_, std::span{&vector, 1}, 0, {nullptr, &ReleaseAndroidHardwareBuffer},
                                    std::move(descriptors), peer_, cancelled_.get());
                if (sent < 0)
                    return;
                offset += static_cast<size_t>(sent);
            }
        }
    }
};

bool StartUnixRelay(PipeHandle pipe, SocketReference retained, GuestCredentials peer, PipeHandle target, bool exporting)
{
    const auto socket = FindSocket(retained.get());
    if (!socket || !socket->pair) {
        errno = 88;
        return false;
    }
    try {
        auto relay = std::make_unique<UnixSocketRelay>(std::move(pipe), std::move(retained), *socket, peer,
                                                       std::move(target), exporting);
        if (!relay->reserve())
            return false;
        std::thread([state = std::move(relay)] {
            state->run();
        }).detach();
        return true;
    }
    catch (const std::exception&) {
        errno = EAGAIN;
        return false;
    }
}

template <bool Send>
intptr_t TransferDatagramBytes(Socket& socket, void* buffer, size_t bytes, int flags, void* address = nullptr,
                               uint32_t* addressLength = nullptr)
{
    const GuestIoVector vector{buffer, bytes};
    GuestMessage message{};
    message.name = address;
    message.nameLength = address && addressLength ? *addressLength : 0;
    const auto count = TransferDatagram<Send>(socket, message, std::span{&vector, 1}, flags);
    if constexpr (!Send) {
        if (count >= 0 && address && addressLength)
            *addressLength = message.nameLength;
    }
    return count;
}

intptr_t ReceiveSocket(Socket& socket, void* buffer, size_t bytes, int flags,
                       AndroidHardwareBuffer** attachment = nullptr)
{
    if (socket.type == SOCK_DGRAM)
        return TransferDatagramBytes<false>(socket, buffer, bytes, flags);
    if (!socket.pair)
        return TransferSocketBytes<false>(socket, buffer, bytes, flags);
    const GuestIoVector vector{buffer, bytes};
    return ReceiveLocalSocket(socket, std::span{&vector, 1}, flags, nullptr, attachment);
}

intptr_t SendSocket(Socket& socket, const void* buffer, size_t bytes, int flags)
{
    if (socket.type == SOCK_DGRAM)
        return TransferDatagramBytes<true>(socket, const_cast<void*>(buffer), bytes, flags);
    if (!socket.pair)
        return TransferSocketBytes<true>(socket, const_cast<void*>(buffer), bytes, flags);
    const GuestIoVector vector{const_cast<void*>(buffer), bytes};
    return SendLocalSocket(socket, std::span{&vector, 1}, flags);
}

template <bool Send> intptr_t TransferSocket(int fd, void* buffer, size_t bytes, int flags)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if constexpr (Send)
        return SendSocket(*socket, buffer, bytes, flags);
    else
        return ReceiveSocket(*socket, buffer, bytes, flags);
}

intptr_t SendTo(int fd, const void* buffer, size_t bytes, int flags, const sockaddr* address, uint32_t length)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if (socket->type == SOCK_DGRAM) {
        return TransferDatagramBytes<true>(*socket, const_cast<void*>(buffer), bytes, flags,
                                           const_cast<sockaddr*>(address), &length);
    }
    if (address) {
        errno = 106;
        return -1;
    } // EISCONN
    return SendSocket(*socket, buffer, bytes, flags);
}

intptr_t ReceiveFrom(int fd, void* buffer, size_t bytes, int flags, sockaddr* address, uint32_t* length)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if (address && !length) {
        errno = EFAULT;
        return -1;
    }
    if (socket->type == SOCK_DGRAM)
        return TransferDatagramBytes<false>(*socket, buffer, bytes, flags, address, length);
    const auto count = ReceiveSocket(*socket, buffer, bytes, flags);
    if (count >= 0 && address)
        *length = 0;
    return count;
}

template <bool Send> intptr_t TransferMessage(int fd, GuestMessage* message, int flags)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if (!socket->pair && socket->type != SOCK_DGRAM)
        return AndroidUnsupportedOperation();
    if (!message || (message->vectorCount && !message->vectors)) {
        errno = EFAULT;
        return -1;
    }
    if (message->vectorCount > 1024) {
        errno = 90;
        return -1;
    } // EMSGSIZE
    const std::span vectors{message->vectors, message->vectorCount};
    if (socket->type == SOCK_DGRAM)
        return TransferDatagram<Send>(*socket, *message, vectors, flags);
    if constexpr (Send) {
        if (message->name) {
            errno = 95;
            return -1;
        }
        try {
            std::vector<QueuedDescriptor> descriptors;
            if (message->controlLength && !ReadSentDescriptors(*message, descriptors))
                return -1;
            return SendLocalSocket(*socket, vectors, flags, {nullptr, &ReleaseAndroidHardwareBuffer},
                                   std::move(descriptors));
        }
        catch (const std::bad_alloc&) {
            errno = ENOMEM;
            return -1;
        }
    }
    else {
        return ReceiveLocalSocket(*socket, vectors, flags, message);
    }
}

GuestControl* NextControl(GuestMessage* message, GuestControl* current)
{
    if (!message || !message->control || !current)
        return nullptr;
    const auto base = reinterpret_cast<uintptr_t>(message->control);
    const auto address = reinterpret_cast<uintptr_t>(current);
    const auto offset = address - base;
    if (address < base || offset > message->controlLength)
        return nullptr;
    const auto remaining = message->controlLength - offset;
    if (remaining < sizeof(GuestControl))
        return nullptr;
    size_t length;
    std::memcpy(&length, current, sizeof(length));
    if (length < sizeof(GuestControl) || length > remaining || length > SIZE_MAX - 7)
        return nullptr;
    const size_t stride = (length + 7) & ~size_t{7};
    if (stride > remaining - sizeof(GuestControl))
        return nullptr;
    return reinterpret_cast<GuestControl*>(static_cast<char*>(message->control) + offset + stride);
}

template <bool Connect> int UseAddress(int fd, const sockaddr* address, int length)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if (socket->pair) {
        errno = Connect ? 106 : EINVAL;
        return -1;
    } // EISCONN
    if (socket->unixSocket)
        return UseUnixAddress(*socket, address, length, Connect);
    sockaddr_storage host{};
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    const int size = ReadInetAddress(address, length, host);
    if (size < 0)
        return -1;
    const auto* converted = reinterpret_cast<const sockaddr*>(&host);
    if constexpr (Connect) {
        if (connect(socket->handle, converted, size) == 0)
            return 0;
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            errno = 115;
            return -1;
        }
    }
    else if (bind(socket->handle, converted, size) == 0) {
        return 0;
    }
    return SocketError();
}

template <bool Peer> int SocketAddress(int fd, sockaddr* address, int* length)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if (!address || !length || *length < 0) {
        errno = EINVAL;
        return -1;
    }
    if (socket->unixSocket) {
        const std::lock_guard lock(socket->addressMutex);
        const auto& guest = Peer ? socket->peerAddress : socket->localAddress;
        if constexpr (Peer) {
            if (guest.empty()) {
                errno = 107;
                return -1;
            }
        }
        const uint16_t family = UnixFamily;
        const int size = guest.empty() ? sizeof(family) : static_cast<int>(guest.size());
        std::memcpy(address, guest.empty() ? static_cast<const void*>(&family) : guest.data(), std::min(*length, size));
        *length = size;
        return 0;
    }
    if (socket->pair) {
        const uint16_t family = UnixFamily;
        const int size = sizeof(family);
        std::memcpy(address, &family, std::min(*length, size));
        *length = size;
        return 0;
    }
    sockaddr_storage host{};
    int size = sizeof(host);
    auto* converted = reinterpret_cast<sockaddr*>(&host);
    const int result =
        Peer ? getpeername(socket->handle, converted, &size) : getsockname(socket->handle, converted, &size);
    if (result == SOCKET_ERROR)
        return SocketError();
    host.ss_family = host.ss_family == AF_UNIX ? UnixFamily : host.ss_family == AF_INET6 ? 10 : 2;
    std::memcpy(address, &host, std::min(*length, size));
    *length = size;
    return 0;
}

int ShutdownSocket(int fd, int how)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if (socket->pair) {
        if (how < SD_RECEIVE || how > SD_BOTH) {
            errno = EINVAL;
            return -1;
        }
        ShutdownLocalSocket(*socket, how);
        return 0;
    }
    return shutdown(socket->handle, how) == SOCKET_ERROR ? SocketError() : 0;
}

int ListenSocket(int fd, int backlog)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if (socket->pair) {
        errno = EINVAL;
        return -1;
    }
    if (listen(socket->handle, backlog) == SOCKET_ERROR)
        return SocketError();
    socket->listening = true;
    return 0;
}

int AcceptSocket(int fd, sockaddr* address, int* length, int flags)
{
    if ((flags & ~(Nonblocking | CloseOnExec)) || (address && (!length || *length < 0))) {
        errno = EINVAL;
        return -1;
    }
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if (socket->pair) {
        errno = EINVAL;
        return -1;
    }
    sockaddr_storage host{};
    int size = sizeof(host);
    const SOCKET accepted =
        accept(socket->handle, address ? reinterpret_cast<sockaddr*>(&host) : nullptr, address ? &size : nullptr);
    if (accepted == INVALID_SOCKET)
        return SocketError();
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(accepted), HANDLE_FLAG_INHERIT, 0)) {
        closesocket(accepted);
        errno = EIO;
        return -1;
    }
    const int result = RegisterSocket(accepted, SOCK_STREAM, (flags & Nonblocking) != 0, (flags & CloseOnExec) != 0, {},
                                      0, socket->unixSocket);
    if (result < 0)
        return result;
    if (socket->unixSocket) {
        try {
            const std::lock_guard lock(socket->addressMutex);
            const auto child = FindSocket(result);
            child->localAddress = socket->localAddress;
            const uint16_t family = UnixFamily;
            child->peerAddress.assign(reinterpret_cast<const char*>(&family),
                                      reinterpret_cast<const char*>(&family) + sizeof(family));
            if (address) {
                std::memcpy(address, &family, std::min(*length, 2));
                *length = 2;
            }
            return result;
        }
        catch (const std::bad_alloc&) {
            CloseAndroidFd(result);
            errno = ENOMEM;
            return -1;
        }
    }
    if (address) {
        host.ss_family = host.ss_family == AF_UNIX ? UnixFamily : host.ss_family == AF_INET6 ? 10 : 2;
        std::memcpy(address, &host, std::min(*length, size));
        *length = size;
    }
    return result;
}

bool SocketOption(int& level, int& option)
{
    if (level == IPPROTO_TCP && option == 1) {
        option = TCP_NODELAY;
        return true;
    }
    if (level == IPPROTO_IP) {
        switch (option) {
        case 2:
            option = IP_TTL;
            return true;
        case 32:
            option = IP_MULTICAST_IF;
            return true;
        case 33:
            option = IP_MULTICAST_TTL;
            return true;
        case 34:
            option = IP_MULTICAST_LOOP;
            return true;
        case 35:
            option = IP_ADD_MEMBERSHIP;
            return true;
        case 36:
            option = IP_DROP_MEMBERSHIP;
            return true;
        case 42:
            option = MCAST_JOIN_GROUP;
            return true;
        case 45:
            option = MCAST_LEAVE_GROUP;
            return true;
        default:
            return false;
        }
    }
    if (level == IPPROTO_IPV6) {
        switch (option) {
        case 16:
            option = IPV6_UNICAST_HOPS;
            return true;
        case 17:
            option = IPV6_MULTICAST_IF;
            return true;
        case 18:
            option = IPV6_MULTICAST_HOPS;
            return true;
        case 19:
            option = IPV6_MULTICAST_LOOP;
            return true;
        case 20:
            option = IPV6_JOIN_GROUP;
            return true;
        case 21:
            option = IPV6_LEAVE_GROUP;
            return true;
        case 26:
            option = IPV6_V6ONLY;
            return true;
        case 42:
            option = MCAST_JOIN_GROUP;
            return true;
        case 45:
            option = MCAST_LEAVE_GROUP;
            return true;
        default:
            return false;
        }
    }
    if (level != 1)
        return false;
    level = SOL_SOCKET;
    switch (option) {
    case 2:
        option = SO_REUSEADDR;
        return true;
    case 3:
        option = SO_TYPE;
        return true;
    case 4:
        option = SO_ERROR;
        return true;
    case 6:
        option = SO_BROADCAST;
        return true;
    case 7:
        option = SO_SNDBUF;
        return true;
    case 8:
        option = SO_RCVBUF;
        return true;
    case 9:
        option = SO_KEEPALIVE;
        return true;
    default:
        return false;
    }
}

int GetSocketOption(int fd, int level, int option, char* value, int* length)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    const auto copy = [&](const auto& result) {
        *length = std::min(*length, static_cast<int>(sizeof(result)));
        if (*length)
            std::memcpy(value, &result, *length);
        return 0;
    };
    if (socket->pair) {
        if (level != SocketLevel) {
            errno = 92;
            return -1;
        }
        if (!length || *length < 0 || (!value && *length)) {
            errno = EFAULT;
            return -1;
        }
        switch (option) {
        case 3:
            return copy(socket->pair->type); // SO_TYPE
        case 4:
            return copy(0); // SO_ERROR
        case 7:
        case 8:
            return copy(static_cast<int>(LocalSocketPair::QueueBytes)); // SO_SNDBUF, SO_RCVBUF
        case PassCredentials: {
            const std::lock_guard lock(socket->pair->mutex);
            return copy(static_cast<int>(socket->pair->ends[socket->side].passCredentials));
        }
        case 17: { // SO_PEERCRED
            const std::lock_guard lock(socket->pair->mutex);
            if (socket->pair->ends[0].relays || socket->pair->ends[1].relays) {
                errno = 95;
                return -1;
            }
            return copy(LocalCredentials());
        }
        case 38:
            return copy(0); // SO_PROTOCOL
        case 39:
            return copy(UnixFamily); // SO_DOMAIN
        default:
            errno = 92;
            return -1;
        }
    }
    if (level == SocketLevel && (option == 38 || option == 39)) { // SO_PROTOCOL, SO_DOMAIN.
        if (!length || *length < 0 || (!value && *length)) {
            errno = EFAULT;
            return -1;
        }
        WSAPROTOCOL_INFOW info{};
        int size = sizeof(info);
        if (getsockopt(socket->handle, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&info), &size) ==
            SOCKET_ERROR)
            return SocketError();
        if (option == 38)
            return copy(info.iProtocol);
        if (info.iAddressFamily == AF_UNIX)
            return copy(UnixFamily);
        if (info.iAddressFamily == AF_INET)
            return copy(2);
        if (info.iAddressFamily == AF_INET6)
            return copy(10);
        errno = 97;
        return -1;
    }
    if (level == IPPROTO_IP && option == 49) { // Linux IP_MULTICAST_ALL.
        if (!length || *length < 0 || (!value && *length)) {
            errno = EFAULT;
            return -1;
        }
        // Winsock receives groups joined by this socket; there is no Linux
        // receive-all-groups mode. Android explicitly selects this zero mode.
        return copy(0);
    }
    if (!SocketOption(level, option)) {
        errno = 92;
        return -1;
    }
    if (!length || *length < 0 || (!value && *length)) {
        errno = EFAULT;
        return -1;
    }
    // The mapped Android options are int-valued. Winsock may return a single
    // byte for boolean options such as SO_KEEPALIVE.
    int result = 0, hostLength = sizeof(result);
    if (getsockopt(socket->handle, level, option, reinterpret_cast<char*>(&result), &hostLength) == SOCKET_ERROR)
        return SocketError();
    if (level == SOL_SOCKET && option == SO_ERROR)
        result = GuestError(result);
    if (level == IPPROTO_IP && option == IP_MULTICAST_IF) {
        // Linux reads an IPv4 address, while Winsock reads an index (whose byte
        // order differs across implementations). Retain the accepted guest
        // selection; resolve an index through the current host address table.
        const auto selected = socket->multicastV4Interface.load();
        if (!(selected >> 32))
            return copy(static_cast<uint32_t>(selected));
        const auto interfaceIndex = static_cast<DWORD>(selected);
        const auto interfaces = ReadClientHostState().interfaces;
        if (!interfaces || interfaces->error) {
            errno = interfaces ? interfaces->error : 11;
            return -1;
        }
        for (const auto& entry : interfaces->entries)
            if (entry.index == interfaceIndex)
                for (const auto& address : entry.addresses)
                    if (address.family == 2) {
                        uint32_t ip;
                        std::memcpy(&ip, address.bytes.data(), sizeof(ip));
                        return copy(ip);
                    }
        errno = ENODEV;
        return -1;
    }
    return copy(result);
}

int SetSocketOption(int fd, int level, int option, const char* value, int length)
{
    const auto socket = FindSocket(fd);
    if (!socket) {
        errno = 88;
        return -1;
    }
    if (socket->pair) {
        if (level != SocketLevel || option != PassCredentials) {
            errno = 92;
            return -1;
        }
        if (length < static_cast<int>(sizeof(int))) {
            errno = EINVAL;
            return -1;
        }
        if (!value) {
            errno = EFAULT;
            return -1;
        }
        int enabled;
        std::memcpy(&enabled, value, sizeof(enabled));
        const std::lock_guard lock(socket->pair->mutex);
        socket->pair->ends[socket->side].passCredentials = enabled != 0;
        return 0;
    }
    if (level == IPPROTO_IP && option == 49) {
        if (length < static_cast<int>(sizeof(int))) {
            errno = EINVAL;
            return -1;
        }
        if (!value) {
            errno = EFAULT;
            return -1;
        }
        int enabled;
        std::memcpy(&enabled, value, sizeof(enabled));
        if (enabled < 0 || enabled > 1) {
            errno = EINVAL;
            return -1;
        }
        if (enabled) {
            errno = 95;
            return -1;
        } // No Windows receive-all-groups equivalent.
        return 0;
    }
    const int guestLevel = level, guestOption = option;
    if (!SocketOption(level, option)) {
        errno = 92;
        return -1;
    }
    if ((guestLevel == IPPROTO_IP || guestLevel == IPPROTO_IPV6) && (guestOption == 42 || guestOption == 45)) {
        static_assert(sizeof(GROUP_REQ) == 136 && offsetof(GROUP_REQ, gr_group) == 8);
        if (length < static_cast<int>(sizeof(GROUP_REQ))) {
            errno = EINVAL;
            return -1;
        }
        if (!value) {
            errno = EFAULT;
            return -1;
        }
        GROUP_REQ group{};
        std::memcpy(&group.gr_interface, value, sizeof(group.gr_interface));
        if (ReadInetAddress(reinterpret_cast<const sockaddr*>(value + 8), 128, group.gr_group) < 0)
            return -1;
        if (group.gr_group.ss_family != (guestLevel == IPPROTO_IP ? AF_INET : AF_INET6)) {
            errno = EINVAL;
            return -1;
        }
        return setsockopt(socket->handle, level, option, reinterpret_cast<const char*>(&group), sizeof(group)) ==
                       SOCKET_ERROR
                   ? SocketError()
                   : 0;
    }
    if (guestLevel == IPPROTO_IP && (guestOption == 32 || guestOption == 35 || guestOption == 36)) {
        struct Request {
            uint32_t group, address;
            int index;
        } request{};

        uint32_t selected = 0;
        if (length < (guestOption == 32 ? 4 : 8)) {
            errno = EINVAL;
            return -1;
        }
        if (!value) {
            errno = EFAULT;
            return -1;
        }
        if (length >= static_cast<int>(sizeof(request))) {
            std::memcpy(&request, value, sizeof(request));
            if (request.index < 0) {
                errno = EINVAL;
                return -1;
            }
            if (request.index > 0xffffff) {
                errno = 95;
                return -1;
            }
            selected = request.index ? htonl(request.index) : request.address;
        }
        else {
            if (guestOption == 32)
                std::memcpy(&request.address, value, 4);
            else
                std::memcpy(&request, value, 8);
            selected = request.address;
        }
        ip_mreq host{};
        host.imr_multiaddr.s_addr = request.group;
        host.imr_interface.s_addr = selected;
        const char* payload =
            guestOption == 32 ? reinterpret_cast<const char*>(&selected) : reinterpret_cast<const char*>(&host);
        const int size = guestOption == 32 ? sizeof(selected) : sizeof(host);
        if (setsockopt(socket->handle, level, option, payload, size) == SOCKET_ERROR)
            return SocketError();
        if (guestOption == 32)
            socket->multicastV4Interface =
                request.index ? (uint64_t{1} << 32) | static_cast<unsigned>(request.index) : request.address;
        return 0;
    }
    if ((guestLevel == IPPROTO_IP && (guestOption == 33 || guestOption == 34)) ||
        (guestLevel == IPPROTO_IPV6 && guestOption == 19)) {
        if (length != 1 && length < static_cast<int>(sizeof(int))) {
            errno = EINVAL;
            return -1;
        }
        if (!value) {
            errno = EFAULT;
            return -1;
        }
        int scalar = 0;
        std::memcpy(&scalar, value, length == 1 ? 1 : sizeof(scalar));
        const bool loop = guestOption == 34 || guestLevel == IPPROTO_IPV6;
        if (loop) {
            // Winsock's off mode filters receiving, whereas Linux filters sending.
            // Do not silently accept that different policy.
            if (!scalar) {
                errno = 95;
                return -1;
            }
            scalar = 1;
        }
        return setsockopt(socket->handle, level, option, reinterpret_cast<const char*>(&scalar), sizeof(scalar)) ==
                       SOCKET_ERROR
                   ? SocketError()
                   : 0;
    }
    return setsockopt(socket->handle, level, option, value, length) == SOCKET_ERROR ? SocketError() : 0;
}

int SocketIoctl(int fd, unsigned long request, int* value)
{
    int memoryResult;
    if (request == 0x541b && AndroidInotifyAvailable(fd, value, memoryResult))
        return memoryResult;
    if (AndroidMemoryFileIoctl(fd, request, reinterpret_cast<uintptr_t>(value), memoryResult))
        return memoryResult;
    if (request >= 0x8900 && request <= 0x89ff) {
        if (!FindSocket(fd)) {
            errno = EBADF;
            return -1;
        }
        if (AndroidInterfaceIoctl(request, value, memoryResult))
            return memoryResult;
    }
    if (request != 0x541b) {
        errno = 38;
        return -1;
    } // Linux FIONREAD
    if (!value) {
        errno = EFAULT;
        return -1;
    }
    const auto socket = FindSocket(fd);
    if (!socket)
        return AndroidFileAvailable(fd, value);
    if (socket->pair) {
        const std::lock_guard lock(socket->pair->mutex);
        const auto& local = socket->pair->ends[socket->side];
        const size_t length = socket->pair->type == SOCK_STREAM ? local.bytes
                              : local.records.empty()           ? 0
                                                                : local.records.front().data.size();
        *value = static_cast<int>(length);
        return 0;
    }
    u_long available = 0;
    if (ioctlsocket(socket->handle, FIONREAD, &available) == SOCKET_ERROR)
        return SocketError();
    *value = static_cast<int>(std::min<u_long>(available, INT_MAX));
    return 0;
}
}

uint64_t AndroidSocketId(int fd)
{
    const std::lock_guard lock(socketMutex);
    const auto entry = sockets.find(fd);
    return entry == sockets.end() ? 0 : entry->second.id;
}

namespace {
uintptr_t OpenSocketRelay(int fd, GuestCredentials peer, PipeHandle target, unsigned& flags)
{
    const auto socket = FindSocket(fd);
    if (!socket || !socket->pair) {
        errno = 91;
        return 0;
    }
    PipeHandle server(nullptr, &CloseHandle), client(nullptr, &CloseHandle);
    try {
        if (!CreateRelayPipe(server, client, !target)) {
            std::fprintf(stderr,
                         "[android-native] Could not create pipe for Unix socket transfer (Windows error %lu)\n",
                         GetLastError());
            errno = EIO;
            return 0;
        }
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return 0;
    }
    int copy = -1;
    if (!DuplicateAndroidSocket(fd, 0, true, copy) || copy < 0)
        return 0;
    if (!StartUnixRelay(std::move(server), SocketReference(copy), peer, std::move(target), true))
        return 0;
    flags = (socket->nonblocking ? 1u : 0u) | (socket->type == Seqpacket ? 2u : 0u);
    return reinterpret_cast<uintptr_t>(client.release());
}

int AdoptSocketRelay(PipeHandle pipe, unsigned flags, GuestCredentials peer, PipeHandle target)
{
    DWORD kind = 0;
    ULONG owner = 0;
    if (flags > 3 || !GetNamedPipeInfo(pipe.get(), &kind, nullptr, nullptr, nullptr) ||
        (kind & (PIPE_SERVER_END | PIPE_TYPE_MESSAGE)) || !GetNamedPipeServerProcessId(pipe.get(), &owner) ||
        owner != static_cast<ULONG>(peer.pid))
        return -1;
    int pair[2];
    const auto type = (flags & 2) ? Seqpacket : SOCK_STREAM;
    if (CreateSocketPair(UnixFamily, type | CloseOnExec | ((flags & 1) ? Nonblocking : 0), 0, pair) != 0)
        return -1;
    SocketReference guest(pair[0]);
    if (!StartUnixRelay(std::move(pipe), SocketReference(pair[1]), peer, std::move(target), false))
        return -1;
    return guest.release();
}
}

uintptr_t ExportAndroidUnixSocket(int fd, uintptr_t target, unsigned peerUid, unsigned& flags)
{
    const auto targetHandle = reinterpret_cast<HANDLE>(target);
    const auto pid = static_cast<int32_t>(GetProcessId(targetHandle));
    auto copy = ForkTargetHandle(targetHandle);
    if (!pid || !copy) {
        errno = EACCES;
        return 0;
    }
    return OpenSocketRelay(fd, {pid, peerUid, peerUid}, std::move(copy), flags);
}

int ImportAndroidUnixSocket(uintptr_t value, unsigned flags, int peerPid, unsigned peerUid)
{
    return AdoptSocketRelay({reinterpret_cast<HANDLE>(value), &CloseHandle}, flags, {peerPid, peerUid, peerUid},
                            {nullptr, &CloseHandle});
}

bool CloseAndroidSocket(int fd, int& result)
{
    std::shared_ptr<Socket> last;
    {
        const std::lock_guard lock(socketMutex);
        auto found = sockets.extract(fd);
        if (!found)
            return false;
        ForgetAndroidFileDescriptor(fd);
        result = _close(fd);
        if (!--found.mapped().socket->descriptors)
            last = std::move(found.mapped().socket);
    }
    if (last && last->pair)
        ShutdownLocalSocket(*last, SD_BOTH);
    return true;
}

bool DuplicateAndroidSocket(int fd, int minimum, bool closeOnExec, int& result, bool allowSocket)
{
    const std::lock_guard lock(socketMutex);
    const auto source = sockets.find(fd);
    if (source == sockets.end())
        return false;
    if (!allowSocket) {
        errno = 95;
        result = -1;
        return true;
    } // EOPNOTSUPP
    const auto socket = source->second.socket;
    result = DuplicateCrtFd(fd, minimum, false);
    if (result < 0)
        return true;
    try {
        sockets.emplace(result, SocketDescriptor{socket, nextSocketId++, closeOnExec});
        ++socket->descriptors;
    }
    catch (const std::bad_alloc&) {
        _close(result);
        errno = ENOMEM;
        result = -1;
    }
    return true;
}

bool ReplaceAndroidSocket(int source, int target, bool closeOnExec, int& result)
{
    std::shared_ptr<Socket> last;
    {
        const std::lock_guard lock(socketMutex);
        const auto from = sockets.find(source), to = sockets.find(target);
        if (from == sockets.end() || to == sockets.end())
            return false;
        const auto previous = _set_thread_local_invalid_parameter_handler(
            +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
            });
        const int copied = _dup2(source, target);
        _set_thread_local_invalid_parameter_handler(previous);
        if (copied < 0) {
            result = -1;
            return true;
        }
        // Publish the new identity only after the CRT reservation succeeds.
        // Increment first: source and target may already alias the same socket.
        ++from->second.socket->descriptors;
        if (!--to->second.socket->descriptors)
            last = to->second.socket;
        // The destination Java/FileDescriptor owner is unchanged by dup2.
        // Keep its fdsan tag; its later tagged close must still match.
        to->second = SocketDescriptor{from->second.socket, nextSocketId++, closeOnExec};
        result = target;
    }
    if (last && last->pair)
        ShutdownLocalSocket(*last, SD_BOTH);
    return true;
}

bool ReadAndroidSocket(int fd, void* buffer, size_t size, intptr_t& result)
{
    const auto socket = FindSocket(fd);
    if (!socket)
        return false;
    result = size ? ReceiveSocket(*socket, buffer, size, 0) : 0;
    return true;
}

bool WriteAndroidSocket(int fd, const void* buffer, size_t size, intptr_t& result)
{
    const auto socket = FindSocket(fd);
    if (!socket)
        return false;
    result = SendSocket(*socket, buffer, size, 0);
    return true;
}

int SendAndroidHardwareBuffer(const AndroidHardwareBuffer* buffer, int fd)
{
    if (!buffer)
        return -EINVAL;
    const auto socket = FindSocket(fd);
    if (!socket)
        return -88; // ENOTSOCK
    if (!socket->pair)
        return -95; // EOPNOTSUPP
    AcquireAndroidHardwareBuffer(buffer);
    char marker = 0;
    const GuestIoVector vector{&marker, 1};
    const auto count =
        SendLocalSocket(*socket, std::span{&vector, 1}, 0, BufferReference(buffer, &ReleaseAndroidHardwareBuffer));
    return count == 1 ? 0 : -errno;
}

int ReceiveAndroidHardwareBuffer(int fd, AndroidHardwareBuffer** buffer)
{
    if (!buffer)
        return -EINVAL;
    *buffer = nullptr;
    const auto socket = FindSocket(fd);
    if (!socket)
        return -88; // ENOTSOCK
    if (!socket->pair)
        return -95; // EOPNOTSUPP
    char marker;
    const auto count = ReceiveSocket(*socket, &marker, 1, 0, buffer);
    if (count <= 0)
        return count < 0 ? -errno : -EPIPE;
    return *buffer ? 0 : -EINVAL;
}

bool AndroidSocketFlags(int fd, int command, int argument, int& result)
{
    if (command == 1 || command == 2) { // F_GETFD, F_SETFD
        const std::lock_guard lock(socketMutex);
        const auto entry = sockets.find(fd);
        if (entry == sockets.end())
            return false;
        if (command == 2)
            entry->second.closeOnExec = (argument & 1) != 0;
        result = command == 1 && entry->second.closeOnExec ? 1 : 0;
        return true;
    }
    const auto socket = FindSocket(fd);
    if (!socket)
        return false;
    switch (command) {
    case 3:
        result = 2 | (socket->nonblocking ? Nonblocking : 0);
        break; // F_GETFL
    case 4:
        result = SetNonblocking(*socket, (argument & Nonblocking) != 0) ? 0 : -1;
        break;
    default:
        errno = EINVAL;
        result = -1;
    }
    return true;
}

uint32_t PollAndroidSocket(int fd, uint64_t id, uint32_t events, uint32_t& previous, uint64_t& readGeneration,
                           uint64_t& writeGeneration)
{
    const auto socket = FindSocket(fd, id);
    if (!socket)
        return 0;
    if (socket->pair) {
        const auto& pair = *socket->pair;
        const std::lock_guard lock(socket->pair->mutex);
        const auto& local = pair.ends[socket->side];
        const auto& peer = pair.ends[1 - socket->side];
        const bool eof = !local.receiving || !peer.sending;
        uint32_t ready = 0;
        if (eof || !local.records.empty())
            ready |= Input;
        if (eof)
            ready |= ReadHangup;
        if ((!local.sending && !local.receiving) || (!peer.sending && !peer.receiving))
            ready |= Hangup;
        if (local.sending && peer.receiving && peer.hasRoom(1))
            ready |= Output;
        ready &= events | Hangup;
        if (!(events & Edge))
            return ready;
        if (local.inputGeneration != readGeneration)
            previous &= ~(Input | ReadHangup);
        if (local.outputGeneration != writeGeneration)
            previous &= ~Output;
        readGeneration = local.inputGeneration;
        writeGeneration = local.outputGeneration;
        const uint32_t changed = ready & ~previous;
        previous = ready;
        return changed;
    }
    fd_set read{}, write{}, error{};
    FD_SET(socket->handle, &read);
    if (events & Output)
        FD_SET(socket->handle, &write);
    FD_SET(socket->handle, &error);
    timeval timeout{};
    const auto reads = socket->reads.load(), writes = socket->writes.load();
    const int count = select(0, &read, &write, &error, &timeout);
    uint32_t ready = 0;
    if (count > 0) {
        if (FD_ISSET(socket->handle, &read)) {
            u_long available = 0;
            if (socket->listening)
                ready |= Input;
            else if (ioctlsocket(socket->handle, FIONREAD, &available) == SOCKET_ERROR)
                ready |= Error;
            else if (!available)
                ready |= Input | ReadHangup;
            else
                ready |= Input;
        }
        if (FD_ISSET(socket->handle, &write))
            ready |= Output;
        if (FD_ISSET(socket->handle, &error))
            ready |= Input | Error | Hangup | Priority;
    }
    ready &= events | Error | Hangup;
    if (ready & Error)
        ready |= Input;
    if (!(events & Edge))
        return ready;
    // A read can drain the socket and more bytes can arrive between select calls.
    if (reads != readGeneration)
        previous &= ~Input;
    if (writes != writeGeneration)
        previous &= ~Output;
    readGeneration = reads;
    writeGeneration = writes;
    const uint32_t changed = ready & ~previous;
    previous = ready;
    return changed;
}

int AndroidGetAddrInfo(const char* name, const char* service, const AndroidAddrInfo* hints, AndroidAddrInfo** output)
{
    if (!output) {
        errno = EFAULT;
        return 11;
    } // EAI_SYSTEM
    *output = nullptr;
    if (!name && !service)
        return 8; // EAI_NONAME
    ADDRINFOA options{};
    if (hints) {
        if (hints->length || hints->canonicalName || hints->address || hints->next)
            return 12; // EAI_BADHINTS
        if (hints->flags & ~0xd0f)
            return 3;
        if (hints->family != 0 && hints->family != 2 && hints->family != 10)
            return 5;
        if (hints->flags & 1)
            options.ai_flags |= AI_PASSIVE;
        if (hints->flags & 2)
            options.ai_flags |= AI_CANONNAME;
        if (hints->flags & 4)
            options.ai_flags |= AI_NUMERICHOST;
        if (hints->flags & 8)
            options.ai_flags |= AI_NUMERICSERV;
        if (hints->flags & 0x100)
            options.ai_flags |= AI_ALL;
        if (hints->flags & 0x400)
            options.ai_flags |= AI_ADDRCONFIG;
        if (hints->flags & 0x800)
            options.ai_flags |= AI_V4MAPPED;
        options.ai_family = hints->family == 10 ? AF_INET6 : hints->family;
        options.ai_socktype = hints->type;
        options.ai_protocol = hints->protocol;
    }
    if (!InitializeAndroidWinsock()) {
        errno = EIO;
        return 11;
    }
    ADDRINFOA* results = nullptr;
    if (const auto error = getaddrinfo(name, service, &options, &results))
        return ResolveError(error);
    const std::unique_ptr<ADDRINFOA, decltype(&freeaddrinfo)> host(results, &freeaddrinfo);
    std::vector<std::unique_ptr<AddressInfoNode, decltype(&FreeAddressInfoNode)>> nodes;
    try {
        for (auto* entry = results; entry; entry = entry->ai_next) {
            if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6)
                continue;
            if (!entry->ai_addr || entry->ai_addrlen > sizeof(sockaddr_storage))
                continue;
            const size_t chars = entry->ai_canonname ? std::strlen(entry->ai_canonname) + 1 : 0;
            const size_t size = sizeof(AddressInfoNode) + chars;
            auto* storage = IsArm64TranslationEnabled() ? reinterpret_cast<void*>(AllocateArm64GuestMemory(size, false))
                                                        : std::malloc(size);
            if (!storage)
                return 6;
            std::unique_ptr<AddressInfoNode, decltype(&FreeAddressInfoNode)> node(new (storage) AddressInfoNode{},
                                                                                  &FreeAddressInfoNode);
            const auto family = entry->ai_family == AF_INET6 ? 10 : 2;
            std::memcpy(&node->address, entry->ai_addr, entry->ai_addrlen);
            node->address.ss_family = static_cast<ADDRESS_FAMILY>(family);
            auto* canonical = chars ? reinterpret_cast<char*>(node.get() + 1) : nullptr;
            if (canonical)
                std::memcpy(canonical, entry->ai_canonname, chars);
            node->info = {hints ? hints->flags : 0,
                          family,
                          entry->ai_socktype,
                          entry->ai_protocol,
                          static_cast<uint32_t>(entry->ai_addrlen),
                          canonical,
                          &node->address,
                          nullptr};
            if (!nodes.empty())
                nodes.back()->info.next = &node->info;
            nodes.push_back(std::move(node));
        }
        if (nodes.empty())
            return 7;
        auto& registry = ResolvedAddresses();
        const std::lock_guard lock(registry.mutex);
        try {
            for (const auto& node : nodes)
                registry.nodes.insert(&node->info);
        }
        catch (...) {
            for (const auto& node : nodes)
                registry.nodes.erase(&node->info);
            throw;
        }
        *output = &nodes.front()->info;
        for (auto& node : nodes)
            node.release();
        return 0;
    }
    catch (const std::bad_alloc&) {
        return 6;
    }
}

void AndroidFreeAddrInfo(AndroidAddrInfo* address)
{
    auto& registry = ResolvedAddresses();
    const std::lock_guard lock(registry.mutex);
    while (address && registry.nodes.erase(address)) {
        auto* next = address->next;
        FreeAddressInfoNode(address);
        address = next;
    }
}

int AndroidGetNameInfo(const sockaddr* address, uint32_t length, char* host, size_t hostLength, char* service,
                       size_t serviceLength, int flags)
{
    if (flags & ~0x3f)
        return 3; // EAI_BADFLAGS
    if (hostLength > UINT32_MAX || serviceLength > UINT32_MAX)
        return 14; // EAI_OVERFLOW
    sockaddr_storage converted{};
    const int size = ReadInetAddress(address, length, converted);
    if (size < 0)
        return 5; // EAI_FAMILY
    if (!InitializeAndroidWinsock()) {
        errno = EIO;
        return 11;
    } // EAI_SYSTEM
    int options = 0;
    if (flags & 1)
        options |= NI_NOFQDN;
    if (flags & 2)
        options |= NI_NUMERICHOST;
    if (flags & 4)
        options |= NI_NAMEREQD;
    if (flags & 8)
        options |= NI_NUMERICSERV;
    if (flags & 16)
        options |= NI_DGRAM;
    // Winsock includes the scope in numeric IPv6 addresses when present.
    const int result = getnameinfo(reinterpret_cast<const sockaddr*>(&converted), size, host,
                                   static_cast<DWORD>(hostLength), service, static_cast<DWORD>(serviceLength), options);
    if (result == WSAEFAULT)
        return 14; // EAI_OVERFLOW
    return result ? ResolveError(result) : 0;
}

uintptr_t AndroidSocketImport(std::string_view name, bool hostAbi)
{
#define BIND(symbol, target)                                                                                           \
    if (name == #symbol) {                                                                                             \
        const auto address = reinterpret_cast<uintptr_t>(target);                                                      \
        return hostAbi ? address : BridgeAndroidImport(name, address);                                                 \
    }
    BIND(socket, &CreateSocket);
    BIND(socketpair, &CreateSocketPair);
    BIND(send, &TransferSocket<true>);
    BIND(recv, &TransferSocket<false>);
    BIND(sendto, &SendTo);
    if (name == "__sendto_chk") {
        const auto function = +[](int fd, const void* buffer, size_t size, size_t bound, int flags,
                                  const sockaddr* address, uint32_t length) -> intptr_t {
            if (size > bound)
                std::abort();
            return SendTo(fd, buffer, size, flags, address, length);
        };
        const auto address = reinterpret_cast<uintptr_t>(function);
        return hostAbi ? address : ImportAndroidScalars<decltype(function)>(name, address);
    }
    BIND(recvfrom, &ReceiveFrom);
    BIND(sendmsg, &TransferMessage<true>);
    BIND(recvmsg, &TransferMessage<false>);
    BIND(__cmsg_nxthdr, &NextControl);
    BIND(connect, &UseAddress<true>);
    BIND(bind, &UseAddress<false>);
    BIND(listen, &ListenSocket);
    BIND(
        accept, +[](int fd, sockaddr* address, int* length) {
            return AcceptSocket(fd, address, length, 0);
        });
    BIND(accept4, &AcceptSocket);
    BIND(shutdown, &ShutdownSocket);
    BIND(getsockname, &SocketAddress<false>);
    BIND(getpeername, &SocketAddress<true>);
    BIND(getaddrinfo, &AndroidGetAddrInfo);
    if (name == "getnameinfo") {
        const auto address = reinterpret_cast<uintptr_t>(&AndroidGetNameInfo);
        return hostAbi ? address : ImportAndroidScalars<decltype(&AndroidGetNameInfo)>(name, address);
    }
    BIND(
        android_getaddrinfofornet, +[](const char* name, const char* service, const AndroidAddrInfo* hints,
                                       unsigned network, unsigned mark, AndroidAddrInfo** output) {
            // Windows selects its default network; Android-specific routing cannot be requested.
            if (network || (mark && mark != UINT_MAX)) {
                if (output)
                    *output = nullptr;
                errno = 95;
                return 11; // EAI_SYSTEM.
            }
            return AndroidGetAddrInfo(name, service, hints, output);
        });
    BIND(freeaddrinfo, &AndroidFreeAddrInfo);
    BIND(getsockopt, &GetSocketOption);
    BIND(setsockopt, &SetSocketOption);
    BIND(ioctl, &SocketIoctl);
#undef BIND
    return 0;
}
}
