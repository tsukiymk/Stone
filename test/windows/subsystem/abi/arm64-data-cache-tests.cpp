#include "../support/test-suite.h"
#include "../support/elf-fixtures.h"
#include "loader/AndroidLibraryState.h"
#include <thread>

namespace stone::windows::test {
namespace {
using namespace android_libraries;

Library& PublishFixture(bool readOnly = false, bool relro = false)
{
    auto bytes = checks::MakeArm64TlsElf();
    if (readOnly) {
        const uint32_t flags = 4;
        std::memcpy(bytes.data() + sizeof(elf::Elf64Ehdr) + 4, &flags, 4);
    }
    if (relro) {
        const uint16_t count = 5;
        std::memcpy(bytes.data() + 56, &count, sizeof(count));
        const elf::Elf64Phdr header{0x6474e552, 4, 0, 0, 0, 4096, 4096, 4096};
        std::memcpy(bytes.data() + sizeof(elf::Elf64Ehdr) + 4 * sizeof(header), &header, sizeof(header));
    }
    auto library = std::make_unique<Library>(bytes, ElfImage::Machine::Arm64, true);
    library->image.Protect(ElfImage::Execution::Translated);
    library->openHandles = 1;
    auto& result = *library;
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    // Match the production lifetime: published libraries stay owned by Process.
    const auto name = "data-cache-fixture-" + std::to_string(result.image.Identity());
    state.application.images.emplace(name, std::move(library));
    return result;
}

bool Enabled()
{
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"STONE_ARM64_ELF_DATA_CACHE", value, 2) == 1 && value[0] == L'1';
}
}

void TestArm64DataCache(Suite& suite)
{
    suite.Run("published-owners-recheck-permissions-bounds-and-late-publication", [] {
        auto& library = PublishFixture();
        const auto& image = library.image;
        const auto base = image.LoadBias();
        for (unsigned i = 0; i < 128; ++i)
            STONE_CHECK(FindArm64Data(base + 0x300, 8, true) == reinterpret_cast<uint8_t*>(base + 0x300));
        STONE_CHECK(image.ProtectData(base, 4096, false));
        STONE_CHECK(!FindArm64Data(base + 0x300, 8, true) && FindArm64Data(base + 0x300, 8, false));
        STONE_CHECK(image.ProtectData(base, 4096, true) && FindArm64Data(base + 0x300, 8, true));
        STONE_CHECK(FindArm64Data(base + 0x1000, 4, false));
        STONE_CHECK(image.ProtectArm64Code(base + 0x1000, 4096, false));
        STONE_CHECK(!FindArm64Data(base + 0x1000, 4, false) && image.ContainsCode(base + 0x1000, 4));
        STONE_CHECK(image.ProtectArm64Code(base + 0x1000, 4096, true) && FindArm64Data(base + 0x1000, 4, false));
        STONE_CHECK(!FindArm64Data(base + 0x1000, 4, true) && !FindArm64Data(base + 0xffc, 8, false));
        STONE_CHECK(!FindArm64Data(base + 0x300, UINT_MAX, false));
        uintptr_t host = 42;
        STONE_CHECK(!FindArm64Data(reinterpret_cast<uintptr_t>(&host), 8, false));
        for (const auto settings : {std::pair{true, false}, std::pair{false, true}}) {
            auto& protectedLibrary = PublishFixture(settings.first, settings.second);
            const auto address = protectedLibrary.image.LoadBias() + 0x300;
            STONE_CHECK(FindArm64Data(address, 8, false) && !FindArm64Data(address, 8, true));
        }
        // A previous miss must not hide a subsequently published owner.
        auto raw = checks::MakeArm64TlsElf();
        auto late = std::make_unique<Library>(raw, ElfImage::Machine::Arm64, true);
        late->image.Protect(ElfImage::Execution::Translated);
        const auto address = late->image.LoadBias() + 0x300;
        STONE_CHECK(!FindArm64Data(address, 8, false));
        {
            auto& state = Process();
            const std::lock_guard lock(state.mutex);
            state.runtime.images.emplace("data-cache-late", std::move(late));
        }
        STONE_CHECK(FindArm64Data(address, 8, false));
        {
            ScopedNamespace scope(Process().application);
            STONE_CHECK(DlClose(&library) == 0);
        }
        STONE_CHECK(FindArm64Data(base + 0x300, 8, true));
    });
    suite.Run("cache-hits-avoid-loader-lock-and-tls-stays-thread-local", [] {
        auto& image = PublishFixture().image;
        const auto address = image.LoadBias() + 0x300;
        const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr),
                     go = CreateEventW(nullptr, TRUE, FALSE, nullptr),
                     done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        STONE_CHECK(ready && go && done);
        std::exception_ptr failure;
        uintptr_t workerTls = 0;
        std::thread worker([&] {
            try {
                workerTls = reinterpret_cast<uintptr_t>(image.ThreadLocalAddress(0));
                STONE_CHECK(workerTls && FindArm64Data(workerTls, 32, true));
                STONE_CHECK(FindArm64Data(address, 8, false));
                SetEvent(ready);
                STONE_CHECK(WaitForSingleObject(go, 5000) == WAIT_OBJECT_0);
                STONE_CHECK(FindArm64Data(address, 8, false));
                STONE_CHECK(FindArm64Data(workerTls, 32, true));
                SetEvent(done);
                STONE_CHECK(!FindArm64Data(workerTls, 33, false));
            }
            catch (...) {
                failure = std::current_exception();
                SetEvent(ready);
            }
            SetEvent(done);
        });
        struct Join {
            HANDLE ready, go, done;
            std::thread& worker;
            ~Join()
            {
                SetEvent(go);
                if (worker.joinable())
                    worker.join();
                CloseHandle(ready);
                CloseHandle(go);
                CloseHandle(done);
            }
        } join{ready, go, done, worker};
        STONE_CHECK(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
        STONE_CHECK(!FindArm64Data(workerTls, 8, false)); // No access to another thread's ELF TLS.
        bool completed = false;
        {
            const std::lock_guard lock(Process().mutex);
            SetEvent(go);
            completed = WaitForSingleObject(done, Enabled() ? 3000 : 200) == WAIT_OBJECT_0;
        }
        STONE_CHECK(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0);
        worker.join();
        if (failure)
            std::rethrow_exception(failure);
        STONE_CHECK(completed == Enabled());
    });
}
}
