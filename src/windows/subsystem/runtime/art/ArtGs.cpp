#include "abi/x64/TlsInstructions.h"
#include "platform/windows/ExecutableMemory.h"
#include "ArtGs.h"
#include "../../loader/ElfImage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

extern "C" {
#include <xz.h>
}

namespace stone::windows {
using namespace x64_tls;

namespace {

struct CodeRange {
    uint64_t begin, end;
};

std::vector<CodeRange> JitCodeRanges(std::span<const uint8_t> code)
{
    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        throw std::runtime_error("could not initialize the JIT code decoder");

    struct Previous {
        size_t offset, length;
        ZydisMnemonic mnemonic;
        ZydisInstructionCategory category;
        std::array<ZydisDecodedOperand, 3> operands;
    };

    std::vector<uint8_t> visited(code.size()); // 1=start, 2=instruction body, 3=jump table.
    std::vector<size_t> targets, addresses;
    std::vector<CodeRange> ranges;
    const auto sameRegister = [](ZydisRegister a, ZydisRegister b) {
        return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, a) ==
               ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, b);
    };
    // ART appends constants after all code, including cold catch handlers.
    // Decode that complete prefix rather than just the ordinary control flow.
    size_t end = code.size();
    std::array<Previous, 5> history{};
    size_t count = 0;
    for (size_t offset = 0; offset < end;) {
        if (!code[offset] && std::all_of(code.begin() + offset, code.begin() + end, [](uint8_t byte) {
                return byte == 0;
            }))
            break;
        for (auto address : addresses)
            if (address == offset)
                throw std::runtime_error("unclassified ART JIT embedded address");
        if (visited[offset])
            throw std::runtime_error("ART JIT branch targets data or an instruction body");
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, code.data() + offset, end - offset, &instruction, operands))) {
            char location[64];
            std::snprintf(location, sizeof(location), "could not decode ART JIT instruction at +0x%zx:", offset);
            std::string message(location);
            for (size_t i = offset; i < std::min(code.size(), offset + 16); ++i) {
                char byte[4];
                std::snprintf(byte, sizeof(byte), " %02x", code[i]);
                message += byte;
            }
            throw std::runtime_error(message);
        }
        visited[offset] = 1;
        for (size_t i = offset + 1; i < offset + instruction.length; ++i) {
            if (visited[i])
                throw std::runtime_error("overlapping ART JIT instructions");
            visited[i] = 2;
        }
        ranges.push_back({offset, offset + instruction.length});
        const auto category = instruction.meta.category;
        for (unsigned i = 0; i < instruction.operand_count_visible; ++i) {
            if (operands[i].type != ZYDIS_OPERAND_TYPE_MEMORY || operands[i].mem.base != ZYDIS_REGISTER_RIP)
                continue;
            const int64_t target = static_cast<int64_t>(offset + instruction.length) + operands[i].mem.disp.value;
            if (target < 0 || static_cast<uint64_t>(target) >= code.size())
                continue;
            if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA)
                addresses.push_back(static_cast<size_t>(target));
            else {
                if (target < static_cast<int64_t>(offset + instruction.length))
                    throw std::runtime_error("ART JIT literal overlaps code");
                end = std::min(end, static_cast<size_t>(target));
            }
        }
        if (category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR ||
            category == ZYDIS_CATEGORY_CALL) {
            for (unsigned i = 0; i < instruction.operand_count_visible; ++i) {
                if (operands[i].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operands[i].imm.is_relative)
                    continue;
                const int64_t target = static_cast<int64_t>(offset + instruction.length) + operands[i].imm.value.s;
                if (target >= 0 && static_cast<uint64_t>(target) < code.size())
                    targets.push_back(static_cast<size_t>(target));
            }
        }
        const auto identifyTable = [&] {
            if (category != ZYDIS_CATEGORY_UNCOND_BR || operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER)
                return;
            // ART's packed switch: CMP/JA, LEA table, MOVSXD relative
            // entry, ADD table base, JMP register. Follow every table
            // target; the appended constants themselves are never code.
            if (count < 5)
                return; // Register tail calls have no embedded table.
            const auto& cmp = history[(count - 5) % 5];
            const auto& branch = history[(count - 4) % 5];
            const auto& lea = history[(count - 3) % 5];
            const auto& load = history[(count - 2) % 5];
            const auto& add = history[(count - 1) % 5];
            if (cmp.mnemonic != ZYDIS_MNEMONIC_CMP || cmp.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                cmp.operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || branch.category != ZYDIS_CATEGORY_COND_BR ||
                lea.mnemonic != ZYDIS_MNEMONIC_LEA || lea.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                lea.operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY || lea.operands[1].mem.base != ZYDIS_REGISTER_RIP ||
                load.mnemonic != ZYDIS_MNEMONIC_MOVSXD || load.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                load.operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY || load.operands[1].mem.scale != 4 ||
                load.operands[1].mem.disp.value != 0 ||
                !sameRegister(load.operands[1].mem.base, lea.operands[0].reg.value) ||
                !sameRegister(load.operands[1].mem.index, cmp.operands[0].reg.value) ||
                add.mnemonic != ZYDIS_MNEMONIC_ADD || add.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                add.operands[1].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                !sameRegister(add.operands[0].reg.value, load.operands[0].reg.value) ||
                !sameRegister(add.operands[1].reg.value, lea.operands[0].reg.value) ||
                !sameRegister(operands[0].reg.value, load.operands[0].reg.value))
                return;
            const int64_t table = static_cast<int64_t>(lea.offset + lea.length) + lea.operands[1].mem.disp.value;
            const uint64_t entries = cmp.operands[1].imm.value.u + 1;
            if (!entries || table < 0 || static_cast<uint64_t>(table) >= code.size() ||
                entries > (code.size() - static_cast<size_t>(table)) / sizeof(int32_t))
                throw std::runtime_error("ART JIT switch table is outside method");
            end = std::min(end, static_cast<size_t>(table));
            for (uint64_t i = 0; i < entries * sizeof(int32_t); ++i) {
                if (visited[static_cast<size_t>(table) + i] && visited[static_cast<size_t>(table) + i] != 3)
                    throw std::runtime_error("ART JIT switch table overlaps code");
                visited[static_cast<size_t>(table) + i] = 3;
            }
            for (uint64_t i = 0; i < entries; ++i) {
                int32_t displacement;
                std::memcpy(&displacement, code.data() + table + i * sizeof(int32_t), sizeof(displacement));
                const auto target = table + displacement;
                if (target < 0 || static_cast<uint64_t>(target) >= code.size())
                    throw std::runtime_error("ART JIT switch target is outside method");
                targets.push_back(static_cast<size_t>(target));
            }
        };
        identifyTable();
        auto& previous = history[count++ % 5];
        previous = {offset, instruction.length, instruction.mnemonic, category, {}};
        std::copy_n(operands, std::min<unsigned>(3, instruction.operand_count_visible), previous.operands.begin());
        offset += instruction.length;
    }
    for (auto target : targets)
        if (target >= end || visited[target] != 1)
            throw std::runtime_error("ART JIT branch targets literal data or an instruction body");
    std::ranges::sort(ranges, {}, &CodeRange::begin);
    return ranges;
}

