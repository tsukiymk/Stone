#include "../../../src/windows/ipc/HostState.h"
#include <cstdio>
#include <stdexcept>
using namespace stone::windows;

namespace {
void Require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

void Codec()
{
    HostSnapshot state;
    state.owner = 42;
    state.network = 7;
    state.battery = {1, 1, 81, 0};
    state.powerSamples = 123;
    state.networkSamples = 456;
    state.systemPowerRequests = 2;
    state.ui = {L"zh-CN,en-US", true};
    const auto encoded = EncodeHostState(state);
    Require(EncodeHostState(DecodeHostState(encoded)) == encoded, "Host snapshot roundtrip lost data");
    state.wifi.state = 3;
    state.wifi.connected = true;
    state.wifi.interfaceIndex = 7;
    state.wifi.samples = 89;
    state.wifi.ipv4 = 0x0100007f;
    state.wifi.netmask = 0x00ffffff;
    state.wifi.gateway = 0x01010101;
    state.wifi.dns1 = 0x08080808;
    state.wifi.dns2 = UINT32_MAX;
    const auto wifi = EncodeHostState(state, true);
    Require(EncodeHostState(DecodeHostState(wifi), true) == wifi, "Wi-Fi snapshot lost fields or unsigned IPv4 bits");
    state.memory = {1024, 8192, 512, 16384};
    const auto memory = EncodeHostState(state, true, true);
    Require(EncodeHostState(DecodeHostState(memory), true, true) == memory, "Memory snapshot lost fields");
    auto invalidMemory = memory;
    const auto memoryAt = invalidMemory.find(L"|1024|8192|512|16384|");
    Require(memoryAt != invalidMemory.npos, "Memory snapshot fixture");
    invalidMemory.replace(memoryAt + 1, 4, L"99999");
    bool memoryRejected = false;
    try {
        DecodeHostState(invalidMemory);
    }
    catch (const std::exception&) {
        memoryRejected = true;
    }
    Require(memoryRejected, "Impossible available memory accepted");
    const auto legacy = DecodeHostState(encoded);
    Require(legacy.wifi.state == 4 && legacy.wifi.samples == 0 && legacy.network == 7,
            "Legacy host state invented Wi-Fi data");
    for (const auto& invalid :
         {L"", L"0|1|1|7|0|1|1|81|0|1|en-US", L"1|1|1|8|0|1|1|81|0|1|en-US", L"1|1|1|7|0|256|1|81|0|1|en-US",
          L"1|1|1|7|0|1|1|81|0|2|en-US", L"1|1|1|7|0|1|1|81|0|1|", L"1|1|1|7|0|1|1|81|0|1|en-US|garbage",
          L"1|1|1|7|4294967296|1|1|81|0|1|en-US"}) {
        bool rejected = false;
        try {
            DecodeHostState(invalid);
        }
        catch (const std::exception&) {
            rejected = true;
        }
        Require(rejected, "Malformed host snapshot was accepted");
    }
    for (const auto& invalid :
         {L"v2|42|1|1|7|0|1|1|81|0|1|5|0|7|0|0|0|0|0|1|en-US", L"v2|42|1|1|7|0|1|1|81|0|1|3|2|7|0|0|0|0|0|1|en-US",
          L"v2|42|1|1|7|0|1|1|81|0|1|3|1|0|0|0|0|0|0|1|en-US", L"v2|42|1|1|7|0|1|1|81|0|1|1|1|7|0|0|0|0|0|1|en-US",
          L"v2|42|1|1|7|0|1|1|81|0|1|3|1|7|4294967296|0|0|0|0|1|en-US"}) {
        bool rejected = false;
        try {
            DecodeHostState(invalid);
        }
        catch (const std::exception&) {
            rejected = true;
        }
        Require(rejected, "Invalid Wi-Fi snapshot was accepted");
    }
}

void Interfaces()
{
    const std::wstring encoded = L"n1|42|1|0|1|lo|1|73|1500|1|2|8|0|7f000001|";
    auto decoded = DecodeHostInterfaces(encoded, 42);
    Require(decoded && EncodeHostInterfaces(42, *decoded) == encoded, "Interface snapshot roundtrip failed");
    Require(!DecodeHostInterfaces(L"42|1|1|7|0|1|1|81|0|1|en-US", 42), "Legacy Service invented interface data");
    for (const auto& invalid :
         {L"", L"n1|43|1|0|0|", L"n1|42|1|0|129|", L"n1|42|1|0|1|bad name|1|1|1500|0|", L"n1|42|1|0|1|lo|0|1|1500|0|",
          L"n1|42|1|0|1|lo|1|4|1500|0|", L"n1|42|1|0|1|lo|1|1|1500|1|2|33|0|7f000001|",
          L"n1|42|1|0|1|lo|1|1|1500|1|2|8|1|7f000001|", L"n1|42|1|0|1|lo|1|1|1500|1|2|8|0|7g000001|",
          L"n1|42|1|5|1|lo|1|1|1500|0|", L"n1|42|0|0|0|", L"n1|42|1|0|0|extra|"}) {
        bool rejected = false;
        try {
            DecodeHostInterfaces(invalid, 42);
        }
        catch (const std::exception&) {
            rejected = true;
        }
        Require(rejected, "Malformed interface snapshot accepted");
    }
}
}

int main()
{
    try {
        Codec();
        Interfaces();
        std::puts("PASS IPC host-state/interface codec roundtrips, legacy versions and malformed payload rejection");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL IPC host state: %s\n", error.what());
        return 1;
    }
}
