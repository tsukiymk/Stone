#include "HostServices.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <algorithm>
#include <climits>
#include <cstring>

namespace stone::windows {
HostInterfaces ReadWindowsInterfaces()
{
    HostInterfaces result;
    ULONG bytes = 16384;
    for (unsigned attempt = 0; attempt < 3 && bytes <= 1024 * 1024; ++attempt) {
        std::vector<uint8_t> storage(bytes);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        const auto code =
            GetAdaptersAddresses(AF_UNSPEC,
                                 GAA_FLAG_INCLUDE_ALL_INTERFACES | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                     GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME,
                                 nullptr, adapters, &bytes);
        if (code == ERROR_BUFFER_OVERFLOW)
            continue;
        if (code == ERROR_NO_DATA) {
            result.error = 0;
            return result;
        }
        if (code != NO_ERROR) {
            result.error = code == ERROR_NOT_ENOUGH_MEMORY ? 12 : 5;
            return result;
        }
        size_t addresses = 0;
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            NET_IFINDEX index = 0;
            if (ConvertInterfaceLuidToIndex(&adapter->Luid, &index) != NO_ERROR) {
                result.entries.clear();
                result.error = 5;
                return result;
            }
            if (!index || index > INT_MAX || result.entries.size() >= 128) {
                result.entries.clear();
                result.error = 75;
                return result;
            }
            HostInterface entry;
            entry.index = index;
            entry.mtu = adapter->Mtu;
            char name[IF_MAX_STRING_SIZE + 1]{};
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                entry.name = "lo";
            else if (ConvertInterfaceLuidToNameA(&adapter->Luid, name, sizeof(name)) == NO_ERROR &&
                     std::strlen(name) <= 15 &&
                     std::string_view(name).find_first_not_of(
                         "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string_view::npos)
                entry.name = name;
            else
                entry.name = "if" + std::to_string(index);
            MIB_IF_ROW2 row{};
            row.InterfaceLuid = adapter->Luid;
            if (GetIfEntry2(&row) != NO_ERROR) {
                result.entries.clear();
                result.error = 5;
                return result;
            }
            if (row.AdminStatus == NET_IF_ADMIN_STATUS_UP)
                entry.flags |= 1;
            if (row.OperStatus == IfOperStatusUp)
                entry.flags |= 0x40;
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                entry.flags |= 8;
            else if (adapter->IfType == IF_TYPE_PPP)
                entry.flags |= 0x10;
            else if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211)
                entry.flags |= 2;
            if (!(adapter->Flags & IP_ADAPTER_NO_MULTICAST))
                entry.flags |= 0x1000;
            for (auto* value = adapter->FirstUnicastAddress; value; value = value->Next) {
                if (!value->Address.lpSockaddr ||
                    (value->DadState != IpDadStatePreferred && value->DadState != IpDadStateDeprecated))
                    continue;
                HostInterfaceAddress address;
                address.prefix = value->OnLinkPrefixLength;
                const auto* socket = value->Address.lpSockaddr;
                if (socket->sa_family == AF_INET && value->Address.iSockaddrLength >= sizeof(sockaddr_in) &&
                    address.prefix <= 32) {
                    address.family = 2;
                    std::memcpy(address.bytes.data(), &reinterpret_cast<const sockaddr_in*>(socket)->sin_addr, 4);
                }
                else if (socket->sa_family == AF_INET6 && value->Address.iSockaddrLength >= sizeof(sockaddr_in6) &&
                         address.prefix <= 128) {
                    address.family = 10;
                    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(socket);
                    address.scope = v6->sin6_scope_id;
                    std::memcpy(address.bytes.data(), &v6->sin6_addr, 16);
                }
                else
                    continue;
                if (++addresses > 256) {
                    result.entries.clear();
                    result.error = 75;
                    return result;
                }
                entry.addresses.push_back(address);
            }
            result.entries.push_back(std::move(entry));
        }
        std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
            return a.index < b.index;
        });
        result.error = 0;
        return result;
    }
    result.error = 5;
    return result;
}
}
