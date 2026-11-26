#include "ApplicationLogs.h"
#include "../ipc/ServiceProtocol.h"
#include "../host/CommandLine.h"
#include "../host/WindowsPath.h"
#include "../host/UserPaths.h"
#include <bcrypt.h>
#include <array>
#include <fstream>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;
using ipc::Check;
constexpr size_t Maximum = 512000;

fs::path Path(const fs::path& data, const std::wstring& package)
{
    Check(data.is_absolute() && stone::session::IsPackageName(package), "Invalid application log identity");
    auto identity = ipc::Normalize(data);
    std::array<unsigned char, 32> digest{};
    Check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(identity.data()),
                     static_cast<ULONG>(identity.size() * sizeof(wchar_t)), digest.data(),
                     static_cast<ULONG>(digest.size())) >= 0,
          "Cannot identify application log data root");
    std::wstring key;
    for (auto byte : digest) {
        key += L"0123456789abcdef"[byte >> 4];
        key += L"0123456789abcdef"[byte & 15];
    }
    return ExtendedWindowsPath(UserLogDirectory() / L"Android" / key / (package + L".log"));
}

std::wstring Read(const fs::path& file)
{
    if (!fs::exists(file))
        return {};
    Check(fs::file_size(file) <= Maximum * 4, "Application log journal exceeds its limit");
    std::ifstream input(file, std::ios::binary);
    Check(static_cast<bool>(input), "Cannot open application log journal");
    std::string bytes(Maximum * 4 + 1, '\0');
    input.read(bytes.data(), bytes.size());
    bytes.resize(static_cast<size_t>(input.gcount()));
    Check(!input.bad() && bytes.size() <= Maximum * 4, "Cannot read bounded application log journal");
    if (bytes.empty())
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    Check(size > 0 && size <= Maximum, "Invalid application log journal");
    std::wstring text(size, L'\0');
    Check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(),
                              size) == size,
          "Cannot decode application log journal");
    Check(text.find(L'\0') == text.npos, "Invalid application log text");
    return text;
}

void Write(const fs::path& file, const std::wstring& text)
{
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    Check(size > 0, "Cannot encode application log journal");
    std::string bytes(size, '\0');
    Check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), bytes.data(),
                              size, nullptr, nullptr) == size,
          "Cannot encode application log journal");
    fs::create_directories(file.parent_path());
    auto temporary = file;
    temporary += L".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), bytes.size());
        output.close();
        Check(static_cast<bool>(output), "Cannot write application log journal");
        Check(MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
              "Cannot replace application log journal");
    }
    catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}
}

std::wstring ApplicationLogRequest(const fs::path& data, const std::wstring& package,
                                   const std::vector<std::wstring>& fields)
{
    const auto file = Path(data, package);
    if (fields.size() == 1 && fields[0] == L"dump")
        return Read(file);
    Check(fields.size() == 2 && fields[0] == L"append" && !fields[1].empty() && fields[1].size() <= 16000 &&
              fields[1].back() == L'\n' && fields[1].find(L'\0') == fields[1].npos,
          "Invalid application log batch");
    auto text = Read(file);
    text += fields[1];
    if (text.size() > Maximum) {
        // Retain complete lines; never split a UTF-16 surrogate pair or UTF-8 record.
        auto boundary = text.find(L'\n', text.size() - Maximum - 1);
        text.erase(0, boundary + 1);
    }
    Write(file, text);
    return {};
}

void RemoveApplicationLogs(const fs::path& data, const std::wstring& package)
{
    auto file = Path(data, package);
    fs::remove(file);
    file += L".tmp";
    fs::remove(file);
}
}
