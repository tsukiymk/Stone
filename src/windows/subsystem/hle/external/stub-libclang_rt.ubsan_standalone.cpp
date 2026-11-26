#include "stub-libclang_rt.ubsan_standalone.h"
#include "loader/ElfImage.h"
#include "hle/system/stub-libc.h"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>

namespace stone::windows {
namespace {
// Android 37 Google Play x86_64 compiler-rt, build ID
// f52a119338b344d371e59c0bf08717555ec6fd82. The hash covers the entire ELF.
constexpr std::array<uint8_t, 32> RuntimeHash{
    0xfd, 0x9a, 0x14, 0x60, 0x67, 0x05, 0x85, 0x30, 0xf3, 0x6c, 0x0f, 0x9d, 0x7b, 0x98, 0xac, 0xbc,
    0x97, 0xd3, 0x64, 0x3f, 0x82, 0x75, 0x3d, 0xd8, 0x98, 0x8d, 0xbd, 0x24, 0x71, 0xe2, 0xc3, 0x4c,
};

struct SyscallSite {
    uint64_t address;
    unsigned number;
};

constexpr SyscallSite Sites[]{
    {0x1baff, 9},   {0x1bb15, 11},  {0x1bb28, 3},   {0x1bb4e, 257}, {0x1bb72, 0},   {0x1bb95, 1},   {0x1bba8, 32},
    {0x1bbbd, 292}, {0x1bbc5, 24},  {0x1bc0c, 35},  {0x1bc15, 59},  {0x1bc29, 231}, {0x1bc9d, 262}, {0x1bd21, 262},
    {0x1bd55, 186}, {0x1c038, 202}, {0x1c058, 202}, {0x1c071, 61},  {0x1c085, 39},  {0x1c0a5, 157}, {0x1c0bc, 56},
    {0x1c0f3, 267}, {0x1c1f3, 267}, {0x1c30a, 14},  {0x1c347, 14},
};
}

bool PrepareUbsanRuntime(std::string_view soname, std::span<const uint8_t> file, ElfImage& image)
{
    if (soname != "libclang_rt.ubsan_standalone-x86_64-android.so")
        return false;
    std::array<uint8_t, RuntimeHash.size()> hash{};
    if (file.size() > ULONG_MAX ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(file.data()),
                   static_cast<ULONG>(file.size()), hash.data(), static_cast<ULONG>(hash.size())) != 0 ||
        hash != RuntimeHash) {
        throw std::runtime_error("unknown Android UBSan runtime syscall layout");
    }
    for (const auto& site : Sites)
        image.TrapLinuxSyscall(site.address);
    return true;
}

bool DispatchUbsanSyscall(const ElfImage& image, CONTEXT& context) noexcept
{
    if (!image.ContainsCode(context.Rip, 2))
        return false;
    const auto offset = context.Rip - image.LoadBias();
    const auto site = std::ranges::lower_bound(Sites, offset, {}, &SyscallSite::address);
    if (site == std::end(Sites) || site->address != offset || site->number != context.Rax)
        return false;
    const auto savedErrno = errno;
    const auto savedError = GetLastError();
    try {
        context.Rax =
            AndroidX64Syscall(site->number, context.Rdi, context.Rsi, context.Rdx, context.R10, context.R8, context.R9);
    }
    catch (const std::bad_alloc&) {
        context.Rax = static_cast<uint64_t>(-ENOMEM);
    }
    catch (const std::exception&) {
        context.Rax = static_cast<uint64_t>(-EIO);
    }
    errno = savedErrno;
    SetLastError(savedError);
    context.Rip += 2;
    context.Rcx = context.Rip;
    context.R11 = context.EFlags;
    return true;
}
}
