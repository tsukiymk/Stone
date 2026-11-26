#pragma once

#include "HostServices.h"
#include <windows.h>
#include <string>
#include <vector>

namespace stone::windows {
std::vector<std::wstring> ServiceJobRequest(DWORD client, const std::vector<std::wstring>& fields,
                                            const HostSnapshot& host, uint64_t now,
                                            const HostJobState& conditions = {});
uint64_t JobClockMillis();
void MaintainServiceJobs(uint64_t now);
// Called while the package manager holds its removal/execution locks. A failure
// leaves its durable removal marker in place so the operation can be retried.
void RemoveServicePackageJobs(const std::filesystem::path& data, const std::wstring& package, uint64_t now);
bool ServiceJobClientIdle(DWORD client, const HostSnapshot& host, uint64_t now, const HostJobState& conditions);
bool ServiceJobsReady(const std::filesystem::path& data, const std::filesystem::path& system,
                      const std::wstring& package, const HostSnapshot& host, uint64_t now,
                      const HostJobState& conditions);
}
