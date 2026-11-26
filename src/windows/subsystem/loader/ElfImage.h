#pragma once

#include "ElfFormat.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>

namespace stone::windows {

inline constexpr unsigned AndroidStackGuardSlot = 5;
inline constexpr unsigned AndroidArtThreadSlot = 7;
uintptr_t AndroidStackGuard();
const uintptr_t* AndroidGlobalStackGuard();
void ResetAndroidStackGuards();

// A mapped Android shared object. Callers own dependency ordering and JNI.
class ElfImage {
public:
    enum class Machine : uint16_t {
        X64 = 62,
        Arm64 = 183
    };
    explicit ElfImage(std::span<const uint8_t> file, Machine machine = Machine::X64, bool dynamicTls = false,
                      bool artCompiler = false);
    ~ElfImage();
    ElfImage(const ElfImage&) = delete;
    ElfImage& operator=(const ElfImage&) = delete;

    const std::vector<std::string>& Dependencies() const
    {
        return needed_;
    }

    Machine Architecture() const
    {
        return machine_;
    }

    uint64_t Identity() const
    {
        return identity_;
    }

    struct TlsSymbol {
        uintptr_t module;
        uint64_t offset;

        explicit operator bool() const
        {
            return module != 0;
        }
    };

    using TlsResolver = std::function<TlsSymbol(std::string_view)>;

    struct LinkSymbol {
        uintptr_t address;
        bool indirect;

        LinkSymbol(uintptr_t address = 0, bool indirect = false) : address(address), indirect(indirect)
        {
        }

        explicit operator bool() const
        {
            return address != 0;
        }
    };

    using IndirectCall = std::function<uintptr_t(uintptr_t)>;
    void Relocate(const std::function<LinkSymbol(std::string_view)>& resolve,
                  std::set<std::string>* missingImports = nullptr, const TlsResolver& resolveTls = {},
                  const std::function<LinkSymbol(std::string_view)>& interpose = {},
                  uintptr_t tlsDescriptorResolver = 0);

    size_t IndirectRelocationCount() const
    {
        return indirect_.size();
    }

    void ResolveIndirectRelocations(const IndirectCall& call);
    void TrapLinuxSyscall(uint64_t virtualAddress);
    void PatchCode(uint64_t virtualAddress, std::span<const uint8_t> bytes);
    enum class Execution {
        Native,
        Translated
    };
    void Protect(Execution execution = Execution::Native);
    std::vector<void*> Initializers() const;
    uintptr_t Export(std::string_view name, bool codeOnly = false) const;
    LinkSymbol FindLinkSymbol(std::string_view name, bool codeOnly = false) const;
    std::vector<std::string> ExportNames() const;
    bool AddressInfo(uintptr_t address, void*& base, const char*& name, void*& symbol) const;
    bool ContainsCode(uintptr_t address, size_t length) const;
    std::span<const uint8_t> ExecutableRangeAt(uintptr_t address) const;
    std::span<const uint8_t> CodeAt(uintptr_t address, size_t limit) const;
    uint8_t* DataAt(uintptr_t address, size_t length, bool write) const;
    bool ProtectData(uintptr_t address, size_t length, bool writable = false) const;
    // ARM64 code stays host-readable for instruction decoding. Guest data
    // reads can switch between PROT_EXEC and PROT_READ|PROT_EXEC independently.
    bool ProtectArm64Code(uintptr_t address, size_t length, bool readable) const;

    uintptr_t LoadBias() const
    {
        return reinterpret_cast<uintptr_t>(memory_) - floor_;
    }

    std::span<const elf::Elf64Phdr> ProgramHeaders() const
    {
        return headers_;
    }

    uintptr_t TlsModule() const
    {
        return tls_.p_type ? reinterpret_cast<uintptr_t>(this) : 0;
    }

    TlsSymbol FindTlsExport(std::string_view name) const;
    void* ThreadLocalAddress(uint64_t offset, bool allocate = true) const;
    uint8_t* ThreadLocalDataAt(uintptr_t address, size_t length) const;
    // Descriptor arguments are opaque: only handles owned by this image may
    // resolve to a TLS module. Never dereference an arbitrary guest argument.
    bool ResolveTlsDescriptor(uintptr_t argument, uintptr_t& address) const;
    void AssignStaticTls(size_t& used);
    static void PublishStaticTls(std::span<const ElfImage* const> images);
    static uintptr_t* GuestThreadPointer();
    static uintptr_t* CurrentGuestThreadPointer() noexcept;
    static uintptr_t* GuestArm64ThreadPointer();
    static uint8_t* Arm64ThreadControlDataAt(uintptr_t address, size_t length);
    static bool RegisterThreadDestructor(void (*invoke)(uintptr_t, void*), uintptr_t callback, void* argument);
    static bool SetThreadKeyCleanup(void (*cleanup)());
    static std::shared_ptr<void>& ThreadExecutionStorage();
    static void RunThreadDestructors();

private:
    using Segment = elf::Elf64Phdr;
    using Symbol = elf::Elf64Sym;
    using Resolver = std::function<LinkSymbol(std::string_view)>;

    struct IndirectRelocation {
        uint8_t* destination;
        uintptr_t resolver;
        uint64_t addend;
    };

    struct RelocationSymbols {
        const Resolver& resolve;
        const TlsResolver& resolveTls;
        const Resolver& interpose;
        std::set<std::string>* missingImports;
        uintptr_t tlsDescriptorResolver;
    };

    uint8_t* At(uint64_t address, uint64_t size, uint32_t flags = 0) const;
    uint64_t Tag(int64_t type) const;
    std::string_view String(uint64_t offset) const;
    uint64_t CountGnuSymbols(uint64_t hash) const;
    LinkSymbol Defined(const Symbol& symbol, bool codeOnly) const;
    void ApplyRela(const elf::Elf64Rela& relocation, const RelocationSymbols& symbols);
    void ApplyRelaTable(uint64_t address, uint64_t size, const RelocationSymbols& symbols);
    void ApplyAndroidRela(uint64_t address, uint64_t size, const RelocationSymbols& symbols);
    void CopyTlsTemplate(uint8_t* destination) const;
    void ProtectSegments(Execution execution, bool applyRelro);

    Machine machine_;
    const uint64_t identity_;
    uint8_t* memory_ = nullptr;
    uint64_t floor_ = 0;
    size_t size_ = 0;
    size_t page_ = 0;
    std::vector<Segment> headers_;
    std::vector<Segment> loads_;
    std::vector<std::pair<int64_t, uint64_t>> dynamic_;
    std::vector<std::string> needed_;
    std::vector<Symbol> symbols_;
    std::map<std::string, size_t, std::less<>> exports_;
    std::map<std::string, uint64_t, std::less<>> tlsExports_;
    std::string_view strings_;
    std::vector<Segment> relro_;
    std::vector<IndirectRelocation> indirect_;
    std::vector<std::unique_ptr<TlsSymbol>> tlsDescriptors_;
    Segment tls_{};
    uint64_t tlsIdentity_ = 0;
    size_t staticTlsOffset_ = 0;

    struct CodeSymbol {
        uint64_t address, size;
    };

    std::vector<CodeSymbol> codeSymbols_;
    mutable std::shared_mutex codePermissionsMutex_;
    mutable std::map<uintptr_t, bool> codeReadablePages_;
};
} // namespace stone::windows
