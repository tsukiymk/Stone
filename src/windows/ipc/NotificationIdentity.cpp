#include "NotificationIdentity.h"
#include "ServiceProtocol.h"
#include <bcrypt.h>
#include <array>

namespace stone::windows {
NotificationIdentity MakeNotificationIdentity(const std::filesystem::path& settings,
                                              const std::filesystem::path& diagnostic)
{
    // Preserve the original flat settings identity and Windows notification permissions.
    auto text = (std::filesystem::canonical(settings).parent_path().parent_path() / L"StoneSettings.exe").wstring();
    if (!diagnostic.empty())
        text += L"\n" + std::filesystem::canonical(diagnostic).wstring();
    std::array<unsigned char, 32> bytes{};
    ipc::Check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(text.data()),
                          static_cast<ULONG>(text.size() * sizeof(wchar_t)), bytes.data(),
                          static_cast<ULONG>(bytes.size())) >= 0,
               "Cannot identify notification sender");
    std::wstring hash;
    for (auto byte : bytes) {
        hash += L"0123456789abcdef"[byte >> 4];
        hash += L"0123456789abcdef"[byte & 15];
    }
    return {L"Stone.Runtime." + hash.substr(0, 24), L"stone-notify-" + hash.substr(0, 16)};
}
}