std::vector<elf::Elf64Shdr> Sections(std::span<const uint8_t> file)
{
    elf::Elf64Ehdr header{};
    if (file.size() < sizeof(header))
        throw std::runtime_error("ART code metadata has no ELF64 header");
    std::memcpy(&header, file.data(), sizeof(header));
    if (header.e_shentsize != sizeof(elf::Elf64Shdr) || header.e_shoff > file.size() ||
        header.e_shnum > (file.size() - header.e_shoff) / sizeof(elf::Elf64Shdr))
        throw std::runtime_error("ART code metadata has invalid section headers");
    std::vector<elf::Elf64Shdr> sections(header.e_shnum);
    std::memcpy(sections.data(), file.data() + header.e_shoff, sections.size() * sizeof(elf::Elf64Shdr));
    return sections;
}

std::span<const uint8_t> SectionData(std::span<const uint8_t> file, const elf::Elf64Shdr& section)
{
    if (section.sh_offset > file.size() || section.sh_size > file.size() - section.sh_offset)
        throw std::runtime_error("ART code metadata section is outside its file");
    return file.subspan(static_cast<size_t>(section.sh_offset), static_cast<size_t>(section.sh_size));
}

std::vector<uint8_t> DebugData(std::span<const uint8_t> compressed)
{
    static const bool initialized = [] {
        xz_crc32_init();
        xz_crc64_init();
        return true;
    }();
    (void)initialized;
    std::unique_ptr<xz_dec, decltype(&xz_dec_end)> decoder(xz_dec_init(XZ_DYNALLOC, 64 * 1024 * 1024), xz_dec_end);
    if (!decoder)
        throw std::bad_alloc{};
    xz_buf buffer{};
    buffer.in = compressed.data();
    buffer.in_size = compressed.size();
    std::vector<uint8_t> result(1024 * 1024);
    for (;;) {
        buffer.out = result.data();
        buffer.out_size = result.size();
        const auto status = xz_dec_run(decoder.get(), &buffer);
        if (status == XZ_STREAM_END && buffer.in_pos == buffer.in_size) {
            result.resize(buffer.out_pos);
            return result;
        }
        if (status != XZ_OK || buffer.out_pos != buffer.out_size || result.size() >= 64 * 1024 * 1024)
            throw std::runtime_error("could not decompress ART ELF code metadata");
        result.resize(result.size() * 2);
    }
}

