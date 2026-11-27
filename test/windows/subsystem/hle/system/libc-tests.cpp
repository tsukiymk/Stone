#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "../../support/test-suite.h"

#include "abi/AndroidLibraryImports.h"
#include "hle/system/stub-libc.h"
#include "hle/system/stub-libc-sockets.h"
#include "hle/system/stub-libc-interfaces.h"
#include "../../../../../src/windows/client/BrokerClient.h"
#include "signals/AndroidSignalContext.h"
#include "filesystem/AndroidPrivateLibraries.h"
#include "filesystem/AndroidSystemFiles.h"
#include "memory/GuestHeap.h"
#include "cpu/arm64/Arm64Initializer.h"

#include <array>
#include <algorithm>
#include <fstream>
#include <fcntl.h>
#include <io.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <chrono>
#include <barrier>
#include <climits>
#include <thread>
#include <malloc.h>
#include <atomic>
#include <iostream>
#include <vector>

namespace stone::windows::test {
namespace {
struct ArtHeapStatistics {
    uint64_t fields[10];
};

void CheckPrivateNativeLibraries()
{
    namespace fs = std::filesystem;
    const auto parent = fs::current_path();
    const auto root = parent / ("stone-private-libraries-" + std::to_string(GetCurrentProcessId()));
    STONE_CHECK(root.parent_path() == parent && fs::create_directory(root));
    const auto cleanup = [&](void*) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    };
    const std::unique_ptr<void, decltype(cleanup)> guard(reinterpret_cast<void*>(1), cleanup);
    const auto own = root / "own";
    fs::create_directories(own / fs::path(u8"\u6838\u5fc3"));
    std::vector<uint8_t> expected(128, 0x3c);
    const auto file = own / fs::path(u8"\u6838\u5fc3/core.so");
    {
        std::ofstream output(file, std::ios::binary);
        output.write(reinterpret_cast<const char*>(expected.data()), expected.size());
    }
    const std::string prefix = "/data/data/com.example/";
    const std::string name = prefix + "\xe6\xa0\xb8\xe5\xbf\x83/core.so";
    STONE_CHECK(ReadAndroidPrivateLibrary(own, prefix, name) == expected);
    const auto reject = [&](const std::string& path) {
        bool denied = false;
        try {
            ReadAndroidPrivateLibrary(own, prefix, path);
        }
        catch (const std::exception&) {
            denied = true;
        }
        STONE_CHECK(denied);
    };
    for (const auto& path :
         {"/data/data/com.other/core.so", "/data/data/com.example/../outside.so", "/data/data/com.example//core.so",
          "/data/data/com.example/core.so:extra", "/data/data/com.example/none.so", "/data/data/com.example/core.dll"})
        reject(path);
    HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                0, nullptr);
    STONE_CHECK(writer != INVALID_HANDLE_VALUE);
    reject(name);
    CloseHandle(writer);
    STONE_CHECK(ReadAndroidPrivateLibrary(own, prefix, name) == expected);
    const auto outside = root / "outside.so";
    fs::copy_file(file, outside);
    const auto link = own / "alias.so";
    if (CreateSymbolicLinkW(link.c_str(), outside.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        reject(prefix + "alias.so");
    else
        std::cout << "SKIP private native library symlink check: Windows symlink privilege unavailable\n";
    const auto outsideDirectory = root / "outside-directory";
    fs::create_directory(outsideDirectory);
    fs::copy_file(file, outsideDirectory / "core.so");
    const auto directoryLink = own / "foreign";
    if (CreateSymbolicLinkW(directoryLink.c_str(), outsideDirectory.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        reject(prefix + "foreign/core.so");
    else
        std::cout << "SKIP private native library directory-link check: Windows symlink privilege unavailable\n";
    {
        std::ofstream small(own / "small.so", std::ios::binary);
        small << "short";
    }
    reject(prefix + "small.so");
}

void CheckMultibyteStrings()
{
    const auto wide = Guest<size_t (*)(uint32_t*, const char*, size_t)>(Libc("mbstowcs"), "(LLJ)J");
    const auto bytes = Guest<size_t (*)(char*, const uint32_t*, size_t)>(Libc("wcstombs"), "(LLJ)J");
    const char source[] = "A\xe4\xb8\xad\xf0\x9f\x98\x80";
    const uint32_t expected[]{'A', 0x4e2d, 0x1f600, 0};
    uint32_t out[5]{0xdead, 0xdead, 0xdead, 0xdead, 0xdead};
    char back[12];
    std::memset(back, 0x5a, sizeof(back));
    STONE_CHECK(wide(nullptr, source, 0) == 3 && bytes(nullptr, expected, 0) == 8);
    STONE_CHECK(wide(out, source, 4) == 3 && std::equal(std::begin(expected), std::end(expected), out) &&
                out[4] == 0xdead);
    STONE_CHECK(bytes(back, out, 9) == 8 && std::strcmp(back, source) == 0 && back[9] == 0x5a);
    out[2] = 0xdead;
    STONE_CHECK(wide(out, source, 2) == 2 && out[2] == 0xdead);
    std::memset(back, 0x5a, sizeof(back));
    STONE_CHECK(bytes(back, expected, 3) == 1 && back[0] == 'A' && back[1] == 0x5a);
    STONE_CHECK(bytes(back, expected, 4) == 4 && back[4] == 0x5a);
    out[0] = 0xdead;
    STONE_CHECK(wide(out, "", 1) == 0 && out[0] == 0);
    STONE_CHECK(wide(out, source, 0) == 0 && bytes(back, expected, 0) == 0);
    for (const char* invalid : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8", "\x80"}) {
        errno = 0;
        STONE_CHECK(wide(nullptr, invalid, 0) == SIZE_MAX && errno == 84);
    }
    for (uint32_t invalid : {0xd800u, 0xdfffu, 0x110000u}) {
        uint32_t text[]{invalid, 0};
        errno = 0;
        STONE_CHECK(bytes(nullptr, text, 0) == SIZE_MAX && errno == 84);
    }
}

void CheckArtAllocationAccounting()
{
    const auto statistics = Guest<ArtHeapStatistics* (*)(ArtHeapStatistics*)>(AndroidArtHeapImport("mallinfo"), "(L)L");
    const auto allocate = Guest<void* (*)(size_t)>(Libc("malloc"), "(J)L");
    const auto zeroed = Guest<void* (*)(size_t, size_t)>(Libc("calloc"), "(JJ)L");
    const auto resize = Guest<void* (*)(void*, size_t)>(Libc("realloc"), "(LJ)L");
    const auto release = Guest<void (*)(void*)>(Libc("free"), "(L)V");
    const auto usable = Guest<size_t (*)(void*)>(Libc("malloc_usable_size"), "(L)J");
    const auto aligned = Guest<int (*)(void**, size_t, size_t)>(Libc("posix_memalign"), "(LJJ)I");
    const auto bytes = [&] {
        struct {
            uint64_t before = 0x1234;
            ArtHeapStatistics stats{};
            uint64_t after = 0x5678;
        } buffer;
        STONE_CHECK(statistics(&buffer.stats) == &buffer.stats && buffer.before == 0x1234 && buffer.after == 0x5678);
        return buffer.stats.fields[7] + buffer.stats.fields[4];
    };
    const auto baseline = bytes();
    STONE_CHECK(!AndroidArtHeapImport("malloc"));
    STONE_CHECK(AndroidArtHeapImport("mallinfo2") == AndroidArtHeapImport("mallinfo"));
    void* foreign = std::malloc(4096);
    STONE_CHECK(foreign && bytes() == baseline);
    const auto fullStatistics = Guest<ArtHeapStatistics* (*)(ArtHeapStatistics*)>(Libc("mallinfo"), "(L)L");
    ArtHeapStatistics full{};
    STONE_CHECK(fullStatistics(&full) == &full && full.fields[7] + full.fields[4] >= 4096);
    release(foreign);
    release(nullptr);
    STONE_CHECK(bytes() == baseline);
    auto* first = static_cast<unsigned char*>(allocate(67));
    STONE_CHECK(first && usable(first) >= 67 && bytes() == baseline + usable(first));
    std::memset(first, 0x5a, 67);
    first = static_cast<unsigned char*>(resize(first, 4096));
    STONE_CHECK(first && bytes() == baseline + usable(first));
    for (size_t i = 0; i < 67; ++i)
        STONE_CHECK(first[i] == 0x5a);
    STONE_CHECK(!resize(first, SIZE_MAX) && bytes() == baseline + usable(first));
    first = static_cast<unsigned char*>(resize(first, 31));
    STONE_CHECK(first && bytes() == baseline + usable(first));
    for (size_t i = 0; i < 31; ++i)
        STONE_CHECK(first[i] == 0x5a);
    auto* second = static_cast<unsigned char*>(zeroed(17, 13));
    STONE_CHECK(second && bytes() == baseline + usable(first) + usable(second));
    for (size_t i = 0; i < 17 * 13; ++i)
        STONE_CHECK(second[i] == 0);
    STONE_CHECK(!zeroed(SIZE_MAX, 2) && bytes() == baseline + usable(first) + usable(second));
    release(second);
    STONE_CHECK(!resize(first, 0) && bytes() == baseline);
    void* third = nullptr;
    STONE_CHECK(aligned(&third, 64, 128) == 0 && reinterpret_cast<uintptr_t>(third) % 64 == 0);
    STONE_CHECK(bytes() == baseline + usable(third));
    std::memset(third, 0x3c, 128);
    third = resize(third, 333);
    STONE_CHECK(third && reinterpret_cast<uintptr_t>(third) % 64 == 0 && bytes() == baseline + usable(third));
    for (size_t i = 0; i < 128; ++i)
        STONE_CHECK(static_cast<unsigned char*>(third)[i] == 0x3c);
    STONE_CHECK(!resize(third, SIZE_MAX) && bytes() == baseline + usable(third));
    release(third);
    foreign = std::malloc(33);
    STONE_CHECK(foreign);
    STONE_CHECK(!resize(foreign, SIZE_MAX) && bytes() == baseline);
    void* adopted = resize(foreign, 96);
    STONE_CHECK(adopted && bytes() == baseline + usable(adopted));
    release(adopted);
    STONE_CHECK(bytes() == baseline);
    std::array<std::thread, 4> threads;
    for (auto& worker : threads)
        worker = std::thread([&] {
            for (size_t i = 1; i <= 256; ++i) {
                void* block = allocate(i);
                block = resize(block, i + 512);
                release(block);
            }
        });
    for (auto& worker : threads)
        worker.join();
    STONE_CHECK(bytes() == baseline);
    // Grow the ownership registry, leave holes in its address clusters, and
    // resize allocations from threads other than their allocating thread.
    std::vector<void*> blocks(8192);
    const auto initialSize = [](size_t i) {
        return 1 + (i * 37) % 1024;
    };
    const auto fill = [&](size_t i) {
        blocks[i] = allocate(initialSize(i));
        STONE_CHECK(blocks[i]);
        std::memset(blocks[i], static_cast<unsigned char>(i), initialSize(i));
    };
    for (size_t i = 0; i < blocks.size(); ++i)
        fill(i);
    for (size_t i = 0; i < blocks.size(); i += 2) {
        release(blocks[i]);
        blocks[i] = nullptr;
    }
    for (size_t i = 0; i < blocks.size(); i += 2)
        fill(i);
    std::atomic<bool> intact{true};
    for (size_t worker = 0; worker < threads.size(); ++worker)
        threads[worker] = std::thread([&, worker] {
            for (size_t i = worker; i < blocks.size(); i += threads.size()) {
                const size_t size = initialSize(i) + 2048;
                void* moved = resize(blocks[i], size);
                if (!moved) {
                    intact = false;
                    continue;
                }
                blocks[i] = moved;
                for (size_t byte = 0; byte < initialSize(i); ++byte)
                    if (static_cast<unsigned char*>(moved)[byte] != static_cast<unsigned char>(i))
                        intact = false;
            }
        });
    for (auto& worker : threads)
        worker.join();
    uint64_t expectedBytes = baseline;
    for (auto* block : blocks)
        expectedBytes += usable(block);
    STONE_CHECK(intact && bytes() == expectedBytes);
    for (size_t i = 0; i < blocks.size(); ++i)
        release(blocks[(i * 4051) % blocks.size()]);
    STONE_CHECK(bytes() == baseline);

    wchar_t benchmark[2]{};
    if (GetEnvironmentVariableW(L"STONE_CORE_ALLOCATION_BENCHMARK", benchmark, 2) == 1 && benchmark[0] == L'1') {
        std::barrier start(5);
        for (size_t worker = 0; worker < threads.size(); ++worker)
            threads[worker] = std::thread([&, worker] {
                start.arrive_and_wait();
                for (size_t i = 0; i < 20000; ++i) {
                    auto* block = static_cast<unsigned char*>(allocate(64 + i % 128));
                    if (!block) {
                        intact = false;
                        continue;
                    }
                    block[0] = static_cast<unsigned char>(worker);
                    auto* moved = static_cast<unsigned char*>(resize(block, 512 + i % 256));
                    if (!moved) {
                        intact = false;
                        release(block);
                        continue;
                    }
                    if (moved[0] != worker || usable(moved) < 512 + i % 256)
                        intact = false;
                    release(moved);
                }
            });
        const auto began = std::chrono::steady_clock::now();
        start.arrive_and_wait();
        for (auto& worker : threads)
            worker.join();
        const auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
        STONE_CHECK(intact && bytes() == baseline);
        std::cout << "BENCH guest allocator: 80000 malloc/realloc/usable/free sequences across four threads; us="
                  << micros << '\n';
    }
    const auto disable = Guest<void (*)()>(Libc("malloc_disable"), "()V");
    const auto enable = Guest<void (*)()>(Libc("malloc_enable"), "()V");
    // Create the host thread before locking its CRT heap.
    std::atomic<bool> ready{false}, go{false}, started{false}, completed{false};
    std::thread blocked([&] {
        ready = true;
        while (!go.load())
            std::this_thread::yield();
        started = true;
        void* block = allocate(7);
        release(block);
        completed = true;
    });
    while (!ready.load())
        std::this_thread::yield();
    disable();
    disable();
    const auto disabledBytes = bytes();
    enable();
    go = true;
    while (!started.load())
        std::this_thread::yield();
    const bool pending = !completed.load();
    enable();
    blocked.join();
    STONE_CHECK(disabledBytes == baseline && pending && completed && bytes() == baseline);
}

void CheckAndroidErrorDescriptions()
{
    const auto describe = Guest<const char* (*)(int)>(Libc("strerror"), "(I)L");
    const auto copy = Guest<int (*)(int, char*, size_t)>(Libc("strerror_r"), "(ILJ)I");
    const auto gnu = Guest<char* (*)(int, char*, size_t)>(Libc("__gnu_strerror_r"), "(ILJ)L");

    struct Error {
        int code;
        const char* text;
    };

    // Use Android numbers explicitly: Windows assigns many of these differently.
    constexpr Error errors[]{
        {0, "Success"},
        {11, "Try again"},
        {36, "File name too long"},
        {38, "Function not implemented"},
        {43, "Identifier removed"},
        {84, "Illegal byte sequence"},
        {88, "Socket operation on non-socket"},
        {95, "Operation not supported on transport endpoint"},
        {98, "Address already in use"},
        {101, "Network is unreachable"},
        {104, "Connection reset by peer"},
        {110, "Connection timed out"},
        {111, "Connection refused"},
        {115, "Operation now in progress"},
        {125, "Operation Canceled"},
        {130, "Owner died"},
        {133, "Memory page has hardware error"},
        {41, "Unknown error 41"},
        {58, "Unknown error 58"},
        {-1, "Unknown error -1"},
        {INT_MIN, "Unknown error -2147483648"},
        {INT_MAX, "Unknown error 2147483647"},
    };
    for (const auto& error : errors) {
        errno = 12;
        STONE_CHECK(std::string_view(describe(error.code)) == error.text && errno == 12);
        std::array<char, 128> buffer{};
        STONE_CHECK(copy(error.code, buffer.data(), buffer.size()) == 0 && errno == 12);
        STONE_CHECK(std::string_view(buffer.data()) == error.text);
        buffer.fill('?');
        STONE_CHECK(gnu(error.code, buffer.data(), buffer.size()) == buffer.data() && errno == 12);
        STONE_CHECK(std::string_view(buffer.data()) == error.text);
    }
    for (size_t size : {size_t{0}, size_t{1}, size_t{4}, size_t{18}, size_t{19}}) {
        std::array<char, 24> buffer;
        buffer.fill('?');
        errno = 12;
        const int result = copy(111, buffer.data() + 1, size);
        STONE_CHECK(result == (size <= std::strlen("Connection refused") ? 34 : 0) && errno == 12);
        STONE_CHECK(buffer[0] == '?' && buffer[1 + size] == '?');
        if (size)
            STONE_CHECK(std::string_view(buffer.data() + 1) ==
                        std::string_view("Connection refused").substr(0, size - 1));
        buffer.fill('?');
        STONE_CHECK(gnu(111, buffer.data() + 1, size) == buffer.data() + 1 && errno == 12);
        STONE_CHECK(buffer[0] == '?' && buffer[1 + size] == '?');
        if (size)
            STONE_CHECK(std::string_view(buffer.data() + 1) ==
                        std::string_view("Connection refused").substr(0, size - 1));
    }
    STONE_CHECK(copy(111, nullptr, 0) == 34 && gnu(111, nullptr, 0) == nullptr && errno == 12);
    const char* stable = describe(111);
    describe(43);
    describe(-1);
    STONE_CHECK(std::string_view(stable) == "Connection refused");
    std::barrier rendezvous(2);
    std::array<std::string, 2> texts;
    std::array<int, 2> saved{};
    const auto worker = [&](int index) {
        errno = 20 + index;
        const char* text = describe(-100 - index);
        rendezvous.arrive_and_wait();
        texts[index] = text;
        saved[index] = errno;
    };
    std::thread first(worker, 0), second(worker, 1);
    first.join();
    second.join();
    STONE_CHECK(texts[0] == "Unknown error -100" && texts[1] == "Unknown error -101");
    STONE_CHECK(saved[0] == 20 && saved[1] == 21);
}

void CheckAdjacentArm64Mappings()
{
    constexpr uintptr_t base = 0x7800000000;
    constexpr size_t size = 0x10000;
    constexpr int privateAnonymous = 0x22;
    const auto release = [](uint8_t* address) {
        AndroidUnmapArm64(address, size);
    };
    const auto map = [&](uintptr_t hint) {
        auto* address = AndroidMapArm64(reinterpret_cast<void*>(hint), size, 3, privateAnonymous, -1, 0);
        STONE_CHECK(address != reinterpret_cast<void*>(-1));
        return std::unique_ptr<uint8_t, decltype(release)>(static_cast<uint8_t*>(address), release);
    };
    const auto first = map(base);
    auto second = map(base + size);
    STONE_CHECK(reinterpret_cast<uintptr_t>(first.get()) == base);
    STONE_CHECK(reinterpret_cast<uintptr_t>(second.get()) == base + size);

    constexpr uintptr_t boundary = base + size - 8;
    auto* bytes = AndroidMappedArm64Data(boundary, 16, true);
    STONE_CHECK(bytes == first.get() + size - 8);
    std::memset(bytes, 0x5a, 16);
    STONE_CHECK(first.get()[size - 1] == 0x5a && second.get()[0] == 0x5a);

    STONE_CHECK(AndroidProtectArm64(second.get(), size, 1) == 0);
    STONE_CHECK(AndroidMappedArm64Data(boundary, 16, true) == nullptr);
    STONE_CHECK(AndroidMappedArm64Data(boundary, 16, false) == bytes);
    second.reset();
    STONE_CHECK(AndroidMappedArm64Data(boundary, 16, false) == nullptr);
}

void CheckSocketOptions()
{
    const auto socket = Guest<int (*)(int, int, int)>(AndroidSocketImport("socket"), "(III)I");
    const auto set = Guest<int (*)(int, int, int, const void*, int)>(AndroidSocketImport("setsockopt"), "(IIILI)I");
    const auto get = Guest<int (*)(int, int, int, void*, int*)>(AndroidSocketImport("getsockopt"), "(IIILL)I");
    const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
    const auto poll = Guest<int (*)(void*, unsigned, int, size_t)>(Libc("__poll_chk"), "(LIIJ)I");
    const int fd = socket(2, 1, 0); // Android AF_INET, SOCK_STREAM.
    STONE_CHECK(fd >= 0);
    constexpr int solSocket = 1, keepAlive = 9, mark = 36;
    const int enabled = 1;
    const int setResult = set(fd, solSocket, keepAlive, &enabled, sizeof(enabled));
    const int setError = errno;
    int actual = -1, length = sizeof(actual);
    const int getResult = get(fd, solSocket, keepAlive, &actual, &length);
    const int getError = errno;
    const int markResult = set(fd, solSocket, mark, &enabled, sizeof(enabled));
    const int markError = errno;
    STONE_CHECK(close(fd) == 0);
    Require(setResult == 0, "setsockopt SO_KEEPALIVE succeeded (errno " + std::to_string(setError) + ')');
    Require(getResult == 0, "getsockopt SO_KEEPALIVE succeeded (errno " + std::to_string(getError) + ')');
    Require(actual == 1 && length == sizeof(actual), "SO_KEEPALIVE is enabled and returned an int (value " +
                                                         std::to_string(actual) + ", length " + std::to_string(length) +
                                                         ')');
    STONE_CHECK(markResult == -1 && markError == 92); // Android ENOPROTOOPT.

    constexpr int ipv6 = 41, ipv6Only = 26;
    const int dualStack = socket(10, 1, 0); // Android AF_INET6, SOCK_STREAM.
    STONE_CHECK(dualStack >= 0);
    actual = 0;
    length = sizeof(actual);
    STONE_CHECK(get(dualStack, 1, 39, &actual, &length) == 0 && actual == 10 && length == sizeof(actual));
    actual = 0;
    length = sizeof(actual);
    STONE_CHECK(get(dualStack, 1, 38, &actual, &length) == 0 && actual == IPPROTO_TCP);
    actual = -1;
    length = sizeof(actual);
    const int dualStackResult = get(dualStack, ipv6, ipv6Only, &actual, &length);
    STONE_CHECK(close(dualStack) == 0);
    STONE_CHECK(dualStackResult == 0 && actual == 0);

    struct {
        int fd;
        int16_t events, ready;
    } ignored{-1, 4, -1};

    STONE_CHECK(poll(&ignored, 1, 0, sizeof(ignored)) == 0 && ignored.ready == 0);
}

void CheckInotifyDirectoryWatches()
{
    namespace fs = std::filesystem;
    const auto root = fs::current_path() / ("stone-inotify-" + std::to_string(GetCurrentProcessId()));
    STONE_CHECK(!fs::exists(root) && fs::create_directory(root));
    const auto cleanup = [&](void*) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    };
    const std::unique_ptr<void, decltype(cleanup)> guard(reinterpret_cast<void*>(1), cleanup);
    const auto text = [](const fs::path& p) {
        const auto s = p.u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    };
    const auto create = Guest<int (*)(int)>(Libc("inotify_init1"), "(I)I");
    const auto add = Guest<int (*)(int, const char*, uint32_t)>(Libc("inotify_add_watch"), "(ILI)I");
    const auto remove = Guest<int (*)(int, int)>(Libc("inotify_rm_watch"), "(II)I");
    const auto read = Guest<intptr_t (*)(int, void*, size_t)>(Libc("read"), "(ILJ)J");
    const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
    const auto flags = Guest<int (*)(int, int, int)>(Libc("fcntl"), "(III)I");
    const auto duplicate = Guest<int (*)(int)>(Libc("dup"), "(I)I");

    struct PollFd {
        int fd;
        int16_t events, returned;
    };

    const auto poll = Guest<int (*)(PollFd*, size_t, int)>(Libc("poll"), "(LJI)I");

    struct Header {
        int32_t watch;
        uint32_t mask, cookie, length;
    };

    struct Event {
        Header header;
        std::string name;
    };

    const int fd = create(0x80800);
    STONE_CHECK(fd >= 0);
    const auto fdCleanup = [&](void*) {
        close(fd);
    };
    const std::unique_ptr<void, decltype(fdCleanup)> fdGuard(reinterpret_cast<void*>(1), fdCleanup);
    STONE_CHECK(create(1) == -1 && errno == EINVAL);
    STONE_CHECK(flags(fd, 1, 0) == 1 && (flags(fd, 3, 0) & 0x800));
    STONE_CHECK(add(fd, nullptr, 0x100) == -1 && errno == EFAULT);
    STONE_CHECK(add(fd, text(root).c_str(), 0) == -1 && errno == EINVAL);
    STONE_CHECK(add(-1, text(root).c_str(), 0x100) == -1 && errno == EBADF);
    const int wd = add(fd, text(root).c_str(), 0xfff);
    STONE_CHECK(wd > 0);
    STONE_CHECK(add(fd, text(root).c_str(), 0x10000100) == -1 && errno == EEXIST);
    STONE_CHECK(add(fd, text(root).c_str(), 0x100) == wd);
    STONE_CHECK(add(fd, text(root).c_str(), 0x20000eff) == wd);
    std::array<char, 65536> bytes{};
    STONE_CHECK(read(fd, bytes.data(), bytes.size()) == -1 && errno == EAGAIN);
    PollFd pending{fd, 1, 0};
    STONE_CHECK(poll(&pending, 1, 0) == 0);
    auto collect = [&](int timeout) {
        PollFd ready{fd, 1, 0};
        STONE_CHECK(poll(&ready, 1, timeout) == 1 && ready.returned == 1);
        const auto count = read(fd, bytes.data(), bytes.size());
        STONE_CHECK(count >= 16);
        std::vector<Event> events;
        for (size_t offset = 0; offset < static_cast<size_t>(count);) {
            STONE_CHECK(offset + sizeof(Header) <= static_cast<size_t>(count));
            Header header;
            std::memcpy(&header, bytes.data() + offset, sizeof(header));
            STONE_CHECK(header.length % 4 == 0 &&
                        offset + sizeof(header) + header.length <= static_cast<size_t>(count));
            const char* name = bytes.data() + offset + sizeof(header);
            STONE_CHECK(!header.length || std::memchr(name, 0, header.length));
            events.push_back({header, header.length ? name : ""});
            offset += sizeof(header) + header.length;
        }
        return events;
    };
    auto await = [&](uint32_t mask, const std::string& name) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
            for (const auto& event : collect(2000))
                if ((event.header.mask & mask) && event.name == name)
                    return event;
        STONE_CHECK(false);
        return Event{};
    };
    const auto file = root / fs::path(u8"\u76d1\u89c6-\U0001f30d.txt");
    const auto name = text(file.filename());
    {
        std::ofstream out(file);
        out << "created";
    }
    STONE_CHECK(poll(&pending, 1, 2000) == 1);
    STONE_CHECK(read(fd, bytes.data(), 1) == -1 && errno == EINVAL);
    STONE_CHECK(await(0x100, name).header.watch == wd);
    STONE_CHECK(add(fd, text(file).c_str(), 0x100) == -1 && errno == ENOTDIR);
    const auto sub = root / "child";
    fs::create_directory(sub);
    STONE_CHECK(await(0x100, "child").header.mask & 0x40000000);
    fs::remove(sub);
    STONE_CHECK(await(0x200, "child").header.mask & 0x40000000);
    const auto moved = root / "renamed.txt";
    fs::rename(file, moved);
    bool from = false, to = false;
    uint32_t cookie = 0;
    while (!from || !to)
        for (const auto& event : collect(2000)) {
            if ((event.header.mask & 0x40) && event.name == name) {
                from = true;
                cookie = event.header.cookie;
            }
            if ((event.header.mask & 0x80) && event.name == "renamed.txt") {
                STONE_CHECK(from && cookie && cookie == event.header.cookie);
                to = true;
            }
        }
    {
        std::ofstream out(moved, std::ios::app);
        out << "changed";
    }
    await(2, "renamed.txt");
    fs::remove(moved);
    await(0x200, "renamed.txt");
    const int copy = duplicate(fd);
    STONE_CHECK(copy >= 0 && flags(copy, 1, 0) == 0);
    STONE_CHECK(remove(copy, wd) == 0);
    STONE_CHECK(await(0x8000, "").header.watch == wd);
    STONE_CHECK(remove(copy, wd) == -1 && errno == EINVAL);
    STONE_CHECK(close(copy) == 0);
    STONE_CHECK(add(fd, text(root).c_str(), 0x80000100) > wd);
    {
        std::ofstream out(root / "once");
    }
    bool created = false, ignored = false;
    while (!created || !ignored)
        for (const auto& event : collect(2000)) {
            if (event.header.mask & 0x100)
                created = true;
            if (event.header.mask & 0x8000)
                ignored = true;
        }
    {
        std::ofstream out(root / "unwatched");
    }
    STONE_CHECK(poll(&pending, 1, 20) == 0);
    const int epollWatch = add(fd, text(root).c_str(), 0x100);
    STONE_CHECK(epollWatch > 0);
#pragma pack(push, 1)

    struct EpollEvent {
        uint32_t events;
        uint64_t data;
    };

#pragma pack(pop)
    const auto epollCreate = Guest<int (*)(int)>(Libc("epoll_create1"), "(I)I");
    const auto epollControl = Guest<int (*)(int, int, int, const EpollEvent*)>(Libc("epoll_ctl"), "(IIIL)I");
    const auto epollWait = Guest<int (*)(int, EpollEvent*, int, int)>(Libc("epoll_wait"), "(ILII)I");
    const int epoll = epollCreate(0);
    STONE_CHECK(epoll >= 0);
    EpollEvent interest{0x80000001, 1234}, ready{};
    STONE_CHECK(epollControl(epoll, 1, fd, &interest) == 0);
    STONE_CHECK(epollWait(epoll, &ready, 1, 0) == 0);
    {
        std::ofstream out(root / "edge-first");
    }
    STONE_CHECK(epollWait(epoll, &ready, 1, 2000) == 1 && ready.events == 1 && ready.data == 1234);
    STONE_CHECK(epollWait(epoll, &ready, 1, 0) == 0);
    const auto available = Guest<int (*)(int, unsigned long, int*)>(Libc("ioctl"), "(IJL)I");
    int count = 0;
    STONE_CHECK(available(fd, 0x541b, &count) == 0 && count >= 16);
    await(0x100, "edge-first");
    {
        std::ofstream out(root / "edge-second");
    }
    STONE_CHECK(epollWait(epoll, &ready, 1, 2000) == 1);
    await(0x100, "edge-second");
    interest.events = 0x40000001;
    STONE_CHECK(epollControl(epoll, 3, fd, &interest) == 0);
    {
        std::ofstream out(root / "one-shot-epoll");
    }
    STONE_CHECK(epollWait(epoll, &ready, 1, 2000) == 1 && epollWait(epoll, &ready, 1, 0) == 0);
    await(0x100, "one-shot-epoll");
    STONE_CHECK(close(epoll) == 0);
    STONE_CHECK(remove(fd, epollWatch) == 0);
    await(0x8000, "");
    // Root rename invalidates the path-bound grant rather than silently following it.
    const auto watched = root / "watched";
    fs::create_directory(watched);
    STONE_CHECK(add(fd, text(watched).c_str(), 0x800) > 0);
    fs::rename(watched, root / "moved-watch");
    bool movedSelf = false, removedSelf = false;
    while (!movedSelf || !removedSelf)
        for (const auto& event : collect(2000)) {
            movedSelf |= (event.header.mask & 0x800) != 0;
            removedSelf |= (event.header.mask & 0x8000) != 0;
        }
    const auto deleted = root / "delete-watch";
    fs::create_directory(deleted);
    STONE_CHECK(add(fd, text(deleted).c_str(), 0x400) > 0);
    fs::remove(deleted);
    bool deleteSelf = false, ignoredSelf = false;
    while (!deleteSelf || !ignoredSelf)
        for (const auto& event : collect(2000)) {
            deleteSelf |= (event.header.mask & 0x400) != 0;
            ignoredSelf |= (event.header.mask & 0x8000) != 0;
        }
    // A duplicate shares the watch queue and status flags, with independent FD flags.
    STONE_CHECK(add(fd, text(root).c_str(), 0x100) > 0);
    const int blocking = duplicate(fd);
    STONE_CHECK(blocking >= 0);
    STONE_CHECK(flags(blocking, 4, 0) == 0 && flags(fd, 3, 0) == 0);
    std::atomic<intptr_t> received{-2};
    std::thread reader([&] {
        std::array<char, 1024> data{};
        received = read(blocking, data.data(), data.size());
    });
    {
        std::ofstream out(root / "blocking");
    }
    reader.join();
    STONE_CHECK(received >= 16);
    STONE_CHECK(close(blocking) == 0);
}

void CheckSymbolicLinkPaths()
{
    namespace fs = std::filesystem;
    const auto parent = fs::current_path();
    const auto root = parent / ("stone-symbolic-links-" + std::to_string(GetCurrentProcessId()));
    STONE_CHECK(root.is_absolute() && root.parent_path() == parent &&
                root.filename().string().starts_with("stone-symbolic-links-"));
    STONE_CHECK(fs::create_directory(root));
    const auto cleanup = [&](void*) {
        std::error_code error;
        fs::remove_all(root, error);
    };
    const std::unique_ptr<void, decltype(cleanup)> guard(reinterpret_cast<void*>(1), cleanup);
    const auto text = [](const fs::path& path) {
        const auto value = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    };
    const auto link = Guest<int (*)(const char*, const char*)>(Libc("symlink"), "(LL)I");
    const auto readlink = Guest<intptr_t (*)(const char*, char*, size_t)>(Libc("readlink"), "(LLJ)J");
    const auto open = Guest<int (*)(const char*, int, int)>(Libc("open"), "(LII)I");
    const auto openat = Guest<int (*)(int, const char*, int, int)>(Libc("openat"), "(ILII)I");
    const auto read = Guest<intptr_t (*)(int, void*, size_t)>(Libc("read"), "(ILJ)J");
    const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
    const auto unlink = Guest<int (*)(const char*)>(Libc("unlink"), "(L)I");
    const auto rename = Guest<int (*)(const char*, const char*)>(Libc("rename"), "(LL)I");
    const auto target = root / "target";
    const auto alias = text(root / "alias"), moved = text(root / "renamed");
    {
        std::ofstream output(target, std::ios::binary);
        output << 'Q';
    }
    STONE_CHECK(link("target", alias.c_str()) == 0);
    const auto list = Guest<intptr_t (*)(const char*, char*, size_t)>(Libc("listxattr"), "(LLJ)J");
    const auto llist = Guest<intptr_t (*)(const char*, char*, size_t)>(Libc("llistxattr"), "(LLJ)J");
    char attributes[8];
    std::memset(attributes, 0x5a, sizeof(attributes));
    for (auto query : {list, llist}) {
        STONE_CHECK(query(alias.c_str(), nullptr, 0) == 0);
        STONE_CHECK(query(alias.c_str(), attributes, sizeof(attributes)) == 0);
        STONE_CHECK(query(nullptr, nullptr, 0) == -1 && errno == EFAULT);
        STONE_CHECK(query("", nullptr, 0) == -1 && errno == ENOENT);
        STONE_CHECK(query(alias.c_str(), nullptr, 1) == -1 && errno == EFAULT);
    }
    STONE_CHECK(std::all_of(std::begin(attributes), std::end(attributes), [](char value) {
        return value == 0x5a;
    }));
    const auto get = Guest<intptr_t (*)(const char*, const char*, void*, size_t)>(Libc("getxattr"), "(LLLJ)J");
    const auto lget = Guest<intptr_t (*)(const char*, const char*, void*, size_t)>(Libc("lgetxattr"), "(LLLJ)J");
    for (auto query : {get, lget}) {
        STONE_CHECK(query(alias.c_str(), "security.selinux", nullptr, 0) == -1 && errno == 61);
        STONE_CHECK(query(alias.c_str(), "user.stone-test", attributes, sizeof(attributes)) == -1 && errno == 61);
    }
    char buffer[32];
    std::memset(buffer, '?', sizeof(buffer));
    STONE_CHECK(readlink(alias.c_str(), buffer, 3) == 3 && std::memcmp(buffer, "tar", 3) == 0 && buffer[3] == '?');
    const int directory = open(text(root).c_str(), 0x10000, 0);
    STONE_CHECK(directory >= 0);
    const int file = openat(directory, "alias", 0, 0);
    STONE_CHECK(file >= 0);
    char byte = 0;
    STONE_CHECK(read(file, &byte, 1) == 1 && byte == 'Q');
    STONE_CHECK(close(file) == 0);
    STONE_CHECK(openat(directory, "alias", 0x20000, 0) == -1 && errno == 40);
    STONE_CHECK(openat(directory, "alias", 0xc1, 0600) == -1 && errno == EEXIST);
    const auto fopen = Guest<void* (*)(const char*, const char*)>(Libc("fopen"), "(LL)L");
    STONE_CHECK(fopen(alias.c_str(), "wx") == nullptr && errno == EEXIST);

    struct StatxBuffer {
        uint32_t mask, blockSize;
        uint64_t attributes;
        uint32_t links, uid, gid;
        uint16_t mode, reserved;
        uint64_t inode, size, blocks;
        std::array<uint8_t, 200> rest;
    } status{};

    static_assert(sizeof(StatxBuffer) == 256 && offsetof(StatxBuffer, mode) == 28 && offsetof(StatxBuffer, size) == 40);
    const auto statx = Guest<int (*)(int, const char*, int, unsigned, void*)>(Libc("statx"), "(ILIIL)I");
    STONE_CHECK(statx(directory, "alias", 0x100, 0x7ff, &status) == 0 && (status.mode & 0170000) == 0120000 &&
                status.size == 6);
    STONE_CHECK(statx(directory, "alias", 0, 0x7ff, &status) == 0 && (status.mode & 0170000) == 0100000 &&
                status.size == 1);
    STONE_CHECK(rename(alias.c_str(), moved.c_str()) == 0 && fs::exists(target));
    STONE_CHECK(unlink(moved.c_str()) == 0 && fs::exists(target));
    STONE_CHECK(close(directory) == 0);
    STONE_CHECK(readlink(text(target).c_str(), buffer, sizeof(buffer)) == -1 && errno == EINVAL);
    STONE_CHECK(link("missing", alias.c_str()) == 0);
    STONE_CHECK(llist(alias.c_str(), nullptr, 0) == 0);
    STONE_CHECK(list(alias.c_str(), nullptr, 0) == -1 && errno == ENOENT);
    STONE_CHECK(lget(alias.c_str(), "security.selinux", nullptr, 0) == -1 && errno == 61);
    STONE_CHECK(get(alias.c_str(), "security.selinux", nullptr, 0) == -1 && errno == ENOENT);
    STONE_CHECK(unlink(alias.c_str()) == 0);
    STONE_CHECK(link(nullptr, alias.c_str()) == -1 && errno == EFAULT);
    STONE_CHECK(link("", alias.c_str()) == -1 && errno == ENOENT);
    std::error_code error;
    fs::remove_all(root, error);
    STONE_CHECK(!error && !fs::exists(root));
}

void CheckSocketReplacement()
{
    const auto socket = Guest<int (*)(int, int, int)>(Libc("socket"), "(III)I");
    const auto pair = Guest<int (*)(int, int, int, int*)>(Libc("socketpair"), "(IIIL)I");
    const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
    const auto replace = Guest<int (*)(int, int)>(Libc("dup2"), "(II)I");
    const auto replace3 = Guest<int (*)(int, int, int)>(Libc("dup3"), "(III)I");
    const auto shutdown = Guest<int (*)(int, int)>(Libc("shutdown"), "(II)I");
    const auto read = Guest<intptr_t (*)(int, void*, size_t)>(Libc("read"), "(ILJ)J");
    const auto write = Guest<intptr_t (*)(int, const void*, size_t)>(Libc("write"), "(ILJ)J");
    const auto flags = Guest<int (*)(int, int, int)>(Libc("fcntl"), "(III)I");
    const auto poll = Guest<int (*)(void*, size_t, int)>(Libc("poll"), "(LJI)I");
    const auto option = Guest<int (*)(int, int, int, void*, int*)>(Libc("getsockopt"), "(IIILL)I");
    const auto owner = Guest<uint64_t (*)(int)>(Libc("android_fdsan_get_owner_tag"), "(I)J");
    const auto retag = Guest<void (*)(int, uint64_t, uint64_t)>(Libc("android_fdsan_exchange_owner_tag"), "(IJJ)V");
    const auto taggedClose = Guest<int (*)(int, uint64_t)>(Libc("android_fdsan_close_with_tag"), "(IJ)I");
    int endpoints[]{-1, -1}, target = -1;
    const auto cleanup = [&](void*) {
        for (int fd : {endpoints[0], endpoints[1], target})
            if (fd >= 0)
                taggedClose(fd, owner(fd));
    };
    const std::unique_ptr<void, decltype(cleanup)> guard(reinterpret_cast<void*>(1), cleanup);
    STONE_CHECK(pair(1, 1, 0, endpoints) == 0 && (target = socket(2, 1, 0)) >= 0);
    int domain = 0, length = sizeof(domain);
    STONE_CHECK(option(target, 1, 39, &domain, &length) == 0 && domain == 2);
    constexpr uint64_t sourceTag = UINT64_C(0xb00000000001234), targetTag = UINT64_C(0xb00000000005678);
    retag(endpoints[0], 0, sourceTag);
    retag(target, 0, targetTag);
    STONE_CHECK(replace3(endpoints[0], target, 0x80000) == target && flags(target, 1, 0) == 1);
    STONE_CHECK(owner(endpoints[0]) == sourceTag && owner(target) == targetTag);
    domain = 0;
    length = sizeof(domain);
    STONE_CHECK(option(target, 1, 39, &domain, &length) == 0 && domain == 1);
    STONE_CHECK(replace(target, target) == target && flags(target, 1, 0) == 1);
    STONE_CHECK(replace3(target, target, 0) == -1 && errno == EINVAL);
    STONE_CHECK(replace3(endpoints[0], target, 1) == -1 && errno == EINVAL);
    STONE_CHECK(replace(-1, target) == -1 && errno == EBADF);
    STONE_CHECK(write(endpoints[1], "Q", 1) == 1);
    char value = 0;
    STONE_CHECK(read(target, &value, 1) == 1 && value == 'Q');
    STONE_CHECK(replace(endpoints[0], target) == target && flags(target, 1, 0) == 0);
    STONE_CHECK(owner(endpoints[0]) == sourceTag && owner(target) == targetTag);
    STONE_CHECK(taggedClose(endpoints[0], sourceTag) == 0);
    endpoints[0] = -1;
    STONE_CHECK(write(endpoints[1], "R", 1) == 1 && read(target, &value, 1) == 1 && value == 'R');
    STONE_CHECK(shutdown(target, 2) == 0 && close(endpoints[1]) == 0);
    endpoints[1] = -1;

    struct {
        int fd;
        int16_t events, returned;
    } ready{target, 1, 0};

    STONE_CHECK(poll(&ready, 1, 1000) == 1 && (ready.returned & (1 | 0x10)) && read(target, &value, 1) == 0);
}

void CheckInterfaceSnapshot()
{
    const auto read = Guest<int (*)(AndroidIfaddrs**)>(Libc("getifaddrs"), "(L)I");
    const auto release = Guest<void (*)(AndroidIfaddrs*)>(Libc("freeifaddrs"), "(L)V");
    const auto name = Guest<char* (*)(unsigned, char*)>(Libc("if_indextoname"), "(IL)L");
    const auto index = Guest<unsigned (*)(const char*)>(Libc("if_nametoindex"), "(L)I");
    const auto ioctl = Guest<int (*)(int, unsigned long, void*)>(Libc("ioctl"), "(IJL)I");
    const auto socket = Guest<int (*)(int, int, int)>(Libc("socket"), "(III)I");
    const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while ((!ReadClientHostState().interfaces || ReadClientHostState().interfaces->error) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    AndroidIfaddrs *first = nullptr, *second = nullptr;
    STONE_CHECK(read(&first) == 0 && first && read(&second) == 0 && second && second != first);
    const auto snapshot = ReadClientHostState().interfaces;
    STONE_CHECK(snapshot && !snapshot->error);
    int fd = socket(2, 2, 0);
    STONE_CHECK(fd >= 0);
    bool loopback = false, ipv6 = false;
    size_t count = 0;
    for (auto* entry = first; entry; entry = entry->next) {
        STONE_CHECK(++count <= 384 && entry->name && std::strlen(entry->name) < 16);
        const auto number = index(entry->name);
        char back[16]{};
        STONE_CHECK(number && name(number, back) == back && std::strcmp(entry->name, back) == 0);

        struct Request {
            char name[16];
            uint8_t data[24];
        } request{};

        strcpy_s(request.name, entry->name);
        STONE_CHECK(ioctl(fd, 0x8913, &request) == 0);
        uint16_t flags = 0;
        std::memcpy(&flags, request.data, 2);
        STONE_CHECK(flags == entry->flags);
        const auto expected = std::find_if(snapshot->entries.begin(), snapshot->entries.end(), [&](const auto& value) {
            return value.index == number;
        });
        STONE_CHECK(expected != snapshot->entries.end() && expected->flags == flags);
        const int mtuResult = ioctl(fd, 0x8921, &request);
        if (expected->mtu > INT_MAX)
            STONE_CHECK(mtuResult == -1 && errno == 75);
        else {
            uint32_t mtu = 0;
            std::memcpy(&mtu, request.data, 4);
            STONE_CHECK(mtuResult == 0 && expected->mtu == mtu);
        }
        if (entry->address) {
            uint16_t family = 0;
            std::memcpy(&family, entry->address, 2);
            STONE_CHECK((family == 2 || family == 10) && entry->netmask && !entry->data);
            const size_t offset = family == 2 ? 4 : 8, size = family == 2 ? 4 : 16;
            const auto* bytes = static_cast<const uint8_t*>(entry->address);
            const auto source =
                std::find_if(expected->addresses.begin(), expected->addresses.end(), [&](const auto& value) {
                    return value.family == family && std::memcmp(value.bytes.data(), bytes + offset, size) == 0;
                });
            STONE_CHECK(source != expected->addresses.end());
            uint16_t maskFamily = 0;
            std::memcpy(&maskFamily, entry->netmask, 2);
            STONE_CHECK(maskFamily == family);
            bool zero = false;
            uint32_t prefix = 0;
            for (size_t byte = 0; byte < size; ++byte)
                for (int bit = 7; bit >= 0; --bit) {
                    const bool set = (static_cast<const uint8_t*>(entry->netmask)[offset + byte] & (1u << bit)) != 0;
                    STONE_CHECK(!zero || !set);
                    if (set)
                        ++prefix;
                    else
                        zero = true;
                }
            STONE_CHECK(prefix == source->prefix);
            if (family == 10) {
                uint32_t scope = 0;
                std::memcpy(&scope, bytes + 24, 4);
                STONE_CHECK(scope == source->scope);
            }
            if (family == 10) {
                ipv6 = true;
                STONE_CHECK(!entry->broadcast);
            }
            if (family == 2 && (flags & 8))
                loopback = static_cast<const uint8_t*>(entry->address)[4] == 127;
        }
    }
    STONE_CHECK(loopback && ipv6); // This Windows fixture exposes real IPv4 and IPv6 loopback.

    struct Request {
        char name[16];
        uint8_t data[24];
    } missing{};

    strcpy_s(missing.name, "stone-missing");
    STONE_CHECK(ioctl(fd, 0x8913, &missing) == -1 && errno == ENODEV);
    STONE_CHECK(ioctl(-1, 0x8913, &missing) == -1 && errno == EBADF);
    STONE_CHECK(ioctl(fd, 0x8921, nullptr) == -1 && errno == EFAULT);
    STONE_CHECK(index("stone-missing") == 0 && errno == ENODEV && !name(0, missing.name) && errno == ENXIO);
    STONE_CHECK(read(nullptr) == -1 && errno == EFAULT);
    first->next = first; // Free uses the allocation registry, not caller-modifiable links.
    errno = 123;
    release(first);
    STONE_CHECK(errno == 123 && index(second->name) != 0);
    release(second);
    release(nullptr);
    STONE_CHECK(close(fd) == 0);
}

void CheckNetworkResolverOwnership()
{
    const auto resolve =
        Guest<int (*)(const char*, const char*, const AndroidAddrInfo*, unsigned, unsigned, AndroidAddrInfo**)>(
            Libc("android_getaddrinfofornet"), "(LLLIIL)I");
    const auto release = Guest<void (*)(AndroidAddrInfo*)>(Libc("freeaddrinfo"), "(L)V");
    const auto describe =
        Guest<int (*)(const void*, uint32_t, char*, size_t, char*, size_t, int)>(Libc("getnameinfo"), "(LILJLJI)I");
    AndroidAddrInfo hints{};
    hints.family = 10; // AF_INET6
    hints.type = 1;    // SOCK_STREAM
    hints.flags = 4;   // AI_NUMERICHOST
    AndroidAddrInfo* address = nullptr;
    STONE_CHECK(resolve("::1", "443", &hints, 0, UINT32_MAX, &address) == 0 && address);
    const std::unique_ptr<AndroidAddrInfo, decltype(release)> result(address, release);
    STONE_CHECK(result->family == hints.family && result->type == hints.type && result->address);
    char host[64]{}, service[8]{};
    constexpr int numericHostAndPort = 2 | 8;
    STONE_CHECK(describe(result->address, result->length, host, sizeof(host), service, sizeof(service),
                         numericHostAndPort) == 0);
    STONE_CHECK(std::strcmp(host, "::1") == 0 && std::strcmp(service, "443") == 0);
}

void CheckMulticastSockets()
{
    const auto socket = Guest<int (*)(int, int, int)>(AndroidSocketImport("socket"), "(III)I");
    const auto set = Guest<int (*)(int, int, int, const void*, int)>(AndroidSocketImport("setsockopt"), "(IIILI)I");
    const auto get = Guest<int (*)(int, int, int, void*, int*)>(AndroidSocketImport("getsockopt"), "(IIILL)I");
    const auto bind = Guest<int (*)(int, const void*, int)>(AndroidSocketImport("bind"), "(ILI)I");
    const auto name = Guest<int (*)(int, void*, int*)>(AndroidSocketImport("getsockname"), "(ILL)I");
    const auto send = Guest<intptr_t (*)(int, const void*, size_t, int, const void*, int)>(
        AndroidSocketImport("sendto"), "(ILJILI)J");
    const auto receive =
        Guest<intptr_t (*)(int, void*, size_t, int, void*, int*)>(AndroidSocketImport("recvfrom"), "(ILJILL)J");
    const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
    const auto poll = Guest<int (*)(void*, unsigned, int, size_t)>(Libc("__poll_chk"), "(LIIJ)I");
    std::array<int, 4> fds{-1, -1, -1, -1};
    const auto cleanup = [&](void*) {
        for (int fd : fds)
            if (fd >= 0)
                close(fd);
    };
    const std::unique_ptr<void, decltype(cleanup)> owned(reinterpret_cast<void*>(1), cleanup);
    for (int index = 0; index < 4; ++index) {
        fds[index] = socket(index == 3 ? 10 : 2, 2 | 0x800, 0);
        STONE_CHECK(fds[index] >= 0);
    }
    constexpr int ipv4 = 0, ipv6 = 41, multicastAll = 49;
    const int enabled = 1, disabled = 0, invalid = 2;
    for (int fd : fds) {
        STONE_CHECK(set(fd, ipv4, multicastAll, &disabled, sizeof(disabled)) == 0);
        int value = -1, size = sizeof(value);
        STONE_CHECK(get(fd, ipv4, multicastAll, &value, &size) == 0 && value == 0 && size == sizeof(value));
        STONE_CHECK(set(fd, ipv4, multicastAll, &enabled, sizeof(enabled)) == -1 && errno == 95);
        STONE_CHECK(set(fd, ipv4, multicastAll, &invalid, sizeof(invalid)) == -1 && errno == EINVAL);
        STONE_CHECK(set(fd, ipv4, multicastAll, &disabled, 1) == -1 && errno == EINVAL);
        STONE_CHECK(set(fd, ipv4, multicastAll, nullptr, sizeof(disabled)) == -1 && errno == EFAULT);
    }
    const unsigned char ttl = 7;
    STONE_CHECK(set(fds[2], ipv4, 33, &ttl, 1) == 0);
    std::array<unsigned char, 3> ttlRead{0x11, 0, 0x22};
    int size = 1;
    STONE_CHECK(get(fds[2], ipv4, 33, &ttlRead[1], &size) == 0 && size == 1 && ttlRead[1] == ttl &&
                ttlRead[0] == 0x11 && ttlRead[2] == 0x22);
    const int hops = 255;
    STONE_CHECK(set(fds[3], ipv6, 18, &hops, sizeof(hops)) == 0);
    int actual = -1;
    size = sizeof(actual);
    STONE_CHECK(get(fds[3], ipv6, 18, &actual, &size) == 0 && actual == hops);
    STONE_CHECK(set(fds[2], ipv4, 34, &enabled, sizeof(enabled)) == 0);
    STONE_CHECK(set(fds[2], ipv4, 34, &disabled, sizeof(disabled)) == -1 && errno == 95);
    STONE_CHECK(set(fds[3], ipv6, 19, &enabled, sizeof(enabled)) == 0);

    sockaddr_in endpoint{};
    endpoint.sin_family = 2;
    for (int index : {0, 1}) {
        STONE_CHECK(set(fds[index], 1, 2, &enabled, sizeof(enabled)) == 0);
        STONE_CHECK(bind(fds[index], &endpoint, sizeof(endpoint)) == 0);
        size = sizeof(endpoint);
        STONE_CHECK(name(fds[index], &endpoint, &size) == 0 && endpoint.sin_port != 0);
    }
    const uint32_t loopback = htonl(INADDR_LOOPBACK);
    STONE_CHECK(set(fds[2], ipv4, 32, &loopback, sizeof(loopback)) == 0);
    sockaddr_in sender{};
    sender.sin_family = 2;
    sender.sin_addr.s_addr = loopback;
    STONE_CHECK(bind(fds[2], &sender, sizeof(sender)) == 0);
    DWORD loopIndex = 0;
    STONE_CHECK(GetBestInterface(loopback, &loopIndex) == NO_ERROR && loopIndex > 0);
    GROUP_REQ group{};
    group.gr_interface = loopIndex;
    static_assert(sizeof(group) == 136 && offsetof(GROUP_REQ, gr_group) == 8);
    auto& groupAddress = reinterpret_cast<sockaddr_in&>(group.gr_group);
    groupAddress.sin_family = 2;
    groupAddress.sin_addr.s_addr = htonl(0xefff4d01); // 239.255.77.1
    STONE_CHECK(set(fds[0], ipv4, 42, &group, sizeof(group)) == 0);
    STONE_CHECK(set(fds[0], ipv4, 42, &group, sizeof(group) - 1) == -1 && errno == EINVAL);

    struct {
        uint32_t group, address;
        int index;
    } legacy{htonl(0xefff4d02), 0, static_cast<int>(loopIndex)};

    STONE_CHECK(set(fds[1], ipv4, 35, &legacy, sizeof(legacy)) == 0);
    decltype(legacy) selection{0, 0, static_cast<int>(loopIndex)};
    STONE_CHECK(set(fds[2], ipv4, 32, &selection, sizeof(selection)) == 0);
    uint32_t selected = 0;
    size = sizeof(selected);
    STONE_CHECK(get(fds[2], ipv4, 32, &selected, &size) == 0 && selected == loopback);
    const auto ready = [&](int fd, int timeout) {
        struct {
            int fd;
            int16_t events, ready;
        } request{fd, 1, 0};
        const int result = poll(&request, 1, timeout, sizeof(request));
        STONE_CHECK(result >= 0);
        return result == 1 && (request.ready & 1);
    };
    const std::array<unsigned char, 7> bytes{0, 1, 0x80, 0xff, 2, 0, 3};
    endpoint.sin_addr.s_addr = groupAddress.sin_addr.s_addr;
    const auto exchange = [&](int target) {
        STONE_CHECK(send(fds[2], bytes.data(), bytes.size(), 0, &endpoint, sizeof(endpoint)) == bytes.size());
        STONE_CHECK(ready(target, 2000));
        sockaddr_in origin{};
        size = sizeof(origin);
        std::array<unsigned char, 16> buffer{};
        STONE_CHECK(receive(target, buffer.data(), buffer.size(), 0, &origin, &size) == bytes.size() &&
                    std::equal(bytes.begin(), bytes.end(), buffer.begin()) && origin.sin_family == 2 &&
                    origin.sin_addr.s_addr == loopback);
    };
    exchange(fds[0]);
    STONE_CHECK(!ready(fds[1], 200));
    STONE_CHECK(set(fds[0], ipv4, 45, &group, sizeof(group)) == 0);
    STONE_CHECK(send(fds[2], bytes.data(), bytes.size(), 0, &endpoint, sizeof(endpoint)) == bytes.size());
    STONE_CHECK(!ready(fds[0], 200) && !ready(fds[1], 200));
    endpoint.sin_addr.s_addr = legacy.group;
    exchange(fds[1]);
    STONE_CHECK(set(fds[1], ipv4, 36, &legacy, sizeof(legacy)) == 0);
}

void CheckThreadSchedulingAttributes()
{
    const auto init = Guest<int (*)(AndroidThreadAttr*)>(AndroidThreadImport("pthread_attr_init"), "(L)I");
    const auto setPolicy =
        Guest<int (*)(AndroidThreadAttr*, int)>(AndroidThreadImport("pthread_attr_setschedpolicy"), "(LI)I");
    const auto setParameters =
        Guest<int (*)(AndroidThreadAttr*, const int*)>(AndroidThreadImport("pthread_attr_setschedparam"), "(LL)I");
    const auto getPolicy =
        Guest<int (*)(const AndroidThreadAttr*, int*)>(AndroidThreadImport("pthread_attr_getschedpolicy"), "(LL)I");
    const auto getParameters =
        Guest<int (*)(const AndroidThreadAttr*, int*)>(AndroidThreadImport("pthread_attr_getschedparam"), "(LL)I");
    AndroidThreadAttr attrs{};
    int policy = -1, priority = -1;
    STONE_CHECK(init(&attrs) == 0);
    STONE_CHECK(getPolicy(&attrs, &policy) == 0 && policy == 0);
    STONE_CHECK(getParameters(&attrs, &priority) == 0 && priority == 0);
    constexpr int fifo = 1, requested = 11;
    STONE_CHECK(setPolicy(&attrs, fifo) == 0 && setParameters(&attrs, &requested) == 0);
    STONE_CHECK(getPolicy(&attrs, &policy) == 0 && policy == fifo);
    STONE_CHECK(getParameters(&attrs, &priority) == 0 && priority == requested);
    const auto getGuard =
        Guest<int (*)(const AndroidThreadAttr*, size_t*)>(AndroidThreadImport("pthread_attr_getguardsize"), "(LL)I");
    size_t guard = SIZE_MAX;
    STONE_CHECK(getGuard(&attrs, &guard) == 0 && guard == 0);
    attrs.guardSize = 8192;
    STONE_CHECK(getGuard(&attrs, &guard) == 0 && guard == 8192);
}

void CheckWideSignalSets()
{
    const auto empty = Guest<int (*)(uint64_t*)>(AndroidThreadImport("sigemptyset64"), "(L)I");
    const auto fill = Guest<int (*)(uint64_t*)>(AndroidThreadImport("sigfillset64"), "(L)I");
    const auto add = Guest<int (*)(uint64_t*, int)>(AndroidThreadImport("sigaddset64"), "(LI)I");
    const auto remove = Guest<int (*)(uint64_t*, int)>(AndroidThreadImport("sigdelset64"), "(LI)I");
    const auto contains = Guest<int (*)(const uint64_t*, int)>(AndroidThreadImport("sigismember64"), "(LI)I");
    const auto mask =
        Guest<int (*)(int, const uint64_t*, uint64_t*)>(AndroidThreadImport("pthread_sigmask64"), "(ILL)I");
    uint64_t signals = 0;
    STONE_CHECK(fill(&signals) == 0 && signals == UINT64_MAX);
    STONE_CHECK(empty(&signals) == 0 && signals == 0);
    STONE_CHECK(add(&signals, 64) == 0 && signals == uint64_t{1} << 63);
    STONE_CHECK(contains(&signals, 64) == 1 && contains(&signals, 63) == 0);
    STONE_CHECK(remove(&signals, 64) == 0 && signals == 0 && add(&signals, 64) == 0);
    uint64_t saved = 0, actual = 0;
    const int blocked = mask(0, &signals, &saved); // SIG_BLOCK.
    const int queried = mask(0, nullptr, &actual);
    const int restored = mask(2, &saved, nullptr); // SIG_SETMASK.
    STONE_CHECK(blocked == 0 && queried == 0 && restored == 0 && (actual & signals) == signals);
    const auto processMask = Guest<int (*)(int, const uint64_t*, uint64_t*)>(Libc("sigprocmask64"), "(ILL)I");
    STONE_CHECK(processMask(0, &signals, &saved) == 0);
    const int processQueried = processMask(0, nullptr, &actual);
    const int processRestored = processMask(2, &saved, nullptr);
    STONE_CHECK(processQueried == 0 && processRestored == 0 && (actual & signals) == signals);
    errno = 0;
    STONE_CHECK(processMask(3, &signals, nullptr) == -1 && errno == EINVAL);
    errno = 0;
    STONE_CHECK(add(&signals, 0) == -1 && errno == EINVAL);
}

void CheckLargeFilePosition()
{
    const auto seek = Guest<int (*)(void*, int64_t, int)>(AndroidLibcImport("fseeko64"), "(LJI)I");
    const auto tell = Guest<int64_t (*)(void*)>(AndroidLibcImport("ftello64"), "(L)J");
    FILE* stream = nullptr;
    STONE_CHECK(tmpfile_s(&stream) == 0 && stream);
    const std::unique_ptr<FILE, decltype(&std::fclose)> file(stream, &std::fclose);
    constexpr int64_t position = (int64_t{1} << 32) + 123;
    STONE_CHECK(seek(file.get(), position, SEEK_SET) == 0 && tell(file.get()) == position);
    STONE_CHECK(seek(file.get(), -123, SEEK_CUR) == 0 && tell(file.get()) == int64_t{1} << 32);
}
}

void TestSignalSets(Suite& suite)
{
    suite.Run("libc-signal-set-64-aliases", CheckWideSignalSets);
}

void TestProcCommandLine(Suite& suite)
{
    struct Stat {
        uint64_t device, inode, links;
        uint32_t mode, uid, gid, padding;
        uint64_t special;
        int64_t size, blockSize, blocks;
        AndroidTimespec accessed, modified, changed;
        int64_t reserved[3];
    };

    static_assert(sizeof(Stat) == 144);
    const auto open = Guest<int (*)(const char*, int, int)>(Libc("open"), "(LII)I");
    const auto openAt = Guest<int (*)(int, const char*, int, int)>(Libc("openat"), "(ILII)I");
    const auto read = Guest<intptr_t (*)(int, void*, size_t)>(Libc("read"), "(ILJ)J");
    const auto write = Guest<intptr_t (*)(int, const void*, size_t)>(Libc("write"), "(ILJ)J");
    const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
    const auto seek = Guest<int64_t (*)(int, int64_t, int)>(Libc("lseek64"), "(IJI)J");
    const auto duplicate = Guest<int (*)(int)>(Libc("dup"), "(I)I");
    const auto fstat = Guest<int (*)(int, Stat*)>(Libc("fstat"), "(IL)I");
    constexpr const char* path = "/proc/self/cmdline";
    const auto contents = [&](int fd, std::string_view expected) {
        STONE_CHECK(fd >= 0);
        const auto release = [&](int* value) {
            close(*value);
        };
        const std::unique_ptr<int, decltype(release)> guard(&fd, release);
        std::array<char, 512> bytes{};
        STONE_CHECK(read(fd, bytes.data(), 3) == 3);
        const auto rest = read(fd, bytes.data() + 3, bytes.size() - 3);
        STONE_CHECK(rest == static_cast<intptr_t>(expected.size() + 1 - 3));
        STONE_CHECK(std::string_view(bytes.data(), expected.size()) == expected && bytes[expected.size()] == '\0');
        STONE_CHECK(read(fd, bytes.data(), bytes.size()) == 0);
        STONE_CHECK(seek(fd, 0, SEEK_SET) == 0 && read(fd, bytes.data(), 1) == 1 && bytes[0] == expected.front());
        const int copy = duplicate(fd);
        STONE_CHECK(copy >= 0);
        Stat metadata{};
        STONE_CHECK(fstat(copy, &metadata) == 0 && metadata.mode == 0100444 && metadata.size == 0);
        const auto copied = read(copy, bytes.data(), 1);
        STONE_CHECK(close(copy) == 0 && copied == 1 && bytes[0] == expected[1]);
        errno = 0;
        STONE_CHECK(write(fd, "x", 1) == -1 && errno == EBADF);
        DWORD flags = 0;
        STONE_CHECK(GetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), &flags) &&
                    !(flags & HANDLE_FLAG_INHERIT));
    };
    suite.Run("proc-cmdline-native-startup-and-application-process-name", [&] {
        contents(open(path, 0x80000, 0), "Stone");
        SetAndroidProcessName("com.trebledot.stone.proc:worker");
        contents(open(path, 0x80000, 0), "com.trebledot.stone.proc:worker");
        const auto pidPath = "/proc/" + std::to_string(GetCurrentProcessId()) + "/cmdline";
        // An absolute openat path ignores an otherwise invalid directory fd.
        contents(openAt(-1, pidPath.c_str(), 0xa0000, 0), "com.trebledot.stone.proc:worker");
        const int pending = open(path, 0x80000, 0);
        STONE_CHECK(pending >= 0);
        SetAndroidProcessName("com.trebledot.stone.proc");
        contents(pending, "com.trebledot.stone.proc:worker");
        contents(open(path, 0x80000, 0), "com.trebledot.stone.proc");
    });
    suite.Run("proc-cmdline-stdio-metadata-and-errors", [&] {
        const auto fopen = Guest<FILE* (*)(const char*, const char*)>(Libc("fopen"), "(LL)L");
        const auto fread = Guest<size_t (*)(void*, size_t, size_t, FILE*)>(Libc("fread"), "(LJJL)J");
        const auto fclose = Guest<int (*)(FILE*)>(Libc("fclose"), "(L)I");
        const auto access = Guest<int (*)(const char*, int)>(Libc("access"), "(LI)I");
        const auto stat = Guest<int (*)(const char*, Stat*)>(Libc("stat"), "(LL)I");
        const auto link = Guest<intptr_t (*)(const char*, char*, size_t)>(Libc("readlink"), "(LLJ)J");
        FILE* file = fopen(path, "re");
        STONE_CHECK(file);
        const std::unique_ptr<FILE, decltype(fclose)> guard(file, fclose);
        std::array<char, 128> bytes{};
        const std::string expected = "com.trebledot.stone.proc";
        STONE_CHECK(fread(bytes.data(), 1, bytes.size(), file) == expected.size() + 1 &&
                    std::string_view(bytes.data(), expected.size()) == expected && bytes[expected.size()] == '\0');
        Stat metadata{};
        STONE_CHECK(stat(path, &metadata) == 0 && metadata.mode == 0100444 && metadata.size == 0);
        STONE_CHECK(access(path, 0) == 0 && access(path, 4) == 0);
        STONE_CHECK(access(path, 2) == -1 && errno == EACCES);
        STONE_CHECK(access(path, 1) == -1 && errno == EACCES);
        STONE_CHECK(link(path, bytes.data(), bytes.size()) == -1 && errno == EINVAL);
        for (int flags : {1, 2, 0x200, 0x400})
            STONE_CHECK(open(path, flags, 0) == -1 && errno == EACCES);
        STONE_CHECK(open(path, 0xc0, 0) == -1 && errno == EEXIST);
        STONE_CHECK(open(path, 0x10000, 0) == -1 && errno == ENOTDIR);
        STONE_CHECK(open(path, 0x40000000, 0) == -1 && errno == EINVAL);
        STONE_CHECK(!fopen(path, "r+") && errno == EACCES);
        STONE_CHECK(!fopen(path, "w") && errno == EACCES);
        STONE_CHECK(open("/proc/0/cmdline", 0, 0) == -1 && errno == ENOENT);
        STONE_CHECK(open("/proc/self/cmdline-extra", 0, 0) == -1 && errno == ENOENT);
        for (const std::string& invalid : {std::string{}, std::string("bad\0name", 8), std::string(4096, 'a')}) {
            bool rejected = false;
            try {
                SetAndroidProcessName(invalid);
            }
            catch (const std::invalid_argument&) {
                rejected = true;
            }
            STONE_CHECK(rejected);
        }
        contents(open(path, 0x80000, 0), expected);
    });
    suite.Run("proc-cmdline-descriptor-replacement-and-reuse", [&] {
        const auto dup2 = Guest<int (*)(int, int)>(Libc("dup2"), "(II)I");
        int proc = open(path, 0x80000, 0);
        STONE_CHECK(proc >= 0);
        const auto release = [&](int* value) {
            close(*value);
        };
        const std::unique_ptr<int, decltype(release)> procGuard(&proc, release);
        const auto diskPath = std::filesystem::current_path() / "proc-cmdline-reuse.bin";
        const auto diskName = diskPath.u8string();
        const auto cleanup = [&](void*) {
            std::filesystem::remove(diskPath);
        };
        const std::unique_ptr<void, decltype(cleanup)> diskGuard(reinterpret_cast<void*>(1), cleanup);
        int ordinary = open(reinterpret_cast<const char*>(diskName.c_str()), 2 | 0x40 | 0x200, 0600);
        STONE_CHECK(ordinary >= 0);
        const std::unique_ptr<int, decltype(release)> ordinaryGuard(&ordinary, release);
        STONE_CHECK(write(ordinary, "ordinary", 8) == 8);
        int copy = duplicate(ordinary);
        STONE_CHECK(copy >= 0);
        const std::unique_ptr<int, decltype(release)> copyGuard(&copy, release);
        Stat metadata{};
        STONE_CHECK(dup2(proc, copy) == copy && fstat(copy, &metadata) == 0 && metadata.mode == 0100444);
        STONE_CHECK(dup2(ordinary, proc) == proc && fstat(proc, &metadata) == 0 && metadata.size == 8);
        STONE_CHECK(close(copy) == 0);
        const int reused = copy;
        copy = open(reinterpret_cast<const char*>(diskName.c_str()), 0, 0);
        STONE_CHECK(copy == reused && fstat(copy, &metadata) == 0 && metadata.size == 8);
    });
    SetAndroidProcessName("Stone");
}

