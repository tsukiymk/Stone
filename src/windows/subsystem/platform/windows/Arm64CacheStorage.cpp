#include "platform/windows/Arm64CacheStorage.h"
#include <windows.h>
#include <wincrypt.h>
#include <compressapi.h>
#include <bcrypt.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace stone::windows::arm64_cache {
namespace fs = std::filesystem;

struct Timer {
    uint64_t& ticks;
    uint64_t start;

    ~Timer()
    {
        if (start)
            ticks += Arm64CacheTimingElapsed(start);
    }
};

struct Handle {
    HANDLE value;

    explicit Handle(HANDLE h) : value(h == INVALID_HANDLE_VALUE ? nullptr : h)
    {
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    ~Handle()
    {
        if (value)
            CloseHandle(value);
    }
};

class Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

public:
    Hash()
    {
        Check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0);
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::bad_alloc();
        }
    }

    ~Hash()
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }

    void Add(std::span<const uint8_t> bytes)
    {
        Check(bytes.size() <= ULONG_MAX &&
              BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0);
    }

    Digest Finish()
    {
        Digest result{};
        Check(BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) >= 0);
        return result;
    }
};

Digest HashFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Check(bool(input));
    Hash hash;
    std::array<uint8_t, 65536> bytes;
    while (input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || input.gcount())
        hash.Add({bytes.data(), static_cast<size_t>(input.gcount())});
    Check(input.eof());
    return hash.Finish();
}

std::wstring Name(const Digest& digest)
{
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name;
    for (auto byte : digest) {
        name += hex[byte >> 4];
        name += hex[byte & 15];
    }
    return name + L".stbc";
}

struct CacheFile {
    Handle handle;
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_ID_INFO identity{};
    bool identified = false;

    explicit CacheFile(HANDLE h) : handle(h)
    {
    }
};

bool Identified(const std::shared_ptr<CacheFile>& file)
{
    return file && file->identified;
}

namespace {
constexpr uint32_t PackedMagic = 0x5a434253; // SBCZ, inside the DPAPI envelope.
constexpr size_t MaximumPlain = MaximumImage + 65536;

uint32_t Word(std::span<const uint8_t> bytes, size_t at)
{
    Check(at <= bytes.size() && bytes.size() - at >= 4);
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= uint32_t{bytes[at + i]} << (i * 8);
    return value;
}

void Word(std::vector<uint8_t>& bytes, size_t at, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<uint8_t>(value >> (i * 8));
}
}

std::vector<uint8_t> Pack(std::vector<uint8_t> plain)
{
    Check(!plain.empty() && plain.size() <= MaximumPlain);
    COMPRESSOR_HANDLE handle = nullptr;
    Check(CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &handle) != FALSE);

    struct Close {
        COMPRESSOR_HANDLE h;

        ~Close()
        {
            CloseCompressor(h);
        }
    } close{handle};

    SIZE_T needed = 0;
    Check(!Compress(handle, plain.data(), plain.size(), nullptr, 0, &needed) &&
          GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    // The size query returns a sufficient bound, not the achieved ratio.
    Check(needed && needed <= MaximumPlain + MaximumPlain / 8 + 65536);
    std::vector<uint8_t> packed(16 + needed);
    SIZE_T written = 0;
    Check(Compress(handle, plain.data(), plain.size(), packed.data() + 16, needed, &written) != FALSE &&
          written <= needed);
    if (written + 16 >= plain.size())
        return plain;
    packed.resize(16 + written);
    Word(packed, 0, PackedMagic);
    Word(packed, 4, 1);
    Word(packed, 8, static_cast<uint32_t>(plain.size()));
    Word(packed, 12, static_cast<uint32_t>(written));
    return packed;
}

std::vector<uint8_t> Unpack(std::vector<uint8_t> plain)
{
    Check(plain.size() <= MaximumPlain);
    if (plain.size() < 4 || Word(plain, 0) != PackedMagic)
        return plain;
    Check(plain.size() >= 16 && Word(plain, 4) == 1);
    const auto size = Word(plain, 8), payload = Word(plain, 12);
    Check(size && size <= MaximumPlain && payload && payload == plain.size() - 16);
    DECOMPRESSOR_HANDLE handle = nullptr;
    Check(CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &handle) != FALSE);

    struct Close {
        DECOMPRESSOR_HANDLE h;

        ~Close()
        {
            CloseDecompressor(h);
        }
    } close{handle};

    std::vector<uint8_t> decoded(size);
    SIZE_T written = 0;
    Check(Decompress(handle, plain.data() + 16, payload, decoded.data(), decoded.size(), &written) != FALSE &&
          written == size);
    return decoded;
}

