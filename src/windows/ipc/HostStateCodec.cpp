#include "HostState.h"
#include <stdexcept>
#include <vector>

namespace stone::windows {
std::wstring EncodeHostState(const HostSnapshot& state, bool includeWifi, bool includeMemory)
{
    includeWifi = includeWifi || includeMemory;
    auto text = (includeMemory ? L"v3|"
                 : includeWifi ? L"v2|"
                               : L"") +
                std::to_wstring(state.owner) + L"|" + std::to_wstring(state.powerSamples) + L"|" +
                std::to_wstring(state.networkSamples) + L"|" + std::to_wstring(state.network) + L"|" +
                std::to_wstring(state.systemPowerRequests);
    for (int value : state.battery)
        text += L"|" + std::to_wstring(value);
    if (includeWifi) {
        text += L"|" + std::to_wstring(state.wifi.samples) + L"|" + std::to_wstring(state.wifi.state) + L"|" +
                (state.wifi.connected ? L"1" : L"0");
        for (auto value : {state.wifi.interfaceIndex, state.wifi.ipv4, state.wifi.netmask, state.wifi.gateway,
                           state.wifi.dns1, state.wifi.dns2})
            text += L"|" + std::to_wstring(value);
    }
    if (includeMemory)
        for (const auto value : state.memory)
            text += L"|" + std::to_wstring(value);
    return text + (state.ui.dark ? L"|1|" : L"|0|") + state.ui.languageTags;
}

HostSnapshot DecodeHostState(const std::wstring& text)
{
    HostSnapshot result;
    const bool memory = text.starts_with(L"v3|");
    const bool wifi = memory || text.starts_with(L"v2|");
    size_t at = wifi ? 3 : 0;
    auto number = [&]() -> uint64_t {
        auto end = text.find(L'|', at);
        if (end == text.npos || end == at || end - at > 20)
            throw std::runtime_error("Invalid host state field");
        auto field = text.substr(at, end - at);
        if (field.find_first_not_of(L"0123456789") != field.npos)
            throw std::runtime_error("Invalid host state number");
        at = end + 1;
        return std::stoull(field);
    };
    auto bounded = [&](uint64_t limit) -> int {
        auto value = number();
        if (value > limit)
            throw std::runtime_error("Host state outside its range");
        return static_cast<int>(value);
    };
    auto owner = number();
    if (!owner || owner > UINT32_MAX)
        throw std::runtime_error("Invalid host owner");
    result.owner = static_cast<uint32_t>(owner);
    result.powerSamples = number();
    result.networkSamples = number();
    result.network = bounded(7);
    auto requests = number();
    if (requests > UINT32_MAX)
        throw std::runtime_error("Invalid power request count");
    result.systemPowerRequests = static_cast<uint32_t>(requests);
    for (auto& value : result.battery)
        value = bounded(255);
    if (wifi) {
        result.wifi.samples = number();
        result.wifi.state = bounded(4);
        result.wifi.connected = bounded(1) != 0;
        for (auto* value : {&result.wifi.interfaceIndex, &result.wifi.ipv4, &result.wifi.netmask, &result.wifi.gateway,
                            &result.wifi.dns1, &result.wifi.dns2}) {
            const auto parsed = number();
            if (parsed > UINT32_MAX)
                throw std::runtime_error("Invalid host Wi-Fi field");
            *value = static_cast<uint32_t>(parsed);
        }
        if (result.wifi.connected && (result.wifi.state != 3 || !result.wifi.interfaceIndex))
            throw std::runtime_error("Invalid connected Wi-Fi state");
    }
    if (memory) {
        for (auto& value : result.memory) {
            value = number();
            if (value > INT64_MAX)
                throw std::runtime_error("Invalid memory size");
        }
        if (result.memory[0] > result.memory[1] || result.memory[2] > result.memory[1] ||
            result.memory[3] < result.memory[1])
            throw std::runtime_error("Invalid memory snapshot");
    }
    result.ui.dark = bounded(1) != 0;
    result.ui.languageTags = text.substr(at);
    if (result.ui.languageTags.empty() || result.ui.languageTags.size() > 2048 ||
        result.ui.languageTags.find_first_of(L"|\r\n") != text.npos)
        throw std::runtime_error("Invalid host language tags");
    return result;
}
}