void AddFunctions(std::span<const uint8_t> file, std::vector<CodeRange>& ranges)
{
    for (const auto& section : Sections(file)) {
        if (section.sh_type != 2 && section.sh_type != 11)
            continue; // SYMTAB, DYNSYM.
        const auto bytes = SectionData(file, section);
        if (section.sh_entsize != sizeof(elf::Elf64Sym) || bytes.size() % sizeof(elf::Elf64Sym))
            throw std::runtime_error("invalid ART function symbol table");
        for (size_t offset = 0; offset < bytes.size(); offset += sizeof(elf::Elf64Sym)) {
            elf::Elf64Sym symbol{};
            std::memcpy(&symbol, bytes.data() + offset, sizeof(symbol));
            if ((symbol.st_info & 15) != 2 || !symbol.st_shndx || !symbol.st_size)
                continue;
            if (symbol.st_value > UINT64_MAX - symbol.st_size)
                throw std::runtime_error("invalid ART function symbol size");
            ranges.push_back({symbol.st_value, symbol.st_value + symbol.st_size});
        }
    }
}

std::vector<CodeRange> FunctionRanges(std::span<const uint8_t> file)
{
    std::vector<CodeRange> ranges;
    AddFunctions(file, ranges);
    elf::Elf64Ehdr header{};
    std::memcpy(&header, file.data(), sizeof(header));
    const auto sections = Sections(file);
    if (header.e_shstrndx >= sections.size())
        throw std::runtime_error("invalid ART section name table");
    const auto names = SectionData(file, sections[header.e_shstrndx]);
    constexpr char debugName[] = ".gnu_debugdata";
    for (const auto& section : sections) {
        if (section.sh_name > names.size() || sizeof(debugName) > names.size() - section.sh_name)
            continue;
        if (std::memcmp(names.data() + section.sh_name, debugName, sizeof(debugName)))
            continue;
        const auto debug = DebugData(SectionData(file, section));
        AddFunctions(debug, ranges);
    }
    std::ranges::sort(ranges, {}, &CodeRange::begin);
    ranges.erase(std::unique(ranges.begin(), ranges.end(),
                             [](auto a, auto b) {
                                 return a.begin == b.begin && a.end == b.end;
                             }),
                 ranges.end());
    return ranges;
}

}