std::vector<uint8_t> Protect(std::span<const uint8_t> input, const Digest& key, bool encrypt,
                             Arm64DiskCacheStatistics& stats)
{
    const Timer timer{encrypt ? stats.protectTicks : stats.unprotectTicks, Arm64CacheTimingStart()};
    Check(!input.empty() && input.size() <= MaximumImage + 65536);
    DATA_BLOB source{static_cast<DWORD>(input.size()), const_cast<BYTE*>(input.data())};
    DATA_BLOB entropy{static_cast<DWORD>(key.size()), const_cast<BYTE*>(key.data())}, output{};
    Check((encrypt ? CryptProtectData(&source, L"Stone ARM64 native blocks", &entropy, nullptr, nullptr,
                                      CRYPTPROTECT_UI_FORBIDDEN, &output)
                   : CryptUnprotectData(&source, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                                        &output)) != FALSE);
    const std::unique_ptr<void, decltype(&LocalFree)> owner(output.pbData, LocalFree);
    Check(output.cbData > 0 && output.cbData <= MaximumImage + 65536);
    return {output.pbData, output.pbData + output.cbData};
}

std::shared_ptr<CacheFile> Open(const fs::path& file)
{
    Handle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.value) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            return {};
        throw std::runtime_error("cache open failed");
    }
    auto input = std::make_shared<CacheFile>(handle.value);
    handle.value = nullptr;
    Check(GetFileInformationByHandle(input->handle.value, &input->info) &&
          !(input->info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
          !input->info.nFileSizeHigh && input->info.nFileSizeLow > 0 &&
          input->info.nFileSizeLow <= MaximumImage + 65536);
    // Filesystems without stable file IDs retain the full merge/read path.
    input->identified = GetFileInformationByHandleEx(input->handle.value, FileIdInfo, &input->identity,
                                                     sizeof(input->identity)) != FALSE;
    return input;
}

bool SameFile(const std::shared_ptr<CacheFile>& first, const std::shared_ptr<CacheFile>& second)
{
    return first && second && first->identified && second->identified &&
           first->identity.VolumeSerialNumber == second->identity.VolumeSerialNumber &&
           !std::memcmp(&first->identity.FileId, &second->identity.FileId, sizeof(FILE_ID_128)) &&
           first->info.nFileSizeLow == second->info.nFileSizeLow &&
           first->info.ftLastWriteTime.dwLowDateTime == second->info.ftLastWriteTime.dwLowDateTime &&
           first->info.ftLastWriteTime.dwHighDateTime == second->info.ftLastWriteTime.dwHighDateTime;
}

std::shared_ptr<Bundle> Read(std::shared_ptr<CacheFile> input, const Digest& key, Arm64DiskCacheStatistics& stats,
                             bool pinBaselines)
{
    if (!input)
        return {};
    std::vector<uint8_t> encrypted(input->info.nFileSizeLow);
    DWORD read = 0;
    Check(ReadFile(input->handle.value, encrypted.data(), static_cast<DWORD>(encrypted.size()), &read, nullptr) &&
          read == encrypted.size());
    auto plain = Protect(encrypted, key, false, stats);
    const Timer timer{stats.parseTicks, Arm64CacheTimingStart()};
    auto result = Decode(Unpack(std::move(plain)), key);
    if (pinBaselines && Identified(input))
        result->baseline = std::move(input);
    return result;
}