void TestAllocations(Suite& suite)
{
    suite.Run("libc-art-allocation-accounting", CheckArtAllocationAccounting);
    suite.Run("arm64-libc-size-class-tail-and-exact-jni-bounds", [] {
        GuestHeap heap;
        const auto exact = heap.Allocate(13, true), slot = heap.AllocateLibc(13, false),
                   aligned = heap.AllocateLibc(25, true, 64);
        STONE_CHECK(exact && slot && aligned && aligned % 64 == 0);
        STONE_CHECK(heap.AllocationSize(exact) == 13 && heap.AllocationSize(slot) == 16 &&
                    heap.AllocationSize(aligned) == 32);
        STONE_CHECK(!heap.At(exact + 8, 8) && heap.At(slot + 8, 8));
        STONE_CHECK(!heap.At(slot + 9, 8) && !heap.At(slot + 16, 1));
        for (size_t i = 13; i < 16; ++i)
            STONE_CHECK(*heap.At(slot + i, 1) == 0);
        STONE_CHECK(!heap.AllocateLibc(SIZE_MAX, false));
        STONE_CHECK(heap.Free(slot) && !heap.At(slot, 1));
        STONE_CHECK(heap.Free(exact) && heap.Free(aligned));
    });
}

void TestInotifyStorage(Suite& suite)
{
    suite.Run("filesystem-unconfigured-mounts-fail-without-jni", [] {
        STONE_CHECK(!AndroidSystemGuestPath(L"/system"));
        STONE_CHECK(!IsAndroidSystemCachePath(std::filesystem::current_path()));
        const auto missing = [](auto operation) {
            try {
                operation();
            }
            catch (const AndroidSystemPathError& error) {
                STONE_CHECK(error.code == ENOENT);
                return;
            }
            STONE_CHECK(false);
        };
        missing([] {
            ResolveAndroidSystemFile(L"system");
        });
        missing([] {
            ListAndroidSystemDirectory(L"system");
        });
        missing([] {
            ReadAndroidSystemLink(L"system");
        });
    });
    suite.Run("inotify-shared-storage-grants-revocation-and-private-boundary", [] {
        namespace fs = std::filesystem;
        const auto profile = fs::current_path() / ("stone-inotify-storage-" + std::to_string(GetCurrentProcessId()));
        STONE_CHECK(!fs::exists(profile));
        fs::create_directories(profile / "cache");
        fs::create_directories(profile / "data");
        const auto cleanup = [&](void*) {
            std::error_code ignored;
            fs::remove_all(profile, ignored);
        };
        const std::unique_ptr<void, decltype(cleanup)> guard(reinterpret_cast<void*>(1), cleanup);
        SetAndroidSystemDirectory(profile / "cache", profile / "data");
        ConfigureAndroidSharedStorage(L"com.trebledot.stone.watch", false);
        const auto create = Guest<int (*)(int)>(Libc("inotify_init1"), "(I)I");
        const auto add = Guest<int (*)(int, const char*, uint32_t)>(Libc("inotify_add_watch"), "(ILI)I");
        const auto read = Guest<intptr_t (*)(int, void*, size_t)>(Libc("read"), "(ILJ)J");
        const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
        const int fd = create(0x800);
        STONE_CHECK(fd >= 0);
        STONE_CHECK(add(fd, "/sdcard", 0x100) == -1 && errno == EACCES);
        const auto root = profile / "storage/emulated/0";
        fs::create_directories(root / "Android/data/com.trebledot.stone.watch/files");
        fs::create_directories(root / "Android/data/foreign.pkg");
        const int own = add(fd, "/sdcard/Android/data/com.trebledot.stone.watch/files", 0x100);
        STONE_CHECK(own > 0);
        STONE_CHECK(add(fd, "/sdcard/Android/data/foreign.pkg", 0x100) == -1 && errno == EACCES);
        ConfigureAndroidSharedStorage(L"com.trebledot.stone.watch", true);
        const int publicWatch = add(fd, "/storage/emulated/0/Documents", 0x100);
        STONE_CHECK(publicWatch > own);
        {
            std::ofstream out(root / "Documents/pending-private-name");
        }
        struct PollFd {
            int fd;
            int16_t events, returned;
        } pending{fd, 1, 0};
        const auto poll = Guest<int (*)(PollFd*, size_t, int)>(Libc("poll"), "(LJI)I");
        STONE_CHECK(poll(&pending, 1, 2000) == 1 && pending.returned == 1);
        // Even events queued before revocation must not disclose names afterwards.
        ConfigureAndroidSharedStorage(L"com.trebledot.stone.watch", false);
        std::array<char, 1024> bytes{};
        const auto count = read(fd, bytes.data(), bytes.size());
        STONE_CHECK(count == 16);
        struct Header {
            int32_t watch;
            uint32_t mask, cookie, length;
        } header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        STONE_CHECK(header.watch == publicWatch && header.mask == 0x8000 && !header.length);
        STONE_CHECK(read(fd, bytes.data(), bytes.size()) == -1 && errno == EAGAIN);
        {
            std::ofstream out(root / "Android/data/com.trebledot.stone.watch/files/own-event");
        }
        intptr_t ownCount = -1;
        for (int retry = 0; retry < 200 && ownCount < 0; ++retry) {
            ownCount = read(fd, bytes.data(), bytes.size());
            if (ownCount < 0)
                Sleep(10);
        }
        STONE_CHECK(ownCount > 16);
        std::memcpy(&header, bytes.data(), sizeof(header));
        STONE_CHECK(header.watch == own && header.mask == 0x100);
        STONE_CHECK(close(fd) == 0);
        // A slow consumer gets an overflow indication, with a bounded queue.
        const int flooded = create(0x800);
        STONE_CHECK(flooded >= 0);
        STONE_CHECK(add(flooded, "/sdcard/Android/data/com.trebledot.stone.watch/files", 0x100) > 0);
        for (int index = 0; index < 4300; ++index) {
            std::ofstream out(root / "Android/data/com.trebledot.stone.watch/files" / std::to_string(index));
        }
        Sleep(200);
        bool overflow = false;
        std::vector<char> batch(1024 * 1024);
        for (int retry = 0; retry < 200 && !overflow; ++retry) {
            const auto length = read(flooded, batch.data(), batch.size());
            if (length < 0) {
                STONE_CHECK(errno == EAGAIN);
                Sleep(10);
                continue;
            }
            for (size_t offset = 0; offset < static_cast<size_t>(length);) {
                Header event{};
                std::memcpy(&event, batch.data() + offset, sizeof(event));
                overflow |= event.watch == -1 && event.mask == 0x4000 && !event.length;
                offset += sizeof(event) + event.length;
                STONE_CHECK(offset <= static_cast<size_t>(length));
            }
        }
        STONE_CHECK(overflow);
        STONE_CHECK(close(flooded) == 0);
        const int blocked = create(0);
        STONE_CHECK(blocked >= 0);
        std::atomic<intptr_t> canceled{-2};
        std::thread reader([&] {
            char data[32];
            canceled = read(blocked, data, sizeof(data));
        });
        Sleep(30);
        STONE_CHECK(close(blocked) == 0);
        reader.join();
        STONE_CHECK(canceled == -1);
    });
}