std::map<std::string, ElfDebugSymbol, std::less<>> ElfDebugSymbols(std::span<const uint8_t> file)
{
    std::map<std::string, ElfDebugSymbol, std::less<>> result;
    const auto add = [&](std::span<const uint8_t> data) {
        const auto sections = Sections(data);
        for (const auto& section : sections) {
            if (section.sh_type != 2 && section.sh_type != 11)
                continue;
            if (section.sh_link >= sections.size())
                throw std::runtime_error("invalid ELF symbol string table");
            const auto strings = SectionData(data, sections[section.sh_link]);
            const auto symbols = SectionData(data, section);
            if (section.sh_entsize != sizeof(elf::Elf64Sym) || symbols.size() % sizeof(elf::Elf64Sym))
                throw std::runtime_error("invalid ELF function table");
            for (size_t offset = 0; offset < symbols.size(); offset += sizeof(elf::Elf64Sym)) {
                elf::Elf64Sym symbol{};
                std::memcpy(&symbol, symbols.data() + offset, sizeof(symbol));
                const auto kind = symbol.st_info & 15;
                if ((kind != 1 && kind != 2) || !symbol.st_shndx || !symbol.st_size)
                    continue;
                if (symbol.st_name >= strings.size())
                    throw std::runtime_error("invalid ELF function name");
                const auto start = reinterpret_cast<const char*>(strings.data() + symbol.st_name);
                const auto end = static_cast<const char*>(std::memchr(start, 0, strings.size() - symbol.st_name));
                if (!end)
                    throw std::runtime_error("unterminated ELF function name");
                result.emplace(std::string(start, end), ElfDebugSymbol{symbol.st_value, symbol.st_size, kind == 2});
            }
        }
    };
    add(file);
    elf::Elf64Ehdr header{};
    std::memcpy(&header, file.data(), sizeof(header));
    const auto sections = Sections(file);
    if (header.e_shstrndx >= sections.size())
        throw std::runtime_error("invalid ELF section names");
    const auto names = SectionData(file, sections[header.e_shstrndx]);
    constexpr char debugName[] = ".gnu_debugdata";
    for (const auto& section : sections) {
        if (section.sh_name > names.size() || sizeof(debugName) > names.size() - section.sh_name)
            continue;
        if (!std::memcmp(names.data() + section.sh_name, debugName, sizeof(debugName)))
            add(DebugData(SectionData(file, section)));
    }
    return result;
}

ArtGsCode::ArtGsCode(ElfImage& image, std::span<const uint8_t> file, bool oatCode)
    : ArtGsCode(&image, file, {}, oatCode)
{
}

ArtGsCode::ArtGsCode(std::span<uint8_t> method) : ArtGsCode(nullptr, {}, method)
{
}

