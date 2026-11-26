#pragma once

#include <windows.h>
#include <cstdint>

namespace stone::windows {
void SetServiceSystemAwake(DWORD client, bool required);
uint32_t ServiceSystemAwakeCount();
void StopServicePowerRequests();
}
