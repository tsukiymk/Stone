#pragma once

#include <string>

namespace stone::windows {
void StartClientApplicationLogs();
void QueueClientApplicationLog(std::wstring record) noexcept;
void FlushClientApplicationLogs();
std::wstring ReadClientApplicationLogs();
void StopClientApplicationLogs() noexcept;
}