ArtGsCode::ArtGsCode(ElfImage* image, std::span<const uint8_t> file, std::span<uint8_t> method, bool oatCode)
{
    struct Region {
        uintptr_t address;
        std::span<const uint8_t> bytes;
        std::vector<CodeRange> ranges;
    };

    std::vector<Region> regions;
    if (image) {
        elf::Elf64Ehdr header{};
        if (file.size() < sizeof(header))
            throw std::runtime_error("ART GS rewriting requires an ELF64 header");
        std::memcpy(&header, file.data(), sizeof(header));
        if (!header.e_shnum || header.e_shentsize != sizeof(elf::Elf64Shdr) || header.e_shoff > file.size() ||
            header.e_shnum > (file.size() - header.e_shoff) / sizeof(elf::Elf64Shdr))
            throw std::runtime_error("ART GS rewriting requires valid ELF64 section headers");
        const auto functions = FunctionRanges(file);
        for (unsigned i = 0; i < header.e_shnum; ++i) {
            elf::Elf64Shdr section{};
            std::memcpy(&section, file.data() + header.e_shoff + i * sizeof(section), sizeof(section));
            if (section.sh_type != 1 || !(section.sh_flags & 4))
                continue; // SHT_PROGBITS, SHF_EXECINSTR.
            if (section.sh_addr > UINTPTR_MAX - image->LoadBias() || section.sh_size > file.size())
                throw std::runtime_error("invalid ELF executable section for ART GS rewriting");
            const auto start = image->LoadBias() + section.sh_addr;
            std::vector<CodeRange> ranges;
            for (const auto function : functions) {
                if (function.begin >= section.sh_addr && function.end <= section.sh_addr + section.sh_size) {
                    const auto offset = function.begin - section.sh_addr;
                    if (oatCode) {
                        const auto bytes =
                            image->CodeAt(start + offset, static_cast<size_t>(function.end - function.begin));
                        for (auto range : JitCodeRanges(bytes))
                            ranges.push_back({offset + range.begin, offset + range.end});
                    }
                    else
                        ranges.push_back({offset, function.end - section.sh_addr});
                }
            }
            // Small, unstripped synthetic ELF inputs may omit symbols entirely.
            if (functions.empty())
                ranges.push_back({0, section.sh_size});
            if (!image->ContainsCode(start, static_cast<size_t>(section.sh_size)))
                throw std::runtime_error("ART code section is outside executable memory");
            regions.push_back({start,
                               {reinterpret_cast<const uint8_t*>(start), static_cast<size_t>(section.sh_size)},
                               std::move(ranges)});
        }
    }
    else {
        if (method.empty())
            throw std::runtime_error("cannot adapt an empty ART JIT method");
        regions.push_back({reinterpret_cast<uintptr_t>(method.data()), method, JitCodeRanges(method)});
    }
    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        throw std::runtime_error("could not initialize the ART instruction decoder");
    std::vector<uint8_t> code;
    std::vector<TlsLoadSite> tlsLoads;
    for (const auto& region : regions) {
        const auto start = region.address;
        std::vector<bool> decoded(region.bytes.size());
        for (const auto range : region.ranges) {
            bool afterTerminator = true;
            for (uint64_t offset = range.begin; offset < range.end;) {
                if (decoded[static_cast<size_t>(offset)])
                    break;
                decoded[static_cast<size_t>(offset)] = true;
                const auto bytes =
                    image ? image->CodeAt(start + offset, static_cast<size_t>(range.end - offset))
                          : region.bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(range.end - offset));
                // ART's assembly interpreter pads its indirect-dispatch handlers with
                // zero bytes. Padding can have odd length and must not shift decoding.
                if (image && afterTerminator && !bytes.empty() && bytes.front() == 0) {
                    ++offset;
                    continue;
                }
                ZydisDecodedInstruction instruction;
                ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
                if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, bytes.data(), bytes.size(), &instruction, operands))) {
                    if (bytes.size() < ZYDIS_MAX_INSTRUCTION_LENGTH && std::ranges::all_of(bytes, [](uint8_t byte) {
                            return byte == 0 || byte == 0x90 || byte == 0xcc;
                        }))
                        break;
                    char location[128];
                    std::snprintf(location, sizeof(location), "could not decode ART code at 0x%llx (%zu bytes remain)",
                                  static_cast<unsigned long long>(start + offset),
                                  static_cast<size_t>(range.end - offset));
                    throw std::runtime_error(location);
                }
                if (instruction.mnemonic != ZYDIS_MNEMONIC_NOP) {
                    afterTerminator =
                        instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
                        instruction.meta.category == ZYDIS_CATEGORY_RET || instruction.mnemonic == ZYDIS_MNEMONIC_UD2 ||
                        instruction.mnemonic == ZYDIS_MNEMONIC_INT3 || instruction.mnemonic == ZYDIS_MNEMONIC_HLT;
                }
                unsigned memoryOperand = instruction.operand_count;
                for (unsigned operand = 0; operand < instruction.operand_count; ++operand) {
                    if (operands[operand].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                        operands[operand].mem.segment == ZYDIS_REGISTER_GS) {
                        if (memoryOperand != instruction.operand_count)
                            throw std::runtime_error("ART GS instruction uses multiple GS memory operands");
                        memoryOperand = operand;
                    }
                }
                if (memoryOperand != instruction.operand_count) {
                    if (memoryOperand >= instruction.operand_count_visible ||
                        (instruction.address_width != 64 && instruction.address_width != 32)) {
                        char location[160];
                        std::snprintf(location, sizeof(location),
                                      "unsupported ART GS operand at 0x%llx (opcode %u, address width %u)",
                                      static_cast<unsigned long long>(start + offset), instruction.mnemonic,
                                      instruction.address_width);
                        throw std::runtime_error(location);
                    }
                    const auto next = start + offset + instruction.length;
                    const auto trampoline = code.size();
                    if (ControlOperation(instruction.mnemonic)) {
                        if (operands[memoryOperand].size != 64)
                            throw std::runtime_error("unsupported ART GS control operand width");
                    }
                    if (!ControlOperation(instruction.mnemonic) || DirectControlOperation(instruction.mnemonic)) {
                        try {
                            MakeTrampoline(code, instruction, operands, memoryOperand, next, tlsLoads);
                        }
                        catch (const std::exception& error) {
                            char location[256];
                            std::snprintf(location, sizeof(location), "ART GS instruction at 0x%llx: %s",
                                          static_cast<unsigned long long>(start + offset), error.what());
                            throw std::runtime_error(location);
                        }
                    }
                    traps_.push_back({start + offset, next, trampoline, instruction.mnemonic,
                                      operands[memoryOperand].mem, instruction.address_width});
                }
                offset += instruction.length;
            }
        }
    }
    for (const auto& load : tlsLoads) {
        const auto resume = code.size();
        // NtContinue may clear FS again after the exception handler returns.
        // Restore it in guest code, using only the faulting MOV's destination,
        // then retry that MOV. No flags, stack slots or other registers change.
        ZydisEncoderRequest restore{};
        restore.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
        restore.mnemonic = ZYDIS_MNEMONIC_WRFSBASE;
        restore.operand_count = 1;
        restore.operands[0] = Register(load.destination);
        Encode(code, restore);
        code.push_back(0xe9);
        const auto distance = static_cast<int64_t>(load.offset) - static_cast<int64_t>(code.size() + 4);
        if (distance < INT32_MIN || distance > INT32_MAX)
            throw std::runtime_error("ART TLS recovery code is outside branch range");
        const auto relative = static_cast<int32_t>(distance);
        const auto* bytes = reinterpret_cast<const uint8_t*>(&relative);
        code.insert(code.end(), bytes, bytes + sizeof(relative));
        tlsRecoveries_.push_back({load.offset, resume, static_cast<uint8_t>(ZydisRegisterGetId(load.destination))});
    }
    const auto free = [](void* memory) {
        if (memory)
            VirtualFree(memory, 0, MEM_RELEASE);
    };
    std::unique_ptr<void, decltype(free)> memory(
        code.empty() ? nullptr
                     : AllocateReplacementCode(code.size(), image ? 0 : reinterpret_cast<uintptr_t>(method.data())),
        free);
    if (!code.empty()) {
        if (!memory)
            throw std::bad_alloc{};
        std::memcpy(memory.get(), code.data(), code.size());
        DWORD previous;
        if (!VirtualProtect(memory.get(), code.size(), PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), memory.get(), code.size()))
            throw std::runtime_error("could not publish ART GS replacement code");
    }
    const auto patch = [&](uintptr_t address, std::span<const uint8_t> bytes) {
        patches_.push_back({address, {bytes.begin(), bytes.end()}});
        if (image) {
            image->PatchCode(address - image->LoadBias(), bytes);
            return;
        }
    };
    for (auto& trap : traps_) {
        if (!ControlOperation(trap.operation) || DirectControlOperation(trap.operation)) {
            trap.trampoline += reinterpret_cast<uintptr_t>(memory.get());
            const auto distance = static_cast<int64_t>(trap.trampoline) - static_cast<int64_t>(trap.address + 5);
            if (trap.next - trap.address >= 5 && distance >= INT32_MIN && distance <= INT32_MAX) {
                std::vector<uint8_t> jump(trap.next - trap.address, 0x90);
                jump[0] = 0xe9;
                const auto relative = static_cast<int32_t>(distance);
                std::memcpy(jump.data() + 1, &relative, sizeof(relative));
                patch(trap.address, jump);
                continue;
            }
        }
        constexpr uint8_t breakpoint = 0xcc;
        patch(trap.address, {&breakpoint, 1});
    }
    if (!image && !patches_.empty()) {
        // This method is unpublished. Change its page permissions once for the
        // complete rewrite, then restore them before ART can install its entry.
        DWORD previous;
        if (!VirtualProtect(method.data(), method.size(), PAGE_EXECUTE_READWRITE, &previous))
            throw std::runtime_error("could not write ART JIT method");
        for (const auto& item : patches_)
            std::memcpy(reinterpret_cast<void*>(item.address), item.bytes.data(), item.bytes.size());
        DWORD ignored;
        if (!VirtualProtect(method.data(), method.size(), previous, &ignored) ||
            !FlushInstructionCache(GetCurrentProcess(), method.data(), method.size()))
            throw std::runtime_error("could not publish adapted ART JIT method");
    }
    std::ranges::sort(traps_, {}, &Trap::address);
    for (auto& recovery : tlsRecoveries_) {
        recovery.address += reinterpret_cast<uintptr_t>(memory.get());
        recovery.resume += reinterpret_cast<uintptr_t>(memory.get());
    }
    codeSize_ = code.size();
    code_ = memory.release();
}

