#include "LaunchProfile.h"
#include "ServiceProtocol.h"
#include "../host/CommandLine.h"
#include "../host/WindowsPath.h"
#include <bcrypt.h>
#include <objbase.h>
#include <array>
#include <fstream>

namespace stone::windows {
namespace fs = std::filesystem;
using ipc::Check;

std::wstring LaunchCodeDigest(const fs::path& file)
{
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE value = nullptr;

        ~Hash()
        {
            if (value)
                BCryptDestroyHash(value);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } hash;

    Check(BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(hash.algorithm, &hash.value, nullptr, 0, nullptr, 0, 0) >= 0,
          "Cannot hash background APK");
    std::ifstream input(ExtendedWindowsPath(file), std::ios::binary);
    Check(static_cast<bool>(input), "Cannot read background APK");
    std::array<char, 65536> buffer;
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
        Check(BCryptHashData(hash.value, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(input.gcount()),
                             0) >= 0,
              "Cannot hash background APK data");
    Check(input.eof(), "Cannot finish reading background APK");
    std::array<unsigned char, 32> bytes{};
    Check(BCryptFinishHash(hash.value, bytes.data(), static_cast<ULONG>(bytes.size()), 0) >= 0,
          "Cannot finish APK hash");
    std::wstring result;
    for (auto byte : bytes) {
        result += L"0123456789abcdef"[byte >> 4];
        result += L"0123456789abcdef"[byte & 15];
    }
    return result;
}

JobProfile DecodeJobProfile(const std::vector<std::wstring>& fields)
{
    Check(fields.size() == 9 && stone::session::IsPackageName(fields[5]), "Invalid background application profile");
    JobProfile result{fields[0], fields[1], fields[2], fields[3], fields[4],
                      fields[5], fields[6], fields[7], fields[8]};
    Check(result.data.is_absolute() && result.image.is_absolute() && result.prepared.is_absolute() &&
              (result.apk.empty() || result.apk.is_absolute()) &&
              (result.source.empty() || result.source.is_absolute()),
          "Profile paths must be absolute");
    Check(!result.imageStamp.empty() &&
              (result.apk.empty() ? result.apkDigest.empty()
                                  : result.apkDigest.size() == 64 && result.apkDigest.find_first_not_of(
                                                                         L"0123456789abcdef") == result.apkDigest.npos),
          "Invalid profile code identity");
    Check(result.token.size() == 32 && result.token.find_first_not_of(L"0123456789abcdef") == result.token.npos,
          "Invalid background launch token");
    return result;
}

std::string StageArtLaunchCode(const fs::path& source, const fs::path& root, std::string_view extension)
{
    Check(extension == ".apk" || extension == ".jar", "Invalid ART code extension");
    const auto hash = LaunchCodeDigest(source);
    std::string hex;
    for (auto ch : hash)
        hex += static_cast<char>(ch);
    const auto guest = "/stone/code/" + hex + std::string(extension);
    const auto target = (ExtendedWindowsPath(root) / fs::path(guest.substr(1))).make_preferred();
    fs::create_directories(target.parent_path());
    if (fs::is_regular_file(target) && LaunchCodeDigest(target) == hash)
        return guest;
    GUID id{};
    Check(SUCCEEDED(CoCreateGuid(&id)), "Cannot identify ART code staging file");
    wchar_t identifier[40]{};
    Check(StringFromGUID2(id, identifier, 40) != 0, "Cannot format ART code staging name");
    const auto stage = target.parent_path() / identifier;
    try {
        fs::copy_file(ExtendedWindowsPath(source), stage);
        Check(LaunchCodeDigest(stage) == hash, "ART code changed while staging");
        auto flags = GetFileAttributesW(stage.c_str());
        Check(flags != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(stage.c_str(), flags | FILE_ATTRIBUTE_READONLY),
              "Cannot make ART code read-only");
        if (!MoveFileExW(stage.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            Check(fs::is_regular_file(target) && LaunchCodeDigest(target) == hash, "Cannot publish immutable ART code");
            SetFileAttributesW(stage.c_str(), FILE_ATTRIBUTE_NORMAL);
            fs::remove(stage);
        }
    }
    catch (...) {
        SetFileAttributesW(stage.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::error_code ignored;
        fs::remove(stage, ignored);
        throw;
    }
    return guest;
}

std::wstring JobProfile::Key() const
{
    return ipc::Normalize(data / L"data") + L"\n" + ipc::Normalize(prepared / L"system") + L"\n" + package;
}

std::vector<std::wstring> JobProfile::Fields() const
{
    return {data.wstring(), image.wstring(), prepared.wstring(), apk.wstring(), source.wstring(),
            package,        imageStamp,      apkDigest,          token};
}

std::wstring JobImageStamp(const fs::path& image)
{
    auto path = ExtendedWindowsPath(image);
    return std::to_wstring(fs::file_size(path)) + L":" +
           std::to_wstring(fs::last_write_time(path).time_since_epoch().count());
}

void JobProfile::ValidateForLaunch() const
{
    Check(fs::is_directory(ExtendedWindowsPath(data / L"data")) &&
              fs::is_directory(ExtendedWindowsPath(prepared / L"system")),
          "Background application paths are unavailable");
    Check(JobImageStamp(image) == imageStamp, "Selected system image changed since job submission");
    if (!apk.empty())
        Check(LaunchCodeDigest(apk) == apkDigest, "Background APK code changed since job submission");
}
}
