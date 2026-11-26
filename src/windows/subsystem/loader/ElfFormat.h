#pragma once

#include <cstddef>
#include <cstdint>

// ELF64 on-disk records from the System V ABI. These contain only file-format
// fields and are shared by Stone's x86_64 and AArch64 loaders.
namespace stone::elf {
struct Elf64Ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct Elf64Sym {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

struct Elf64Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

struct Elf64Rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
};

static_assert(sizeof(Elf64Ehdr) == 64 && offsetof(Elf64Ehdr, e_phoff) == 32);
static_assert(sizeof(Elf64Phdr) == 56 && offsetof(Elf64Phdr, p_vaddr) == 16);
static_assert(sizeof(Elf64Sym) == 24 && offsetof(Elf64Sym, st_value) == 8);
static_assert(sizeof(Elf64Rela) == 24);
static_assert(sizeof(Elf64Shdr) == 64);
}