bool ArtGsCode::MatchesPublishedCode() const
{
    return std::ranges::all_of(patches_, [](const auto& patch) {
        return !std::memcmp(reinterpret_cast<const void*>(patch.address), patch.bytes.data(), patch.bytes.size());
    });
}

ArtGsCode::~ArtGsCode()
{
    if (code_)
        VirtualFree(code_, 0, MEM_RELEASE);
}

bool ArtGsCode::RecoverTlsLoad(CONTEXT& context) const
{
    const auto address = static_cast<uintptr_t>(context.Rip);
    const auto recovery = std::ranges::lower_bound(tlsRecoveries_, address, {}, &TlsRecovery::address);
    if (recovery == tlsRecoveries_.end() || recovery->address != address)
        return false;
    const auto* tls = ElfImage::CurrentGuestThreadPointer();
    if (!tls)
        return false;
    DWORD64* registers[]{&context.Rax, &context.Rcx, &context.Rdx, &context.Rbx, nullptr,      &context.Rbp,
                         &context.Rsi, &context.Rdi, &context.R8,  &context.R9,  &context.R10, &context.R11,
                         &context.R12, &context.R13, &context.R14, &context.R15};
    const auto reg = recovery->destination;
    if (reg >= std::size(registers) || !registers[reg])
        return false;
    *registers[reg] = reinterpret_cast<uintptr_t>(tls);
    context.Rip = recovery->resume;
    return true;
}

