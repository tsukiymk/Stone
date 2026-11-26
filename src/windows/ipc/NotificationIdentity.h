#pragma once

#include <filesystem>
#include <string>

namespace stone::windows {
struct NotificationIdentity {
    std::wstring appId, protocol;
};

NotificationIdentity MakeNotificationIdentity(const std::filesystem::path& settings,
                                              const std::filesystem::path& diagnostic);
void RegisterNotificationIdentity(const std::filesystem::path& settings, const std::filesystem::path& diagnostic);
void RemoveDiagnosticNotificationIdentity(const std::filesystem::path& settings,
                                          const std::filesystem::path& diagnostic);
}
