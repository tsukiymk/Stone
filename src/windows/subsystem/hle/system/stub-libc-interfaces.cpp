#include "stub-libc-interfaces.h"
#include "../../../client/BrokerClient.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "abi/AndroidLibraryImports.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace stone::windows {
namespace {
struct InterfaceNode {
    AndroidIfaddrs info;
    char name[16];
    alignas(8) uint8_t address[128], mask[128], broadcast[128];
};

struct Allocation {
    void* block;
    bool arm;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<AndroidIfaddrs*, Allocation> blocks;
};

Registry& Allocations()
{
    static auto* registry = new Registry;
    return *registry;
}

void Release(Allocation allocation)
{
    if (allocation.arm)
        FreeArm64GuestMemory(reinterpret_cast<uintptr_t>(allocation.block));
    else
        std::free(allocation.block);
}

std::shared_ptr<const HostInterfaces> Snapshot()
{
    try {
        const auto interfaces = ReadClientHostState().interfaces;
        if (!interfaces || interfaces->error) {
            errno = interfaces ? interfaces->error : 11;
            return {};
        }
        return interfaces;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return {};
    }
}

void Address(InterfaceNode& node, const HostInterfaceAddress& address, bool broadcast)
{
    const uint16_t family = static_cast<uint16_t>(address.family);
    std::memcpy(node.address, &family, 2);
    std::memcpy(node.mask, &family, 2);
    const size_t count = family == 2 ? 4 : 16, offset = family == 2 ? 4 : 8;
    std::memcpy(node.address + offset, address.bytes.data(), count);
    if (family == 10)
        std::memcpy(node.address + 24, &address.scope, 4);
    for (size_t at = 0; at < count; ++at) {
        const int bits = std::clamp(static_cast<int>(address.prefix) - static_cast<int>(at * 8), 0, 8);
        node.mask[offset + at] = bits ? static_cast<uint8_t>(0xffu << (8 - bits)) : 0;
    }
    node.info.address = node.address;
    node.info.netmask = node.mask;
    if (family == 2 && broadcast) {
        std::memcpy(node.broadcast, &family, 2);
        for (size_t at = 0; at < 4; ++at)
            node.broadcast[4 + at] = node.address[4 + at] | static_cast<uint8_t>(~node.mask[4 + at]);
        node.info.broadcast = node.broadcast;
    }
}
}

int AndroidGetIfAddrs(AndroidIfaddrs** output)
{
    if (!output) {
        errno = EFAULT;
        return -1;
    }
    *output = nullptr;
    const auto snapshot = Snapshot();
    if (!snapshot)
        return -1;
    size_t count = 0;
    for (const auto& entry : snapshot->entries)
        count += std::max<size_t>(1, entry.addresses.size());
    if (!count)
        return 0;
    const bool arm = IsArm64TranslationEnabled();
    Allocation allocation{arm ? reinterpret_cast<void*>(AllocateArm64GuestMemory(count * sizeof(InterfaceNode), false))
                              : std::calloc(count, sizeof(InterfaceNode)),
                          arm};
    if (!allocation.block) {
        errno = ENOMEM;
        return -1;
    }
    const auto cleanup = [](void* pointer) {
        const auto* item = static_cast<Allocation*>(pointer);
        Release(*item);
    };
    std::unique_ptr<void, decltype(cleanup)> guard(&allocation, cleanup);
    std::memset(allocation.block, 0, count * sizeof(InterfaceNode));
    auto* nodes = static_cast<InterfaceNode*>(allocation.block);
    size_t at = 0;
    for (const auto& entry : snapshot->entries) {
        for (size_t item = 0; item < std::max<size_t>(1, entry.addresses.size()); ++item, ++at) {
            auto& node = nodes[at];
            std::memcpy(node.name, entry.name.c_str(), entry.name.size() + 1);
            node.info.name = node.name;
            node.info.flags = entry.flags;
            if (at + 1 < count)
                node.info.next = &nodes[at + 1].info;
            if (!entry.addresses.empty())
                Address(node, entry.addresses[item], (entry.flags & 2) != 0);
        }
    }
    try {
        auto& registry = Allocations();
        const std::lock_guard lock(registry.mutex);
        registry.blocks.emplace(&nodes[0].info, allocation);
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
    *output = &nodes[0].info;
    guard.release();
    return 0;
}

void AndroidFreeIfAddrs(AndroidIfaddrs* head)
{
    const int saved = errno;
    Allocation allocation{};
    {
        auto& registry = Allocations();
        const std::lock_guard lock(registry.mutex);
        const auto found = registry.blocks.find(head);
        if (found == registry.blocks.end())
            return;
        allocation = found->second;
        registry.blocks.erase(found);
    }
    Release(allocation);
    errno = saved;
}

char* AndroidInterfaceName(unsigned index, char* name)
{
    if (!name) {
        errno = EFAULT;
        return nullptr;
    }
    const auto snapshot = Snapshot();
    if (!snapshot)
        return nullptr;
    for (const auto& entry : snapshot->entries)
        if (entry.index == index) {
            std::memcpy(name, entry.name.c_str(), entry.name.size() + 1);
            return name;
        }
    errno = ENXIO;
    return nullptr;
}

unsigned AndroidInterfaceIndex(const char* name)
{
    if (!name) {
        errno = EFAULT;
        return 0;
    }
    const auto snapshot = Snapshot();
    if (!snapshot)
        return 0;
    const auto size = strnlen(name, 16);
    if (size < 16)
        for (const auto& entry : snapshot->entries)
            if (entry.name == name)
                return entry.index;
    errno = ENODEV;
    return 0;
}

bool AndroidInterfaceIoctl(unsigned long request, void* argument, int& result)
{
    if (request != 0x8913 && request != 0x8921 && request != 0x8933 && request != 0x8915 && request != 0x891b &&
        request != 0x8919)
        return false;
    result = -1;
    if (!argument) {
        errno = EFAULT;
        return true;
    }
    const auto snapshot = Snapshot();
    if (!snapshot)
        return true;
    auto* bytes = static_cast<uint8_t*>(argument);
    const auto* name = static_cast<const char*>(argument);
    if (strnlen(name, 16) < 16)
        for (const auto& entry : snapshot->entries) {
            if (entry.name != name)
                continue;
            if (request == 0x8913) {
                const auto flags = static_cast<uint16_t>(entry.flags);
                std::memcpy(bytes + 16, &flags, 2);
            }
            else if (request == 0x8921 || request == 0x8933) {
                if (request == 0x8921 && entry.mtu > INT_MAX) {
                    errno = 75;
                    return true;
                }
                const uint32_t value = request == 0x8921 ? entry.mtu : entry.index;
                std::memcpy(bytes + 16, &value, 4);
            }
            else {
                const auto address =
                    std::find_if(entry.addresses.begin(), entry.addresses.end(), [](const auto& value) {
                        return value.family == 2;
                    });
                if (address == entry.addresses.end() || (request == 0x8919 && !(entry.flags & 2))) {
                    errno = 99;
                    return true;
                }
                InterfaceNode node{};
                Address(node, *address, (entry.flags & 2) != 0);
                std::memcpy(bytes + 16,
                            request == 0x8915   ? node.address
                            : request == 0x891b ? node.mask
                                                : node.broadcast,
                            16);
            }
            result = 0;
            return true;
        }
    errno = ENODEV;
    return true;
}
}