bool ArtGsCode::Dispatch(uintptr_t address, CONTEXT& context) const
{
    const auto found = std::ranges::lower_bound(traps_, address, {}, &Trap::address);
    if (found == traps_.end() || found->address != address)
        return false;
    if (!ControlOperation(found->operation) || DirectControlOperation(found->operation)) {
        context.Rip = found->trampoline;
        return true;
    }
    const auto* tls = ElfImage::CurrentGuestThreadPointer();
    if (!tls)
        return false;
    const auto& memory = found->memory;
    // POP computes a stack-relative destination after consuming its source.
    const bool pop = found->operation == ZYDIS_MNEMONIC_POP;
    const auto base = RegisterValue(memory.base, context, found->next) +
                      (pop && memory.base == ZYDIS_REGISTER_RSP ? sizeof(uintptr_t) : 0);
    auto offset = base + RegisterValue(memory.index, context, found->next) * memory.scale +
                  (memory.disp.has_displacement ? memory.disp.value : 0);
    if (found->addressWidth == 32)
        offset = static_cast<uint32_t>(offset);
    const auto location = tls[AndroidArtThreadSlot] + offset;
    uintptr_t value = 0;
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(pop ? context.Rsp : location), &value,
                           sizeof(value), &copied) ||
        copied != sizeof(value))
        return false;
    if (found->operation == ZYDIS_MNEMONIC_CALL || found->operation == ZYDIS_MNEMONIC_PUSH) {
        const uintptr_t pushed = found->operation == ZYDIS_MNEMONIC_CALL ? found->next : value;
        if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp - sizeof(value)), &pushed,
                                sizeof(pushed), &copied) ||
            copied != sizeof(pushed))
            return false;
        context.Rsp -= sizeof(value);
    }
    else if (pop) {
        if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(location), &value, sizeof(value),
                                &copied) ||
            copied != sizeof(value))
            return false;
        context.Rsp += sizeof(value);
    }
    context.Rip =
        found->operation == ZYDIS_MNEMONIC_CALL || found->operation == ZYDIS_MNEMONIC_JMP ? value : found->next;
    return true;
}
}
