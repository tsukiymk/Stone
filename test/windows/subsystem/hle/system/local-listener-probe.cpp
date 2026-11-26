#include <winsock2.h>
#include "../../support/test-suite.h"
#include "hle/system/stub-libc-sockets.h"
#include "hle/system/stub-libc.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>

namespace stone::windows::test {
int ProbeLocalListener(const wchar_t* peerName)
{
    const auto create = reinterpret_cast<int (*)(int, int, int)>(AndroidSocketImport("socket", true));
    const auto bind = reinterpret_cast<int (*)(int, const sockaddr*, int)>(AndroidSocketImport("bind", true));
    const auto listen = reinterpret_cast<int (*)(int, int)>(AndroidSocketImport("listen", true));
    const auto connect = reinterpret_cast<int (*)(int, const sockaddr*, int)>(AndroidSocketImport("connect", true));
    const auto accept = reinterpret_cast<int (*)(int, sockaddr*, int*, int)>(AndroidSocketImport("accept4", true));
    const auto nameOf = reinterpret_cast<int (*)(int, sockaddr*, int*)>(AndroidSocketImport("getsockname", true));
    const auto option = reinterpret_cast<int (*)(int, int, int, char*, int*)>(AndroidSocketImport("getsockopt", true));
    STONE_CHECK(create && bind && listen && connect && accept && nameOf && option);

    struct Fd {
        int fd = -1;

        ~Fd()
        {
            if (fd >= 0)
                CloseAndroidFd(fd);
        }
    } first, second, client, accepted, distinct, remote, alias;

    const auto unavailable = [](const char* operation, int error) {
        std::cout << "{\"available\":false,\"operation\":\"" << operation << "\",\"androidErrno\":" << error << "}\n";
        return 2;
    };
    first.fd = create(1, 1 | 0x80000, 0); // AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC.
    if (first.fd < 0)
        return unavailable("socket", errno);

    struct Address {
        uint16_t family = 1;
        std::array<char, 108> path{};
    } address;

    const std::wstring peer = peerName ? peerName : L"";
    if (peerName)
        STONE_CHECK(peer.starts_with(L"stone-listener-probe-") &&
                    peer.find_first_not_of(L"stone-listener-prob0123456789-") == peer.npos);
    std::string peerBytes;
    for (const auto value : peer)
        peerBytes += static_cast<char>(value);
    const auto name = peerName ? peerBytes
                               : "stone-listener-probe-" + std::to_string(GetCurrentProcessId()) + '-' +
                                     std::to_string(GetTickCount64());
    STONE_CHECK(name.size() < address.path.size() - 1);
    std::memcpy(address.path.data() + 1, name.data(), name.size()); // Android abstract namespace.
    const auto length = static_cast<int>(sizeof(address.family) + 1 + name.size());
    const auto* raw = reinterpret_cast<const sockaddr*>(&address);
    if (peerName) {
        STONE_CHECK(bind(first.fd, raw, length) == -1 && errno == 98);
        STONE_CHECK(connect(first.fd, raw, length) == 0);
        const char payload = 'P';
        intptr_t transferred = 0;
        STONE_CHECK(WriteAndroidSocket(first.fd, &payload, 1, transferred) && transferred == 1);
        return 0;
    }
    STONE_CHECK(bind(first.fd, raw, 1) == -1 && errno == EINVAL);
    Address pathname = address;
    pathname.path[0] = 'x';
    STONE_CHECK(bind(first.fd, reinterpret_cast<const sockaddr*>(&pathname), length) == -1 && errno == 95);
    if (bind(first.fd, raw, length) < 0)
        return unavailable("bind", errno);
    if (listen(first.fd, 1) < 0)
        return unavailable("listen", errno);
    second.fd = create(1, 1 | 0x80000, 0);
    if (second.fd < 0)
        return unavailable("second-socket", errno);
    errno = 0;
    STONE_CHECK(bind(second.fd, raw, length) == -1 && errno == 98); // EADDRINUSE: no fake successful bind.
    std::array<wchar_t, 32768> executable{};
    const auto executableLength = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    STONE_CHECK(executableLength && executableLength < executable.size());
    auto command =
        L"\"" + std::wstring(executable.data()) + L"\" local-listener-probe " + std::wstring(name.begin(), name.end());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    STONE_CHECK(CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                               nullptr, &startup, &process));
    const std::unique_ptr<void, decltype(&CloseHandle)> child(process.hProcess, &CloseHandle),
        thread(process.hThread, &CloseHandle);
    const auto wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    STONE_CHECK(wait == WAIT_OBJECT_0);
    DWORD exit = 1;
    STONE_CHECK(GetExitCodeProcess(process.hProcess, &exit) && exit == 0);
    remote.fd = accept(first.fd, nullptr, nullptr, 0x800 | 0x80000);
    char remoteByte = 0;
    intptr_t remoteCount = 0;
    STONE_CHECK(remote.fd >= 0 && ReadAndroidSocket(remote.fd, &remoteByte, 1, remoteCount) && remoteCount == 1 &&
                remoteByte == 'P');
    STONE_CHECK(ReadAndroidSocket(remote.fd, &remoteByte, 1, remoteCount) && remoteCount == 0);