void TestLibcEnvironment(Suite& suite, std::wstring_view first)
{
    suite.Run("libc-environment-before-narrow-crt-initialization", [&] {
        // wmain initializes only the wide environment. Reproduce ART's first
        // environ access without an earlier getenv masking the startup bug.
        STONE_CHECK(_wputenv_s(L"STONE_ENVIRONMENT_STARTUP", L"inherited") == 0);
        STONE_CHECK(*__p__environ() == nullptr);
        const auto checkEntries = [](char** entries) {
            STONE_CHECK(entries != nullptr);
            bool found = false;
            for (size_t index = 0; entries[index]; ++index) {
                STONE_CHECK(index < 32768);
                found |= std::strcmp(entries[index], "STONE_ENVIRONMENT_STARTUP=inherited") == 0;
            }
            STONE_CHECK(found);
        };
        const auto get = Guest<const char* (*)(const char*)>(AndroidLibcImport("getenv"), "(L)L");
        if (first == L"getenv") {
            const auto value = get("STONE_ENVIRONMENT_STARTUP");
            STONE_CHECK(value && std::strcmp(value, "inherited") == 0);
        }
        else if (first == L"arm64") {
            const auto slot = Arm64LibcDataImport("environ");
            uintptr_t entries = 0;
            STONE_CHECK(ReadArm64GuestAllocation(slot, &entries, sizeof(entries)));
            checkEntries(reinterpret_cast<char**>(entries));
        }
        else {
            STONE_CHECK(first == L"environ");
            const auto slot = reinterpret_cast<char***>(AndroidLibcImport("environ"));
            STONE_CHECK(slot != nullptr);
            checkEntries(*slot);
        }
        const auto slot = reinterpret_cast<char***>(AndroidLibcImport("environ"));
        checkEntries(*slot);
        const auto set = Guest<int (*)(const char*, const char*, int)>(AndroidLibcImport("setenv"), "(LLI)I");
        const auto unset = Guest<int (*)(const char*)>(AndroidLibcImport("unsetenv"), "(L)I");
        STONE_CHECK(set("STONE_ENVIRONMENT_STARTUP", "ignored", 0) == 0);
        STONE_CHECK(std::strcmp(get("STONE_ENVIRONMENT_STARTUP"), "inherited") == 0);
        STONE_CHECK(set("STONE_ENVIRONMENT_STARTUP", "updated", 1) == 0);
        STONE_CHECK(slot == reinterpret_cast<char***>(AndroidLibcImport("environ")));
        STONE_CHECK(std::strcmp(get("STONE_ENVIRONMENT_STARTUP"), "updated") == 0);
        bool updated = false;
        for (auto entry = *slot; *entry; ++entry)
            updated |= std::strcmp(*entry, "STONE_ENVIRONMENT_STARTUP=updated") == 0;
        STONE_CHECK(updated);
        STONE_CHECK(get("STONE_ENVIRONMENT_STARTUP_MISSING") == nullptr);
        STONE_CHECK(unset("STONE_ENVIRONMENT_STARTUP") == 0 && get("STONE_ENVIRONMENT_STARTUP") == nullptr);
    });
}

