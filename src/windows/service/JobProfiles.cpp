#include "JobProfiles.h"
#include "../ipc/ServiceProtocol.h"
#include "../host/CommandLine.h"
#include "../host/WindowsPath.h"
#include "../host/UserPaths.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <array>
#include <cstring>
#include <fstream>
#include <objbase.h>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;
using ipc::Check;
constexpr uint64_t Magic = 0x3152464f52504aULL;
constexpr size_t Maximum = 8388608;

void Put(std::vector<unsigned char>& bytes, uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<unsigned char>(value >> shift));
}

void Put(std::vector<unsigned char>& bytes, const std::wstring& value)
{
    Check(value.size() <= 32767 && value.find(L'\0') == value.npos, "Invalid background profile text");
    Check(bytes.size() + 8 + value.size() * sizeof(wchar_t) <= Maximum, "Background profile catalog is too large");
    Put(bytes, value.size());
    auto begin = reinterpret_cast<const unsigned char*>(value.data());
    bytes.insert(bytes.end(), begin, begin + value.size() * sizeof(wchar_t));
}

struct Reader {
    std::vector<unsigned char> bytes;
    size_t at = 0;

    uint64_t Number(uint64_t maximum = UINT64_MAX)
    {
        Check(bytes.size() - at >= 8, "Truncated background profile");
        uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8)
            value |= static_cast<uint64_t>(bytes[at++]) << shift;
        Check(value <= maximum, "Invalid background profile size");
        return value;
    }

    std::wstring Text()
    {
        auto count = static_cast<size_t>(Number(32767));
        Check(bytes.size() - at >= count * sizeof(wchar_t), "Truncated profile path");
        std::wstring value(count, L'\0');
        std::memcpy(value.data(), bytes.data() + at, count * sizeof(wchar_t));
        at += count * sizeof(wchar_t);
        Check(value.find(L'\0') == value.npos, "Invalid profile path");
        return value;
    }
};

}

fs::path JobProfileStorePath(const fs::path& directory)
{
    ipc::Identity identity(directory);
    auto key = identity.mutex.substr(identity.mutex.find_last_of(L'.') + 1);
    return UserStateDirectory() / L"Service" / key / L"job-profiles.bin";
}

JobProfiles::JobProfiles(fs::path file) : file_(ExtendedWindowsPath(std::move(file)))
{
    if (!fs::exists(file_))
        return;
    auto length = fs::file_size(file_);
    Check(length <= Maximum, "Background profile catalog is too large");
    Reader reader;
    reader.bytes.resize(static_cast<size_t>(length));
    std::ifstream input(file_, std::ios::binary);
    Check(input.read(reinterpret_cast<char*>(reader.bytes.data()), static_cast<std::streamsize>(length)).good(),
          "Cannot read profile catalog");
    Check(reader.Number() == Magic, "Unknown background profile catalog");
    auto count = reader.Number(1024);
    for (uint64_t index = 0; index < count; ++index) {
        std::vector<std::wstring> fields;
        for (int field = 0; field < 9; ++field)
            fields.push_back(reader.Text());
        auto profile = DecodeJobProfile(fields);
        Check(profiles_.emplace(profile.Key(), std::move(profile)).second, "Duplicate background profile");
    }
    Check(reader.at == reader.bytes.size(), "Trailing background profile data");
}

void JobProfiles::Save(const std::map<std::wstring, JobProfile>& profiles) const
{
    Check(profiles.size() <= 1024, "Too many background application profiles");
    std::vector<unsigned char> bytes;
    Put(bytes, Magic);
    Put(bytes, profiles.size());
    for (const auto& [key, profile] : profiles)
        for (const auto& field : profile.Fields())
            Put(bytes, field);
    Check(bytes.size() <= Maximum, "Background profile catalog is too large");
    fs::create_directories(file_.parent_path());
    auto temporary = file_;
    temporary += L".tmp";
    {
        ipc::Handle output(
            CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        Check(output.value &&
                  WriteFile(output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                  written == bytes.size() && FlushFileBuffers(output.value),
              "Cannot save background profile catalog");
    }
    Check(MoveFileExW(temporary.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
          "Cannot publish background profile catalog");
}

JobProfile JobProfiles::Register(const std::vector<std::wstring>& fields)
{
    Check(fields.size() == 8, "Invalid Client launch profile fields");
    auto values = fields;
    auto requestedToken = values.back();
    values.pop_back();
    for (int index : {0, 1, 2, 3})
        if (!values[index].empty())
            values[index] = fs::canonical(ExtendedWindowsPath(values[index])).wstring();
    Check(JobImageStamp(values[1]) == values[6], "System image changed while Client was starting");
    values.push_back(values[3].empty() ? L"" : LaunchCodeDigest(values[3]));
    std::array<unsigned char, 16> random{};
    Check(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >=
              0,
          "Cannot identify background launch authorization");
    std::wstring token;
    for (auto byte : random) {
        token += L"0123456789abcdef"[byte >> 4];
        token += L"0123456789abcdef"[byte & 15];
    }
    values.push_back(requestedToken.empty() ? token : requestedToken);
    auto profile = DecodeJobProfile(values);
    profile.ValidateForLaunch();
    auto previous = profiles_.find(profile.Key());
    if (!requestedToken.empty()) {
        Check(previous != profiles_.end() && previous->second.Fields() == profile.Fields(),
              "Background launch was revoked or superseded");
        return previous->second;
    }
    if (previous != profiles_.end()) {
        profile.token = previous->second.token;
        if (previous->second.Fields() == profile.Fields())
            return previous->second;
        profile.token = token;
    }
    auto updated = profiles_;
    updated.insert_or_assign(profile.Key(), profile);
    Save(updated);
    profiles_ = std::move(updated);
    return profile;
}

void JobProfiles::Remove(const fs::path& androidData, const std::wstring& package)
{
    auto updated = profiles_;
    for (auto it = updated.begin(); it != updated.end();) {
        if (it->second.package == package && ipc::Normalize(it->second.data / L"data") == ipc::Normalize(androidData))
            it = updated.erase(it);
        else
            ++it;
    }
    if (updated.size() != profiles_.size()) {
        Save(updated);
        profiles_ = std::move(updated);
    }
}
}