    Address actual{};
    int actualLength = sizeof(actual);
    STONE_CHECK(nameOf(first.fd, reinterpret_cast<sockaddr*>(&actual), &actualLength) == 0);
    STONE_CHECK(actualLength == length && std::memcmp(&actual, &address, length) == 0);
    int domain = 0, optionLength = sizeof(domain);
    STONE_CHECK(option(first.fd, 1, 39, reinterpret_cast<char*>(&domain), &optionLength) == 0 && domain == 1);
    // Abstract names include embedded/trailing NUL bytes in their identity.
    distinct.fd = create(1, 1 | 0x80000, 0);
    STONE_CHECK(distinct.fd >= 0 && bind(distinct.fd, raw, length + 1) == 0 && listen(distinct.fd, 1) == 0);
    STONE_CHECK(connect(distinct.fd, raw, length) == -1 && errno == 95);
    client.fd = create(1, 1 | 0x80000, 0);
    STONE_CHECK(client.fd >= 0 && connect(client.fd, raw, length) == 0);
    uint32_t previous = 0;
    uint64_t reads = 0, writes = 0;
    const auto ready = PollAndroidSocket(first.fd, AndroidSocketId(first.fd), 1, previous, reads, writes);
    STONE_CHECK((ready & 1) && !(ready & 0x2000)); // A pending accept is not EOF.
    accepted.fd = accept(first.fd, nullptr, nullptr, 0x800 | 0x80000);
    STONE_CHECK(accepted.fd >= 0);
    int descriptorFlags = 0;
    STONE_CHECK(AndroidSocketFlags(accepted.fd, 1, 0, descriptorFlags) && descriptorFlags == 1);
    intptr_t transferred = 0;
    char received = 0;
    STONE_CHECK(ReadAndroidSocket(accepted.fd, &received, 1, transferred) && transferred == -1 && errno == EAGAIN);
    const char payload = 'W';
    STONE_CHECK(WriteAndroidSocket(client.fd, &payload, 1, transferred) && transferred == 1);
    STONE_CHECK(ReadAndroidSocket(accepted.fd, &received, 1, transferred) && transferred == 1 && received == payload);
    STONE_CHECK(WriteAndroidSocket(accepted.fd, &received, 1, transferred) && transferred == 1);
    STONE_CHECK(ReadAndroidSocket(client.fd, &received, 1, transferred) && transferred == 1 && received == payload);
    int duplicated = -1;
    STONE_CHECK(DuplicateAndroidSocket(first.fd, 0, true, duplicated) && duplicated >= 0);
    alias.fd = duplicated;
    STONE_CHECK(CloseAndroidFd(first.fd) == 0);
    first.fd = -1;
    STONE_CHECK(bind(second.fd, raw, length) == -1 && errno == 98);
    STONE_CHECK(CloseAndroidFd(alias.fd) == 0);
    alias.fd = -1;
    STONE_CHECK(bind(second.fd, raw, length) == 0 && listen(second.fd, 1) == 0);
    STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
    std::cout
        << "{\"available\":true,\"operation\":\"exclusive-abstract-listener\",\"streamRoundTrip\":true,\"crossProcessExclusive\":true,\"crossProcessTransfer\":true,\"androidErrno\":0}\n";
    return 0;
}
}
