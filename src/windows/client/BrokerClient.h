#pragma once

#include "../ipc/HostState.h"
#include <filesystem>
#include <vector>
#include <span>
#include <cstdint>

namespace stone::windows {
void ConfigureClientService(const std::filesystem::path& executable);
void RegisterBrokerClientPackage(const std::wstring& packageName) noexcept;
void InitializeClientServices();
bool StopClientServices() noexcept; // Orderly diagnostic shutdown; cannot be reinitialized.
HostSnapshot ReadClientHostState(); // Cache only: never performs an IPC or Windows query.
void SetClientSystemAwake(bool required);
std::vector<std::wstring> ClientJobRequest(const std::vector<std::wstring>& fields);
bool ClientJobsEnabled();
void EnableClientJobs();
void RemoveClientPackageJobs(const std::filesystem::path& data, const std::wstring& packageName);
void InitializeClientSession();
int ReadClientSessionCommands();
bool RetireClientSession(); // Off the ART main thread, after pausing job polling.
void ConfigureClientLaunchProfile(const std::filesystem::path& prepared, const std::filesystem::path& apk,
                                  const std::filesystem::path& origin, const std::wstring& expectedPackage,
                                  const std::wstring& imageStamp = {}, const std::wstring& token = {},
                                  const std::wstring& process = {});
std::wstring ClientApplicationProcess();
void ValidateClientLaunchPackage(const std::wstring& package);
void RegisterClientLaunchProfile(const std::wstring& package);
bool ClientWakeRequested();
bool ClientNotificationHostEnabled();
std::vector<std::wstring> ClientNotificationRequest(const std::vector<std::wstring>& fields, uint32_t* owner = nullptr);
std::vector<std::wstring> ClientActivityRequest(const std::vector<std::wstring>& fields);
std::vector<std::wstring> ClientClipboardRequest(const std::vector<std::wstring>& fields);
std::wstring ClientApplicationLogRequest(const std::vector<std::wstring>& fields);
bool ClientClipboardEnabled();
void ClientOpenWebUrl(const std::vector<std::wstring>& fields);
using HostChangedCallback = void (*)() noexcept;
void SetClientHostChangedCallback(HostChangedCallback callback);
void ConfigureClientShell(const std::filesystem::path& executable, const std::filesystem::path& data,
                          const std::filesystem::path& image, bool application = false);
bool ClientShellEnabled();
void UpdateClientShortcut(const std::wstring& name, const std::wstring& label, bool installed);
std::vector<std::wstring> UpdateClientApplicationIcon(const std::wstring& name, const std::wstring& label,
                                                      std::span<const uint8_t> png);
void EnsureClientNotificationIdentity(const std::filesystem::path& settings, const std::filesystem::path& diagnostic);
void CleanupClientNotificationTestIdentity(const std::filesystem::path& settings,
                                           const std::filesystem::path& diagnostic);
}
