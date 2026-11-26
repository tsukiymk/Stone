#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace stone::windows {
struct HostUiConfiguration {
    std::wstring languageTags;
    bool dark;
};

struct HostWifiState {
    int state = 4; // Android WIFI_STATE_UNKNOWN until a real WLAN sample is available.
    bool connected = false;
    uint32_t interfaceIndex = 0, ipv4 = 0, netmask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    uint64_t samples = 0;
};

struct HostInterfaceAddress {
    int family = 0; // Linux AF_INET=2, AF_INET6=10; address bytes in network order.
    uint32_t prefix = 0, scope = 0;
    std::array<uint8_t, 16> bytes{};
};

struct HostInterface {
    std::string name;
    uint32_t index = 0, flags = 0, mtu = 0;
    std::vector<HostInterfaceAddress> addresses;
};

struct HostInterfaces {
    uint64_t samples = 0;
    int error = 11; // EAGAIN until the Service has sampled the interface table.
    std::vector<HostInterface> entries;
};

std::wstring EncodeHostInterfaces(uint32_t owner, const HostInterfaces& interfaces);
std::shared_ptr<const HostInterfaces> DecodeHostInterfaces(const std::wstring& text, uint32_t owner);

struct HostSnapshot {
    HostUiConfiguration ui{L"en-US", false};
    std::array<int, 4> battery{255, 255, 255, 255};
    int network = 0;
    HostWifiState wifi;
    std::shared_ptr<const HostInterfaces> interfaces;
    uint32_t owner = 0;
    uint32_t systemPowerRequests = 0;
    uint64_t powerSamples = 0, networkSamples = 0;
    // available, usable total, Stone low-memory threshold, installed bytes.
    std::array<uint64_t, 4> memory{};
};

std::wstring EncodeHostState(const HostSnapshot& state, bool includeWifi = false, bool includeMemory = false);
HostSnapshot DecodeHostState(const std::wstring& text);
}
