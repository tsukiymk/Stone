#pragma once

#include "platform/windows/GuestAllocation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace stone::windows {
struct GuestHeap {
    struct Bounds {
        uintptr_t base = 0;
        size_t size = 0;

        bool Contains(uintptr_t address, size_t bytes) const
        {
            const auto offset = address - base;
            return offset < size && bytes <= size - offset;
        }
    };

    std::mutex mutex;
    std::map<uintptr_t, size_t> allocations;
    size_t allocatedBytes = 0, peakAllocatedBytes = 0;
    std::atomic<uint64_t> freeGeneration{0};

    uint8_t* At(uintptr_t address, size_t bytes)
    {
        struct Cache {
            uint64_t generation = 0;
            std::array<Bounds, 4> allocations{};
            size_t next = 0;
        };

        thread_local Cache cache;
        // A cached range stays valid until a free. The guest must synchronize
        // using an allocation with freeing it, as for a full map lookup.
        if (const auto generation = freeGeneration.load(std::memory_order_acquire); cache.generation != generation)
            cache = Cache{generation};
        for (const auto& bounds : cache.allocations)
            if (bounds.Contains(address, bytes))
                return reinterpret_cast<uint8_t*>(address);
        const std::lock_guard lock(mutex);
        auto next = allocations.upper_bound(address);
        if (next == allocations.begin())
            return nullptr;
        const auto [base, size] = *std::prev(next);
        const Bounds bounds{base, size};
        if (!bounds.Contains(address, bytes))
            return nullptr;
        cache.allocations[cache.next++ % cache.allocations.size()] = bounds;
        return reinterpret_cast<uint8_t*>(address);
    }

    uintptr_t Allocate(size_t bytes, bool clear, size_t alignment = 16)
    {
        bytes = std::max(bytes, size_t{1});
        if (bytes > SIZE_MAX - alignment - sizeof(void*))
            return 0;
        std::unique_ptr<void, decltype(&FreeWindowsGuestAllocation)> memory(
            AllocateWindowsGuestAllocation(bytes, alignment), FreeWindowsGuestAllocation);
        if (!memory)
            return 0;
        if (clear)
            std::memset(memory.get(), 0, bytes);
        const auto address = reinterpret_cast<uintptr_t>(memory.get());
        const std::lock_guard lock(mutex);
        allocations.emplace(address, bytes);
        allocatedBytes += bytes;
        peakAllocatedBytes = std::max(peakAllocatedBytes, allocatedBytes);
        memory.release();
        return address;
    }

    bool Free(uintptr_t address)
    {
        if (!address)
            return true;
        const std::lock_guard lock(mutex);
        const auto entry = allocations.find(address);
        if (entry == allocations.end())
            return false;
        allocatedBytes -= entry->second;
        allocations.erase(entry);
        freeGeneration.fetch_add(1, std::memory_order_release);
        FreeWindowsGuestAllocation(reinterpret_cast<void*>(address));
        return true;
    }

    uintptr_t AllocateLibc(size_t bytes, bool clear, size_t alignment = 16)
    {
        // Bionic's allocator exposes size classes. ART's bit-table reader can
        // fetch the final machine word of such a slot. Own and publish the
        // rounded bytes explicitly; JNI buffers retain their exact bounds.
        constexpr size_t quantum = 16;
        if (bytes > SIZE_MAX - (quantum - 1))
            return 0;
        const auto capacity = (std::max(bytes, size_t{1}) + quantum - 1) & ~(quantum - 1);
        const auto address = Allocate(capacity, clear, alignment);
        if (address && !clear && bytes < capacity)
            std::memset(reinterpret_cast<uint8_t*>(address) + bytes, 0, capacity - bytes);
        return address;
    }

    size_t AllocationSize(uintptr_t address)
    {
        const std::lock_guard lock(mutex);
        const auto found = allocations.find(address);
        return found == allocations.end() ? 0 : found->second;
    }

    std::array<uint64_t, 10> Statistics()
    {
        const std::lock_guard lock(mutex);
        std::array<uint64_t, 10> result{};
        // Slot bytes owned by this guest allocator, not the Windows CRT
        // heap, Java managed heap, or independent mmap regions.
        result[0] = result[7] = allocatedBytes; // arena, uordblks
        result[5] = peakAllocatedBytes;         // usmblks
        return result;
    }
};

GuestHeap& Arm64GuestHeap();
}