void TestLibc(Suite& suite, int api)
{
    TestLogAndMath(suite);
    suite.Run("libc-art-allocation-accounting", CheckArtAllocationAccounting);
    suite.Run("libc-sigchld-registration-for-java-processes", [] {
        const auto action =
            Guest<int (*)(int, const AndroidSignalAction*, AndroidSignalAction*)>(Libc("sigaction"), "(ILL)I");
        const auto raise = Guest<int (*)(int)>(Libc("raise"), "(I)I");
        AndroidSignalAction previous{}, current{};
        const AndroidSignalAction defaults{0x10000001, 0, 0,
                                           0}; // Image UNIXProcess: SA_NOCLDSTOP | SA_RESTART, SIG_DFL.
        STONE_CHECK(action(17, &defaults, &previous) == 0);
        struct Restore {
            decltype(action) set;
            AndroidSignalAction old;
            ~Restore()
            {
                set(17, &old, nullptr);
            }
        } restore{action, previous};
        STONE_CHECK(action(17, nullptr, &current) == 0 && current.flags == defaults.flags && current.handler == 0);
        STONE_CHECK(raise(17) == 0); // SIGCHLD's default action is ignored; no child process was created.
    });
    suite.Run("libc-native-siginfo-software-and-hardware-context", [] {
        const auto action =
            Guest<int (*)(int, const AndroidSignalAction*, AndroidSignalAction*)>(Libc("sigaction"), "(ILL)I");
        const auto raise = Guest<int (*)(int)>(Libc("raise"), "(I)I");
        static bool software = false, hardware = false;
        software = hardware = false;
        const auto callback = Import(
            "Stone native siginfo handler", +[](int signal, AndroidSignalInfo* info, AndroidSignalContext* context) {
                if (signal == 10)
                    software = info->number == 10 && info->code == -6 && info->detail[0] != 0 &&
                               context->machine.floating == context->floating.data() && context->stack.size > 0 &&
                               !(context->mask & (uint64_t{1} << 9));
                if (signal == 11) {
                    hardware = info->number == 11 && info->code == 1 && info->detail[0] == 0x1234 &&
                               context->machine.registers[16] == 0x8765 && context->machine.registers[22] == 0x1234;
                    context->machine.registers[16] += 2;
                    context->machine.registers[13] = 42;
                }
            });
        const AndroidSignalAction handler{4, callback, 0, 0};
        const AndroidSignalAction probingHandler{0x18000c04, callback, 0,
                                                 0}; // sigchain's flags, including SA_UNSUPPORTED.
        AndroidSignalAction oldSoftware{}, oldHardware{};
        STONE_CHECK(action(10, &handler, &oldSoftware) == 0 && action(11, &probingHandler, &oldHardware) == 0);
        struct Restore {
            decltype(action) set;
            AndroidSignalAction user, fault;
            ~Restore()
            {
                set(10, &user, nullptr);
                set(11, &fault, nullptr);
            }
        } restore{action, oldSoftware, oldHardware};
        AndroidSignalAction supported{};
        STONE_CHECK(action(11, nullptr, &supported) == 0 && supported.flags == 0x18000004 &&
                    supported.handler == callback);
        STONE_CHECK(raise(10) == 0 && software);
        CONTEXT machine{};
        RtlCaptureContext(&machine);
        machine.Rip = 0x8765;
        EXCEPTION_RECORD fault{};
        fault.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        fault.ExceptionAddress = reinterpret_cast<void*>(machine.Rip);
        fault.NumberParameters = 2;
        fault.ExceptionInformation[1] = 0x1234;
        STONE_CHECK(DeliverAndroidHardwareSignal(fault, machine) && hardware && machine.Rip == 0x8767 &&
                    machine.Rax == 42);
    });
    suite.Run("libc-vfscanf-sysv-va-list-and-linux-long", [] {
        FILE* stream = nullptr;
        STONE_CHECK(tmpfile_s(&stream) == 0 && stream);
        const std::unique_ptr<FILE, decltype(&std::fclose)> close(stream, &std::fclose);
        STONE_CHECK(std::fputs("4294967297 2.5 Stone", stream) >= 0);
        std::rewind(stream);
        int64_t integer = 0;
        double fraction = 0;
        char text[8]{};
        const uint64_t destinations[]{reinterpret_cast<uintptr_t>(&integer),
                                      reinterpret_cast<uintptr_t>(&fraction),
                                      reinterpret_cast<uintptr_t>(text),
                                      0,
                                      0,
                                      0};
        AndroidVaList args{0, 48, nullptr, reinterpret_cast<const char*>(destinations)};
        const auto scan = Guest<int (*)(void*, const char*, AndroidVaList*)>(Libc("vfscanf"), "(LLL)I");
        STONE_CHECK(scan(stream, "%ld %lf %7s", &args) == 3);
        STONE_CHECK(integer == 4294967297LL && fraction == 2.5 && std::strcmp(text, "Stone") == 0);
    });
    suite.Run("libc-getchar-reads-stdin-and-eof", [] {
        FILE* stream = nullptr;
        STONE_CHECK(tmpfile_s(&stream) == 0 && stream);
        const std::unique_ptr<FILE, decltype(&std::fclose)> close(stream, &std::fclose);
        const int saved = _dup(_fileno(stdin));
        STONE_CHECK(saved >= 0);
        struct Restore {
            int fd;
            ~Restore()
            {
                _dup2(fd, _fileno(stdin));
                _close(fd);
                std::clearerr(stdin);
            }
        } restore{saved};
        STONE_CHECK(std::fputs("A\xff", stream) >= 0 && std::fflush(stream) == 0);
        std::rewind(stream);
        STONE_CHECK(_dup2(_fileno(stream), _fileno(stdin)) == 0);
        std::clearerr(stdin);
        const auto read = Guest<int (*)()>(Libc("getchar"), "()I");
        STONE_CHECK(read() == 'A' && read() == 255 && read() == EOF && std::feof(stdin));
    });
    suite.Run("libc-resolver-query-wire-format-and-base64", [] {
        const auto query =
            Guest<int (*)(int, const char*, int, int, const void*, int, const void*, unsigned char*, int)>(
                Libc("res_mkquery"), "(ILIILILLI)I");
        std::array<unsigned char, 64> packet{};
        STONE_CHECK(query(0, "www.example.com.", 1, 28, nullptr, 0, nullptr, packet.data(),
                          static_cast<int>(packet.size())) == 33);
        constexpr unsigned char question[]{3,   'w', 'w', 'w', 7,   'e', 'x', 'a', 'm', 'p', 'l',
                                           'e', 3,   'c', 'o', 'm', 0,   0,   28,  0,   1};
        STONE_CHECK(packet[2] == 1 && packet[5] == 1 &&
                    std::memcmp(packet.data() + 12, question, sizeof(question)) == 0);
        STONE_CHECK(
            query(0, "bad..example", 1, 1, nullptr, 0, nullptr, packet.data(), static_cast<int>(packet.size())) == -1 &&
            errno == EINVAL);
        STONE_CHECK(query(0, "example", 1, 1, nullptr, 0, nullptr, packet.data(), 1) == -1 && errno == ENOSPC);
        const auto encode = Guest<int (*)(const unsigned char*, size_t, char*, size_t)>(Libc("__b64_ntop"), "(LJLJ)I");
        char text[9]{};
        STONE_CHECK(encode(reinterpret_cast<const unsigned char*>("Stone"), 5, text, sizeof(text)) == 8);
        STONE_CHECK(std::strcmp(text, "U3RvbmU=") == 0);
        STONE_CHECK(encode(reinterpret_cast<const unsigned char*>("Stone"), 5, text, 8) == -1 && errno == ENOSPC);
    });
    suite.Run("libc-search-tree-insert-find-delete-destroy", [] {
        const auto insert = Guest<void* (*)(const void*, void**, uintptr_t)>(Libc("tsearch"), "(LLJ)L");
        const auto find = Guest<void* (*)(const void*, void* const*, uintptr_t)>(Libc("tfind"), "(LLJ)L");
        const auto erase = Guest<void* (*)(const void*, void**, uintptr_t)>(Libc("tdelete"), "(LLJ)L");
        const auto destroy = Guest<void (*)(void*, uintptr_t)>(Libc("tdestroy"), "(LJ)V");
        const auto compare = Import(
            "Stone tree comparator", +[](const void* a, const void* b) {
                const int left = *static_cast<const int*>(a), right = *static_cast<const int*>(b);
                return (left > right) - (left < right);
            });
        int values[]{3, 1, 5, 2, 4};
        void* root = nullptr;
        for (auto& value : values)
            STONE_CHECK(insert(&value, &root, compare));
        const int existing = 2, removed = 3;
        const auto* found = static_cast<const int* const*>(find(&existing, &root, compare));
        STONE_CHECK(found && **found == existing);
        STONE_CHECK(erase(&removed, &root, compare) && !find(&removed, &root, compare));
        static int freed = 0, sum = 0;
        freed = sum = 0;
        const auto release = Import(
            "Stone tree key release", +[](const void* key) {
                ++freed;
                sum += *static_cast<const int*>(key);
            });
        destroy(root, release);
        STONE_CHECK(freed == 4 && sum == 12);
    });
    suite.Run("libc-futex-compare-and-requeue", [] {
        std::array<uint32_t, 2> words{};
        auto& [source, destination] = words;
        const auto call = [&](unsigned command, uint32_t wake, uintptr_t fourth, uint32_t* target, uint32_t compare) {
            return AndroidX64Syscall(202, reinterpret_cast<uintptr_t>(&source), command | 128u, wake, fourth,
                                     reinterpret_cast<uintptr_t>(target), compare);
        };
        STONE_CHECK(call(4, 0, 2, &destination, 1) == -11);
        STONE_CHECK(call(4, 0, 2, reinterpret_cast<uint32_t*>(1), 0) == -EINVAL);
        std::array<int, 2> results{-1, -1};
        std::array<std::jthread, 2> threads;
        for (size_t i = 0; i < threads.size(); ++i)
            threads[i] = std::jthread([&, i] {
                const AndroidTimespec timeout{2, 0};
                results[i] = AndroidFutex(source, 128, 0, &timeout, UINT_MAX);
            });
        intptr_t queued = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (queued < 2 && std::chrono::steady_clock::now() < deadline) {
            // Requeueing to the same word counts waiters without moving them.
            queued = call(4, 0, 2, &source, 0);
            std::this_thread::yield();
        }
        STONE_CHECK(queued == 2);
        STONE_CHECK(call(4, 0, 3, &destination, 0) == 2);
        STONE_CHECK(AndroidFutex(source, 129, 2, nullptr, UINT_MAX) == 0);
        STONE_CHECK(AndroidFutex(destination, 129, 2, nullptr, UINT_MAX) == 2);
        for (auto& thread : threads)
            thread.join();
        STONE_CHECK(results[0] == 0 && results[1] == 0);
    });
    suite.Run("libc-shared-anonymous-map-protection", [] {
        const auto map =
            Guest<void* (*)(void*, size_t, int, int, int, int64_t)>(AndroidLibcImport("mmap"), "(LJIIIJ)L");
        const auto protect = Guest<int (*)(void*, size_t, int)>(AndroidLibcImport("mprotect"), "(LJI)I");
        const auto unmap = Guest<int (*)(void*, size_t)>(AndroidLibcImport("munmap"), "(LJ)I");
        auto* address = static_cast<uint8_t*>(map(nullptr, 4096, 3, 0x21, -1, 0));
        STONE_CHECK(address != reinterpret_cast<void*>(-1));
        struct Release {
            decltype(unmap) call;
            void* address;
            ~Release()
            {
                call(address, 4096);
            }
        } release{unmap, address};
        address[0] = 42;
        STONE_CHECK(protect(address, 4096, 0) == 0 && protect(address, 4096, 1) == 0);
        STONE_CHECK(address[0] == 42);
    });
    suite.Run("libc-dynamic-code-ownership-permissions-and-signals", [] {
        const auto map = Guest<void* (*)(void*, size_t, int, int, int, int64_t)>(Libc("mmap"), "(LJIIIJ)L");
        const auto protect = Guest<int (*)(void*, size_t, int)>(Libc("mprotect"), "(LJI)I");
        const auto unmap = Guest<int (*)(void*, size_t)>(Libc("munmap"), "(LJ)I");
        constexpr size_t page = 4096, size = 3 * page;
        auto* memory = static_cast<uint8_t*>(map(nullptr, size, 3, 0x22, -1, 0));
        STONE_CHECK(memory != reinterpret_cast<void*>(-1));
        const auto release = [unmap](uint8_t* address) {
            unmap(address, size);
        };
        std::unique_ptr<uint8_t, decltype(release)> allocation(memory, release);
        const auto start = reinterpret_cast<uintptr_t>(memory);
        STONE_CHECK(!AndroidExecutableMapping(start, 1) && !AndroidExecutableMapping(start, 0));
        STONE_CHECK(!AndroidExecutableMapping(UINTPTR_MAX, 2));
        // mov eax,[0]; ret. A real implicit null check in guest dynamic code.
        constexpr uint8_t code[]{0x8b, 0x04, 0x25, 0, 0, 0, 0, 0xc3};
        std::memcpy(memory + page, code, sizeof(code));
        STONE_CHECK(protect(memory + page, page, 5) == 0);
        STONE_CHECK(FlushInstructionCache(GetCurrentProcess(), memory + page, sizeof(code)));
        STONE_CHECK(AndroidExecutableMapping(start + page, page));
        STONE_CHECK(!AndroidExecutableMapping(start + page - 1, 2));
        STONE_CHECK(!AndroidExecutableMapping(start + 2 * page - 1, 2));
        STONE_CHECK(!AndroidExecutableMapping(start + size, 1));
        DWORD previousProtection;
        STONE_CHECK(VirtualProtect(memory + page, page, PAGE_EXECUTE_READ | PAGE_GUARD, &previousProtection));
        STONE_CHECK(!AndroidExecutableMapping(start + page, 1));
        STONE_CHECK(VirtualProtect(memory + page, page, previousProtection, &previousProtection));
        STONE_CHECK(!AndroidExecutableMapping(reinterpret_cast<uintptr_t>(&TestLibc), 1));
        auto* host = VirtualAlloc(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        STONE_CHECK(host);
        const auto freeHost = [](void* address) {
            VirtualFree(address, 0, MEM_RELEASE);
        };
        const std::unique_ptr<void, decltype(freeHost)> native(host, freeHost);
        STONE_CHECK(!AndroidExecutableMapping(reinterpret_cast<uintptr_t>(host), 1));
        auto* arm = AndroidMapArm64(nullptr, page, 3, 0x22, -1, 0);
        STONE_CHECK(arm != reinterpret_cast<void*>(-1));
        STONE_CHECK(VirtualProtect(arm, page, PAGE_EXECUTE_READWRITE, &previousProtection));
        const bool armAccepted = AndroidExecutableMapping(reinterpret_cast<uintptr_t>(arm), 1);
        STONE_CHECK(AndroidUnmapArm64(arm, page) == 0 && !armAccepted);
        const auto action =
            Guest<int (*)(int, const AndroidSignalAction*, AndroidSignalAction*)>(Libc("sigaction"), "(ILL)I");
        static uintptr_t expected;
        static bool delivered;
        expected = start + page;
        delivered = false;
        const auto callback = Import(
            "Stone dynamic code implicit null check",
            +[](int signal, AndroidSignalInfo* info, AndroidSignalContext* context) {
                delivered = signal == 11 && info->number == 11 && info->code == 1 && info->detail[0] == 0 &&
                            context->machine.registers[16] == expected && context->machine.registers[22] == 0;
                context->machine.registers[16] += 7;
                context->machine.registers[13] = 42;
            });
        const AndroidSignalAction handler{4, callback, 0, 0};
        AndroidSignalAction old{};
        STONE_CHECK(action(11, &handler, &old) == 0);
        struct Restore {
            decltype(action) set;
            AndroidSignalAction old;
            ~Restore()
            {
                set(11, &old, nullptr);
            }
        } restore{action, old};
        STONE_CHECK(InitializeAndroidNativeBackend());
        const auto run = Guest<int (*)()>(start + page, "()I");
        STONE_CHECK(run() == 42 && delivered);
        STONE_CHECK(protect(memory + page, page, 1) == 0 && !AndroidExecutableMapping(start + page, 1));
        STONE_CHECK(unmap(allocation.release(), size) == 0 && !AndroidExecutableMapping(start + page, 1));
    });
    suite.Run("libc-mount-table-escaped-fields", [] {
        struct Entry {
            char *device, *directory, *type, *options;
            int frequency, pass;
        };
        const auto read = Guest<Entry* (*)(void*)>(AndroidLibcImport("getmntent"), "(L)L");
        FILE* stream = nullptr;
        STONE_CHECK(tmpfile_s(&stream) == 0 && stream);
        const std::unique_ptr<FILE, decltype(&std::fclose)> close(stream, &std::fclose);
        STONE_CHECK(std::fputs("# ignored\ninvalid\n/dev/a /mnt/a\\040b ext4 ro,nosuid 1 2\n", stream) >= 0);
        std::rewind(stream);
        const auto* entry = read(stream);
        STONE_CHECK(entry && std::strcmp(entry->device, "/dev/a") == 0);
        STONE_CHECK(std::strcmp(entry->directory, "/mnt/a b") == 0 && std::strcmp(entry->type, "ext4") == 0);
        STONE_CHECK(std::strcmp(entry->options, "ro,nosuid") == 0 && entry->frequency == 1 && entry->pass == 2);
        STONE_CHECK(!read(stream));
    });
    suite.Run("libc-thread-niceness-round-trip", [] {
        const auto self = Guest<unsigned (*)()>(AndroidLibcImport("gettid"), "()I");
        const auto get = Guest<int (*)(int, unsigned)>(AndroidLibcImport("getpriority"), "(II)I");
        const auto set = Guest<int (*)(int, unsigned, int)>(AndroidLibcImport("setpriority"), "(III)I");
        errno = 0;
        const auto id = self();
        const auto previous = get(0, id);
        STONE_CHECK(errno == 0);
        struct Restore {
            decltype(set) call;
            unsigned id;
            int value;
            ~Restore()
            {
                call(0, id, value);
            }
        } restore{set, id, previous};
        STONE_CHECK(set(0, id, 7) == 0 && get(0, id) == 7 && get(0, 0) == 7);
        STONE_CHECK(set(0, id, -1) == 0);
        errno = 0;
        STONE_CHECK(get(0, id) == -1 && errno == 0);
        STONE_CHECK(get(0, UINT_MAX) == -1 && errno == ESRCH);
    });
    suite.Run("libc-thread-cpu-clock-id", [] {
        const auto self = Guest<uint64_t (*)()>(AndroidThreadImport("pthread_self"), "()J");
        const auto get = Guest<int (*)(uint64_t, int*)>(AndroidThreadImport("pthread_getcpuclockid"), "(JL)I");
        const auto time = Guest<int (*)(int, AndroidTimespec*)>(AndroidLibcImport("clock_gettime"), "(IL)I");
        const auto resolution = Guest<int (*)(int, AndroidTimespec*)>(AndroidLibcImport("clock_getres"), "(IL)I");
        int clock = 0;
        STONE_CHECK(get(self(), &clock) == 0 && clock < 0 && (clock & 7) == 6);
        AndroidTimespec before{}, after{}, precision{};
        STONE_CHECK(time(clock, &before) == 0 && time(clock, &after) == 0);
        STONE_CHECK(after.seconds > before.seconds || (after.seconds == before.seconds && after.nanos >= before.nanos));
        STONE_CHECK(resolution(clock, &precision) == 0 && precision.seconds == 0 && precision.nanos > 0);
        STONE_CHECK(get(0, &clock) == 3);
    });
    suite.Run("libc-sigwaitinfo-64-queued-signal", [] {
        const auto self = Guest<uint64_t (*)()>(AndroidThreadImport("pthread_self"), "()J");
        const auto pid = Guest<unsigned (*)()>(AndroidLibcImport("getpid"), "()I");
        const auto send = Guest<int (*)(int, int, int)>(AndroidThreadImport("tgkill"), "(III)I");
        const auto mask =
            Guest<int (*)(int, const uint64_t*, uint64_t*)>(AndroidThreadImport("pthread_sigmask64"), "(ILL)I");
        const auto wait = Guest<int (*)(const uint64_t*, void*)>(AndroidThreadImport("sigwaitinfo64"), "(LL)I");
        const auto queue = Guest<int (*)(int, int, uint64_t)>(AndroidThreadImport("sigqueue"), "(IIJ)I");
        const uint64_t signal = uint64_t{1} << 63;
        uint64_t saved = 0;
        STONE_CHECK(mask(0, &signal, &saved) == 0);
        const int sent = send(static_cast<int>(pid()), static_cast<int>(self()), 64);
        std::array<int32_t, 32> info{};
        const int received = sent == 0 ? wait(&signal, info.data()) : -1;
        STONE_CHECK(sent == 0 && received == 64 && info[2] == -6);
        STONE_CHECK(queue(static_cast<int>(pid()), 64, 0x12345678abcdef00) == 0);
        STONE_CHECK(queue(static_cast<int>(pid()), 64, 42) == 0);
        uint64_t payload = 0;
        STONE_CHECK(wait(&signal, info.data()) == 64 && info[2] == -1);
        std::memcpy(&payload, info.data() + 6, sizeof(payload));
        STONE_CHECK(payload == 0x12345678abcdef00);
        STONE_CHECK(wait(&signal, info.data()) == 64 && info[2] == -1);
        std::memcpy(&payload, info.data() + 6, sizeof(payload));
        STONE_CHECK(payload == 42);
        const int restored = mask(2, &saved, nullptr);
        STONE_CHECK(sent == 0 && received == 64 && restored == 0);
        STONE_CHECK(info[0] == 64 && static_cast<unsigned>(info[4]) == pid());
    });
    suite.Run("libc-process-vm-readv-own-process", [] {
        struct Vector {
            const void* data;
            size_t size;
        };
        const auto pid = Guest<int (*)()>(AndroidLibcImport("getpid"), "()I");
        const auto read = Guest<intptr_t (*)(int, const Vector*, size_t, const Vector*, size_t, uint64_t)>(
            AndroidLibcImport("process_vm_readv"), "(ILJLJJ)J");
        constexpr char input[] = "Stone ART";
        std::array<char, sizeof(input)> output{};
        const Vector destination[]{{output.data(), 3}, {output.data() + 3, output.size() - 3}};
        const Vector source[]{{input, sizeof(input)}};
        STONE_CHECK(read(pid(), destination, 2, source, 1, 0) == sizeof(input));
        STONE_CHECK(std::memcmp(input, output.data(), sizeof(input)) == 0);
        errno = 0;
        STONE_CHECK(read(pid(), destination, 2, source, 1, 1) == -1 && errno == EINVAL);
        const Vector invalid[]{{reinterpret_cast<void*>(1), 4}};
        errno = 0;
        STONE_CHECK(read(pid(), destination, 2, invalid, 1, 0) == -1 && errno == EFAULT);
    });
    suite.Run("libc-x86-64-sprintf", [] {
        const auto format = Guest<int (*)(char*, const char*, const char*, int64_t, double, int, int, int)>(
            Libc("sprintf"), "(LLLJDIII)I");
        char buffer[64]{};
        constexpr std::string_view expected = "stone:-4294967297:2.5:3:4:5";
        STONE_CHECK(format(buffer, "%s:%lld:%.1f:%d:%d:%d", "stone", -4294967297LL, 2.5, 3, 4, 5) ==
                    static_cast<int>(expected.size()));
        STONE_CHECK(std::string_view(buffer) == expected);
    });
    suite.Run("libc-x86-64-pthread-exit", [] {
        const auto start = Guest<int (*)(uint64_t*, const AndroidThreadAttr*, uintptr_t, void*)>(
            AndroidThreadImport("pthread_create"), "(LLLL)I");
        const auto join = Guest<int (*)(uint64_t, void**)>(AndroidThreadImport("pthread_join"), "(JL)I");
        const auto exit = AndroidThreadImport("pthread_exit");
        STONE_CHECK(exit);
        uint64_t thread = 0;
        int value = 42;
        void* result = nullptr;
        STONE_CHECK(start(&thread, nullptr, exit, &value) == 0);
        STONE_CHECK(join(thread, &result) == 0 && result == &value);
    });
    suite.Run("libc-basic-memory-and-strings", [] {
        const auto copy = Guest<void* (*)(void*, const void*, size_t)>(Libc("memcpy"), "(LLJ)L");
        const auto length = Guest<size_t (*)(const char*)>(Libc("strlen"), "(L)J");
        char buffer[6]{};
        STONE_CHECK(copy(buffer, "stone", sizeof(buffer)) == buffer);
        STONE_CHECK(length(buffer) == 5 && std::string_view(buffer) == "stone");
    });
    suite.Run("libc-android-error-descriptions-and-buffers", CheckAndroidErrorDescriptions);
    suite.Run("libc-bionic-string-copy-and-fd-ownership-policy", [api] {
        const auto copy = Guest<size_t (*)(char*, const char*, size_t)>(AndroidLibcImport("strlcpy"), "(LLJ)J");
        char buffer[4]{};
        STONE_CHECK(copy(buffer, "stone", sizeof(buffer)) == 5);
        STONE_CHECK(std::string_view(buffer) == "sto");
        const auto level = Guest<int (*)()>(AndroidLibcImport("android_fdsan_get_error_level"), "()I");
        ConfigureAndroidFdSan(api);
        STONE_CHECK(level() == (api < 30 ? 1 : 3)); // WARN_ONCE on Q, FATAL on R and later.
    });
    suite.Run("libc-posix-wildcard-and-pathname-flags", [] {
        const auto match =
            Guest<int (*)(const char*, const char*, int)>(AndroidPosixImport("fnmatch", false), "(LLI)I");
        STONE_CHECK(match("lib*.so", "libunity.so", 0) == 0);
        constexpr int fnmPathname = 2;
        STONE_CHECK(match("*", "lib/libunity.so", fnmPathname) == 1);
    });
    suite.Run("libc-pthread-identity", [] {
        const auto self = Guest<uint64_t (*)()>(AndroidThreadImport("pthread_self"), "()J");
        const auto equal = Guest<int (*)(uint64_t, uint64_t)>(AndroidThreadImport("pthread_equal"), "(JJ)I");
        const auto current = self();
        STONE_CHECK(current != 0 && equal(current, self()) == 1 && equal(current, 0) == 0);
    });
    suite.Run("libc-arm64-adjacent-mappings", CheckAdjacentArm64Mappings);
    suite.Run("libc-private-fixed-file-overlays", [] {
        const auto map = Guest<void* (*)(void*, size_t, int, int, int, int64_t)>(Libc("mmap"), "(LJIIIJ)L");
        const auto protect = Guest<int (*)(void*, size_t, int)>(Libc("mprotect"), "(LJI)I");
        const auto unmap = Guest<int (*)(void*, size_t)>(Libc("munmap"), "(LJ)I");
        constexpr size_t page = 4096, size = 3 * page;
        const auto name = L"mmap-fixed-" + std::to_wstring(GetCurrentProcessId()) + L".bin";
        const auto file = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        STONE_CHECK(file != INVALID_HANDLE_VALUE);
        const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_RDWR | _O_BINARY);
        STONE_CHECK(fd >= 0);
        struct Close {
            int fd;
            ~Close()
            {
                _close(fd);
            }
        } close{fd};
        std::array<uint8_t, 2 * page> bytes;
        bytes.fill(0x7b);
        STONE_CHECK(_write(fd, bytes.data(), static_cast<unsigned>(bytes.size())) == bytes.size());
        auto* address = static_cast<uint8_t*>(map(nullptr, size, 3, 0x22, -1, 0));
        STONE_CHECK(address != reinterpret_cast<void*>(-1));
        const auto release = [unmap](uint8_t* value) {
            unmap(value, size);
        };
        const std::unique_ptr<uint8_t, decltype(release)> allocation(address, release);
        std::memset(address, 0x5a, size);
        STONE_CHECK(map(address + page, page, 1, 0x12, fd, 1) == reinterpret_cast<void*>(-1));
        STONE_CHECK(address[page] == 0x5a);
        STONE_CHECK(map(address + page, page, 1, 0x12, -1, page) == reinterpret_cast<void*>(-1));
        STONE_CHECK(address[page] == 0x5a);
        STONE_CHECK(map(address + page, page, 1, 0x12, fd, page) == address + page);
        for (size_t i = 0; i < size; ++i)
            STONE_CHECK(address[i] == (i >= page && i < 2 * page ? 0x7b : 0x5a));
        MEMORY_BASIC_INFORMATION protection{};
        STONE_CHECK(VirtualQuery(address + page, &protection, sizeof(protection)) &&
                    protection.Protect == PAGE_READONLY);
        STONE_CHECK(protect(address + page, page, 3) == 0);
        address[page] = 42;
        STONE_CHECK(_lseeki64(fd, page, SEEK_SET) == page && _read(fd, bytes.data(), 1) == 1 && bytes[0] == 0x7b);
        auto* reservation = static_cast<uint8_t*>(map(nullptr, size, 0, 0x22, -1, 0));
        STONE_CHECK(reservation != reinterpret_cast<void*>(-1));
        const std::unique_ptr<uint8_t, decltype(release)> reserved(reservation, release);
        STONE_CHECK(map(reservation + page, page, 1, 0x12, fd, 0) == reservation + page && reservation[page] == 0x7b);
    });
    suite.Run("libc-dontneed-zeroes-private-anonymous-pages", [] {
        const auto map = Guest<void* (*)(void*, size_t, int, int, int, int64_t)>(Libc("mmap"), "(LJIIIJ)L");
        const auto advise = Guest<int (*)(void*, size_t, int)>(Libc("madvise"), "(LJI)I");
        const auto unmap = Guest<int (*)(void*, size_t)>(Libc("munmap"), "(LJ)I");
        constexpr size_t page = 4096;
        constexpr size_t size = 3 * page;
        constexpr int privateAnonymous = 0x22, readWrite = 3, dontNeed = 4;
        auto* address = static_cast<uint8_t*>(map(nullptr, size, readWrite, privateAnonymous, -1, 0));
        STONE_CHECK(address != reinterpret_cast<void*>(-1));
        const auto release = [unmap](uint8_t* bytes) {
            unmap(bytes, size);
        };
        const std::unique_ptr<uint8_t, decltype(release)> memory(address, release);
        std::memset(address, 0x5a, size);
        STONE_CHECK(advise(address + page, page, dontNeed) == 0);
        for (size_t index = page; index < 2 * page; ++index)
            STONE_CHECK(address[index] == 0);
        STONE_CHECK(address[page - 1] == 0x5a && address[2 * page] == 0x5a);
        address[page] = 42;
        STONE_CHECK(address[page] == 42);
    });
    suite.Run("libc-dontneed-retains-mixed-protection-and-reserved-pages", [] {
        const auto map = Guest<void* (*)(void*, size_t, int, int, int, int64_t)>(Libc("mmap"), "(LJIIIJ)L");
        const auto protect = Guest<int (*)(void*, size_t, int)>(Libc("mprotect"), "(LJI)I");
        const auto advise = Guest<int (*)(void*, size_t, int)>(Libc("madvise"), "(LJI)I");
        const auto unmap = Guest<int (*)(void*, size_t)>(Libc("munmap"), "(LJ)I");
        constexpr size_t page = 4096, size = 4 * page;
        auto* address = static_cast<uint8_t*>(map(nullptr, size, 0, 0x22, -1, 0));
        STONE_CHECK(address != reinterpret_cast<void*>(-1));
        const auto release = [unmap](uint8_t* memory) {
            unmap(memory, size);
        };
        const std::unique_ptr<uint8_t, decltype(release)> memory(address, release);
        STONE_CHECK(protect(address, page, 3) == 0 && protect(address + 2 * page, 2 * page, 3) == 0);
        std::memset(address, 0x5a, page);
        std::memset(address + 2 * page, 0x5a, 2 * page);
        STONE_CHECK(protect(address + 2 * page, page, 1) == 0 && advise(address, 3 * page, 4) == 0);
        MEMORY_BASIC_INFORMATION middle{}, third{};
        STONE_CHECK(VirtualQuery(address + page, &middle, sizeof(middle)) && middle.State == MEM_RESERVE);
        STONE_CHECK(VirtualQuery(address + 2 * page, &third, sizeof(third)) && third.Protect == PAGE_READONLY);
        for (size_t index = 0; index < page; ++index)
            STONE_CHECK(address[index] == 0 && address[2 * page + index] == 0 && address[3 * page + index] == 0x5a);
        STONE_CHECK(protect(address + page, page, 3) == 0);
        for (size_t index = 0; index < page; ++index)
            STONE_CHECK(address[page + index] == 0);
    });
    suite.Run("libc-pthread-scheduling-attributes", CheckThreadSchedulingAttributes);
    TestSignalSets(suite);
    suite.Run("libc-large-file-position-64", CheckLargeFilePosition);
    suite.Run("libc-utf8-utf32-string-conversions", CheckMultibyteStrings);
    suite.Run("libc-file-sync-and-unopened-descriptor", [] {
        const auto path = std::filesystem::current_path() / "file-sync.bin";
        const int fd = _wopen(path.c_str(), _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
        STONE_CHECK(fd >= 0 && _write(fd, "durable save", 12) == 12);
        for (const char* name : {"fsync", "fdatasync"}) {
            const auto sync = Guest<int (*)(int)>(Libc(name), "(I)I");
            STONE_CHECK(sync(fd) == 0);
            errno = 0;
            STONE_CHECK(sync(-1) == -1 && errno == EBADF);
        }
        STONE_CHECK(_close(fd) == 0);
        std::ifstream saved(path, std::ios::binary);
        STONE_CHECK(std::string(std::istreambuf_iterator<char>(saved), {}) == "durable save");
    });
    suite.Run("current-app-private-native-library-paths", CheckPrivateNativeLibraries);
    suite.Run("libc-symbolic-link-paths-and-directory-fds", CheckSymbolicLinkPaths);
    suite.Run("libc-inotify-directory-events-and-descriptor-lifecycle", CheckInotifyDirectoryWatches);
    suite.Run("libc-socket-replacement-and-deferred-close", CheckSocketReplacement);
    suite.Run("libc-cached-network-interfaces", CheckInterfaceSnapshot);
    suite.Run("libc-winsock-option-mapping", CheckSocketOptions);
    suite.Run("libc-multicast-loopback-and-membership", CheckMulticastSockets);
    suite.Run("libc-network-resolver-ownership", CheckNetworkResolverOwnership);
    suite.Run("libdl-invalid-open-flags-and-one-shot-dlerror", [] {
        const auto open = Guest<void* (*)(const char*, int)>(Libdl("dlopen"), "(LI)L");
        const auto error = Guest<char* (*)()>(Libdl("dlerror"), "()L");
        STONE_CHECK(open("libfoo.so", 0x40000000) == nullptr);
        const auto message = error();
        STONE_CHECK(message && std::string_view(message).find("flags") != std::string_view::npos);
        STONE_CHECK(error() == nullptr);
    });
    suite.Run("libdl-android-private-namespaces", [] {
        const auto exported = Guest<void* (*)(const char*)>(LibdlAndroid("android_get_exported_namespace"), "(L)L");
        const auto link = Guest<bool (*)(void*, void*, const char*)>(LibdlAndroid("android_link_namespaces"), "(LLL)Z");
        const auto error = Guest<char* (*)()>(Libdl("dlerror"), "()L");
        STONE_CHECK(exported("vendor") == nullptr);
        STONE_CHECK(!link(nullptr, nullptr, "libc.so"));
        STONE_CHECK(error() != nullptr);
    });

    suite.Run("libstdcxx-paired-allocation", [] {
        const auto allocate = Guest<void* (*)(size_t)>(Libstdcxx("_Znwm"), "(J)L");
        const auto release = Guest<void (*)(void*)>(Libstdcxx("_ZdlPv"), "(L)V");
        const auto buffer = static_cast<char*>(allocate(8));
        STONE_CHECK(buffer);
        std::memcpy(buffer, "stone", 6);
        const bool matches = std::string_view(buffer) == "stone";
        release(buffer);
        STONE_CHECK(matches);
    });
    suite.Run("libandroidicu-unicode-case-folding", [] {
        const auto fold = Guest<int32_t (*)(int32_t, uint32_t)>(Libandroidicu("u_foldCase_android"), "(II)I");
        STONE_CHECK(fold('Z', 0) == 'z');
        STONE_CHECK(fold(0x03a3, 0) == 0x03c3);
    });
}

void TestLogAndMath(Suite& suite)
{
    suite.Run("libm-floating-point-abi", [] {
        const auto squareRoot = Guest<double (*)(double)>(Libm("sqrt"), "(D)D");
        STONE_CHECK(squareRoot(81.0) == 9.0);
        STONE_CHECK(squareRoot(0.0) == 0.0);
    });
    suite.Run("liblog-minimum-priority-round-trip", [] {
        const auto set = Guest<int (*)(int)>(AndroidLogImport("__android_log_set_minimum_priority"), "(I)I");
        const auto get = Guest<int (*)()>(AndroidLogImport("__android_log_get_minimum_priority"), "()I");
        const auto original = set(5);
        const auto actual = get();
        STONE_CHECK(set(original) == 5 && actual == 5);
    });
}

}
