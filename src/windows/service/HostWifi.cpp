#include "HostServices.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wlanapi.h>
#include <iphlpapi.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace stone::windows {
namespace {
struct WlanHandle {
    HANDLE value = nullptr;

    ~WlanHandle()
    {
        if (value)
            WlanCloseHandle(value, nullptr);
    }
};

struct WlanMemory {
    void* value = nullptr;

    ~WlanMemory()
    {
        if (value)
            WlanFreeMemory(value);
    }
};

int RadioState(HANDLE handle, const WLAN_INTERFACE_INFO& info)
{
    // Association is itself proof that the radio is enabled. Other states need
    // the hardware/software radio query; disconnected does not mean switched off.
    if (info.isState == wlan_interface_state_connected || info.isState == wlan_interface_state_ad_hoc_network_formed)
        return 3;
    DWORD bytes = 0;
    WLAN_OPCODE_VALUE_TYPE source{};
    WlanMemory data;
    if (WlanQueryInterface(handle, &info.InterfaceGuid, wlan_intf_opcode_radio_state, nullptr, &bytes, &data.value,
                           &source) != ERROR_SUCCESS ||
        bytes < sizeof(DWORD))
        return 4;
    const auto* radio = static_cast<const WLAN_RADIO_STATE*>(data.value);
    if (radio->dwNumberOfPhys > WLAN_MAX_PHY_INDEX ||
        bytes < offsetof(WLAN_RADIO_STATE, PhyRadioState) + radio->dwNumberOfPhys * sizeof(WLAN_PHY_RADIO_STATE))
        return 4;
    bool disabled = false;
    for (DWORD index = 0; index < radio->dwNumberOfPhys; ++index) {
        const auto& phy = radio->PhyRadioState[index];
        if (phy.dot11HardwareRadioState == dot11_radio_state_on && phy.dot11SoftwareRadioState == dot11_radio_state_on)
            return 3;
        disabled |= phy.dot11HardwareRadioState == dot11_radio_state_off ||
                    phy.dot11SoftwareRadioState == dot11_radio_state_off;
    }
    return disabled ? 1 : 4;
}

uint32_t Address(const SOCKET_ADDRESS& address)
{
    return address.lpSockaddr && address.iSockaddrLength >= sizeof(sockaddr_in) &&
                   address.lpSockaddr->sa_family == AF_INET
               ? reinterpret_cast<const sockaddr_in*>(address.lpSockaddr)->sin_addr.s_addr
               : 0;
}

void IpConfiguration(const NET_LUID& luid, HostWifiState& result)
{
    ULONG bytes = 16384;
    for (unsigned attempt = 0; attempt < 3 && bytes <= 1024 * 1024; ++attempt) {
        std::vector<uint8_t> storage(bytes);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        const ULONG code = GetAdaptersAddresses(AF_INET,
                                                GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                                    GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_FRIENDLY_NAME,
                                                nullptr, adapters, &bytes);
        if (code == ERROR_BUFFER_OVERFLOW)
            continue;
        if (code != NO_ERROR)
            return;
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->Luid.Value != luid.Value)
                continue;
            for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
                const auto ip = Address(entry->Address);
                if (!ip || entry->OnLinkPrefixLength > 32 ||
                    (entry->DadState != IpDadStatePreferred && entry->DadState != IpDadStateDeprecated))
                    continue;
                result.ipv4 = ip;
                result.netmask = entry->OnLinkPrefixLength ? htonl(0xffffffffu << (32 - entry->OnLinkPrefixLength)) : 0;
                break;
            }
            for (auto* entry = adapter->FirstGatewayAddress; entry && !result.gateway; entry = entry->Next)
                result.gateway = Address(entry->Address);
            for (auto* entry = adapter->FirstDnsServerAddress; entry; entry = entry->Next) {
                const auto ip = Address(entry->Address);
                if (!ip)
                    continue;
                if (!result.dns1)
                    result.dns1 = ip;
                else if (ip != result.dns1) {
                    result.dns2 = ip;
                    break;
                }
            }
            return;
        }
        return;
    }
}
}

HostWifiState ReadWindowsWifi()
{
    HostWifiState result;
    WlanHandle handle;
    DWORD negotiated = 0;
    if (WlanOpenHandle(2, nullptr, &negotiated, &handle.value) != ERROR_SUCCESS)
        return result;
    WlanMemory memory;
    if (WlanEnumInterfaces(handle.value, nullptr, reinterpret_cast<WLAN_INTERFACE_INFO_LIST**>(&memory.value)) !=
        ERROR_SUCCESS)
        return result;
    const auto* list = static_cast<const WLAN_INTERFACE_INFO_LIST*>(memory.value);
    if (!list || list->dwNumberOfItems > 128)
        return result;
    const WLAN_INTERFACE_INFO* selected = nullptr;
    int best = -1;
    for (DWORD index = 0; index < list->dwNumberOfItems; ++index) {
        const auto& info = list->InterfaceInfo[index];
        const int state = RadioState(handle.value, info);
        const bool connected = info.isState == wlan_interface_state_connected ||
                               info.isState == wlan_interface_state_ad_hoc_network_formed;
        const int score = connected ? 3 : state == 3 ? 2 : state == 1 ? 1 : 0;
        // Stable selection when several adapters have equal state.
        if (score < best || (score == best && selected &&
                             std::memcmp(&selected->InterfaceGuid, &info.InterfaceGuid, sizeof(GUID)) <= 0))
            continue;
        selected = &info;
        best = score;
        result.state = state;
        result.connected = connected;
    }
    if (!selected)
        return result;
    NET_LUID luid{};
    NET_IFINDEX index = 0;
    if (ConvertInterfaceGuidToLuid(&selected->InterfaceGuid, &luid) != NO_ERROR ||
        ConvertInterfaceLuidToIndex(&luid, &index) != NO_ERROR)
        return HostWifiState{};
    result.interfaceIndex = index;
    if (result.connected)
        IpConfiguration(luid, result);
    return result;
}
}
