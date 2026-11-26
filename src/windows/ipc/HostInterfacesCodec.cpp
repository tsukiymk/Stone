#include "HostState.h"
#include <climits>
#include <stdexcept>
#include <unordered_set>

namespace stone::windows {
std::wstring EncodeHostInterfaces(uint32_t owner, const HostInterfaces& interfaces)
{
    std::wstring output = L"n1|";
    const auto field = [&](uint64_t value) {
        output += std::to_wstring(value) + L'|';
    };
    field(owner);
    field(interfaces.samples);
    field(interfaces.error);
    field(interfaces.entries.size());
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const auto& entry : interfaces.entries) {
        output.append(entry.name.begin(), entry.name.end());
        output += L'|';
        field(entry.index);
        field(entry.flags);
        field(entry.mtu);
        field(entry.addresses.size());
        for (const auto& address : entry.addresses) {
            field(address.family);
            field(address.prefix);
            field(address.scope);
            for (size_t at = 0; at < (address.family == 2 ? 4u : 16u); ++at) {
                output += hex[address.bytes[at] >> 4];
                output += hex[address.bytes[at] & 15];
            }
            output += L'|';
        }
    }
    if (output.size() > 32767)
        throw std::runtime_error("Host interface snapshot exceeds IPC limit");
    return output;
}

std::shared_ptr<const HostInterfaces> DecodeHostInterfaces(const std::wstring& text, uint32_t owner)
{
    if (!text.starts_with(L"n1|")) {
        if (DecodeHostState(text).owner != owner)
            throw std::runtime_error("Legacy host interface provider mismatch");
        return {}; // A legacy Service returns its ordinary host snapshot.
    }
    if (text.size() > 32767)
        throw std::runtime_error("Oversized interface snapshot");
    size_t at = 3;
    const auto field = [&]() {
        const auto end = text.find(L'|', at);
        if (end == text.npos || end == at)
            throw std::runtime_error("Invalid interface snapshot field");
        auto value = text.substr(at, end - at);
        at = end + 1;
        return value;
    };
    const auto number = [&](uint64_t max) {
        const auto value = field();
        if (value.size() > 20 || value.find_first_not_of(L"0123456789") != value.npos)
            throw std::runtime_error("Invalid interface snapshot number");
        const auto parsed = std::stoull(value);
        if (parsed > max)
            throw std::runtime_error("Interface snapshot number outside range");
        return parsed;
    };
    if (!owner || number(UINT32_MAX) != owner)
        throw std::runtime_error("Interface snapshot owner mismatch");
    auto result = std::make_shared<HostInterfaces>();
    result->samples = number(UINT64_MAX);
    result->error = static_cast<int>(number(75));
    if (result->error != 0 && result->error != 5 && result->error != 11 && result->error != 12 && result->error != 75)
        throw std::runtime_error("Invalid interface snapshot error");
    const auto count = number(128);
    size_t addresses = 0;
    std::unordered_set<uint32_t> indexes;
    std::unordered_set<std::string> names;
    for (size_t index = 0; index < count; ++index) {
        HostInterface entry;
        const auto name = field();
        if (name.size() > 15 ||
            name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != name.npos)
            throw std::runtime_error("Invalid interface name");
        entry.name.assign(name.begin(), name.end());
        entry.index = static_cast<uint32_t>(number(INT_MAX));
        entry.flags = static_cast<uint32_t>(number(0x105b));
        entry.mtu = static_cast<uint32_t>(number(UINT32_MAX));
        if (!entry.index || (entry.flags & ~0x105bu) || !indexes.insert(entry.index).second ||
            !names.insert(entry.name).second)
            throw std::runtime_error("Invalid or duplicate interface");
        const auto size = number(256);
        addresses += size;
        if (addresses > 256)
            throw std::runtime_error("Too many interface addresses");
        for (size_t item = 0; item < size; ++item) {
            HostInterfaceAddress address;
            address.family = static_cast<int>(number(10));
            if (address.family != 2 && address.family != 10)
                throw std::runtime_error("Invalid interface address family");
            address.prefix = static_cast<uint32_t>(number(address.family == 2 ? 32 : 128));
            address.scope = static_cast<uint32_t>(number(UINT32_MAX));
            const auto bytes = field();
            if (bytes.size() != (address.family == 2 ? 8u : 32u) || (address.family == 2 && address.scope))
                throw std::runtime_error("Invalid interface address length/scope");
            const auto digit = [](wchar_t ch) -> unsigned {
                if (ch >= L'0' && ch <= L'9')
                    return ch - L'0';
                if (ch >= L'a' && ch <= L'f')
                    return ch - L'a' + 10;
                throw std::runtime_error("Invalid interface address hex");
            };
            for (size_t byte = 0; byte < bytes.size() / 2; ++byte)
                address.bytes[byte] = static_cast<uint8_t>((digit(bytes[byte * 2]) << 4) | digit(bytes[byte * 2 + 1]));
            entry.addresses.push_back(address);
        }
        result->entries.push_back(std::move(entry));
    }
    if (at != text.size() || (result->error && !result->entries.empty()) || (!result->samples && !result->error))
        throw std::runtime_error("Invalid interface snapshot completion");
    return result;
}
}
