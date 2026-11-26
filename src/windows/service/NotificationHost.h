#pragma once

#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include "PendingIntents.h"
#include "JobProfiles.h"

namespace stone::windows {
std::vector<std::wstring> ServiceNotificationRequest(DWORD client, const std::filesystem::path& settings,
                                                     const std::vector<std::wstring>& fields, PendingIntents& store,
                                                     const std::wstring& scope);
bool ServiceNotificationPolicy(const std::filesystem::path& data, const std::wstring& package);
void RestoreServiceNotifications(PendingIntents& store, const JobProfiles& profiles,
                                 const std::filesystem::path& settings);
std::vector<std::wstring> ServiceNotificationDismissals();
void AcceptServiceNotification(PendingIntents& store, const JobProfile& profile, const std::wstring& route, int action);
void RemoveServiceNotifications(const std::filesystem::path& androidData, const std::wstring& package);
void StopServiceNotifications() noexcept;
}
