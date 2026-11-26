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

void CheckArm64StackProtection()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1,
                            0xaa0303e2, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    const uint32_t read[]{0xa9bf7bfd, 0xd1400fff, 0xf9400000, 0x91400fff, 0xa8c17bfd, 0xd65f03c0};
    const uint32_t write[]{0xa9bf7bfd, 0xd1400fff, 0xf9000001, 0xf9400000, 0x91400fff, 0xa8c17bfd, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    std::memcpy(bytes.data() + 0x1280, read, sizeof(read));
    std::memcpy(bytes.data() + 0x1300, write, sizeof(write));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(sizeof(AndroidThreadAttr), true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    const Arm64ImportName imports = [](uintptr_t target) -> std::string_view {
        constexpr std::string_view names[]{"", "pthread_self", "pthread_getattr_np", "mprotect"};
        return target < std::size(names) ? names[target] : std::string_view{};
    };
    const auto invoke = [&](uintptr_t token, uint64_t first = 0, uint64_t second = 0, uint64_t third = 0) {
        const std::array<Arm64Argument, 4> args{{{'J', token}, {'J', first}, {'J', second}, {'J', third}}};
        return RunArm64Function(image, image.LoadBias() + 0x1200, 'J', imports, nullptr, nullptr, args);
    };
    STONE_CHECK(invoke(2, invoke(1), memory) == 0);
    const auto attr = *reinterpret_cast<const AndroidThreadAttr*>(memory);
    const auto top = reinterpret_cast<uintptr_t>(attr.stackBase) + attr.stackSize;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto pageSize = system.dwPageSize;
    STONE_CHECK(pageSize == 4096);
    const auto page = top - 2 * pageSize;

    struct Restore {
        std::function<void()> call;

        ~Restore()
        {
            try {
                call();
            }
            catch (...) {
            }
        }
    } restore{[&] {
        invoke(3, page, pageSize, 3);
    }};

    STONE_CHECK(invoke(3, page, pageSize, 0) == 0);
    const auto access = [&](bool write, uint64_t value = 0) {
        const std::array<Arm64Argument, 2> args{{{'L', page}, {'J', value}}};
        return RunArm64Function(image, image.LoadBias() + (write ? 0x1300 : 0x1280), 'J', imports, nullptr, nullptr,
                                args);
    };
    bool rejected = false;
    try {
        access(false);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    MEMORY_BASIC_INFORMATION region{};
    STONE_CHECK(VirtualQuery(reinterpret_cast<const void*>(page), &region, sizeof(region)));
    STONE_CHECK(region.State == MEM_COMMIT && region.Protect == PAGE_NOACCESS); // Deep commit preserved the guard.
    STONE_CHECK(invoke(3, page, pageSize, 3) == 0 && access(true, 42) == 42 && access(false) == 42);
    STONE_CHECK(invoke(3, page, pageSize, 1) == 0 && access(false) == 42);
    rejected = false;
    try {
        access(true, 75);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && access(false) == 42);
    STONE_CHECK(invoke(3, page, pageSize, 3) == 0 && access(true, 75) == 75);
    STONE_CHECK(invoke(3, page + 1, pageSize, 0) == UINT32_MAX);
    STONE_CHECK(invoke(3, top - pageSize, 2 * pageSize, 0) == UINT32_MAX);
    STONE_CHECK(invoke(3, page, pageSize, 4) == UINT32_MAX);
    uintptr_t host = 7;
    STONE_CHECK(invoke(3, reinterpret_cast<uintptr_t>(&host) & ~(uintptr_t{pageSize} - 1), pageSize, 0) == UINT32_MAX &&
                host == 7);
}

void CheckArm64SharedCommittedStack()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t save[]{0xd10043ff, 0x910003e0, 0x52800541, 0xb90003e1, 0x910043ff, 0xd65f03c0};
    const uint32_t read[]{0xb9400000, 0xd65f03c0};
    const uint32_t write[]{0xb9000001, 0xb9400000, 0xd65f03c0};
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1,
                            0xaa0303e2, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, save, sizeof(save));
    std::memcpy(bytes.data() + 0x1280, read, sizeof(read));
    std::memcpy(bytes.data() + 0x1300, write, sizeof(write));
    std::memcpy(bytes.data() + 0x1380, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const Arm64ImportName imports = [](uintptr_t token) -> std::string_view {
        return token == 1 ? "mprotect" : std::string_view{};
    };
    const auto invoke = [&](uintptr_t offset, std::span<const Arm64Argument> args = {}) {
        return RunArm64Function(image, image.LoadBias() + offset, 'J', imports, nullptr, nullptr, args);
    };
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr),
                 done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    STONE_CHECK(ready && done);
    uintptr_t address = 0;
    std::exception_ptr failure;
    std::thread owner([&] {
        try {
            address = invoke(0x1200);
            SetEvent(ready);
            WaitForSingleObject(done, INFINITE);
        }
        catch (...) {
            failure = std::current_exception();
            SetEvent(ready);
        }
    });

    struct Join {
        HANDLE ready, done;
        std::thread& owner;

        ~Join()
        {
            SetEvent(done);
            if (owner.joinable())
                owner.join();
            CloseHandle(ready);
            CloseHandle(done);
        }
    } join{ready, done, owner};

    STONE_CHECK(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
    if (failure)
        std::rethrow_exception(failure);
    const auto load = [&](uintptr_t pointer) {
        const std::array<Arm64Argument, 1> args{{{'L', pointer}}};
        return invoke(0x1280, args);
    };
    // Returning the guest function pops SP, not its committed stack mapping.
    STONE_CHECK(address && load(address) == 42);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto page = address & ~(uintptr_t{system.dwPageSize} - 1);
    const auto protect = [&](int access) {
        const std::array<Arm64Argument, 4> args{
            {{'J', 1}, {'L', page}, {'J', system.dwPageSize}, {'I', static_cast<uint64_t>(access)}}};
        return invoke(0x1380, args);
    };
    const auto denied = [&](uintptr_t pointer) {
        bool rejected = false;
        try {
            load(pointer);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
    };
    STONE_CHECK(protect(0) == 0);
    denied(address);
    STONE_CHECK(protect(1) == 0 && load(address) == 42);
    const std::array<Arm64Argument, 2> args{{{'L', address}, {'I', 75}}};
    bool rejected = false;
    try {
        invoke(0x1300, args);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && load(address) == 42);
    STONE_CHECK(protect(3) == 0 && invoke(0x1300, args) == 75);
    MEMORY_BASIC_INFORMATION committed{}, reserved{};
    STONE_CHECK(VirtualQuery(reinterpret_cast<void*>(address), &committed, sizeof(committed)));
    const auto base = reinterpret_cast<uintptr_t>(committed.AllocationBase);
    STONE_CHECK(VirtualQuery(reinterpret_cast<void*>(base), &reserved, sizeof(reserved)) &&
                reserved.State == MEM_RESERVE);
    denied(base); // Ownership of reserved address space does not commit it for a peer.
    uintptr_t host = 91;
    denied(reinterpret_cast<uintptr_t>(&host));
    STONE_CHECK(host == 91);
    SetEvent(done);
    owner.join();
    denied(address); // The exited thread no longer publishes a stack.
}

void CheckArm64StackProbesBody()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1,
                            0xaa0303e2, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    const uint32_t read[]{0xd14043f0, 0xf9400200, 0xd65f03c0}; // Probe SP-64KiB without changing SP.
    const uint32_t writeCode[]{0xd14043f0, 0xf9000200, 0xf9400200, 0xd65f03c0};
    const uint32_t deeper[]{0xd14047ff, 0x914047ff, 0xd65f03c0}; // Commit below the protected page.
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    std::memcpy(bytes.data() + 0x1280, read, sizeof(read));
    std::memcpy(bytes.data() + 0x1300, writeCode, sizeof(writeCode));
    std::memcpy(bytes.data() + 0x1380, deeper, sizeof(deeper));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(sizeof(AndroidThreadAttr), true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    const Arm64ImportName imports = [](uintptr_t target) -> std::string_view {
        constexpr std::string_view names[]{"", "pthread_self", "pthread_getattr_np", "mprotect"};
        return target < std::size(names) ? names[target] : std::string_view{};
    };
    const auto invoke = [&](uintptr_t token, uint64_t first = 0, uint64_t second = 0, uint64_t third = 0) {
        const std::array<Arm64Argument, 4> args{{{'J', token}, {'J', first}, {'J', second}, {'J', third}}};
        return RunArm64Function(image, image.LoadBias() + 0x1200, 'J', imports, nullptr, nullptr, args);
    };
    STONE_CHECK(invoke(2, invoke(1), memory) == 0);
    const auto attr = *reinterpret_cast<const AndroidThreadAttr*>(memory);
    const auto page = reinterpret_cast<uintptr_t>(attr.stackBase) + attr.stackSize - 65536;
    MEMORY_BASIC_INFORMATION region{};
    STONE_CHECK(VirtualQuery(reinterpret_cast<const void*>(page), &region, sizeof(region)) &&
                region.State == MEM_RESERVE);
    const auto probe = [&](bool write, uint64_t value = 0) {
        const std::array<Arm64Argument, 1> args{{{'J', value}}};
        return RunArm64Function(image, image.LoadBias() + (write ? 0x1300 : 0x1280), 'J', imports, nullptr, nullptr,
                                args);
    };
    STONE_CHECK(probe(false) == 0);
    STONE_CHECK(VirtualQuery(reinterpret_cast<const void*>(page), &region, sizeof(region)) &&
                region.State == MEM_COMMIT && region.Protect == PAGE_READWRITE);
    constexpr uint64_t value = 0x0102030405060708;
    STONE_CHECK(probe(true, value) == value && probe(false) == value);

    struct Restore {
        std::function<void()> call;

        ~Restore()
        {
            try {
                call();
            }
            catch (...) {
            }
        }
    } restore{[&] {
        invoke(3, page, 4096, 3);
    }};

    STONE_CHECK(invoke(3, page, 4096, 1) == 0);
    bool rejected = false;
    try {
        probe(true, 7);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && probe(false) == value);
    STONE_CHECK(invoke(3, page, 4096, 0) == 0);
    RunArm64Function(image, image.LoadBias() + 0x1380, 'V', imports, nullptr, nullptr, {});
    for (const bool write : {false, true}) {
        rejected = false;
        try {
            probe(write, 7);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
    }
    STONE_CHECK(VirtualQuery(reinterpret_cast<const void*>(page), &region, sizeof(region)) &&
                region.Protect == PAGE_NOACCESS);
    STONE_CHECK(invoke(3, page, 4096, 3) == 0 && probe(false) == value);
}

void CheckArm64StackProbes()
{
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            CheckArm64StackProbesBody();
        }
        catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();
    if (failure)
        std::rethrow_exception(failure);
}

void CheckArm64MappedCode()
{
    auto bytes = MakeArm64TlsElf();
    // Call x0 with x1..x6 as its arguments; the callee may revoke our mapping.
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1, 0xaa0303e2, 0xaa0403e3,
                            0xaa0503e4, 0xaa0603e5, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    const uint32_t syscall[]{0xaa0203e8, 0xd4000001, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    std::memcpy(bytes.data() + 0x1280, syscall, sizeof(syscall));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const Arm64ImportName imports = [](uintptr_t target) -> std::string_view {
        constexpr std::string_view names[]{"", "memfd_create", "ftruncate", "close", "mprotect", "android_mallopt"};
        return target < std::size(names) ? names[target] : std::string_view{};
    };
    const auto invoke = [&](uintptr_t entry, std::initializer_list<uint64_t> values) {
        std::vector<Arm64Argument> args;
        for (const auto value : values)
            args.push_back({'J', value});
        return RunArm64Function(image, entry, 'J', imports, nullptr, nullptr, args);
    };
    const auto call = [&](uintptr_t token, uint64_t a = 0, uint64_t b = 0) {
        return invoke(image.LoadBias() + 0x1200, {token, a, b});
    };
    const auto rejected = [&](uintptr_t code) {
        try {
            invoke(code, {});
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };

    struct MappingOwner {
        void* address;
        size_t size;

        ~MappingOwner()
        {
            if (address != reinterpret_cast<void*>(-1))
                AndroidUnmapArm64(address, size);
        }
    } anonymous{AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0), 4096};

    STONE_CHECK(anonymous.address != reinterpret_cast<void*>(-1));
    {
        MappingOwner split{AndroidMapArm64(nullptr, 12288, 3, 0x22, -1, 0), 12288};
        STONE_CHECK(split.address != reinterpret_cast<void*>(-1));
        const auto base = reinterpret_cast<uintptr_t>(split.address);
        STONE_CHECK(AndroidArm64CodeAt(base).empty());
        STONE_CHECK(AndroidProtectArm64(split.address, 12288, 5) == 0);
        for (unsigned i = 0; i < 16; ++i)
            STONE_CHECK(AndroidArm64CodeAt(base + 4096 + 32).size() == 12288 - 4096 - 32);
        STONE_CHECK(AndroidArm64CodeAt(base + 1).empty() && AndroidArm64CodeAt(base + 12284).size() == 4);
        STONE_CHECK(AndroidProtectArm64(reinterpret_cast<void*>(base + 4096), 4096, 1) == 0);
        STONE_CHECK(AndroidArm64CodeAt(base).size() == 4096 && AndroidArm64CodeAt(base + 4096).empty() &&
                    AndroidArm64CodeAt(base + 8192).size() == 4096);
        STONE_CHECK(AndroidProtectArm64(reinterpret_cast<void*>(base + 4096), 4096, 0) == 0 &&
                    AndroidArm64CodeAt(base + 4096).empty());
        STONE_CHECK(AndroidProtectArm64(reinterpret_cast<void*>(base + 4096), 4096, 5) == 0 &&
                    !AndroidArm64CodeAt(base + 4096).empty());
        STONE_CHECK(AndroidUnmapArm64(split.address, 12288) == 0 && AndroidArm64CodeAt(base).empty());
        split.address = AndroidMapArm64(reinterpret_cast<void*>(base), 12288, 3, 0x22, -1, 0);
        STONE_CHECK(split.address == reinterpret_cast<void*>(base) && AndroidArm64CodeAt(base).empty());
    }
    const auto address = reinterpret_cast<uintptr_t>(anonymous.address);
    const uint32_t code[]{0x52800540, 0xd65f03c0}; // mov w0,#42; ret
    std::memcpy(anonymous.address, code, sizeof(code));
    STONE_CHECK(rejected(address));
    STONE_CHECK(AndroidProtectArm64(anonymous.address, 4096, 5) == 0 && invoke(address, {}) == 42);
    MEMORY_BASIC_INFORMATION region{};
    STONE_CHECK(VirtualQuery(anonymous.address, &region, sizeof(region)) && region.Protect == PAGE_READONLY);
    STONE_CHECK(!AndroidMappedArm64Data(address, 4, true));
    STONE_CHECK(AndroidProtectArm64(anonymous.address, 4096, 1) == 0 && rejected(address));
    STONE_CHECK(AndroidProtectArm64(anonymous.address, 4096, 7) == 0 && invoke(address, {}) == 42);
    STONE_CHECK(VirtualQuery(anonymous.address, &region, sizeof(region)) && region.Protect == PAGE_READWRITE);
    // Revoke execution while this interpreter has a cached executable range.
    std::memcpy(anonymous.address, caller, sizeof(caller));
    bool revoked = false;
    try {
        invoke(address, {4, address, 4096, 1});
    }
    catch (const std::runtime_error&) {
        revoked = true;
    }
    STONE_CHECK(revoked && AndroidArm64CodeAt(address).empty());
    STONE_CHECK(AndroidProtectArm64(anonymous.address, 4096, 0x15) == -1 && errno == EINVAL);
    STONE_CHECK(AndroidArm64CodeAt(reinterpret_cast<uintptr_t>(&CheckArm64MappedCode)).empty());
    STONE_CHECK(AndroidProtectArm64(anonymous.address, 4096, 7) == 0);
    std::memcpy(anonymous.address, code, sizeof(code));
    STONE_CHECK(AndroidProtectArm64(anonymous.address, 4096, 5) == 0);
    MappingOwner changing{AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0), 4096};
    STONE_CHECK(changing.address != reinterpret_cast<void*>(-1));
    std::atomic<unsigned> publications{0};
    std::atomic<bool> protectionFailed{false};
    std::jthread publisher([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (AndroidProtectArm64(changing.address, 4096, 5) || AndroidProtectArm64(changing.address, 4096, 3))
                protectionFailed = true;
            ++publications;
        }
    });
    while (!publications.load())
        std::this_thread::yield();
    for (unsigned i = 0; i < 512; ++i)
        STONE_CHECK(invoke(address, {}) == 42);
    publisher.request_stop();
    publisher.join();
    STONE_CHECK(!protectionFailed && publications >= 1);
    STONE_CHECK(AndroidProtectArm64(anonymous.address, 4096, 1) == 0 && rejected(address));
    const auto name = AllocateArm64GuestMemory(256, true);
    STONE_CHECK(name);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{name};

    strcpy_s(reinterpret_cast<char*>(name), 256, "stone-arm64-jit-test");

    struct FdOwner {
        int fd;

        ~FdOwner()
        {
            if (fd >= 0)
                reinterpret_cast<int (*)(int)>(AndroidLibcImport("close", true))(fd);
        }
    };

    FdOwner file{static_cast<int>(invoke(image.LoadBias() + 0x1280, {name, 0, 279}))};
    STONE_CHECK(file.fd >= 0 && call(2, file.fd, 131072) == 0);
    MappingOwner read{AndroidMapArm64(reinterpret_cast<void*>(0x58001000), 131072, 1, 1, file.fd, 0), 131072};
    STONE_CHECK(read.address != reinterpret_cast<void*>(-1));
    STONE_CHECK(reinterpret_cast<uintptr_t>(read.address) < 0x100000000ULL);
    const auto tail = reinterpret_cast<uintptr_t>(read.address) + 65536;
    STONE_CHECK(AndroidMapArm64(reinterpret_cast<void*>(tail), 65536, 5, 0x11, file.fd, 65536) ==
                reinterpret_cast<void*>(tail));
    MappingOwner write{AndroidMapArm64(nullptr, 65536, 3, 1, file.fd, 65536), 65536};
    STONE_CHECK(write.address != reinterpret_cast<void*>(-1));
    std::memcpy(write.address, code, sizeof(code));
    STONE_CHECK(invoke(tail, {}) == 42 && rejected(reinterpret_cast<uintptr_t>(write.address)));
    *static_cast<uint32_t*>(write.address) = 0x52800960; // mov w0,#75
    STONE_CHECK(invoke(tail, {}) == 75);
    STONE_CHECK(VirtualQuery(reinterpret_cast<void*>(tail), &region, sizeof(region)) &&
                region.Protect == PAGE_READONLY);
    STONE_CHECK(AndroidMapArm64(reinterpret_cast<void*>(tail), 4096, 5, 0x11, file.fd, 0) ==
                reinterpret_cast<void*>(-1));
    STONE_CHECK(AndroidMapArm64(reinterpret_cast<void*>(tail + 1), 4096, 5, 0x11, file.fd, 65536) ==
                reinterpret_cast<void*>(-1));
    STONE_CHECK(AndroidAdviseArm64(write.address, write.size, 10) == 0);
    STONE_CHECK(AndroidAdviseArm64(&region, 4096, 10) == -1);
    STONE_CHECK(AndroidUnmapArm64(read.address, 65536) == 0);
    STONE_CHECK(rejected(reinterpret_cast<uintptr_t>(read.address)) && invoke(tail, {}) == 75);
    read.address = reinterpret_cast<void*>(tail);
    read.size = 65536;
    STONE_CHECK(AndroidUnmapArm64(read.address, read.size) == 0 && rejected(tail));
    read.address = reinterpret_cast<void*>(-1);
    FdOwner noexec{static_cast<int>(call(1, name, 8))};
    STONE_CHECK(noexec.fd >= 0 && call(2, noexec.fd, 4096) == 0);
    STONE_CHECK(AndroidMapArm64(write.address, 4096, 1, 0x11, noexec.fd, 65536) == reinterpret_cast<void*>(-1));
    STONE_CHECK(*static_cast<uint32_t*>(write.address) == 0x52800960);
    STONE_CHECK(AndroidMapArm64(nullptr, 4096, 5, 1, noexec.fd, 0) == reinterpret_cast<void*>(-1) && errno == EACCES);
    STONE_CHECK(call(5) == 0 && errno == 95);
    STONE_CHECK(static_cast<int64_t>(invoke(image.LoadBias() + 0x1280, {0, 0, 279})) == -EFAULT);
    std::memset(reinterpret_cast<void*>(name), 'a', 250);
    STONE_CHECK(static_cast<int64_t>(invoke(image.LoadBias() + 0x1280, {name, 0, 279})) == -EINVAL);
    STONE_CHECK(call(1, name) == UINT32_MAX && errno == EINVAL);
}

void CheckArm64CodePublication()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t ctr[]{0xd53b0029, 0xaa0903e0, 0xd65f03c0}; // mrs x9,ctr_el0; mov x0,x9
    const uint32_t publish[]{0xd50b7b20, 0xd5033b9f, 0xd50b7520, 0xd5033b9f, 0xd5033fdf, 0xd65f03c0};
    const uint32_t getparam[]{0xa9bf7bfd, 0xd2800030, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, ctr, sizeof(ctr));
    std::memcpy(bytes.data() + 0x1280, publish, sizeof(publish));
    std::memcpy(bytes.data() + 0x1300, getparam, sizeof(getparam));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto invoke = [&](size_t offset, std::initializer_list<uint64_t> values) {
        std::vector<Arm64Argument> args;
        for (const auto value : values)
            args.push_back({'J', value});
        return RunArm64Function(
            image, image.LoadBias() + offset, 'J',
            [](uintptr_t target) -> std::string_view {
                return target == 1 ? "sched_getparam" : "";
            },
            nullptr, nullptr, args);
    };
    const auto value = invoke(0x1200, {});
    STONE_CHECK(value == 0xb004c004 && (4u << (value & 15)) == 64 && (4u << ((value >> 16) & 15)) == 64);
    const auto memory = AllocateArm64GuestMemory(16, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    invoke(0x1280, {memory});
    bool rejected = false;
    int host = 7;
    try {
        invoke(0x1280, {reinterpret_cast<uintptr_t>(&host)});
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && host == 7);
    *reinterpret_cast<int*>(memory) = 7;
    STONE_CHECK(invoke(0x1300, {0, memory}) == 0 && *reinterpret_cast<int*>(memory) == 0);
    STONE_CHECK(invoke(0x1300, {uint64_t(-1), memory}) == UINT32_MAX && errno == EINVAL);
    STONE_CHECK(invoke(0x1300, {0, 0}) == UINT32_MAX && errno == EINVAL);
    rejected = false;
    try {
        invoke(0x1300, {0, reinterpret_cast<uintptr_t>(&host)});
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && host == 7);
}

void CheckArm64MemoryFaultRecovery()
{
    auto bytes = MakeArm64TlsElf();
    // Record siginfo.code, si_addr, fault_address and PC, then skip the load,
    // return 42 in the saved x0 and restore a changed SIGUSR1 mask.
    const uint32_t handler[]{0xf9406044, 0xb9400825, 0xf9000085, 0xf9400825, 0xf9000485, 0xf9405845, 0xf9000885,
                             0xf940dc45, 0xf9000c85, 0x910010a5, 0xf900dc45, 0x52800545, 0xf9005c45, 0x52806005,
                             0xf9001445, 0xf9401085, 0xf900f045, 0xf9401485, 0xf900e045, 0xd65f03c0};
    const uint32_t badPc[]{0xf900dc5f, 0xd65f03c0};
    const uint32_t badSp[]{0xf900d85f, 0xd65f03c0};
    const uint32_t nested[]{0xd2800000, 0xf9400000, 0xd65f03c0};
    const uint32_t import[]{0xd2800030, 0xd61f0200};
    const uint32_t altStack[]{0xd2800050, 0xd61f0200};
    const uint32_t adjustSp[]{0xf940d845, 0xd10040a5, 0xf900d845, 0xf940dc45, 0x910010a5,
                              0xf900dc45, 0x52800545, 0xf9005c45, 0xd65f03c0};
    auto pointerSp = std::to_array(adjustSp);
    pointerSp[1] = 0xd10020a5; // sub x5,x5,#8: ART's single return-PC push.
    auto unalignedSp = std::to_array(adjustSp);
    unalignedSp[1] = 0xd10010a5; // sub x5,x5,#4: still reject.
    // Two hot ELF blocks pass NZCV between them. The last iteration selects a
    // null load; recovery must keep all 4096 increments and the adjusted SP.
    const uint32_t chainEntry[]{0xd2820003, 0xd2800004, 0xd10043ff, 0xd503201f, 0x14000004};
    const uint32_t chainFirst[]{0xf1000463, 0x9a9f1005, 0xd503201f, 0x14000005};
    const uint32_t chainSecond[]{0xf94000a6, 0x91000484, 0xd503201f, 0x54fffea1, 0x8b040000, 0x910043ff, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, handler, sizeof(handler));
    std::memcpy(bytes.data() + 0x1300, badPc, sizeof(badPc));
    std::memcpy(bytes.data() + 0x1380, badSp, sizeof(badSp));
    std::memcpy(bytes.data() + 0x1400, nested, sizeof(nested));
    std::memcpy(bytes.data() + 0x1480, import, sizeof(import));
    std::memcpy(bytes.data() + 0x1500, adjustSp, sizeof(adjustSp));
    std::memcpy(bytes.data() + 0x1580, altStack, sizeof(altStack));
    std::memcpy(bytes.data() + 0x1600, pointerSp.data(), sizeof(pointerSp));
    std::memcpy(bytes.data() + 0x1680, unalignedSp.data(), sizeof(unalignedSp));
    std::memcpy(bytes.data() + 0x1700, chainEntry, sizeof(chainEntry));
    std::memcpy(bytes.data() + 0x1720, chainFirst, sizeof(chainFirst));
    std::memcpy(bytes.data() + 0x1740, chainSecond, sizeof(chainSecond));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(64, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    auto* record = reinterpret_cast<uint64_t*>(memory);
    record[4] = std::bit_cast<uint64_t>(42.5);
    record[5] = uint64_t{1} << 30; // Z flag.
    const auto code = AndroidMapArm64(nullptr, 8192, 7, 0x22, -1, 0);
    STONE_CHECK(code != reinterpret_cast<void*>(-1));

    struct Unmap {
        void* address;

        ~Unmap()
        {
            AndroidUnmapArm64(address, 8192);
        }
    } unmap{code};

    const uint32_t load[]{0xf9400000, 0xd65f03c0};
    const uint32_t floating[]{0xf9400000, 0x9e660000, 0xd65f03c0};     // fmov x0,d0
    const uint32_t flags[]{0xf9400000, 0x1a9f17e0, 0xd65f03c0};        // cset w0,eq
    const uint32_t stack[]{0xf9400000, 0x910043ff, 0xd65f03c0};        // add sp,sp,#16
    const uint32_t pointerStack[]{0xf9400000, 0x910023ff, 0xd65f03c0}; // add sp,sp,#8
    const uint32_t blockFault[]{0xeb1f03ff, 0xd503201f, 0xf9400000,
                                0xd65f03c0}; // CMP before a fault in the same native block.
    const uint32_t vectorFault[]{0x4f00e420, 0x4e208401, 0xd503201f, 0xf9400000, 0x3d800c21, 0x9e660020, 0xd65f03c0};
    std::memcpy(code, load, sizeof(load));
    std::memcpy(static_cast<uint8_t*>(code) + 32, floating, sizeof(floating));
    std::memcpy(static_cast<uint8_t*>(code) + 64, flags, sizeof(flags));
    std::memcpy(static_cast<uint8_t*>(code) + 96, stack, sizeof(stack));
    std::memcpy(static_cast<uint8_t*>(code) + 128, pointerStack, sizeof(pointerStack));
    std::memcpy(static_cast<uint8_t*>(code) + 192, blockFault, sizeof(blockFault));
    std::memcpy(static_cast<uint8_t*>(code) + 256, vectorFault, sizeof(vectorFault));
    const auto entry = reinterpret_cast<uintptr_t>(code);
    AndroidSignalAction previous{}, action{4, image.LoadBias() + 0x1200, uint64_t{1} << 12, 0};
    STONE_CHECK(AndroidArm64Sigaction(11, &action, &previous) == 0);

    struct Restore {
        AndroidSignalAction previous;
        uint64_t mask;

        ~Restore()
        {
            AndroidArm64Sigaction(11, &previous, nullptr);
            ExchangeAndroidSignalMask(mask);
        }
    } restore{previous, ExchangeAndroidSignalMask(0)};

    const auto invoke = [&](uintptr_t pc, uintptr_t address) {
        const std::array<Arm64Argument, 2> args{{{'L', address}, {'L', memory}}};
        return RunArm64Function(
            image, pc, 'J',
            [](uintptr_t target) -> std::string_view {
                return target == 1 ? "strlen" : target == 2 ? "sigaltstack" : "";
            },
            nullptr, nullptr, args);
    };
    for (unsigned i = 0; i < 2; ++i) {
        std::memset(record, 0, 32);
        STONE_CHECK(invoke(entry, 0) == 42);
        STONE_CHECK(record[0] == 1 && !record[1] && !record[2] && record[3] == entry);
        STONE_CHECK(ExchangeAndroidSignalMask(0) == (uint64_t{1} << 9));
    }
    const auto nativeFaults = GetArm64BlockStatistics().instructions;
    for (unsigned i = 0; i < 128; ++i) {
        std::memset(record, 0, 32);
        STONE_CHECK(invoke(entry + 192, 0) == 42 && record[0] == 1 && record[3] == entry + 200);
        ExchangeAndroidSignalMask(0);
    }
    if (Arm64BlockJitEnabled())
        STONE_CHECK(GetArm64BlockStatistics().instructions > nativeFaults);
    const auto vectorInstructions = GetArm64BlockStatistics().instructions;
    for (unsigned i = 0; i < 128; ++i) {
        std::memset(record, 0, 32);
        record[6] = record[7] = 0;
        STONE_CHECK(invoke(entry + 256, 0) == 0x0202020202020202ull && record[3] == entry + 268 &&
                    record[6] == 0x0202020202020202ull && record[7] == 0x0202020202020202ull);
        ExchangeAndroidSignalMask(0);
    }
    if (Arm64BlockJitEnabled())
        STONE_CHECK(GetArm64BlockStatistics().instructions > vectorInstructions);
    std::memset(record, 0, 32);
    const auto chainBefore = GetArm64BlockStatistics();
    STONE_CHECK(invoke(image.LoadBias() + 0x1700, memory) == 4138);
    STONE_CHECK(record[0] == 1 && record[1] == 0 && record[2] == 0 && record[3] == image.LoadBias() + 0x1740);
    ExchangeAndroidSignalMask(0);
    const auto chainAfter = GetArm64BlockStatistics();
    if (Arm64BlockJitEnabled()) {
        STONE_CHECK(chainAfter.executions - chainBefore.executions > 1000);
        wchar_t control[2]{};
        const bool chain = GetEnvironmentVariableW(L"STONE_ARM64_BLOCK_CHAIN", control, 2) == 1 && control[0] == L'1';
        if (chain)
            STONE_CHECK(chainAfter.dispatches - chainBefore.dispatches <
                        (chainAfter.executions - chainBefore.executions) / 2);
    }
    STONE_CHECK(invoke(entry + 32, 0) == std::bit_cast<uint64_t>(42.5));
    STONE_CHECK(invoke(entry + 64, 0) == 1);
    ExchangeAndroidSignalMask(0);
    STONE_CHECK(AndroidProtectArm64(static_cast<uint8_t*>(code) + 4096, 4096, 0) == 0);
    const auto guard = entry + 4096;
    STONE_CHECK(invoke(entry, guard) == 42 && record[0] == 2 && record[1] == guard && record[2] == guard);
    ExchangeAndroidSignalMask(uint64_t{1} << 10); // Block SIGSEGV; reject rather than deliver.
    bool rejected = false;
    try {
        invoke(entry, 0);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    ExchangeAndroidSignalMask(0);
    // A bad host-import argument must never be converted into a hardware signal.
    record[0] = 77;
    rejected = false;
    try {
        invoke(image.LoadBias() + 0x1480, 0);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && record[0] == 77);
    for (const auto offset : {0x1300, 0x1380, 0x1400}) {
        action.handler = image.LoadBias() + offset;
        STONE_CHECK(AndroidArm64Sigaction(11, &action, nullptr) == 0);
        rejected = false;
        try {
            invoke(entry, 0);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected && ExchangeAndroidSignalMask(0) == 0);
    }
    action.handler = image.LoadBias() + 0x1500;
    STONE_CHECK(AndroidArm64Sigaction(11, &action, nullptr) == 0);
    STONE_CHECK(invoke(entry + 96, 0) == 42); // Saved SP is applied after signal-frame destruction.
    action.handler = image.LoadBias() + 0x1600;
    STONE_CHECK(AndroidArm64Sigaction(11, &action, nullptr) == 0);
    STONE_CHECK(invoke(entry + 128, 0) == 42);
    action.handler = image.LoadBias() + 0x1680;
    STONE_CHECK(AndroidArm64Sigaction(11, &action, nullptr) == 0);
    rejected = false;
    try {
        invoke(entry + 128, 0);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && ExchangeAndroidSignalMask(0) == 0);
    std::memset(record, 0xa5, 32);
    STONE_CHECK(invoke(image.LoadBias() + 0x1580, 0) == 0);
    STONE_CHECK(record[0] == 0 && record[1] == 2 && record[2] == 0 && record[3] == 0xa5a5a5a5a5a5a5a5);
    STONE_CHECK(invoke(image.LoadBias() + 0x1580, memory) == UINT32_MAX && errno == 38);
}

void CheckArm64CodeReadProtection()
{
    for (const bool executeOnly : {false, true}) {
        auto bytes = MakeArm64TlsElf();
        bytes.resize(0x3000);
        auto* segment =
            reinterpret_cast<elf::Elf64Phdr*>(bytes.data() + sizeof(elf::Elf64Ehdr) + 3 * sizeof(elf::Elf64Phdr));
        segment->p_flags = executeOnly ? 1 : 5;
        segment->p_filesz = segment->p_memsz = 0x2000;
        const uint32_t caller[]{0xd2800030, 0xd61f0200}; // mov x16,#1; br x16 (no code-data read).
        const uint32_t read[]{0xb9400000, 0xd65f03c0}, answer[]{0x52800540, 0xd65f03c0};
        std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
        std::memcpy(bytes.data() + 0x1280, read, sizeof(read));
        std::memcpy(bytes.data() + 0x2200, answer, sizeof(answer));
        const uint32_t value = 42;
        std::memcpy(bytes.data() + 0x2100, &value, 4);
        ElfImage image(bytes, ElfImage::Machine::Arm64, true);
        image.Protect(ElfImage::Execution::Translated);
        const auto base = image.LoadBias(), code = base + 0x1000;
        const auto run = [&](uintptr_t entry, std::initializer_list<uint64_t> values) {
            std::vector<Arm64Argument> args;
            for (const auto value : values)
                args.push_back({'J', value});
            return RunArm64Function(
                image, entry, 'I',
                [](uintptr_t) -> std::string_view {
                    return "mprotect";
                },
                nullptr, nullptr, args);
        };
        STONE_CHECK(bool(image.DataAt(code, 4, false)) != executeOnly && !image.DataAt(code, 4, true));
        STONE_CHECK(run(base + 0x2200, {}) == 42);
        STONE_CHECK(run(base + 0x1200, {code, 5000, 5}) == 0);
        STONE_CHECK(image.DataAt(code + 4094, 4, false) && run(base + 0x1280, {base + 0x2100}) == 42);
        MEMORY_BASIC_INFORMATION region{};
        STONE_CHECK(VirtualQuery(reinterpret_cast<void*>(code), &region, sizeof(region)) &&
                    region.Protect == PAGE_READONLY);
        STONE_CHECK(run(base + 0x1200, {code + 4096, 1, 4}) == 0 && image.DataAt(code, 4, false) &&
                    !image.DataAt(code + 4094, 4, false) && !image.DataAt(base + 0x2100, 4, false));
        bool rejected = false;
        try {
            run(base + 0x1280, {base + 0x2100});
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected && run(base + 0x2200, {}) == 42);
        STONE_CHECK(run(base + 0x1200, {code, 5000, 4}) == 0 && !image.DataAt(code, 4, false));
        STONE_CHECK(run(base + 0x2200, {}) == 42 && run(base + 0x1200, {code, 8192, 5}) == 0);
        STONE_CHECK(run(base + 0x1280, {base + 0x2100}) == 42 && !image.DataAt(code, 4, true));
        for (const auto access : {3, 7})
            STONE_CHECK(run(base + 0x1200, {code, 4096, static_cast<uint64_t>(access)}) == UINT32_MAX);
        STONE_CHECK(!image.ProtectArm64Code(base, 4096, true) && !image.ProtectArm64Code(code + 1, 4096, true) &&
                    !image.ProtectArm64Code(code, 8193, true) && !image.ProtectArm64Code(code, 0, true) &&
                    !image.ProtectArm64Code(reinterpret_cast<uintptr_t>(bytes.data()), 4096, true));
    }
    ElfImage x64(MakeElf(false));
    STONE_CHECK(!x64.ProtectArm64Code(x64.LoadBias(), 4096, true));
}

void CheckArm64AdvisoryMappingHint()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xd2800030, 0xd61f0200};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto invoke = [&](uintptr_t hint, size_t length, int access, int flags, int fd) {
        const Arm64Argument args[]{{'J', hint},         {'J', length}, {'I', uint32_t(access)}, {'I', uint32_t(flags)},
                                   {'I', uint32_t(fd)}, {'J', 0}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [](uintptr_t target) -> std::string_view {
                return target == 1 ? "mmap" : "";
            },
            nullptr, nullptr, args);
    };
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto page = system.dwPageSize, granularity = system.dwAllocationGranularity;
    STONE_CHECK(page < granularity && granularity < UINT32_MAX / 8);
    const size_t span = 8 * granularity, length = 2 * granularity;
    const auto warm = invoke(0, page, 0, 0x22, -1);
    STONE_CHECK(warm != UINT64_MAX);
    STONE_CHECK(AndroidUnmapArm64(reinterpret_cast<void*>(warm), page) == 0);
    const auto fixture =
        std::filesystem::current_path() / (L"arm64-hint-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    const auto file = CreateFileW(fixture.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    STONE_CHECK(file != INVALID_HANDLE_VALUE);
    constexpr uint64_t marker = 0xfedcba9876543210ull;
    DWORD written = 0;
    const bool wrote = WriteFile(file, &marker, sizeof(marker), &written, nullptr);
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_RDWR | _O_BINARY);
    if (fd < 0)
        CloseHandle(file);
    STONE_CHECK(fd >= 0);

    struct Close {
        int value;

        ~Close()
        {
            CloseAndroidFd(value);
        }
    } close{fd};

    STONE_CHECK(wrote && written == sizeof(marker));
    for (unsigned test = 0; test < 6; ++test) {
        const bool fragmented = test >= 3;
        const unsigned kind = test % 3;
        void* arena = nullptr;
        for (uintptr_t candidate = 0x10000000; candidate < 0xd0000000 && !arena; candidate += 0x02000000)
            arena = VirtualAlloc(reinterpret_cast<void*>(candidate), span, MEM_RESERVE, PAGE_NOACCESS);
        STONE_CHECK(arena);

        struct Host {
            void* p;

            ~Host()
            {
                if (p)
                    VirtualFree(p, 0, MEM_RELEASE);
            }
        } host{arena};

        const auto base = reinterpret_cast<uintptr_t>(arena);
        STONE_CHECK(VirtualFree(arena, 0, MEM_RELEASE));
        host.p = nullptr;
        host.p = VirtualAlloc(arena, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        STONE_CHECK(host.p == arena);
        std::memset(host.p, 0xa5, page);
        Host neighbor{nullptr};
        if (fragmented) {
            neighbor.p = VirtualAlloc(reinterpret_cast<void*>(base + granularity), page, MEM_RESERVE | MEM_COMMIT,
                                      PAGE_READWRITE);
            STONE_CHECK(neighbor.p == reinterpret_cast<void*>(base + granularity));
            std::memset(neighbor.p, 0x5a, page);
        }
        MEMORY_BASIC_INFORMATION available{};
        const auto firstFree = base + (fragmented ? 2 : 1) * granularity;
        STONE_CHECK(VirtualQuery(reinterpret_cast<void*>(firstFree), &available, sizeof(available)) &&
                    available.State == MEM_FREE && available.RegionSize >= length);
        const int access = kind == 0 ? 0 : kind == 1 ? 3 : 1;
        const auto result = invoke(base + page, length, access, kind == 2 ? 2 : 0x22, kind == 2 ? fd : -1);
        STONE_CHECK(result != UINT64_MAX);

        struct Guest {
            uintptr_t p;
            size_t size;

            ~Guest()
            {
                AndroidUnmapArm64(reinterpret_cast<void*>(p), size);
            }
        } guest{result, length};

        if (result < firstFree || result > base + span - length)
            throw std::runtime_error("ARM64 advisory mmap abandoned a free low-address hint region");
        for (unsigned i = 0; i < page; ++i)
            STONE_CHECK(static_cast<const uint8_t*>(host.p)[i] == 0xa5);
        if (neighbor.p)
            for (unsigned i = 0; i < page; ++i)
                STONE_CHECK(static_cast<const uint8_t*>(neighbor.p)[i] == 0x5a);
        if (kind == 0) {
            STONE_CHECK(!AndroidMappedArm64Data(result, 8, false));
            STONE_CHECK(AndroidProtectArm64(reinterpret_cast<void*>(result), length, 3) == 0);
        }
        const auto* data = AndroidMappedArm64Data(result, 8, false);
        STONE_CHECK(data);
        STONE_CHECK(*reinterpret_cast<const uint64_t*>(data) == (kind == 2 ? marker : 0));
        STONE_CHECK((AndroidMappedArm64Data(result, 8, true) != nullptr) == (kind != 2));
    }
}

void CheckArm64DataMappingLookup()
{
    using NativeMap = void* (*)(void*, size_t, int, int, int, int64_t);
    using NativeUnmap = int (*)(void*, size_t);
    const auto nativeMap = reinterpret_cast<NativeMap>(AndroidLibcImport("mmap", true));
    const auto nativeUnmap = reinterpret_cast<NativeUnmap>(AndroidLibcImport("munmap", true));
    STONE_CHECK(nativeMap && nativeUnmap);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const size_t page = system.dwPageSize;

    struct Owner {
        void* p = nullptr;
        size_t size = 0;
        NativeUnmap unmap = AndroidUnmapArm64;

        ~Owner()
        {
            if (p && p != reinterpret_cast<void*>(-1))
                unmap(p, size);
        }

        uintptr_t address() const
        {
            return reinterpret_cast<uintptr_t>(p);
        }
    } changing;

    changing.size = page * 3;
    changing.p = AndroidMapArm64(nullptr, changing.size, 3, 0x22, -1, 0);
    STONE_CHECK(changing.p != reinterpret_cast<void*>(-1));
    const auto start = changing.address();
    STONE_CHECK(AndroidMappedArm64Data(start, 8, true) == changing.p);
    STONE_CHECK(AndroidUnmapArm64(changing.p, page) == 0);
    changing.p = reinterpret_cast<void*>(start + page);
    changing.size -= page;
    STONE_CHECK(!AndroidMappedArm64Data(start, 1, false) && AndroidMappedArm64Data(changing.address(), page, true));
    STONE_CHECK(AndroidUnmapArm64(reinterpret_cast<void*>(start + 2 * page), page) == 0);
    changing.size -= page;
    STONE_CHECK(!AndroidMappedArm64Data(start + 2 * page, 1, false) &&
                !AndroidMappedArm64Data(changing.address(), page + 1, false));
    STONE_CHECK(AndroidProtectArm64(changing.p, page, 1) == 0);
    STONE_CHECK(AndroidMappedArm64Data(changing.address(), page, false) &&
                !AndroidMappedArm64Data(changing.address(), 1, true));
    STONE_CHECK(AndroidProtectArm64(changing.p, page, 0) == 0);
    STONE_CHECK(!AndroidMappedArm64Data(changing.address(), 1, false) &&
                AndroidMappedArm64Data(changing.address(), 0, true));
    STONE_CHECK(AndroidProtectArm64(changing.p, page, 3) == 0);
    Owner host;
    host.size = page;
    host.unmap = nativeUnmap;
    host.p = nativeMap(nullptr, page, 3, 0x22, -1, 0);
    STONE_CHECK(host.p != reinterpret_cast<void*>(-1));
    STONE_CHECK(!AndroidMappedArm64Data(host.address(), 1, false) && !AndroidMappedArm64Data(host.address(), 0, false));
    const auto reused = host.address();
    STONE_CHECK(AndroidMappedArm64Data(changing.address(), 1, false)); // Publish the index before the ABI changes.
    STONE_CHECK(nativeUnmap(host.p, page) == 0);
    host.p = nullptr;
    Owner replacement;
    replacement.size = page;
    replacement.p = AndroidMapArm64(reinterpret_cast<void*>(reused), page, 3, 0x22, -1, 0);
    STONE_CHECK(replacement.address() == reused && AndroidMappedArm64Data(reused, page, true) == replacement.p);
    STONE_CHECK(AndroidUnmapArm64(replacement.p, page) == 0);
    replacement.p = nullptr;
    STONE_CHECK(!AndroidMappedArm64Data(reused, 1, false));
    host.p = nativeMap(reinterpret_cast<void*>(reused), page, 3, 0x22, -1, 0);
    STONE_CHECK(host.address() == reused && !AndroidMappedArm64Data(reused, 1, false));
    Owner shared;
    shared.size = page;
    shared.p = AndroidMapArm64(nullptr, page, 3, 0x21, -1, 0);
    STONE_CHECK(shared.p != reinterpret_cast<void*>(-1) &&
                AndroidMappedArm64Data(shared.address(), page, true) == shared.p);
    STONE_CHECK(!AndroidMappedArm64Data(UINTPTR_MAX - 3, 8, false) && !AndroidMappedArm64Data(0, 1, false));
    std::array<Owner, 8> samples;
    for (unsigned i = 0; i < samples.size(); ++i) {
        samples[i].size = page;
        samples[i].p = AndroidMapArm64(nullptr, page, 3, 0x22, -1, 0);
        STONE_CHECK(samples[i].p != reinterpret_cast<void*>(-1));
        *static_cast<uint64_t*>(samples[i].p) = i + 1;
        STONE_CHECK(AndroidMappedArm64Data(samples[i].address(), 8, false) == samples[i].p);
    }
    // Exercise the interpreter's full ownership union after warming the mmap
    // permission cache, including a real generated load/store when enabled.
    auto bytes = MakeArm64TlsElf();
    const uint32_t readCode[]{0xf9400000, 0xd503201f, 0xd503201f, 0xd65f03c0};
    const uint32_t writeCode[]{0xf9000001, 0xd503201f, 0xd503201f, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1100, readCode, sizeof(readCode));
    std::memcpy(bytes.data() + 0x1200, writeCode, sizeof(writeCode));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto access = [&](uintptr_t address, bool write, uint64_t value = 0) {
        const Arm64Argument arguments[]{{'J', address}, {'J', value}};
        return RunArm64Function(image, image.LoadBias() + (write ? 0x1200 : 0x1100), write ? 'V' : 'J', {}, nullptr,
                                nullptr, arguments);
    };
    const auto rejects = [&](uintptr_t address, bool write) {
        try {
            access(address, write);
            return false;
        }
        catch (const std::runtime_error&) {
            return true;
        }
    };
    // Use an entire allocation: a page left after a partial unmap can sit
    // inside a still-reserved Windows allocation granule and cannot be reused.
    Owner cycled;
    cycled.size = page;
    cycled.p = AndroidMapArm64(nullptr, page, 3, 0x22, -1, 0);
    STONE_CHECK(cycled.p != reinterpret_cast<void*>(-1));
    constexpr uint64_t marker = 0x13579bdf2468ace0ull;
    const auto nativeBefore = GetArm64BlockStatistics().executions;
    for (unsigned i = 0; i < 128; ++i) {
        access(cycled.address(), true, marker);
        STONE_CHECK(access(cycled.address(), false) == marker);
    }
    if (Arm64BlockJitEnabled())
        STONE_CHECK(GetArm64BlockStatistics().executions > nativeBefore);
    STONE_CHECK(AndroidProtectArm64(cycled.p, page, 1) == 0);
    STONE_CHECK(access(cycled.address(), false) == marker && rejects(cycled.address(), true));
    STONE_CHECK(*static_cast<const uint64_t*>(cycled.p) == marker);
    STONE_CHECK(AndroidProtectArm64(cycled.p, page, 0) == 0);
    STONE_CHECK(rejects(cycled.address(), false) && rejects(cycled.address(), true));
    STONE_CHECK(AndroidProtectArm64(cycled.p, page, 3) == 0);
    STONE_CHECK(access(cycled.address(), false) == marker);
    const auto former = cycled.address();
    STONE_CHECK(AndroidUnmapArm64(cycled.p, page) == 0);
    cycled.p = nullptr;
    STONE_CHECK(rejects(former, false) && rejects(former, true));
    Owner borrowed;
    borrowed.size = page;
    borrowed.unmap = nativeUnmap;
    borrowed.p = nativeMap(reinterpret_cast<void*>(former), page, 3, 0x22, -1, 0);
    STONE_CHECK(borrowed.address() == former);
    *static_cast<uint64_t*>(borrowed.p) = marker;
    STONE_CHECK(rejects(former, false) && rejects(former, true));
    STONE_CHECK(*static_cast<const uint64_t*>(borrowed.p) == marker);
    STONE_CHECK(nativeUnmap(borrowed.p, page) == 0);
    borrowed.p = nullptr;
    Owner renewed;
    renewed.size = page;
    renewed.p = AndroidMapArm64(reinterpret_cast<void*>(former), page, 3, 0x22, -1, 0);
    STONE_CHECK(renewed.address() == former);
    access(former, true, marker + 1);
    STONE_CHECK(access(former, false) == marker + 1);
    char benchmark[2]{};
    if (GetEnvironmentVariableA("STONE_ARM64_MAPPING_BENCHMARK", benchmark, 2) == 1 && benchmark[0] == '1') {
        constexpr unsigned iterations = 131072;
        const auto begin = std::chrono::steady_clock::now();
        uint64_t total = 0;
        for (unsigned i = 0; i < iterations; ++i) {
            const auto address = samples[i & 7].address();
            const auto* value = AndroidMappedArm64Data(address, 8, false);
            STONE_CHECK(value == reinterpret_cast<const uint8_t*>(address));
            total += *reinterpret_cast<const uint64_t*>(value);
        }
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
        STONE_CHECK(total == 589824);
        std::cout << "ARM64 mapping lookup benchmark: iterations=" << iterations << " total=" << total << " us=" << us
                  << '\n';
    }
}
}
