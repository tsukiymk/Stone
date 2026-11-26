#pragma once

#include "../../service/WindowsNotifications.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace stone::windows {
struct ArtNotification : WindowsNotification {
    std::wstring channel;
    uint32_t flags = 0;
    std::vector<std::wstring> targets;
};

// The isolated probe alone supplies diagnosticHost; production uses StoneService.exe above settings/.
void ConfigureArtNotifications(const std::filesystem::path& settings, const std::filesystem::path& diagnosticHost = {});
bool InitializeArtNotifications(const std::wstring& packageName);
bool ArtNotificationsAllowed();
std::vector<std::wstring> PostArtNotification(const ArtNotification& notification);
void RemoveArtNotification(const std::wstring& key);
void RemoveArtNotifications();
std::vector<int64_t> PollArtNotifications();
std::vector<std::wstring> ReadArtNotificationHistory();
void CleanupArtNotificationTestIdentity();
}