std::shared_ptr<CacheFile> Write(const fs::path& root, const Bundle& bundle, Arm64DiskCacheStatistics& stats,
                                 bool& pinBaselines, const std::function<void()>& releaseBaselines)
{
    const auto serializeStart = Arm64CacheTimingStart();
    auto plain = Encode(bundle);
    if (serializeStart)
        stats.serializeTicks += Arm64CacheTimingElapsed(serializeStart);
    wchar_t compression[2]{};
    if (GetEnvironmentVariableW(L"STONE_ARM64_CACHE_COMPRESSION", compression, 2) == 1 && compression[0] == L'1')
        plain = Pack(std::move(plain));
    auto bytes = Protect(plain, bundle.key, true, stats);
    const auto destination = root / Name(bundle.key);
    const auto stage =
        root / (L".write-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));

    struct Remove {
        fs::path path;

        ~Remove()
        {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    } remove{stage};

    const auto name = destination.wstring();
    // The Win32 wrapper also consumes a terminating NUL even though the
    // FileNameLength field itself excludes that terminator.
    std::vector<uint8_t> renameBytes(offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t));
    auto& rename = *reinterpret_cast<FILE_RENAME_INFO*>(renameBytes.data());
    rename.Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename.FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename.FileName, name.data(), rename.FileNameLength);
    DWORD renameError = 0;
    {
        Handle output(CreateFileW(stage.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        Check(output.value &&
              WriteFile(output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size() && FlushFileBuffers(output.value));
        // Preserve readers of the previous file while atomically publishing
        // the new name. MoveFileEx cannot replace a pinned destination.
        if (!SetFileInformationByHandle(output.value, FileRenameInfoEx, renameBytes.data(),
                                        static_cast<DWORD>(renameBytes.size())))
            renameError = GetLastError();
    }
    if (renameError) {
        if (renameError == ERROR_INVALID_PARAMETER || renameError == ERROR_NOT_SUPPORTED ||
            renameError == ERROR_CALL_NOT_IMPLEMENTED) {
            // On older/filesystem-specific implementations retain the former
            // authenticated read/merge path without our long-lived file pins.
            pinBaselines = false;
            releaseBaselines();
            if (!MoveFileExW(stage.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                renameError = GetLastError();
            else
                renameError = 0;
        }
        if (renameError)
            throw std::runtime_error("native cache atomic replacement failed, Windows error " +
                                     std::to_string(renameError));
    }
    if (!pinBaselines)
        return {};
    auto baseline = Open(destination);
    Check(bool(baseline));
    return Identified(baseline) ? baseline : nullptr;
}

Digest TranslatorIdentity()
{
    std::wstring executable(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Check(size && size < executable.size());
    executable.resize(size);
    Hash identity;
    const auto binary = HashFile(executable);
    identity.Add(binary);
    // Generated code currently requires x64/SSE2; key all CPU feature bits
    // conservatively, and bind context layout/emitter changes to this binary.
    std::array<int, 12> cpu{};
    __cpuid(cpu.data(), 0);
    __cpuid(cpu.data() + 4, 1);
    __cpuidex(cpu.data() + 8, 7, 0);
    cpu[5] &= 0x00ffffff; // Initial APIC ID varies with the scheduling core.
    identity.Add({reinterpret_cast<const uint8_t*>(cpu.data()), sizeof(cpu)});
    return identity.Finish();
}

Digest SourceKey(const Digest& translator, std::span<const uint8_t> file)
{
    Hash hash;
    hash.Add(translator);
    hash.Add(file);
    return hash.Finish();
}

std::shared_ptr<void> AcquireWriter(const fs::path& root)
{
    auto writer = std::make_shared<Handle>(CreateFileW((root / L".writer.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                                       0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    return writer->value ? writer : nullptr;
}

std::vector<StoredFile> ListFiles(const fs::path& root)
{
    std::vector<StoredFile> files;
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = entry.path().filename().wstring();
        if (name.size() != 69 || !name.ends_with(L".stbc") ||
            name.substr(0, 64).find_first_not_of(L"0123456789abcdef") != name.npos || !entry.is_regular_file() ||
            entry.is_symlink())
            continue;
        Digest key{};
        const auto digit = [](wchar_t value) {
            return value <= L'9' ? value - L'0' : value - L'a' + 10;
        };
        for (size_t i = 0; i < key.size(); ++i)
            key[i] = static_cast<uint8_t>((digit(name[i * 2]) << 4) | digit(name[i * 2 + 1]));
        const auto size = entry.file_size();
        files.push_back({entry.path(), entry.last_write_time(), size, key});
    }
    return files;
}

bool RemoveFile(const fs::path& file)
{
    return fs::remove(file);
}
}
