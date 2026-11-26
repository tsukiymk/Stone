#pragma once

#include "../ipc/HostState.h"
#include <filesystem>

namespace stone::windows {
HostWifiState ReadWindowsWifi();
HostInterfaces ReadWindowsInterfaces();
void StartHostServices();
HostSnapshot ReadServiceHostState();

struct HostJobState {
    bool userAbsent = false, storageKnown = false;
    uint64_t idleMillis = 0, storageFree = 0, storageTotal = 0;
};

HostJobState ReadServiceJobState(const std::filesystem::path& data);
}
