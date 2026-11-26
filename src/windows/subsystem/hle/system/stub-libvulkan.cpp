#include "stub-libvulkan.h"
#include "abi/AndroidLibraryImports.h"
#include "../../../client/Window.h"
#include "stub-libandroid.h"

#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stone::windows {
namespace {
enum class Scope {
    Global,
    Instance,
    Device,
    Resource,
    Swapchain,
    Commands,
    Descriptor,
    Draw,
    Query
};

struct Command {
    std::string_view name;
    Scope scope;
};

constexpr Command commands[]{
    {"vkGetInstanceProcAddr", Scope::Global},
    {"vkEnumerateInstanceVersion", Scope::Global},
    {"vkEnumerateInstanceExtensionProperties", Scope::Global},
    {"vkEnumerateInstanceLayerProperties", Scope::Global},
    {"vkCreateInstance", Scope::Global},
    {"vkDestroyInstance", Scope::Instance},
    {"vkEnumeratePhysicalDevices", Scope::Instance},
    {"vkEnumerateDeviceExtensionProperties", Scope::Instance},
    {"vkEnumerateDeviceLayerProperties", Scope::Instance},
    {"vkGetPhysicalDeviceFeatures", Scope::Instance},
    {"vkGetPhysicalDeviceFeatures2", Scope::Instance},
    {"vkGetPhysicalDeviceFeatures2KHR", Scope::Instance},
    {"vkGetPhysicalDeviceProperties", Scope::Instance},
    {"vkGetPhysicalDeviceProperties2", Scope::Instance},
    {"vkGetPhysicalDeviceProperties2KHR", Scope::Instance},
    {"vkGetPhysicalDeviceFormatProperties", Scope::Instance},
    {"vkGetPhysicalDeviceImageFormatProperties", Scope::Instance},
    {"vkGetPhysicalDeviceMemoryProperties", Scope::Instance},
    {"vkGetPhysicalDeviceMemoryProperties2", Scope::Instance},
    {"vkGetPhysicalDeviceMemoryProperties2KHR", Scope::Instance},
    {"vkGetPhysicalDeviceQueueFamilyProperties", Scope::Instance},
    {"vkGetPhysicalDeviceSparseImageFormatProperties", Scope::Instance},
    {"vkCreateAndroidSurfaceKHR", Scope::Instance},
    {"vkDestroySurfaceKHR", Scope::Instance},
    {"vkGetPhysicalDeviceSurfaceSupportKHR", Scope::Instance},
    {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", Scope::Instance},
    {"vkGetPhysicalDeviceSurfaceFormatsKHR", Scope::Instance},
    {"vkGetPhysicalDeviceSurfacePresentModesKHR", Scope::Instance},
    {"vkCreateDevice", Scope::Instance},
    {"vkGetDeviceProcAddr", Scope::Instance},
    {"vkDestroyDevice", Scope::Device},
    {"vkGetDeviceQueue", Scope::Device},
    {"vkDeviceWaitIdle", Scope::Device},
    {"vkCreateSampler", Scope::Device},
    {"vkDestroySampler", Scope::Device},
    {"vkCreateSemaphore", Scope::Device},
    {"vkDestroySemaphore", Scope::Device},
    {"vkCreateFence", Scope::Device},
    {"vkDestroyFence", Scope::Device},
    {"vkGetFenceStatus", Scope::Device},
    {"vkResetFences", Scope::Device},
    {"vkWaitForFences", Scope::Device},
    {"vkCreateImage", Scope::Resource},
    {"vkDestroyImage", Scope::Resource},
    {"vkCreateImageView", Scope::Resource},
    {"vkDestroyImageView", Scope::Resource},
    {"vkCreateBuffer", Scope::Resource},
    {"vkDestroyBuffer", Scope::Resource},
    {"vkGetImageSubresourceLayout", Scope::Resource},
    {"vkGetDeviceMemoryCommitment", Scope::Resource},
    {"vkGetImageMemoryRequirements", Scope::Resource},
    {"vkGetImageMemoryRequirements2", Scope::Resource},
    {"vkGetImageMemoryRequirements2KHR", Scope::Resource},
    {"vkGetBufferMemoryRequirements", Scope::Resource},
    {"vkGetBufferMemoryRequirements2", Scope::Resource},
    {"vkGetBufferMemoryRequirements2KHR", Scope::Resource},
    {"vkBindImageMemory", Scope::Resource},
    {"vkBindImageMemory2", Scope::Resource},
    {"vkBindImageMemory2KHR", Scope::Resource},
    {"vkBindBufferMemory", Scope::Resource},
    {"vkBindBufferMemory2", Scope::Resource},
    {"vkBindBufferMemory2KHR", Scope::Resource},
    {"vkAllocateMemory", Scope::Resource},
    {"vkFreeMemory", Scope::Resource},
    {"vkMapMemory", Scope::Resource},
    {"vkUnmapMemory", Scope::Resource},
    {"vkFlushMappedMemoryRanges", Scope::Resource},
    {"vkInvalidateMappedMemoryRanges", Scope::Resource},
    {"vkCreatePipelineCache", Scope::Resource},
    {"vkDestroyPipelineCache", Scope::Resource},
    {"vkGetPipelineCacheData", Scope::Resource},
    {"vkMergePipelineCaches", Scope::Resource},
    {"vkCreateRenderPass", Scope::Resource},
    {"vkDestroyRenderPass", Scope::Resource},
    {"vkGetRenderAreaGranularity", Scope::Resource},
    {"vkCreateFramebuffer", Scope::Resource},
    {"vkDestroyFramebuffer", Scope::Resource},
    {"vkCreateShaderModule", Scope::Resource},
    {"vkDestroyShaderModule", Scope::Resource},
    {"vkCreateEvent", Scope::Resource},
    {"vkDestroyEvent", Scope::Resource},
    {"vkGetEventStatus", Scope::Resource},
    {"vkSetEvent", Scope::Resource},
    {"vkResetEvent", Scope::Resource},
    {"vkCreateGraphicsPipelines", Scope::Resource},
    {"vkCreateComputePipelines", Scope::Resource},
    {"vkDestroyPipeline", Scope::Resource},
    {"vkCreateSwapchainKHR", Scope::Swapchain},
    {"vkDestroySwapchainKHR", Scope::Swapchain},
    {"vkGetSwapchainImagesKHR", Scope::Swapchain},
    {"vkAcquireNextImageKHR", Scope::Swapchain},
    {"vkAcquireNextImage2KHR", Scope::Swapchain},
    {"vkQueuePresentKHR", Scope::Swapchain},
    {"vkQueueWaitIdle", Scope::Device},
    {"vkCreateCommandPool", Scope::Commands},
    {"vkDestroyCommandPool", Scope::Commands},
    {"vkResetCommandPool", Scope::Commands},
    {"vkAllocateCommandBuffers", Scope::Commands},
    {"vkFreeCommandBuffers", Scope::Commands},
    {"vkBeginCommandBuffer", Scope::Commands},
    {"vkEndCommandBuffer", Scope::Commands},
    {"vkResetCommandBuffer", Scope::Commands},
    {"vkQueueSubmit", Scope::Commands},
    {"vkCmdPipelineBarrier", Scope::Commands},
    {"vkCmdClearColorImage", Scope::Commands},
    {"vkCmdCopyBuffer", Scope::Commands},
    {"vkCmdCopyBufferToImage", Scope::Commands},
    {"vkCmdCopyImageToBuffer", Scope::Commands},
    {"vkCmdCopyImage", Scope::Commands},
    {"vkCmdBlitImage", Scope::Commands},
    {"vkCmdResolveImage", Scope::Commands},
    {"vkCmdClearDepthStencilImage", Scope::Commands},
    {"vkCmdClearAttachments", Scope::Commands},
    {"vkCmdFillBuffer", Scope::Commands},
    {"vkCmdUpdateBuffer", Scope::Commands},
    {"vkCmdSetEvent", Scope::Commands},
    {"vkCmdResetEvent", Scope::Commands},
    {"vkCmdWaitEvents", Scope::Commands},
    {"vkCmdBeginRenderPass", Scope::Commands},
    {"vkCmdNextSubpass", Scope::Commands},
    {"vkCmdEndRenderPass", Scope::Commands},
    {"vkCmdExecuteCommands", Scope::Commands},
    {"vkCreateBufferView", Scope::Descriptor},
    {"vkDestroyBufferView", Scope::Descriptor},
    {"vkCreateDescriptorSetLayout", Scope::Descriptor},
    {"vkDestroyDescriptorSetLayout", Scope::Descriptor},
    {"vkCreatePipelineLayout", Scope::Descriptor},
    {"vkDestroyPipelineLayout", Scope::Descriptor},
    {"vkCreateDescriptorPool", Scope::Descriptor},
    {"vkDestroyDescriptorPool", Scope::Descriptor},
    {"vkResetDescriptorPool", Scope::Descriptor},
    {"vkAllocateDescriptorSets", Scope::Descriptor},
    {"vkFreeDescriptorSets", Scope::Descriptor},
    {"vkUpdateDescriptorSets", Scope::Descriptor},
    {"vkCmdBindDescriptorSets", Scope::Commands},
    {"vkCmdPushConstants", Scope::Commands},
    {"vkCmdBindPipeline", Scope::Draw},
    {"vkCmdBindVertexBuffers", Scope::Draw},
    {"vkCmdBindIndexBuffer", Scope::Draw},
    {"vkCmdDraw", Scope::Draw},
    {"vkCmdDrawIndexed", Scope::Draw},
    {"vkCmdDrawIndirect", Scope::Draw},
    {"vkCmdDrawIndexedIndirect", Scope::Draw},
    {"vkCmdDispatch", Scope::Draw},
    {"vkCmdDispatchIndirect", Scope::Draw},
    {"vkCmdSetViewport", Scope::Draw},
    {"vkCmdSetScissor", Scope::Draw},
    {"vkCmdSetLineWidth", Scope::Draw},
    {"vkCmdSetDepthBias", Scope::Draw},
    {"vkCmdSetBlendConstants", Scope::Draw},
    {"vkCmdSetDepthBounds", Scope::Draw},
    {"vkCmdSetStencilCompareMask", Scope::Draw},
    {"vkCmdSetStencilWriteMask", Scope::Draw},
    {"vkCmdSetStencilReference", Scope::Draw},
    {"vkCreateQueryPool", Scope::Query},
    {"vkDestroyQueryPool", Scope::Query},
    {"vkGetQueryPoolResults", Scope::Query},
    {"vkCmdBeginQuery", Scope::Query},
    {"vkCmdEndQuery", Scope::Query},
    {"vkCmdResetQueryPool", Scope::Query},
    {"vkCmdWriteTimestamp", Scope::Query},
    {"vkCmdCopyQueryPoolResults", Scope::Query},
};

HMODULE VulkanLoader()
{
    static const auto loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return loader;
}

template <typename Function> Function Native(const char* name)
{
    return reinterpret_cast<Function>(GetProcAddress(VulkanLoader(), name));
}

const char* HostName(const Command& command)
{
    if (command.name == "vkCreateAndroidSurfaceKHR")
        return "vkCreateWin32SurfaceKHR";
    if (command.name == "vkGetPhysicalDeviceFeatures2KHR")
        return "vkGetPhysicalDeviceFeatures2";
    if (command.name == "vkGetPhysicalDeviceProperties2KHR")
        return "vkGetPhysicalDeviceProperties2";
    if (command.name == "vkGetPhysicalDeviceMemoryProperties2KHR")
        return "vkGetPhysicalDeviceMemoryProperties2";
    if (command.name == "vkGetImageMemoryRequirements2KHR")
        return "vkGetImageMemoryRequirements2";
    if (command.name == "vkGetBufferMemoryRequirements2KHR")
        return "vkGetBufferMemoryRequirements2";
    if (command.name == "vkBindImageMemory2KHR")
        return "vkBindImageMemory2";
    if (command.name == "vkBindBufferMemory2KHR")
        return "vkBindBufferMemory2";
    return command.name.data();
}

bool HostSurfacesAvailable()
{
    static const bool available = [] {
        const auto query = Native<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
        uint32_t count = 0;
        if (!query || query(nullptr, &count, nullptr) != VK_SUCCESS || !count)
            return false;
        std::vector<VkExtensionProperties> extensions(count);
        if (query(nullptr, &count, extensions.data()) != VK_SUCCESS)
            return false;
        const auto has = [&](std::string_view name) {
            return std::ranges::any_of(std::span(extensions).first(count),
                                       [name](const VkExtensionProperties& extension) {
                                           return extension.extensionName == name;
                                       });
        };
        return has(VK_KHR_SURFACE_EXTENSION_NAME) && has(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    }();
    return available;
}

struct DeviceMemory {
    VkDeviceSize size;
    uint32_t type;
    void* mapped = nullptr;
    VkDeviceSize mappedOffset = 0;
    VkDeviceSize mappedSize = 0;
};

struct ResourceBinding {
    VkMemoryRequirements requirements;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
};

struct ImageView {
    VkImage image;
    size_t references = 1;
};

struct Swapchain {
    VkSurfaceKHR surface;
    std::vector<VkImage> images;
    bool retired = false;
};

struct Queue {
    VkDevice device;
    uint32_t family;
};

struct CommandBuffer {
    VkDevice device;
    VkCommandPool pool;
    VkCommandBufferLevel level;
};

struct QueryPool {
    VkQueryType type;
    uint32_t count;
};

struct Device {
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    std::unordered_map<uint32_t, uint32_t> queueCounts;
    std::unordered_map<VkSampler, size_t> samplers;
    std::unordered_set<VkSemaphore> semaphores;
    std::unordered_set<VkFence> fences;
    std::unordered_set<VkEvent> events;
    std::unordered_map<VkImage, ResourceBinding> images;
    std::unordered_map<VkImageView, ImageView> views;
    std::unordered_map<VkBuffer, ResourceBinding> buffers;
    std::unordered_map<VkDeviceMemory, DeviceMemory> memory;
    std::unordered_set<VkPipelineCache> pipelineCaches;
    std::unordered_set<VkRenderPass> renderPasses;
    std::unordered_set<VkFramebuffer> framebuffers;
    std::unordered_set<VkShaderModule> shaders;
    std::unordered_map<VkPipeline, VkPipelineBindPoint> pipelines;
    std::unordered_map<VkQueryPool, QueryPool> queryPools;
    std::unordered_set<VkBufferView> bufferViews;
    std::unordered_set<VkDescriptorSetLayout> descriptorLayouts;
    std::unordered_set<VkPipelineLayout> pipelineLayouts;
    std::unordered_map<VkDescriptorPool, VkDescriptorPoolCreateFlags> descriptorPools;
    std::unordered_map<VkDescriptorSet, VkDescriptorPool> descriptorSets;
    std::unordered_map<VkCommandPool, uint32_t> commandPools;
    bool swapchainEnabled = false;
    std::unordered_map<VkSwapchainKHR, Swapchain> swapchains;
};

struct Instance {
    bool surface = false;
    bool androidSurface = false;
};

struct Surface {
    VkInstance instance;
    std::shared_ptr<NativeWindow> window;
    VkSwapchainKHR active = VK_NULL_HANDLE;
    VkSwapchainKHR presenting = VK_NULL_HANDLE;
    size_t swapchains = 0;
    bool creating = false;
};

struct VulkanObjects {
    std::unordered_map<VkInstance, Instance> instances;
    std::unordered_map<VkPhysicalDevice, VkInstance> physicalDevices;
    std::unordered_map<VkDevice, Device> devices;
    std::unordered_map<VkSurfaceKHR, Surface> surfaces;
    std::unordered_map<VkQueue, Queue> queues;
    std::unordered_map<VkCommandBuffer, CommandBuffer> commandBuffers;
};

VulkanObjects& Objects()
{
    static auto* objects = new VulkanObjects;
    return *objects;
}

// The interpreter checks these spans without taking the loader lock. Vulkan
// clients synchronize guest accesses with unmap and free, and device accesses
// with the Vulkan memory and execution synchronization commands.
struct MappedRanges {
    std::atomic<size_t> count{0};
    std::mutex mutex;
    std::map<uintptr_t, VkDeviceSize> ranges;
};

MappedRanges& Mappings()
{
    static MappedRanges mappings;
    return mappings;
}

struct Guest {
    std::span<const uint64_t, 8> args;
    uintptr_t stack;
    const std::function<uint8_t*(uintptr_t, unsigned, bool)>& memory;
    const std::function<std::string(uintptr_t, unsigned)>& string;
    const std::function<float(unsigned)>& floatArgument;

    template <typename T> T read(uintptr_t address) const
    {
        T value;
        std::memcpy(&value, memory(address, sizeof(T), false), sizeof(T));
        return value;
    }

    template <typename T> uint8_t* output(uintptr_t address, uint32_t count = 1) const
    {
        if (count > UINT_MAX / sizeof(T))
            throw std::runtime_error("ARM64 Vulkan output is too large");
        return count ? memory(address, static_cast<unsigned>(count * sizeof(T)), true) : nullptr;
    }

    template <typename T> std::vector<T> array(const T* address, uint32_t count) const
    {
        if (count > UINT_MAX / sizeof(T))
            throw std::runtime_error("ARM64 Vulkan input is too large");
        if (!count)
            return {};
        const auto bytes = static_cast<unsigned>(count * sizeof(T));
        const auto* source = memory(reinterpret_cast<uintptr_t>(address), bytes, false);
        std::vector<T> values(count);
        std::memcpy(values.data(), source, bytes);
        return values;
    }

    template <typename T> void write(uintptr_t address, const T& value) const
    {
        std::memcpy(output<T>(address), &value, sizeof(T));
    }

    template <typename T> T handle(unsigned index) const
    {
        return reinterpret_cast<T>(args[index]);
    }

    uint64_t argument(unsigned index) const
    {
        return index < args.size() ? args[index] : read<uint64_t>(stack + (index - args.size()) * sizeof(uint64_t));
    }
};

uint64_t Result(VkResult result)
{
    return static_cast<uint32_t>(result);
}

bool TraceVulkan()
{
    static const bool enabled = [] {
        char value[2]{};
        return GetEnvironmentVariableA("STONE_TRACE_DEVICEINFO", value, sizeof(value)) == 1 && value[0] == '1';
    }();
    return enabled;
}

uint64_t TraceResult(std::string_view name, VkResult result)
{
    if (TraceVulkan())
        std::fprintf(stderr, "[android-vulkan] %.*s returned %d\n", static_cast<int>(name.size()), name.data(), result);
    return Result(result);
}

template <typename Call> uint64_t TraceFrameCall(std::string_view name, Call call)
{
    if (!TraceVulkan())
        return call();
    // The dispatcher enters with AndroidLoaderMutex held. The Vulkan call may release it.
    static std::unordered_set<std::string_view> started;
    const bool first = started.insert(name).second;
    if (first)
        std::fprintf(stderr, "[android-vulkan] entering %.*s\n", static_cast<int>(name.size()), name.data());
    const auto result = call();
    const auto status = std::bit_cast<int32_t>(static_cast<uint32_t>(result));
    return first || status < 0 ? TraceResult(name, static_cast<VkResult>(status)) : result;
}

template <typename Info, typename Handle, typename Create, typename Destroy>
VkResult CreateSyncObject(const Guest& guest, VkDevice device, VkStructureType type, VkFlags allowedFlags,
                          std::unordered_set<Handle>& tracked, Create create, Destroy destroy)
{
    const auto info = guest.read<Info>(guest.args[1]);
    if (info.sType != type || info.pNext || (info.flags & ~allowedFlags) || guest.args[2])
        return VK_ERROR_INITIALIZATION_FAILED;
    auto* output = guest.output<Handle>(guest.args[3]);
    Handle handle = VK_NULL_HANDLE;
    const auto result = create(device, &info, nullptr, &handle);
    if (result == VK_SUCCESS) {
        try {
            tracked.insert(handle);
        }
        catch (...) {
            destroy(device, handle, nullptr);
            throw;
        }
        std::memcpy(output, &handle, sizeof(handle));
    }
    return result;
}

template <typename Handle, typename Destroy>
void DestroyTrackedObject(const Guest& guest, VkDevice device, std::unordered_set<Handle>& tracked, Destroy destroy)
{
    const auto handle = guest.handle<Handle>(1);
    if (!handle)
        return;
    const auto found = tracked.find(handle);
    if (guest.args[2] || found == tracked.end())
        throw std::runtime_error("invalid ARM64 Vulkan resource destruction");
    destroy(device, handle, nullptr);
    tracked.erase(found);
}

template <typename Info, typename Handle, typename Create, typename Destroy, typename Track>
VkResult CreateResource(const Guest& guest, VkDevice device, const Info& info, Create create, Destroy destroy,
                        Track track)
{
    auto* output = guest.output<Handle>(guest.args[3]);
    Handle handle = VK_NULL_HANDLE;
    const auto result = create(device, &info, nullptr, &handle);
    if (result == VK_SUCCESS) {
        try {
            track(handle);
        }
        catch (...) {
            destroy(device, handle, nullptr);
            throw;
        }
        std::memcpy(output, &handle, sizeof(handle));
    }
    return result;
}

bool CopySharingInfo(const Guest& guest, const Device& device, VkSharingMode mode, uint32_t& count,
                     const uint32_t*& indices, std::vector<uint32_t>& families)
{
    if (mode == VK_SHARING_MODE_EXCLUSIVE) {
        count = 0;
        indices = nullptr;
        return true;
    }
    if (mode != VK_SHARING_MODE_CONCURRENT || count < 2 || count > device.queueCounts.size())
        return false;
    families = guest.array(indices, count);
    std::ranges::sort(families);
    if (std::ranges::adjacent_find(families) != families.end() || !std::ranges::all_of(families, [&](uint32_t family) {
            return device.queueCounts.contains(family);
        }))
        return false;
    indices = families.data();
    return true;
}

VkResult CreateImage(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkImageCreateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || guest.args[2] ||
        (info.flags & VK_IMAGE_CREATE_DISJOINT_BIT))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkImageFormatListCreateInfo formatList{};
    std::vector<VkFormat> formats;
    if (info.pNext) {
        const auto address = reinterpret_cast<uintptr_t>(info.pNext);
        if (guest.read<VkStructureType>(address) != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        formatList = guest.read<VkImageFormatListCreateInfo>(address);
        if (formatList.pNext)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        formats = guest.array(formatList.pViewFormats, formatList.viewFormatCount);
        formatList.pViewFormats = formats.empty() ? nullptr : formats.data();
        info.pNext = &formatList;
    }
    std::vector<uint32_t> families;
    if (!CopySharingInfo(guest, record, info.sharingMode, info.queueFamilyIndexCount, info.pQueueFamilyIndices,
                         families))
        return VK_ERROR_INITIALIZATION_FAILED;
    return CreateResource<VkImageCreateInfo, VkImage>(
        guest, device, info, Native<PFN_vkCreateImage>("vkCreateImage"), Native<PFN_vkDestroyImage>("vkDestroyImage"),
        [&](VkImage image) {
            VkMemoryRequirements requirements{};
            Native<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, image, &requirements);
            record.images.emplace(image, ResourceBinding{requirements});
        });
}

VkResult CreateBuffer(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkBufferCreateInfo>(guest.args[1]);
    std::vector<uint32_t> families;
    if (info.sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO || info.pNext || guest.args[2] ||
        !CopySharingInfo(guest, record, info.sharingMode, info.queueFamilyIndexCount, info.pQueueFamilyIndices,
                         families))
        return VK_ERROR_INITIALIZATION_FAILED;
    return CreateResource<VkBufferCreateInfo, VkBuffer>(
        guest, device, info, Native<PFN_vkCreateBuffer>("vkCreateBuffer"),
        Native<PFN_vkDestroyBuffer>("vkDestroyBuffer"), [&](VkBuffer buffer) {
            VkMemoryRequirements requirements{};
            Native<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, buffer, &requirements);
            record.buffers.emplace(buffer, ResourceBinding{requirements});
        });
}

VkResult CreateImageView(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkImageViewCreateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO || guest.args[2] || !record.images.contains(info.image))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkImageViewUsageCreateInfo usage{};
    if (info.pNext) {
        const auto address = reinterpret_cast<uintptr_t>(info.pNext);
        if (guest.read<VkStructureType>(address) != VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        usage = guest.read<VkImageViewUsageCreateInfo>(address);
        if (usage.pNext)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        info.pNext = &usage;
    }
    return CreateResource<VkImageViewCreateInfo, VkImageView>(
        guest, device, info, Native<PFN_vkCreateImageView>("vkCreateImageView"),
        Native<PFN_vkDestroyImageView>("vkDestroyImageView"), [&](VkImageView view) {
            const auto [entry, inserted] = record.views.try_emplace(view, ImageView{info.image});
            if (!inserted)
                ++entry->second.references;
        });
}

template <typename Handle, typename Record, typename Destroy>
void DestroyResource(const Guest& guest, VkDevice device, std::unordered_map<Handle, Record>& resources,
                     Destroy destroy)
{
    const auto handle = guest.handle<Handle>(1);
    if (!handle)
        return;
    const auto entry = resources.find(handle);
    if (guest.args[2] || entry == resources.end())
        throw std::runtime_error("invalid ARM64 Vulkan resource");
    destroy(device, handle, nullptr);
    resources.erase(entry);
}

template <typename Handle>
void GetMemoryRequirements(const Guest& guest, const std::unordered_map<Handle, ResourceBinding>& resources)
{
    const auto resource = resources.find(guest.handle<Handle>(1));
    if (resource == resources.end() || resource->second.swapchain)
        throw std::runtime_error("invalid ARM64 Vulkan resource");
    guest.write(guest.args[2], resource->second.requirements);
}

template <typename Info, typename Handle, typename Query>
void GetMemoryRequirements2(const Guest& guest, VkDevice device, VkStructureType type,
                            const std::unordered_map<Handle, ResourceBinding>& resources, Handle Info::* handle,
                            Query query)
{
    const auto info = guest.read<Info>(guest.args[1]);
    auto output = guest.read<VkMemoryRequirements2>(guest.args[2]);
    const auto resource = resources.find(info.*handle);
    if (info.sType != type || info.pNext || output.sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 ||
        resource == resources.end() || resource->second.swapchain)
        throw std::runtime_error("invalid ARM64 Vulkan memory requirements query");
    VkMemoryDedicatedRequirements dedicated{};
    uint8_t* dedicatedOutput = nullptr;
    const auto address = reinterpret_cast<uintptr_t>(output.pNext);
    if (address) {
        dedicated = guest.read<VkMemoryDedicatedRequirements>(address);
        if (dedicated.sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS || dedicated.pNext)
            throw std::runtime_error("unsupported ARM64 Vulkan memory requirements chain");
        if (!query)
            throw std::runtime_error("Vulkan device does not provide memory requirements 2");
        dedicatedOutput = guest.output<VkMemoryDedicatedRequirements>(address);
    }
    auto* result = guest.output<VkMemoryRequirements2>(guest.args[2]);
    output.pNext = dedicatedOutput ? &dedicated : nullptr;
    if (query)
        query(device, &info, &output);
    else
        output.memoryRequirements = resource->second.requirements;
    output.pNext = reinterpret_cast<void*>(address);
    std::memcpy(result, &output, sizeof(output));
    if (dedicatedOutput)
        std::memcpy(dedicatedOutput, &dedicated, sizeof(dedicated));
}

bool CanBind(const ResourceBinding& binding, const DeviceMemory& memory, VkDeviceSize offset)
{
    const auto& requirements = binding.requirements;
    return !binding.swapchain && !binding.memory && offset < memory.size && requirements.size <= memory.size - offset &&
           !(offset % requirements.alignment) && (requirements.memoryTypeBits & (uint32_t{1} << memory.type));
}

template <typename Handle, typename Bind>
VkResult BindMemory(const Guest& guest, VkDevice device, const Device& record,
                    std::unordered_map<Handle, ResourceBinding>& resources, Bind bind)
{
    const auto handle = guest.handle<Handle>(1);
    const auto memory = guest.handle<VkDeviceMemory>(2);
    const auto resource = resources.find(handle);
    const auto allocation = record.memory.find(memory);
    if (resource == resources.end() || allocation == record.memory.end() ||
        !CanBind(resource->second, allocation->second, guest.args[3]))
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = bind(device, handle, memory, guest.args[3]);
    if (result == VK_SUCCESS)
        resource->second.memory = memory;
    return result;
}

template <typename Info, typename Handle, typename Bind>
VkResult BindMemory2(const Guest& guest, VkDevice device, VkStructureType type, const Device& record,
                     std::unordered_map<Handle, ResourceBinding>& resources, Handle Info::* handle, Bind bind)
{
    const auto count = static_cast<uint32_t>(guest.args[1]);
    if (!bind || !count || count > resources.size())
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto infos = guest.array(reinterpret_cast<const Info*>(guest.args[2]), count);
    std::unordered_set<Handle> seen;
    for (const auto& info : infos) {
        const auto resource = resources.find(info.*handle);
        const auto allocation = record.memory.find(info.memory);
        if (info.sType != type || info.pNext || resource == resources.end() || allocation == record.memory.end() ||
            !CanBind(resource->second, allocation->second, info.memoryOffset) || !seen.insert(info.*handle).second)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto result = bind(device, count, infos.data());
    if (result == VK_SUCCESS)
        for (const auto& info : infos)
            resources.at(info.*handle).memory = info.memory;
    return result;
}

VkResult AllocateMemory(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkMemoryAllocateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO || guest.args[2] || !info.allocationSize ||
        info.memoryTypeIndex >= record.memoryProperties.memoryTypeCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkMemoryDedicatedAllocateInfo dedicated{};
    if (info.pNext) {
        const auto address = reinterpret_cast<uintptr_t>(info.pNext);
        if (guest.read<VkStructureType>(address) != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        dedicated = guest.read<VkMemoryDedicatedAllocateInfo>(address);
        if (dedicated.pNext || (dedicated.image && dedicated.buffer) ||
            (dedicated.image && !record.images.contains(dedicated.image)) ||
            (dedicated.buffer && !record.buffers.contains(dedicated.buffer)))
            return VK_ERROR_INITIALIZATION_FAILED;
        info.pNext = &dedicated;
    }
    return CreateResource<VkMemoryAllocateInfo, VkDeviceMemory>(
        guest, device, info, Native<PFN_vkAllocateMemory>("vkAllocateMemory"), Native<PFN_vkFreeMemory>("vkFreeMemory"),
        [&](VkDeviceMemory memory) {
            record.memory.emplace(memory, DeviceMemory{info.allocationSize, info.memoryTypeIndex});
        });
}

void RemoveMapping(DeviceMemory& memory)
{
    if (!memory.mapped)
        return;
    auto& mappings = Mappings();
    const std::lock_guard lock(mappings.mutex);
    mappings.ranges.erase(reinterpret_cast<uintptr_t>(memory.mapped));
    mappings.count.fetch_sub(1, std::memory_order_release);
    memory.mapped = nullptr;
}

VkResult MapMemory(const Guest& guest, VkDevice device, Device& record)
{
    const auto memory = guest.handle<VkDeviceMemory>(1);
    const auto allocation = record.memory.find(memory);
    const auto offset = guest.args[2], length = guest.args[3];
    if (allocation == record.memory.end() || guest.args[4])
        return VK_ERROR_MEMORY_MAP_FAILED;
    auto& entry = allocation->second;
    if (entry.mapped || offset >= entry.size || !length || (length != VK_WHOLE_SIZE && length > entry.size - offset) ||
        !(record.memoryProperties.memoryTypes[entry.type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        return VK_ERROR_MEMORY_MAP_FAILED;
    auto* output = guest.output<void*>(guest.args[5]);
    void* data = nullptr;
    const auto result = Native<PFN_vkMapMemory>("vkMapMemory")(device, memory, offset, length, 0, &data);
    if (result != VK_SUCCESS)
        return result;
    const auto mappedSize = length == VK_WHOLE_SIZE ? entry.size - offset : length;
    try {
        auto& mappings = Mappings();
        const std::lock_guard lock(mappings.mutex);
        const auto base = reinterpret_cast<uintptr_t>(data);
        if (!data || mappedSize > UINTPTR_MAX - base || !mappings.ranges.emplace(base, mappedSize).second)
            throw std::runtime_error("invalid ARM64 Vulkan mapped range");
        mappings.count.fetch_add(1, std::memory_order_release);
    }
    catch (...) {
        Native<PFN_vkUnmapMemory>("vkUnmapMemory")(device, memory);
        throw;
    }
    entry.mapped = data;
    entry.mappedOffset = offset;
    entry.mappedSize = mappedSize;
    std::memcpy(output, &data, sizeof(data));
    return result;
}

VkResult SyncMappedRanges(const Guest& guest, VkDevice device, const Device& record, bool flush)
{
    const auto count = static_cast<uint32_t>(guest.args[1]);
    if (!count)
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto ranges = guest.array(guest.handle<const VkMappedMemoryRange*>(2), count);
    for (const auto& range : ranges) {
        const auto allocation = record.memory.find(range.memory);
        if (range.sType != VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE || range.pNext || allocation == record.memory.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        const auto& entry = allocation->second;
        const auto end = entry.mappedOffset + entry.mappedSize;
        if (!entry.mapped || range.offset < entry.mappedOffset || range.offset >= end || !range.size ||
            (range.size == VK_WHOLE_SIZE ? end != entry.size : range.size > end - range.offset))
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (flush)
        return Native<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(device, count, ranges.data());
    return Native<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device, count, ranges.data());
}

VkResult CreatePipelineCache(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkPipelineCacheCreateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO || info.pNext || info.flags || guest.args[2] ||
        info.initialDataSize > UINT_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (info.initialDataSize) {
        const auto address = reinterpret_cast<uintptr_t>(info.pInitialData);
        info.pInitialData = guest.memory(address, static_cast<unsigned>(info.initialDataSize), false);
        VkPipelineCacheHeaderVersionOne header{};
        if (info.initialDataSize >= sizeof(header))
            std::memcpy(&header, info.pInitialData, sizeof(header));
        VkPhysicalDeviceProperties properties{};
        Native<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(record.physical, &properties);
        if (header.headerSize != sizeof(header) || header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
            header.vendorID != properties.vendorID || header.deviceID != properties.deviceID ||
            std::memcmp(header.pipelineCacheUUID, properties.pipelineCacheUUID, VK_UUID_SIZE))
            info.initialDataSize = 0;
    }
    if (!info.initialDataSize)
        info.pInitialData = nullptr;
    return CreateResource<VkPipelineCacheCreateInfo, VkPipelineCache>(
        guest, device, info, Native<PFN_vkCreatePipelineCache>("vkCreatePipelineCache"),
        Native<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache"), [&](VkPipelineCache cache) {
            record.pipelineCaches.insert(cache);
        });
}

VkResult GetPipelineCacheData(const Guest& guest, VkDevice device, VkPipelineCache cache)
{
    auto* sizeOutput = guest.output<size_t>(guest.args[2]);
    size_t size = 0;
    uint8_t unused = 0;
    uint8_t* data = nullptr;
    if (guest.args[3]) {
        std::memcpy(&size, sizeOutput, sizeof(size));
        if (size > UINT_MAX)
            return VK_ERROR_INITIALIZATION_FAILED;
        data = size ? guest.memory(guest.args[3], static_cast<unsigned>(size), true) : &unused;
    }
    const auto result = Native<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData")(device, cache, &size, data);
    if (result == VK_SUCCESS || result == VK_INCOMPLETE)
        std::memcpy(sizeOutput, &size, sizeof(size));
    return result;
}

VkResult CreateRenderPass(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkRenderPassCreateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO || info.pNext || info.flags || guest.args[2] ||
        !info.subpassCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto attachments = guest.array(info.pAttachments, info.attachmentCount);
    const auto dependencies = guest.array(info.pDependencies, info.dependencyCount);
    auto subpasses = guest.array(info.pSubpasses, info.subpassCount);

    struct SubpassAttachments {
        std::vector<VkAttachmentReference> inputs;
        std::vector<VkAttachmentReference> colors;
        std::vector<VkAttachmentReference> resolves;
        VkAttachmentReference depthStencil{};
        std::vector<uint32_t> preserve;
    };

    std::vector<SubpassAttachments> arrays(subpasses.size());
    const auto valid = [&](const VkAttachmentReference& attachment) {
        return attachment.attachment == VK_ATTACHMENT_UNUSED || attachment.attachment < attachments.size();
    };
    for (size_t i = 0; i < subpasses.size(); ++i) {
        auto& subpass = subpasses[i];
        auto& data = arrays[i];
        data.inputs = guest.array(subpass.pInputAttachments, subpass.inputAttachmentCount);
        data.colors = guest.array(subpass.pColorAttachments, subpass.colorAttachmentCount);
        if (subpass.pResolveAttachments)
            data.resolves = guest.array(subpass.pResolveAttachments, subpass.colorAttachmentCount);
        if (subpass.pDepthStencilAttachment)
            data.depthStencil =
                guest.read<VkAttachmentReference>(reinterpret_cast<uintptr_t>(subpass.pDepthStencilAttachment));
        data.preserve = guest.array(subpass.pPreserveAttachments, subpass.preserveAttachmentCount);
        if (!std::ranges::all_of(data.inputs, valid) || !std::ranges::all_of(data.colors, valid) ||
            !std::ranges::all_of(data.resolves, valid) ||
            (subpass.pDepthStencilAttachment && !valid(data.depthStencil)) ||
            !std::ranges::all_of(data.preserve, [&](uint32_t index) {
                return index < attachments.size();
            }))
            return VK_ERROR_INITIALIZATION_FAILED;
        subpass.pInputAttachments = data.inputs.empty() ? nullptr : data.inputs.data();
        subpass.pColorAttachments = data.colors.empty() ? nullptr : data.colors.data();
        subpass.pResolveAttachments = data.resolves.empty() ? nullptr : data.resolves.data();
        subpass.pDepthStencilAttachment = subpass.pDepthStencilAttachment ? &data.depthStencil : nullptr;
        subpass.pPreserveAttachments = data.preserve.empty() ? nullptr : data.preserve.data();
    }
    const auto validSubpass = [&](uint32_t subpass) {
        return subpass == VK_SUBPASS_EXTERNAL || subpass < subpasses.size();
    };
    if (!std::ranges::all_of(dependencies, [&](const auto& dependency) {
            return validSubpass(dependency.srcSubpass) && validSubpass(dependency.dstSubpass);
        }))
        return VK_ERROR_INITIALIZATION_FAILED;
    info.pAttachments = attachments.empty() ? nullptr : attachments.data();
    info.pSubpasses = subpasses.data();
    info.pDependencies = dependencies.empty() ? nullptr : dependencies.data();
    return CreateResource<VkRenderPassCreateInfo, VkRenderPass>(
        guest, device, info, Native<PFN_vkCreateRenderPass>("vkCreateRenderPass"),
        Native<PFN_vkDestroyRenderPass>("vkDestroyRenderPass"), [&](VkRenderPass pass) {
            record.renderPasses.insert(pass);
        });
}

VkResult CreateFramebuffer(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkFramebufferCreateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO || info.pNext || info.flags || guest.args[2] ||
        !record.renderPasses.contains(info.renderPass))
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto attachments = guest.array(info.pAttachments, info.attachmentCount);
    if (!std::ranges::all_of(attachments, [&](VkImageView view) {
            return record.views.contains(view);
        }))
        return VK_ERROR_INITIALIZATION_FAILED;
    info.pAttachments = attachments.empty() ? nullptr : attachments.data();
    return CreateResource<VkFramebufferCreateInfo, VkFramebuffer>(
        guest, device, info, Native<PFN_vkCreateFramebuffer>("vkCreateFramebuffer"),
        Native<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer"), [&](VkFramebuffer framebuffer) {
            record.framebuffers.insert(framebuffer);
        });
}

VkResult CreateShaderModule(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkShaderModuleCreateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO || info.pNext || info.flags || guest.args[2] ||
        info.codeSize < 5 * sizeof(uint32_t) || info.codeSize > UINT_MAX || info.codeSize % sizeof(uint32_t))
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto code = guest.array(info.pCode, static_cast<uint32_t>(info.codeSize / sizeof(uint32_t)));
    if (code.front() != 0x07230203)
        return VK_ERROR_INITIALIZATION_FAILED;
    info.pCode = code.data();
    return CreateResource<VkShaderModuleCreateInfo, VkShaderModule>(
        guest, device, info, Native<PFN_vkCreateShaderModule>("vkCreateShaderModule"),
        Native<PFN_vkDestroyShaderModule>("vkDestroyShaderModule"), [&](VkShaderModule shader) {
            record.shaders.insert(shader);
        });
}

struct ShaderStageData {
    std::string name;
    VkSpecializationInfo specialization{};
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<uint8_t> values;

    bool Copy(const Guest& guest, const Device& record, VkPipelineShaderStageCreateInfo& stage)
    {
        if (stage.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || stage.pNext ||
            !record.shaders.contains(stage.module) || !stage.pName)
            return false;
        name = guest.string(reinterpret_cast<uintptr_t>(stage.pName), 4096);
        if (name.empty())
            return false;
        stage.pName = name.c_str();
        if (stage.pSpecializationInfo) {
            specialization = guest.read<VkSpecializationInfo>(reinterpret_cast<uintptr_t>(stage.pSpecializationInfo));
            if (specialization.dataSize > UINT_MAX)
                return false;
            entries = guest.array(specialization.pMapEntries, specialization.mapEntryCount);
            if (!std::ranges::all_of(entries, [&](const auto& entry) {
                    return entry.offset <= specialization.dataSize &&
                           entry.size <= specialization.dataSize - entry.offset;
                }))
                return false;
            values = guest.array(static_cast<const uint8_t*>(specialization.pData),
                                 static_cast<uint32_t>(specialization.dataSize));
            specialization.pMapEntries = entries.empty() ? nullptr : entries.data();
            specialization.pData = values.empty() ? nullptr : values.data();
            stage.pSpecializationInfo = &specialization;
        }
        return true;
    }
};

template <typename State>
bool CopyPipelineState(const Guest& guest, VkStructureType type, const State*& source, State& host)
{
    if (!source)
        return true;
    host = guest.read<State>(reinterpret_cast<uintptr_t>(source));
    source = &host;
    return host.sType == type && !host.pNext;
}

struct GraphicsPipelineData {
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<ShaderStageData> shaders;
    VkPipelineVertexInputStateCreateInfo vertex{};
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    VkPipelineTessellationStateCreateInfo tessellation{};
    VkPipelineViewportStateCreateInfo viewport{};
    VkPipelineRasterizationStateCreateInfo rasterization{};
    VkPipelineMultisampleStateCreateInfo multisample{};
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    VkPipelineColorBlendStateCreateInfo blend{};
    VkPipelineDynamicStateCreateInfo dynamic{};
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkDynamicState> states;
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    std::vector<VkSampleMask> mask;
    std::vector<VkPipelineColorBlendAttachmentState> attachments;

    bool Copy(const Guest& guest, const Device& record, VkGraphicsPipelineCreateInfo& info)
    {
        if (info.sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO || info.pNext || !info.stageCount ||
            !record.pipelineLayouts.contains(info.layout) || !record.renderPasses.contains(info.renderPass))
            return false;
        stages = guest.array(info.pStages, info.stageCount);
        shaders.resize(stages.size());
        for (size_t i = 0; i < stages.size(); ++i)
            if (!shaders[i].Copy(guest, record, stages[i]))
                return false;
        info.pStages = stages.data();

        if (!CopyPipelineState(guest, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, info.pVertexInputState,
                               vertex) ||
            !CopyPipelineState(guest, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                               info.pInputAssemblyState, assembly) ||
            !CopyPipelineState(guest, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                               info.pRasterizationState, rasterization) ||
            !CopyPipelineState(guest, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, info.pDynamicState,
                               dynamic) ||
            !info.pVertexInputState || !info.pInputAssemblyState || !info.pRasterizationState)
            return false;
        const bool tessellated = std::ranges::any_of(stages, [](const auto& stage) {
            return stage.stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        });
        if (!tessellated)
            info.pTessellationState = nullptr;
        if (!CopyPipelineState(guest, VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                               info.pTessellationState, tessellation) ||
            (tessellated && !info.pTessellationState))
            return false;
        bindings = guest.array(vertex.pVertexBindingDescriptions, vertex.vertexBindingDescriptionCount);
        attributes = guest.array(vertex.pVertexAttributeDescriptions, vertex.vertexAttributeDescriptionCount);
        vertex.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
        vertex.pVertexAttributeDescriptions = attributes.empty() ? nullptr : attributes.data();
        states = guest.array(dynamic.pDynamicStates, dynamic.dynamicStateCount);
        dynamic.pDynamicStates = states.empty() ? nullptr : states.data();

        if (rasterization.rasterizerDiscardEnable) {
            info.pViewportState = nullptr;
            info.pMultisampleState = nullptr;
            info.pDepthStencilState = nullptr;
            info.pColorBlendState = nullptr;
            return true;
        }
        if (!CopyPipelineState(guest, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, info.pViewportState,
                               viewport) ||
            !CopyPipelineState(guest, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, info.pMultisampleState,
                               multisample) ||
            !CopyPipelineState(guest, VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                               info.pDepthStencilState, depthStencil) ||
            !CopyPipelineState(guest, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, info.pColorBlendState,
                               blend) ||
            !info.pViewportState || !info.pMultisampleState)
            return false;
        if (std::ranges::find(states, VK_DYNAMIC_STATE_VIEWPORT) == states.end())
            viewports = guest.array(viewport.pViewports, viewport.viewportCount);
        if (std::ranges::find(states, VK_DYNAMIC_STATE_SCISSOR) == states.end())
            scissors = guest.array(viewport.pScissors, viewport.scissorCount);
        viewport.pViewports = viewports.empty() ? nullptr : viewports.data();
        viewport.pScissors = scissors.empty() ? nullptr : scissors.data();
        const auto samples = static_cast<uint32_t>(multisample.rasterizationSamples);
        if (!std::has_single_bit(samples) || samples > VK_SAMPLE_COUNT_64_BIT)
            return false;
        if (multisample.pSampleMask)
            mask = guest.array(multisample.pSampleMask, (samples + 31) / 32);
        multisample.pSampleMask = mask.empty() ? nullptr : mask.data();
        attachments = guest.array(blend.pAttachments, blend.attachmentCount);
        blend.pAttachments = attachments.empty() ? nullptr : attachments.data();
        return true;
    }
};

template <typename Info, typename Data, typename Copy, typename Create>
VkResult CreatePipelines(const Guest& guest, VkDevice device, Device& record, VkPipelineBindPoint type, Copy copy,
                         Create create)
{
    const auto cache = guest.handle<VkPipelineCache>(1);
    const auto count = static_cast<uint32_t>(guest.args[2]);
    if ((cache && !record.pipelineCaches.contains(cache)) || !count || guest.args[4])
        return VK_ERROR_INITIALIZATION_FAILED;
    auto infos = guest.array(guest.handle<const Info*>(3), count);
    std::vector<Data> data(count);
    for (size_t i = 0; i < infos.size(); ++i) {
        auto& info = infos[i];
        if (!copy(guest, record, info, data[i]))
            return VK_ERROR_INITIALIZATION_FAILED;
        if (info.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) {
            if (info.basePipelineHandle) {
                const auto base = record.pipelines.find(info.basePipelineHandle);
                if (base == record.pipelines.end() || base->second != type || info.basePipelineIndex != -1)
                    return VK_ERROR_INITIALIZATION_FAILED;
            }
            else if (info.basePipelineIndex < 0 || static_cast<size_t>(info.basePipelineIndex) >= i) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        else {
            info.basePipelineHandle = VK_NULL_HANDLE;
            info.basePipelineIndex = -1;
        }
    }
    auto* output = guest.output<VkPipeline>(guest.args[5], count);
    std::vector<VkPipeline> pipelines(count);
    const auto result = create(device, cache, count, infos.data(), nullptr, pipelines.data());
    try {
        // The native implementation may return pipelines for successful entries in a failed batch.
        for (const auto pipeline : pipelines)
            if (pipeline)
                record.pipelines.emplace(pipeline, type);
    }
    catch (...) {
        const auto destroy = Native<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        for (const auto pipeline : pipelines) {
            if (!pipeline)
                continue;
            record.pipelines.erase(pipeline);
            destroy(device, pipeline, nullptr);
        }
        throw;
    }
    std::memcpy(output, pipelines.data(), pipelines.size() * sizeof(VkPipeline));
    return result;
}

uint64_t CallResource(std::string_view name, const Guest& guest, VkDevice device, Device& record)
{
    if (name == "vkCreateImage")
        return TraceResult(name, CreateImage(guest, device, record));
    if (name == "vkCreateImageView")
        return TraceResult(name, CreateImageView(guest, device, record));
    if (name == "vkCreateBuffer")
        return Result(CreateBuffer(guest, device, record));
    if (name == "vkAllocateMemory")
        return Result(AllocateMemory(guest, device, record));
    if (name == "vkMapMemory")
        return Result(MapMemory(guest, device, record));
    if (name == "vkCreateEvent")
        return TraceResult(name, CreateSyncObject<VkEventCreateInfo, VkEvent>(
                                     guest, device, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, 0, record.events,
                                     Native<PFN_vkCreateEvent>("vkCreateEvent"),
                                     Native<PFN_vkDestroyEvent>("vkDestroyEvent")));
    if (name == "vkDestroyEvent") {
        DestroyTrackedObject(guest, device, record.events, Native<PFN_vkDestroyEvent>("vkDestroyEvent"));
        return 0;
    }
    if (name == "vkGetEventStatus" || name == "vkSetEvent" || name == "vkResetEvent") {
        const auto event = guest.handle<VkEvent>(1);
        if (!record.events.contains(event))
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        if (name == "vkGetEventStatus")
            return Result(Native<PFN_vkGetEventStatus>("vkGetEventStatus")(device, event));
        if (name == "vkSetEvent")
            return Result(Native<PFN_vkSetEvent>("vkSetEvent")(device, event));
        return Result(Native<PFN_vkResetEvent>("vkResetEvent")(device, event));
    }
    if (name == "vkGetDeviceMemoryCommitment") {
        const auto allocation = guest.handle<VkDeviceMemory>(1);
        if (!record.memory.contains(allocation))
            throw std::runtime_error("invalid ARM64 Vulkan memory allocation");
        VkDeviceSize size = 0;
        Native<PFN_vkGetDeviceMemoryCommitment>("vkGetDeviceMemoryCommitment")(device, allocation, &size);
        guest.write(guest.args[2], size);
        return 0;
    }
    if (name == "vkGetImageSubresourceLayout") {
        const auto image = guest.handle<VkImage>(1);
        if (!record.images.contains(image))
            throw std::runtime_error("invalid ARM64 Vulkan image");
        const auto subresource = guest.read<VkImageSubresource>(guest.args[2]);
        VkSubresourceLayout layout{};
        Native<PFN_vkGetImageSubresourceLayout>("vkGetImageSubresourceLayout")(device, image, &subresource, &layout);
        guest.write(guest.args[3], layout);
        return 0;
    }
    if (name == "vkCreateRenderPass")
        return TraceResult(name, CreateRenderPass(guest, device, record));
    if (name == "vkCreateFramebuffer")
        return TraceResult(name, CreateFramebuffer(guest, device, record));
    if (name == "vkCreateShaderModule")
        return TraceResult(name, CreateShaderModule(guest, device, record));
    if (name == "vkCreateGraphicsPipelines")
        return TraceResult(name, CreatePipelines<VkGraphicsPipelineCreateInfo, GraphicsPipelineData>(
                                     guest, device, record, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     [](const Guest& source, const Device& owner, auto& info, auto& data) {
                                         return data.Copy(source, owner, info);
                                     },
                                     Native<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")));
    if (name == "vkCreateComputePipelines")
        return TraceResult(name, CreatePipelines<VkComputePipelineCreateInfo, ShaderStageData>(
                                     guest, device, record, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     [](const Guest& source, const Device& owner, auto& info, auto& stage) {
                                         return info.sType == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO &&
                                                !info.pNext && owner.pipelineLayouts.contains(info.layout) &&
                                                stage.Copy(source, owner, info.stage);
                                     },
                                     Native<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")));
    if (name == "vkDestroyPipeline") {
        DestroyResource(guest, device, record.pipelines, Native<PFN_vkDestroyPipeline>("vkDestroyPipeline"));
        return 0;
    }
    if (name == "vkDestroyRenderPass") {
        DestroyTrackedObject(guest, device, record.renderPasses,
                             Native<PFN_vkDestroyRenderPass>("vkDestroyRenderPass"));
        return 0;
    }
    if (name == "vkDestroyFramebuffer") {
        DestroyTrackedObject(guest, device, record.framebuffers,
                             Native<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer"));
        return 0;
    }
    if (name == "vkDestroyShaderModule") {
        DestroyTrackedObject(guest, device, record.shaders, Native<PFN_vkDestroyShaderModule>("vkDestroyShaderModule"));
        return 0;
    }
    if (name == "vkGetRenderAreaGranularity") {
        const auto pass = guest.handle<VkRenderPass>(1);
        if (!record.renderPasses.contains(pass))
            throw std::runtime_error("invalid ARM64 Vulkan render pass");
        VkExtent2D granularity{};
        Native<PFN_vkGetRenderAreaGranularity>("vkGetRenderAreaGranularity")(device, pass, &granularity);
        guest.write(guest.args[2], granularity);
        return 0;
    }
    if (name == "vkCreatePipelineCache")
        return TraceResult(name, CreatePipelineCache(guest, device, record));
    if (name == "vkDestroyPipelineCache") {
        DestroyTrackedObject(guest, device, record.pipelineCaches,
                             Native<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache"));
        return 0;
    }
    if (name == "vkGetPipelineCacheData" || name == "vkMergePipelineCaches") {
        const auto cache = guest.handle<VkPipelineCache>(1);
        if (!record.pipelineCaches.contains(cache))
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        if (name == "vkGetPipelineCacheData")
            return Result(GetPipelineCacheData(guest, device, cache));
        const auto count = static_cast<uint32_t>(guest.args[2]);
        const auto sources = guest.array(guest.handle<const VkPipelineCache*>(3), count);
        if (!count || !std::ranges::all_of(sources, [&](VkPipelineCache source) {
                return source != cache && record.pipelineCaches.contains(source);
            }))
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        return Result(Native<PFN_vkMergePipelineCaches>("vkMergePipelineCaches")(device, cache, count, sources.data()));
    }
    if (name == "vkFlushMappedMemoryRanges" || name == "vkInvalidateMappedMemoryRanges")
        return Result(SyncMappedRanges(guest, device, record, name == "vkFlushMappedMemoryRanges"));
    if (name == "vkBindImageMemory")
        return Result(
            BindMemory(guest, device, record, record.images, Native<PFN_vkBindImageMemory>("vkBindImageMemory")));
    if (name == "vkBindBufferMemory")
        return Result(
            BindMemory(guest, device, record, record.buffers, Native<PFN_vkBindBufferMemory>("vkBindBufferMemory")));
    if (name == "vkBindImageMemory2" || name == "vkBindImageMemory2KHR")
        return Result(BindMemory2(guest, device, VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, record, record.images,
                                  &VkBindImageMemoryInfo::image, Native<PFN_vkBindImageMemory2>("vkBindImageMemory2")));
    if (name == "vkBindBufferMemory2" || name == "vkBindBufferMemory2KHR")
        return Result(BindMemory2(guest, device, VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, record, record.buffers,
                                  &VkBindBufferMemoryInfo::buffer,
                                  Native<PFN_vkBindBufferMemory2>("vkBindBufferMemory2")));
    if (name == "vkGetImageMemoryRequirements") {
        GetMemoryRequirements(guest, record.images);
    }
    else if (name == "vkGetBufferMemoryRequirements") {
        GetMemoryRequirements(guest, record.buffers);
    }
    else if (name == "vkGetImageMemoryRequirements2" || name == "vkGetImageMemoryRequirements2KHR") {
        GetMemoryRequirements2(guest, device, VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, record.images,
                               &VkImageMemoryRequirementsInfo2::image,
                               Native<PFN_vkGetImageMemoryRequirements2>("vkGetImageMemoryRequirements2"));
    }
    else if (name == "vkGetBufferMemoryRequirements2" || name == "vkGetBufferMemoryRequirements2KHR") {
        GetMemoryRequirements2(guest, device, VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, record.buffers,
                               &VkBufferMemoryRequirementsInfo2::buffer,
                               Native<PFN_vkGetBufferMemoryRequirements2>("vkGetBufferMemoryRequirements2"));
    }
    else if (name == "vkDestroyImage") {
        const auto image = guest.handle<VkImage>(1);
        const auto entry = record.images.find(image);
        if (entry != record.images.end() && entry->second.swapchain)
            throw std::runtime_error("ARM64 Vulkan image is owned by a swapchain");
        if (std::ranges::any_of(record.views, [&](const auto& entry) {
                return entry.second.image == image;
            }))
            throw std::runtime_error("ARM64 Vulkan image still owns views");
        DestroyResource(guest, device, record.images, Native<PFN_vkDestroyImage>("vkDestroyImage"));
    }
    else if (name == "vkDestroyBuffer") {
        DestroyResource(guest, device, record.buffers, Native<PFN_vkDestroyBuffer>("vkDestroyBuffer"));
    }
    else if (name == "vkDestroyImageView") {
        const auto view = guest.handle<VkImageView>(1);
        if (!view)
            return 0;
        const auto entry = record.views.find(view);
        if (guest.args[2] || entry == record.views.end())
            throw std::runtime_error("invalid ARM64 Vulkan image view");
        Native<PFN_vkDestroyImageView>("vkDestroyImageView")(device, view, nullptr);
        if (!--entry->second.references)
            record.views.erase(entry);
    }
    else if (name == "vkFreeMemory" || name == "vkUnmapMemory") {
        const auto memory = guest.handle<VkDeviceMemory>(1);
        const bool free = name == "vkFreeMemory";
        if (free && !memory)
            return 0;
        const auto entry = record.memory.find(memory);
        if (entry == record.memory.end() || (free ? !!guest.args[2] : !entry->second.mapped))
            throw std::runtime_error("invalid ARM64 Vulkan memory allocation");
        RemoveMapping(entry->second);
        if (free) {
            Native<PFN_vkFreeMemory>("vkFreeMemory")(device, memory, nullptr);
            record.memory.erase(entry);
        }
        else {
            Native<PFN_vkUnmapMemory>("vkUnmapMemory")(device, memory);
        }
    }
    else {
        throw std::runtime_error("unsupported ARM64 Vulkan resource command");
    }
    return 0;
}

template <typename Element, typename Enumerate>
VkResult EnumerateResult(const Guest& guest, unsigned countIndex, Enumerate enumerate)
{
    const auto countAddress = guest.args[countIndex];
    const auto dataAddress = guest.args[countIndex + 1];
    auto* countOutput = guest.output<uint32_t>(countAddress);
    uint32_t count = 0;
    const auto available = enumerate(&count, nullptr);
    if (available != VK_SUCCESS)
        return available;
    if (!dataAddress) {
        std::memcpy(countOutput, &count, sizeof(count));
        return VK_SUCCESS;
    }
    uint32_t requested;
    std::memcpy(&requested, countOutput, sizeof(requested));
    const auto capacity = std::min(count, requested);
    auto* output = guest.output<Element>(dataAddress, capacity);
    if (!capacity) {
        const uint32_t zero = 0;
        std::memcpy(countOutput, &zero, sizeof(zero));
        return count ? VK_INCOMPLETE : VK_SUCCESS;
    }
    std::vector<Element> values(capacity);
    count = capacity;
    const auto result = enumerate(&count, values.data());
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
        if (count > capacity)
            return VK_ERROR_INITIALIZATION_FAILED;
        std::memcpy(output, values.data(), count * sizeof(Element));
        std::memcpy(countOutput, &count, sizeof(count));
    }
    return result;
}

VkResult CreateDescriptorSetLayout(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkDescriptorSetLayoutCreateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO || info.pNext || info.flags || guest.args[2])
        return VK_ERROR_INITIALIZATION_FAILED;
    auto bindings = guest.array(info.pBindings, info.bindingCount);
    std::vector<std::vector<VkSampler>> samplers(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        auto& binding = bindings[i];
        if (binding.pImmutableSamplers && (binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                                           binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
            auto& values = samplers[i];
            values = guest.array(binding.pImmutableSamplers, binding.descriptorCount);
            if (!std::ranges::all_of(values, [&](VkSampler sampler) {
                    return record.samplers.contains(sampler);
                }))
                return VK_ERROR_INITIALIZATION_FAILED;
            binding.pImmutableSamplers = values.empty() ? nullptr : values.data();
        }
        else {
            binding.pImmutableSamplers = nullptr;
        }
    }
    info.pBindings = bindings.empty() ? nullptr : bindings.data();
    return CreateResource<VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout>(
        guest, device, info, Native<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout"),
        Native<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout"), [&](VkDescriptorSetLayout layout) {
            record.descriptorLayouts.insert(layout);
        });
}

VkResult CreatePipelineLayout(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkPipelineLayoutCreateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO || info.pNext || info.flags || guest.args[2])
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto layouts = guest.array(info.pSetLayouts, info.setLayoutCount);
    const auto ranges = guest.array(info.pPushConstantRanges, info.pushConstantRangeCount);
    if (!std::ranges::all_of(layouts, [&](VkDescriptorSetLayout layout) {
            return record.descriptorLayouts.contains(layout);
        }))
        return VK_ERROR_INITIALIZATION_FAILED;
    info.pSetLayouts = layouts.empty() ? nullptr : layouts.data();
    info.pPushConstantRanges = ranges.empty() ? nullptr : ranges.data();
    return CreateResource<VkPipelineLayoutCreateInfo, VkPipelineLayout>(
        guest, device, info, Native<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout"),
        Native<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout"), [&](VkPipelineLayout layout) {
            record.pipelineLayouts.insert(layout);
        });
}

VkResult AllocateDescriptorSets(const Guest& guest, VkDevice device, Device& record)
{
    auto info = guest.read<VkDescriptorSetAllocateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO || info.pNext || !info.descriptorSetCount ||
        !record.descriptorPools.contains(info.descriptorPool))
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto layouts = guest.array(info.pSetLayouts, info.descriptorSetCount);
    if (!std::ranges::all_of(layouts, [&](VkDescriptorSetLayout layout) {
            return record.descriptorLayouts.contains(layout);
        }))
        return VK_ERROR_INITIALIZATION_FAILED;
    info.pSetLayouts = layouts.data();
    auto* output = guest.output<VkDescriptorSet>(guest.args[2], info.descriptorSetCount);
    std::vector<VkDescriptorSet> sets(info.descriptorSetCount);
    // Prepare map entries before invoking drivers that cannot free individual sets.
    std::unordered_map<VkDescriptorSet, VkDescriptorPool> entries;
    entries.reserve(sets.size());
    record.descriptorSets.reserve(record.descriptorSets.size() + sets.size());
    for (uintptr_t i = 0; i < sets.size(); ++i)
        entries.emplace(reinterpret_cast<VkDescriptorSet>(i + 1), info.descriptorPool);
    const auto result = Native<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(device, &info, sets.data());
    if (result == VK_SUCCESS) {
        for (const auto set : sets) {
            auto entry = entries.extract(entries.begin());
            entry.key() = set;
            record.descriptorSets.insert(std::move(entry));
        }
    }
    else {
        std::ranges::fill(sets, VK_NULL_HANDLE);
    }
    std::memcpy(output, sets.data(), sets.size() * sizeof(VkDescriptorSet));
    return result;
}

void UpdateDescriptorSets(const Guest& guest, VkDevice device, const Device& record)
{
    auto writes = guest.array(guest.handle<const VkWriteDescriptorSet*>(2), static_cast<uint32_t>(guest.args[1]));
    const auto copies = guest.array(guest.handle<const VkCopyDescriptorSet*>(4), static_cast<uint32_t>(guest.args[3]));

    struct DescriptorData {
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkDescriptorBufferInfo> buffers;
        std::vector<VkBufferView> views;
    };

    std::vector<DescriptorData> arrays(writes.size());
    for (size_t i = 0; i < writes.size(); ++i) {
        auto& write = writes[i];
        auto& data = arrays[i];
        if (write.sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || write.pNext || !write.descriptorCount ||
            !record.descriptorSets.contains(write.dstSet))
            throw std::runtime_error("invalid ARM64 Vulkan descriptor write");
        switch (write.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            data.images = guest.array(write.pImageInfo, write.descriptorCount);
            for (auto& image : data.images) {
                if (write.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER &&
                    write.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                    image.sampler = VK_NULL_HANDLE;
                if (write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER)
                    image.imageView = VK_NULL_HANDLE;
                if ((image.sampler && !record.samplers.contains(image.sampler)) ||
                    (image.imageView && !record.views.contains(image.imageView)))
                    throw std::runtime_error("invalid ARM64 Vulkan descriptor image");
            }
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            data.buffers = guest.array(write.pBufferInfo, write.descriptorCount);
            if (!std::ranges::all_of(data.buffers, [&](const auto& buffer) {
                    return record.buffers.contains(buffer.buffer);
                }))
                throw std::runtime_error("invalid ARM64 Vulkan descriptor buffer");
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            data.views = guest.array(write.pTexelBufferView, write.descriptorCount);
            if (!std::ranges::all_of(data.views, [&](VkBufferView view) {
                    return record.bufferViews.contains(view);
                }))
                throw std::runtime_error("invalid ARM64 Vulkan descriptor buffer view");
            break;
        default:
            throw std::runtime_error("unsupported ARM64 Vulkan descriptor type");
        }
        write.pImageInfo = data.images.empty() ? nullptr : data.images.data();
        write.pBufferInfo = data.buffers.empty() ? nullptr : data.buffers.data();
        write.pTexelBufferView = data.views.empty() ? nullptr : data.views.data();
    }
    if (!std::ranges::all_of(copies, [&](const auto& copy) {
            return copy.sType == VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET && !copy.pNext &&
                   record.descriptorSets.contains(copy.srcSet) && record.descriptorSets.contains(copy.dstSet);
        }))
        throw std::runtime_error("invalid ARM64 Vulkan descriptor copy");
    Native<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(device, static_cast<uint32_t>(writes.size()),
                                                                 writes.data(), static_cast<uint32_t>(copies.size()),
                                                                 copies.data());
}

uint64_t CallDescriptors(std::string_view name, const Guest& guest, VkDevice device, Device& record)
{
    if (name == "vkCreateDescriptorSetLayout")
        return TraceResult(name, CreateDescriptorSetLayout(guest, device, record));
    if (name == "vkCreatePipelineLayout")
        return TraceResult(name, CreatePipelineLayout(guest, device, record));
    if (name == "vkCreateBufferView") {
        const auto info = guest.read<VkBufferViewCreateInfo>(guest.args[1]);
        if (info.sType != VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO || info.pNext || info.flags || guest.args[2] ||
            !record.buffers.contains(info.buffer))
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        return Result(CreateResource<VkBufferViewCreateInfo, VkBufferView>(
            guest, device, info, Native<PFN_vkCreateBufferView>("vkCreateBufferView"),
            Native<PFN_vkDestroyBufferView>("vkDestroyBufferView"), [&](VkBufferView view) {
                record.bufferViews.insert(view);
            }));
    }
    if (name == "vkDestroyBufferView") {
        DestroyTrackedObject(guest, device, record.bufferViews, Native<PFN_vkDestroyBufferView>("vkDestroyBufferView"));
        return 0;
    }
    if (name == "vkDestroyDescriptorSetLayout") {
        DestroyTrackedObject(guest, device, record.descriptorLayouts,
                             Native<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout"));
        return 0;
    }
    if (name == "vkDestroyPipelineLayout") {
        DestroyTrackedObject(guest, device, record.pipelineLayouts,
                             Native<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout"));
        return 0;
    }
    if (name == "vkCreateDescriptorPool") {
        auto info = guest.read<VkDescriptorPoolCreateInfo>(guest.args[1]);
        if (info.sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO || info.pNext ||
            (info.flags & ~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) || guest.args[2])
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        const auto sizes = guest.array(info.pPoolSizes, info.poolSizeCount);
        info.pPoolSizes = sizes.empty() ? nullptr : sizes.data();
        return TraceResult(name, CreateResource<VkDescriptorPoolCreateInfo, VkDescriptorPool>(
                                     guest, device, info, Native<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool"),
                                     Native<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool"),
                                     [&](VkDescriptorPool pool) {
                                         record.descriptorPools.emplace(pool, info.flags);
                                     }));
    }
    if (name == "vkAllocateDescriptorSets")
        return Result(AllocateDescriptorSets(guest, device, record));
    if (name == "vkUpdateDescriptorSets") {
        UpdateDescriptorSets(guest, device, record);
        return 0;
    }
    const auto pool = guest.handle<VkDescriptorPool>(1);
    if (name == "vkDestroyDescriptorPool" && !pool)
        return 0;
    const auto entry = record.descriptorPools.find(pool);
    if (entry == record.descriptorPools.end())
        throw std::runtime_error("invalid ARM64 Vulkan descriptor pool");
    if (name == "vkFreeDescriptorSets") {
        const auto count = static_cast<uint32_t>(guest.args[2]);
        const auto sets = guest.array(guest.handle<const VkDescriptorSet*>(3), count);
        if (!count || !(entry->second & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) ||
            !std::ranges::all_of(sets, [&](VkDescriptorSet set) {
                const auto found = record.descriptorSets.find(set);
                return !set || (found != record.descriptorSets.end() && found->second == pool);
            }))
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        const auto result = Native<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")(device, pool, count, sets.data());
        if (result == VK_SUCCESS)
            for (const auto set : sets)
                record.descriptorSets.erase(set);
        return Result(result);
    }
    if (guest.args[2])
        throw std::runtime_error("invalid ARM64 Vulkan descriptor pool flags or callbacks");
    if (name == "vkResetDescriptorPool") {
        const auto result = Native<PFN_vkResetDescriptorPool>("vkResetDescriptorPool")(device, pool, 0);
        if (result != VK_SUCCESS)
            return Result(result);
    }
    else {
        Native<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(device, pool, nullptr);
        record.descriptorPools.erase(entry);
    }
    std::erase_if(record.descriptorSets, [pool](const auto& item) {
        return item.second == pool;
    });
    return VK_SUCCESS;
}

template <typename Function> Function DeviceFunction(VkDevice device, const char* name)
{
    return reinterpret_cast<Function>(Native<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr")(device, name));
}

VkResult QueueSubmit(const Guest& guest, VulkanObjects& objects, std::unique_lock<std::recursive_mutex>& lock)
{
    const auto queue = guest.handle<VkQueue>(0);
    const auto owner = objects.queues.find(queue);
    if (owner == objects.queues.end())
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto& record = objects.devices.at(owner->second.device);
    const auto fence = guest.handle<VkFence>(3);
    if (fence && !record.fences.contains(fence))
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto count = static_cast<uint32_t>(guest.args[1]);
    auto submits = guest.array(guest.handle<const VkSubmitInfo*>(2), count);

    struct SubmitArrays {
        std::vector<VkSemaphore> waits;
        std::vector<VkPipelineStageFlags> stages;
        std::vector<VkCommandBuffer> commands;
        std::vector<VkSemaphore> signals;
    };

    std::vector<SubmitArrays> arrays(count);
    for (size_t i = 0; i < submits.size(); ++i) {
        auto& submit = submits[i];
        if (submit.sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || submit.pNext)
            return VK_ERROR_INITIALIZATION_FAILED;
        auto& data = arrays[i];
        data.waits = guest.array(submit.pWaitSemaphores, submit.waitSemaphoreCount);
        data.stages = guest.array(submit.pWaitDstStageMask, submit.waitSemaphoreCount);
        data.commands = guest.array(submit.pCommandBuffers, submit.commandBufferCount);
        data.signals = guest.array(submit.pSignalSemaphores, submit.signalSemaphoreCount);
        const auto ownsSemaphore = [&](VkSemaphore semaphore) {
            return record.semaphores.contains(semaphore);
        };
        if (!std::ranges::all_of(data.waits, ownsSemaphore) || !std::ranges::all_of(data.signals, ownsSemaphore))
            return VK_ERROR_INITIALIZATION_FAILED;
        for (const auto command : data.commands) {
            const auto registered = objects.commandBuffers.find(command);
            if (registered == objects.commandBuffers.end() || registered->second.device != owner->second.device ||
                registered->second.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
                record.commandPools.at(registered->second.pool) != owner->second.family)
                return VK_ERROR_INITIALIZATION_FAILED;
        }
        submit.pWaitSemaphores = data.waits.empty() ? nullptr : data.waits.data();
        submit.pWaitDstStageMask = data.stages.empty() ? nullptr : data.stages.data();
        submit.pCommandBuffers = data.commands.empty() ? nullptr : data.commands.data();
        submit.pSignalSemaphores = data.signals.empty() ? nullptr : data.signals.data();
    }
    lock.unlock();
    return Native<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, count, submits.empty() ? nullptr : submits.data(), fence);
}

VkResult AllocateCommandBuffers(const Guest& guest, VkDevice device, VulkanObjects& objects, const Device& record)
{
    const auto info = guest.read<VkCommandBufferAllocateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO || info.pNext || !info.commandBufferCount ||
        !record.commandPools.contains(info.commandPool) ||
        (info.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY && info.level != VK_COMMAND_BUFFER_LEVEL_SECONDARY))
        return VK_ERROR_INITIALIZATION_FAILED;
    auto* output = guest.output<VkCommandBuffer>(guest.args[2], info.commandBufferCount);
    std::vector<VkCommandBuffer> handles(info.commandBufferCount);
    const auto result = Native<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &info, handles.data());
    if (result == VK_SUCCESS) {
        try {
            for (const auto buffer : handles)
                objects.commandBuffers.emplace(buffer, CommandBuffer{device, info.commandPool, info.level});
        }
        catch (...) {
            for (const auto buffer : handles)
                objects.commandBuffers.erase(buffer);
            Native<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device, info.commandPool, info.commandBufferCount,
                                                                     handles.data());
            throw;
        }
    }
    // Vulkan writes VK_NULL_HANDLE to all entries on allocation failure.
    if (result != VK_SUCCESS)
        std::ranges::fill(handles, VK_NULL_HANDLE);
    std::memcpy(output, handles.data(), handles.size() * sizeof(VkCommandBuffer));
    return result;
}

struct BarrierData {
    std::vector<VkMemoryBarrier> memory;
    std::vector<VkBufferMemoryBarrier> buffers;
    std::vector<VkImageMemoryBarrier> images;

    BarrierData(const Guest& guest, const Device& record, unsigned index)
        : memory(guest.array(reinterpret_cast<const VkMemoryBarrier*>(guest.argument(index + 1)),
                             static_cast<uint32_t>(guest.argument(index)))),
          buffers(guest.array(reinterpret_cast<const VkBufferMemoryBarrier*>(guest.argument(index + 3)),
                              static_cast<uint32_t>(guest.argument(index + 2)))),
          images(guest.array(reinterpret_cast<const VkImageMemoryBarrier*>(guest.argument(index + 5)),
                             static_cast<uint32_t>(guest.argument(index + 4))))
    {
        if (std::ranges::any_of(memory,
                                [](const auto& barrier) {
                                    return barrier.sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER || barrier.pNext;
                                }) ||
            std::ranges::any_of(buffers,
                                [&](const auto& barrier) {
                                    return barrier.sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER || barrier.pNext ||
                                           !record.buffers.contains(barrier.buffer);
                                }) ||
            std::ranges::any_of(images, [&](const auto& barrier) {
                return barrier.sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER || barrier.pNext ||
                       !record.images.contains(barrier.image);
            }))
            throw std::runtime_error("invalid ARM64 Vulkan pipeline barrier");
    }
};

void PipelineBarrier(const Guest& guest, VkCommandBuffer buffer, const Device& record)
{
    const BarrierData data(guest, record, 4);
    Native<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        buffer, static_cast<VkPipelineStageFlags>(guest.args[1]), static_cast<VkPipelineStageFlags>(guest.args[2]),
        static_cast<VkDependencyFlags>(guest.args[3]), static_cast<uint32_t>(data.memory.size()), data.memory.data(),
        static_cast<uint32_t>(data.buffers.size()), data.buffers.data(), static_cast<uint32_t>(data.images.size()),
        data.images.data());
}

void WaitEvents(const Guest& guest, VkCommandBuffer buffer, const Device& record)
{
    const auto count = static_cast<uint32_t>(guest.args[1]);
    const auto events = guest.array(guest.handle<const VkEvent*>(2), count);
    if (!count || !std::ranges::all_of(events, [&](VkEvent event) {
            return record.events.contains(event);
        }))
        throw std::runtime_error("invalid ARM64 Vulkan events");
    const BarrierData data(guest, record, 5);
    Native<PFN_vkCmdWaitEvents>("vkCmdWaitEvents")(
        buffer, count, events.data(), static_cast<VkPipelineStageFlags>(guest.args[3]),
        static_cast<VkPipelineStageFlags>(guest.args[4]), static_cast<uint32_t>(data.memory.size()), data.memory.data(),
        static_cast<uint32_t>(data.buffers.size()), data.buffers.data(), static_cast<uint32_t>(data.images.size()),
        data.images.data());
}

void CopyImage(std::string_view name, const Guest& guest, VkCommandBuffer command, const Device& record)
{
    const auto source = guest.handle<VkImage>(1), destination = guest.handle<VkImage>(3);
    if (!record.images.contains(source) || !record.images.contains(destination))
        throw std::runtime_error("invalid ARM64 Vulkan image copy");
    const auto sourceLayout = static_cast<VkImageLayout>(guest.args[2]);
    const auto destinationLayout = static_cast<VkImageLayout>(guest.args[4]);
    const auto count = static_cast<uint32_t>(guest.args[5]);
    if (name == "vkCmdBlitImage") {
        const auto regions = guest.array(guest.handle<const VkImageBlit*>(6), count);
        Native<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command, source, sourceLayout, destination, destinationLayout,
                                                     count, regions.data(), static_cast<VkFilter>(guest.args[7]));
    }
    else if (name == "vkCmdResolveImage") {
        const auto regions = guest.array(guest.handle<const VkImageResolve*>(6), count);
        Native<PFN_vkCmdResolveImage>("vkCmdResolveImage")(command, source, sourceLayout, destination,
                                                           destinationLayout, count, regions.data());
    }
    else {
        const auto regions = guest.array(guest.handle<const VkImageCopy*>(6), count);
        Native<PFN_vkCmdCopyImage>("vkCmdCopyImage")(command, source, sourceLayout, destination, destinationLayout,
                                                     count, regions.data());
    }
}

void CallDraw(std::string_view name, const Guest& guest, VkCommandBuffer command, const Device& record)
{
    const auto u = [&](unsigned index) {
        return static_cast<uint32_t>(guest.args[index]);
    };
    const auto& f = guest.floatArgument;
    if (name == "vkCmdBindPipeline") {
        const auto pipeline = guest.handle<VkPipeline>(2);
        const auto type = static_cast<VkPipelineBindPoint>(guest.args[1]);
        const auto found = record.pipelines.find(pipeline);
        if (found == record.pipelines.end() || found->second != type)
            throw std::runtime_error("invalid ARM64 Vulkan pipeline binding");
        Native<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command, type, pipeline);
    }
    else if (name == "vkCmdBindVertexBuffers") {
        const auto buffers = guest.array(guest.handle<const VkBuffer*>(3), u(2));
        const auto offsets = guest.array(guest.handle<const VkDeviceSize*>(4), u(2));
        if (!std::ranges::all_of(buffers, [&](VkBuffer buffer) {
                return record.buffers.contains(buffer);
            }))
            throw std::runtime_error("invalid ARM64 Vulkan vertex buffer");
        Native<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")(command, u(1), u(2), buffers.data(),
                                                                     offsets.data());
    }
    else if (name == "vkCmdBindIndexBuffer" || name == "vkCmdDrawIndirect" || name == "vkCmdDrawIndexedIndirect" ||
             name == "vkCmdDispatchIndirect") {
        const auto buffer = guest.handle<VkBuffer>(1);
        if (!record.buffers.contains(buffer))
            throw std::runtime_error("invalid ARM64 Vulkan draw buffer");
        if (name == "vkCmdBindIndexBuffer")
            Native<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer")(command, buffer, guest.args[2],
                                                                     static_cast<VkIndexType>(u(3)));
        else if (name == "vkCmdDispatchIndirect")
            Native<PFN_vkCmdDispatchIndirect>("vkCmdDispatchIndirect")(command, buffer, guest.args[2]);
        else
            Native<PFN_vkCmdDrawIndirect>(name == "vkCmdDrawIndirect"
                                              ? "vkCmdDrawIndirect"
                                              : "vkCmdDrawIndexedIndirect")(command, buffer, guest.args[2], u(3), u(4));
    }
    else if (name == "vkCmdDraw") {
        Native<PFN_vkCmdDraw>("vkCmdDraw")(command, u(1), u(2), u(3), u(4));
    }
    else if (name == "vkCmdDrawIndexed") {
        Native<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed")(command, u(1), u(2), u(3), static_cast<int32_t>(u(4)), u(5));
    }
    else if (name == "vkCmdDispatch") {
        Native<PFN_vkCmdDispatch>("vkCmdDispatch")(command, u(1), u(2), u(3));
    }
    else if (name == "vkCmdSetViewport") {
        const auto viewports = guest.array(guest.handle<const VkViewport*>(3), u(2));
        Native<PFN_vkCmdSetViewport>("vkCmdSetViewport")(command, u(1), u(2), viewports.data());
    }
    else if (name == "vkCmdSetScissor") {
        const auto scissors = guest.array(guest.handle<const VkRect2D*>(3), u(2));
        Native<PFN_vkCmdSetScissor>("vkCmdSetScissor")(command, u(1), u(2), scissors.data());
    }
    else if (name == "vkCmdSetLineWidth") {
        Native<PFN_vkCmdSetLineWidth>("vkCmdSetLineWidth")(command, f(0));
    }
    else if (name == "vkCmdSetDepthBias") {
        Native<PFN_vkCmdSetDepthBias>("vkCmdSetDepthBias")(command, f(0), f(1), f(2));
    }
    else if (name == "vkCmdSetDepthBounds") {
        Native<PFN_vkCmdSetDepthBounds>("vkCmdSetDepthBounds")(command, f(0), f(1));
    }
    else if (name == "vkCmdSetBlendConstants") {
        const auto blend = guest.read<std::array<float, 4>>(guest.args[1]);
        Native<PFN_vkCmdSetBlendConstants>("vkCmdSetBlendConstants")(command, blend.data());
    }
    else if (name == "vkCmdSetStencilCompareMask") {
        Native<PFN_vkCmdSetStencilCompareMask>("vkCmdSetStencilCompareMask")(command, u(1), u(2));
    }
    else if (name == "vkCmdSetStencilWriteMask") {
        Native<PFN_vkCmdSetStencilWriteMask>("vkCmdSetStencilWriteMask")(command, u(1), u(2));
    }
    else if (name == "vkCmdSetStencilReference") {
        Native<PFN_vkCmdSetStencilReference>("vkCmdSetStencilReference")(command, u(1), u(2));
    }
}

uint64_t CallQueries(std::string_view name, const Guest& guest, VulkanObjects& objects,
                     std::unique_lock<std::recursive_mutex>& lock)
{
    const auto u = [&](unsigned index) {
        return static_cast<uint32_t>(guest.args[index]);
    };
    if (!name.starts_with("vkCmd")) {
        const auto device = guest.handle<VkDevice>(0);
        const auto owner = objects.devices.find(device);
        if (owner == objects.devices.end())
            throw std::runtime_error("invalid ARM64 Vulkan device");
        auto& pools = owner->second.queryPools;
        if (name == "vkCreateQueryPool") {
            const auto info = guest.read<VkQueryPoolCreateInfo>(guest.args[1]);
            if (info.sType != VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO || info.pNext || info.flags || guest.args[2] ||
                !info.queryCount ||
                (info.queryType != VK_QUERY_TYPE_OCCLUSION && info.queryType != VK_QUERY_TYPE_TIMESTAMP &&
                 info.queryType != VK_QUERY_TYPE_PIPELINE_STATISTICS))
                return Result(VK_ERROR_INITIALIZATION_FAILED);
            return TraceResult(name, CreateResource<VkQueryPoolCreateInfo, VkQueryPool>(
                                         guest, device, info, Native<PFN_vkCreateQueryPool>("vkCreateQueryPool"),
                                         Native<PFN_vkDestroyQueryPool>("vkDestroyQueryPool"), [&](VkQueryPool handle) {
                                             pools.emplace(handle, QueryPool{info.queryType, info.queryCount});
                                         }));
        }
        if (name == "vkDestroyQueryPool") {
            DestroyResource(guest, device, pools, Native<PFN_vkDestroyQueryPool>("vkDestroyQueryPool"));
            return 0;
        }
        const auto pool = guest.handle<VkQueryPool>(1);
        const auto query = pools.find(pool);
        const auto bytes = guest.args[4];
        if (query == pools.end() || u(2) >= query->second.count || !u(3) || u(3) > query->second.count - u(2) ||
            !bytes || bytes > UINT_MAX)
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        auto* output = guest.memory(guest.args[5], static_cast<unsigned>(bytes), true);
        lock.unlock();
        return Result(Native<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(
            device, pool, u(2), u(3), static_cast<size_t>(bytes), output, guest.args[6], u(7)));
    }
    const auto command = guest.handle<VkCommandBuffer>(0);
    const auto owner = objects.commandBuffers.find(command);
    if (owner == objects.commandBuffers.end())
        throw std::runtime_error("invalid ARM64 Vulkan command buffer");
    const auto& record = objects.devices.at(owner->second.device);
    const bool timestamp = name == "vkCmdWriteTimestamp";
    const auto pool = guest.handle<VkQueryPool>(timestamp ? 2 : 1);
    const auto query = record.queryPools.find(pool);
    const auto first = u(timestamp ? 3 : 2);
    const auto count = name == "vkCmdResetQueryPool" || name == "vkCmdCopyQueryPoolResults" ? u(3) : 1;
    if (query == record.queryPools.end() || first >= query->second.count || !count ||
        count > query->second.count - first)
        throw std::runtime_error("invalid ARM64 Vulkan query range");
    if (timestamp) {
        if (query->second.type != VK_QUERY_TYPE_TIMESTAMP)
            throw std::runtime_error("invalid ARM64 Vulkan timestamp pool");
        Native<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(command, static_cast<VkPipelineStageFlagBits>(u(1)),
                                                               pool, first);
    }
    else if (name == "vkCmdBeginQuery") {
        Native<PFN_vkCmdBeginQuery>("vkCmdBeginQuery")(command, pool, first, u(3));
    }
    else if (name == "vkCmdEndQuery") {
        Native<PFN_vkCmdEndQuery>("vkCmdEndQuery")(command, pool, first);
    }
    else if (name == "vkCmdResetQueryPool") {
        Native<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(command, pool, first, count);
    }
    else {
        const auto buffer = guest.handle<VkBuffer>(4);
        if (!record.buffers.contains(buffer))
            throw std::runtime_error("invalid ARM64 Vulkan query result buffer");
        Native<PFN_vkCmdCopyQueryPoolResults>("vkCmdCopyQueryPoolResults")(command, pool, first, count, buffer,
                                                                           guest.args[5], guest.args[6], u(7));
    }
    return 0;
}

uint64_t CallCommands(std::string_view name, const Guest& guest, VulkanObjects& objects,
                      std::unique_lock<std::recursive_mutex>& lock)
{
    if (name == "vkQueueSubmit")
        return Result(QueueSubmit(guest, objects, lock));
    if (name == "vkAllocateCommandBuffers" || name == "vkFreeCommandBuffers" || name.ends_with("CommandPool")) {
        const auto device = guest.handle<VkDevice>(0);
        const auto owner = objects.devices.find(device);
        if (owner == objects.devices.end())
            throw std::runtime_error("invalid ARM64 Vulkan device");
        auto& record = owner->second;
        if (name == "vkAllocateCommandBuffers")
            return Result(AllocateCommandBuffers(guest, device, objects, record));
        if (name == "vkCreateCommandPool") {
            const auto info = guest.read<VkCommandPoolCreateInfo>(guest.args[1]);
            if (info.sType != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO || info.pNext || guest.args[2] ||
                !record.queueCounts.contains(info.queueFamilyIndex))
                return Result(VK_ERROR_INITIALIZATION_FAILED);
            return TraceResult(name,
                               CreateResource<VkCommandPoolCreateInfo, VkCommandPool>(
                                   guest, device, info, Native<PFN_vkCreateCommandPool>("vkCreateCommandPool"),
                                   Native<PFN_vkDestroyCommandPool>("vkDestroyCommandPool"), [&](VkCommandPool pool) {
                                       record.commandPools.emplace(pool, info.queueFamilyIndex);
                                   }));
        }
        const auto pool = guest.handle<VkCommandPool>(1);
        if (name == "vkDestroyCommandPool" && !pool)
            return 0;
        if (!record.commandPools.contains(pool))
            throw std::runtime_error("invalid ARM64 Vulkan command pool");
        if (name == "vkResetCommandPool")
            return Result(Native<PFN_vkResetCommandPool>("vkResetCommandPool")(
                device, pool, static_cast<VkCommandPoolResetFlags>(guest.args[2])));
        if (name == "vkFreeCommandBuffers") {
            const auto count = static_cast<uint32_t>(guest.args[2]);
            const auto buffers = guest.array(guest.handle<const VkCommandBuffer*>(3), count);
            for (const auto buffer : buffers) {
                if (!buffer)
                    continue;
                const auto entry = objects.commandBuffers.find(buffer);
                if (entry == objects.commandBuffers.end() || entry->second.device != device ||
                    entry->second.pool != pool)
                    throw std::runtime_error("invalid ARM64 Vulkan command buffer free");
            }
            Native<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device, pool, count, buffers.data());
            for (const auto buffer : buffers)
                objects.commandBuffers.erase(buffer);
            return 0;
        }
        if (guest.args[2])
            throw std::runtime_error("ARM64 Vulkan allocation callbacks are unsupported");
        Native<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
        std::erase_if(objects.commandBuffers, [&](const auto& entry) {
            return entry.second.device == device && entry.second.pool == pool;
        });
        record.commandPools.erase(pool);
        return 0;
    }

    const auto buffer = guest.handle<VkCommandBuffer>(0);
    const auto owner = objects.commandBuffers.find(buffer);
    if (owner == objects.commandBuffers.end())
        throw std::runtime_error("invalid ARM64 Vulkan command buffer");
    const auto& record = objects.devices.at(owner->second.device);
    if (name == "vkBeginCommandBuffer") {
        auto info = guest.read<VkCommandBufferBeginInfo>(guest.args[1]);
        if (info.sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO || info.pNext)
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        VkCommandBufferInheritanceInfo inheritance{};
        if (owner->second.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
            if (!info.pInheritanceInfo)
                return Result(VK_ERROR_INITIALIZATION_FAILED);
            inheritance =
                guest.read<VkCommandBufferInheritanceInfo>(reinterpret_cast<uintptr_t>(info.pInheritanceInfo));
            if (inheritance.sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO || inheritance.pNext ||
                (inheritance.renderPass && !record.renderPasses.contains(inheritance.renderPass)) ||
                (inheritance.framebuffer && !record.framebuffers.contains(inheritance.framebuffer)))
                return Result(VK_ERROR_INITIALIZATION_FAILED);
            info.pInheritanceInfo = &inheritance;
        }
        else {
            info.pInheritanceInfo = nullptr;
        }
        return Result(Native<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(buffer, &info));
    }
    if (name == "vkEndCommandBuffer")
        return Result(Native<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(buffer));
    if (name == "vkResetCommandBuffer")
        return Result(Native<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(
            buffer, static_cast<VkCommandBufferResetFlags>(guest.args[1])));
    if (name == "vkCmdPipelineBarrier") {
        PipelineBarrier(guest, buffer, record);
    }
    else if (name == "vkCmdWaitEvents") {
        WaitEvents(guest, buffer, record);
    }
    else if (name == "vkCmdSetEvent" || name == "vkCmdResetEvent") {
        const auto event = guest.handle<VkEvent>(1);
        if (!record.events.contains(event))
            throw std::runtime_error("invalid ARM64 Vulkan event");
        const auto stage = static_cast<VkPipelineStageFlags>(guest.args[2]);
        if (name == "vkCmdSetEvent")
            Native<PFN_vkCmdSetEvent>("vkCmdSetEvent")(buffer, event, stage);
        else
            Native<PFN_vkCmdResetEvent>("vkCmdResetEvent")(buffer, event, stage);
    }
    else if (name == "vkCmdBeginRenderPass") {
        auto info = guest.read<VkRenderPassBeginInfo>(guest.args[1]);
        if (info.sType != VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO || info.pNext ||
            !record.renderPasses.contains(info.renderPass) || !record.framebuffers.contains(info.framebuffer))
            throw std::runtime_error("invalid ARM64 Vulkan render pass begin");
        const auto clears = guest.array(info.pClearValues, info.clearValueCount);
        info.pClearValues = clears.empty() ? nullptr : clears.data();
        Native<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(buffer, &info,
                                                                 static_cast<VkSubpassContents>(guest.args[2]));
    }
    else if (name == "vkCmdNextSubpass") {
        Native<PFN_vkCmdNextSubpass>("vkCmdNextSubpass")(buffer, static_cast<VkSubpassContents>(guest.args[1]));
    }
    else if (name == "vkCmdEndRenderPass") {
        Native<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(buffer);
    }
    else if (name == "vkCmdBindDescriptorSets") {
        const auto layout = guest.handle<VkPipelineLayout>(2);
        const auto sets = guest.array(guest.handle<const VkDescriptorSet*>(5), static_cast<uint32_t>(guest.args[4]));
        const auto offsets = guest.array(guest.handle<const uint32_t*>(7), static_cast<uint32_t>(guest.args[6]));
        if (!record.pipelineLayouts.contains(layout) || !std::ranges::all_of(sets, [&](VkDescriptorSet set) {
                return record.descriptorSets.contains(set);
            }))
            throw std::runtime_error("invalid ARM64 Vulkan descriptor binding");
        Native<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
            buffer, static_cast<VkPipelineBindPoint>(guest.args[1]), layout, static_cast<uint32_t>(guest.args[3]),
            static_cast<uint32_t>(sets.size()), sets.data(), static_cast<uint32_t>(offsets.size()), offsets.data());
    }
    else if (name == "vkCmdPushConstants") {
        const auto layout = guest.handle<VkPipelineLayout>(1);
        const auto bytes = static_cast<uint32_t>(guest.args[4]);
        if (!record.pipelineLayouts.contains(layout) || !bytes)
            throw std::runtime_error("invalid ARM64 Vulkan push constants");
        const auto* values = guest.memory(guest.args[5], bytes, false);
        Native<PFN_vkCmdPushConstants>("vkCmdPushConstants")(buffer, layout,
                                                             static_cast<VkShaderStageFlags>(guest.args[2]),
                                                             static_cast<uint32_t>(guest.args[3]), bytes, values);
    }
    else if (name == "vkCmdExecuteCommands") {
        const auto count = static_cast<uint32_t>(guest.args[1]);
        const auto secondaryBuffers = guest.array(guest.handle<const VkCommandBuffer*>(2), count);
        for (const auto command : secondaryBuffers) {
            const auto secondary = objects.commandBuffers.find(command);
            if (secondary == objects.commandBuffers.end() || secondary->second.device != owner->second.device ||
                secondary->second.level != VK_COMMAND_BUFFER_LEVEL_SECONDARY ||
                record.commandPools.at(secondary->second.pool) != record.commandPools.at(owner->second.pool))
                throw std::runtime_error("invalid ARM64 Vulkan secondary command buffer");
        }
        Native<PFN_vkCmdExecuteCommands>("vkCmdExecuteCommands")(buffer, count, secondaryBuffers.data());
    }
    else if (name == "vkCmdClearColorImage" || name == "vkCmdClearDepthStencilImage") {
        const auto image = guest.handle<VkImage>(1);
        if (!record.images.contains(image))
            throw std::runtime_error("invalid ARM64 Vulkan image");
        const auto layout = static_cast<VkImageLayout>(guest.args[2]);
        const auto count = static_cast<uint32_t>(guest.args[4]);
        const auto ranges = guest.array(guest.handle<const VkImageSubresourceRange*>(5), count);
        if (name == "vkCmdClearColorImage") {
            const auto color = guest.read<VkClearColorValue>(guest.args[3]);
            Native<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(buffer, image, layout, &color, count,
                                                                     ranges.data());
        }
        else {
            const auto depth = guest.read<VkClearDepthStencilValue>(guest.args[3]);
            Native<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(buffer, image, layout, &depth, count,
                                                                                   ranges.data());
        }
    }
    else if (name == "vkCmdClearAttachments") {
        const auto attachments =
            guest.array(guest.handle<const VkClearAttachment*>(2), static_cast<uint32_t>(guest.args[1]));
        const auto rects = guest.array(guest.handle<const VkClearRect*>(4), static_cast<uint32_t>(guest.args[3]));
        Native<PFN_vkCmdClearAttachments>("vkCmdClearAttachments")(buffer, static_cast<uint32_t>(attachments.size()),
                                                                   attachments.data(),
                                                                   static_cast<uint32_t>(rects.size()), rects.data());
    }
    else if (name == "vkCmdCopyImage" || name == "vkCmdBlitImage" || name == "vkCmdResolveImage") {
        CopyImage(name, guest, buffer, record);
    }
    else if (name == "vkCmdFillBuffer" || name == "vkCmdUpdateBuffer") {
        const auto destination = guest.handle<VkBuffer>(1);
        if (!record.buffers.contains(destination))
            throw std::runtime_error("invalid ARM64 Vulkan destination buffer");
        if (name == "vkCmdFillBuffer") {
            Native<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(buffer, destination, guest.args[2], guest.args[3],
                                                           static_cast<uint32_t>(guest.args[4]));
        }
        else {
            const auto size = guest.args[3];
            if (!size || size > 65536)
                throw std::runtime_error("invalid ARM64 Vulkan update buffer size");
            const auto* data = guest.memory(guest.args[4], static_cast<unsigned>(size), false);
            Native<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(buffer, destination, guest.args[2], size, data);
        }
    }
    else if (name == "vkCmdCopyBuffer") {
        const auto source = guest.handle<VkBuffer>(1), destination = guest.handle<VkBuffer>(2);
        if (!record.buffers.contains(source) || !record.buffers.contains(destination))
            throw std::runtime_error("invalid ARM64 Vulkan copy buffer");
        const auto count = static_cast<uint32_t>(guest.args[3]);
        const auto regions = guest.array(guest.handle<const VkBufferCopy*>(4), count);
        Native<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(buffer, source, destination, count, regions.data());
    }
    else {
        const bool toImage = name == "vkCmdCopyBufferToImage";
        const auto data = guest.handle<VkBuffer>(toImage ? 1 : 3);
        const auto image = guest.handle<VkImage>(toImage ? 2 : 1);
        const auto layout = static_cast<VkImageLayout>(guest.args[toImage ? 3 : 2]);
        if (!record.buffers.contains(data) || !record.images.contains(image))
            throw std::runtime_error("invalid ARM64 Vulkan buffer image copy");
        const auto count = static_cast<uint32_t>(guest.args[4]);
        const auto regions = guest.array(guest.handle<const VkBufferImageCopy*>(5), count);
        if (toImage)
            Native<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(buffer, data, image, layout, count,
                                                                         regions.data());
        else
            Native<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(buffer, image, layout, data, count,
                                                                         regions.data());
    }
    return 0;
}

bool HostSwapchainAvailable(VulkanObjects& objects, VkPhysicalDevice physical)
{
    if (!objects.instances.at(objects.physicalDevices.at(physical)).surface)
        return false;
    const auto enumerate = Native<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    uint32_t count = 0;
    if (enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS || !count)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (enumerate(physical, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    return std::ranges::any_of(std::span(extensions).first(count), [](const auto& extension) {
        return std::string_view(extension.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    });
}

VkResult CreateSwapchain(const Guest& guest, VkDevice device, Device& record, VulkanObjects& objects,
                         std::unique_lock<std::recursive_mutex>& lock)
{
    auto info = guest.read<VkSwapchainCreateInfoKHR>(guest.args[1]);
    std::vector<uint32_t> families;
    if (info.sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR || info.pNext || info.flags || guest.args[2] ||
        !CopySharingInfo(guest, record, info.imageSharingMode, info.queueFamilyIndexCount, info.pQueueFamilyIndices,
                         families))
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto surfaceEntry = objects.surfaces.find(info.surface);
    if (surfaceEntry == objects.surfaces.end() ||
        surfaceEntry->second.instance != objects.physicalDevices.at(record.physical))
        return VK_ERROR_SURFACE_LOST_KHR;
    auto& surface = surfaceEntry->second;
    const auto previous = record.swapchains.find(info.oldSwapchain);
    if (surface.creating ||
        (info.oldSwapchain ? previous == record.swapchains.end() || previous->second.surface != info.surface ||
                                 surface.active != info.oldSwapchain
                           : surface.active != VK_NULL_HANDLE))
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;

    auto* output = guest.output<VkSwapchainKHR>(guest.args[3]);
    const auto create = DeviceFunction<PFN_vkCreateSwapchainKHR>(device, "vkCreateSwapchainKHR");
    const auto destroy = DeviceFunction<PFN_vkDestroySwapchainKHR>(device, "vkDestroySwapchainKHR");
    const auto enumerate = DeviceFunction<PFN_vkGetSwapchainImagesKHR>(device, "vkGetSwapchainImagesKHR");
    if (!create || !destroy || !enumerate)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info.oldSwapchain) {
        // The old swapchain is retired even when the replacement fails.
        previous->second.retired = true;
        surface.active = VK_NULL_HANDLE;
    }
    surface.creating = true;
    lock.unlock(); // The Win32 driver can send synchronous messages to the window.
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    VkResult result;
    try {
        result = create(device, &info, nullptr, &swapchain);
        if (result == VK_SUCCESS) {
            uint32_t count = 0;
            result = enumerate(device, swapchain, &count, nullptr);
            if (result == VK_SUCCESS && count) {
                images.resize(count);
                result = enumerate(device, swapchain, &count, images.data());
                images.resize(count);
            }
            if (result == VK_SUCCESS && images.empty())
                result = VK_ERROR_INITIALIZATION_FAILED;
        }
        lock.lock();
        if (result == VK_SUCCESS) {
            for (const auto image : images) {
                if (!record.images.emplace(image, ResourceBinding{{}, VK_NULL_HANDLE, swapchain}).second)
                    throw std::runtime_error("duplicate ARM64 Vulkan swapchain image");
            }
            record.swapchains.emplace(swapchain, Swapchain{info.surface, std::move(images)});
            surface.active = swapchain;
            std::memcpy(output, &swapchain, sizeof(swapchain));
        }
    }
    catch (...) {
        if (!lock.owns_lock())
            lock.lock();
        std::erase_if(record.images, [swapchain](const auto& entry) {
            return swapchain && entry.second.swapchain == swapchain;
        });
        lock.unlock();
        if (swapchain)
            destroy(device, swapchain, nullptr);
        lock.lock();
        surface.creating = false;
        throw;
    }
    if (result != VK_SUCCESS && swapchain) {
        lock.unlock();
        destroy(device, swapchain, nullptr);
        lock.lock();
    }
    surface.creating = false;
    if (result == VK_SUCCESS)
        ++surface.swapchains;
    return result;
}

uint64_t CallSwapchain(std::string_view name, const Guest& guest, VulkanObjects& objects,
                       std::unique_lock<std::recursive_mutex>& lock)
{
    auto device = guest.handle<VkDevice>(0);
    const bool present = name == "vkQueuePresentKHR";
    const auto queue = guest.handle<VkQueue>(0);
    if (present) {
        const auto owner = objects.queues.find(queue);
        if (owner == objects.queues.end())
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        device = owner->second.device;
    }
    const auto found = objects.devices.find(device);
    if (found == objects.devices.end() || !found->second.swapchainEnabled)
        return Result(VK_ERROR_EXTENSION_NOT_PRESENT);
    auto& record = found->second;
    if (name == "vkCreateSwapchainKHR")
        return TraceResult(name, CreateSwapchain(guest, device, record, objects, lock));
    if (present) {
        auto info = guest.read<VkPresentInfoKHR>(guest.args[1]);
        if (info.sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR || info.pNext || !info.swapchainCount ||
            info.swapchainCount > record.swapchains.size())
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        const auto chains = guest.array(info.pSwapchains, info.swapchainCount);
        const auto indices = guest.array(info.pImageIndices, info.swapchainCount);
        const auto waits = guest.array(info.pWaitSemaphores, info.waitSemaphoreCount);
        if (!std::ranges::all_of(waits, [&](VkSemaphore semaphore) {
                return record.semaphores.contains(semaphore);
            }))
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        for (size_t i = 0; i < chains.size(); ++i) {
            const auto chain = record.swapchains.find(chains[i]);
            if (chain == record.swapchains.end() || indices[i] >= chain->second.images.size())
                return Result(VK_ERROR_INITIALIZATION_FAILED);
        }
        auto* output = info.pResults
                           ? guest.output<VkResult>(reinterpret_cast<uintptr_t>(info.pResults), info.swapchainCount)
                           : nullptr;
        std::vector<VkResult> results(info.swapchainCount, VK_ERROR_UNKNOWN);
        info.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
        info.pSwapchains = chains.data();
        info.pImageIndices = indices.data();
        info.pResults = results.data();
        const auto submit = DeviceFunction<PFN_vkQueuePresentKHR>(device, "vkQueuePresentKHR");
        lock.unlock();
        const auto result = submit(queue, &info);
        lock.lock();
        if (output)
            std::memcpy(output, results.data(), results.size() * sizeof(VkResult));
        for (size_t i = 0; i < chains.size(); ++i) {
            if (results[i] == VK_SUCCESS || results[i] == VK_SUBOPTIMAL_KHR) {
                const auto& chain = record.swapchains.at(chains[i]);
                auto& surface = objects.surfaces.at(chain.surface);
                const bool first = surface.presenting == VK_NULL_HANDLE;
                surface.presenting = chains[i];
                SetWindowsVulkanPresentation(true);
                if (first)
                    std::fprintf(stderr, "[win32] first Vulkan frame presented\n");
            }
        }
        return Result(result);
    }

    const bool acquire2 = name == "vkAcquireNextImage2KHR";
    VkAcquireNextImageInfoKHR info{};
    if (acquire2) {
        info = guest.read<VkAcquireNextImageInfoKHR>(guest.args[1]);
        if (info.sType != VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR || info.pNext || info.deviceMask != 1)
            return Result(VK_ERROR_INITIALIZATION_FAILED);
    }
    const auto handle = acquire2 ? info.swapchain : guest.handle<VkSwapchainKHR>(1);
    if (name == "vkDestroySwapchainKHR" && !handle)
        return 0;
    const auto chain = record.swapchains.find(handle);
    if (chain == record.swapchains.end())
        throw std::runtime_error("invalid ARM64 Vulkan swapchain");
    if (name == "vkDestroySwapchainKHR") {
        if (guest.args[2] || std::ranges::any_of(record.views, [&](const auto& entry) {
                return record.images.at(entry.second.image).swapchain == handle;
            }))
            throw std::runtime_error("invalid ARM64 Vulkan swapchain destruction");
        auto& surface = objects.surfaces.at(chain->second.surface);
        for (const auto image : chain->second.images)
            record.images.erase(image);
        record.swapchains.erase(chain);
        const auto destroy = DeviceFunction<PFN_vkDestroySwapchainKHR>(device, "vkDestroySwapchainKHR");
        lock.unlock();
        destroy(device, handle, nullptr);
        lock.lock();
        --surface.swapchains;
        if (surface.active == handle)
            surface.active = VK_NULL_HANDLE;
        if (surface.presenting == handle) {
            surface.presenting = VK_NULL_HANDLE;
            SetWindowsVulkanPresentation(false);
        }
        return 0;
    }
    if (name == "vkGetSwapchainImagesKHR") {
        return Result(EnumerateResult<VkImage>(guest, 2, [&](uint32_t* count, VkImage* values) {
            const auto& images = chain->second.images;
            const auto available = static_cast<uint32_t>(images.size());
            if (!values) {
                *count = available;
                return VK_SUCCESS;
            }
            *count = std::min(*count, available);
            std::copy_n(images.begin(), *count, values);
            return *count < available ? VK_INCOMPLETE : VK_SUCCESS;
        }));
    }
    if (chain->second.retired)
        return Result(VK_ERROR_OUT_OF_DATE_KHR);
    const auto semaphore = acquire2 ? info.semaphore : guest.handle<VkSemaphore>(3);
    const auto fence = acquire2 ? info.fence : guest.handle<VkFence>(4);
    if ((!semaphore && !fence) || (semaphore && !record.semaphores.contains(semaphore)) ||
        (fence && !record.fences.contains(fence)))
        return Result(VK_ERROR_INITIALIZATION_FAILED);
    auto* output = guest.output<uint32_t>(guest.args[acquire2 ? 2 : 5]);
    const auto acquire = DeviceFunction<PFN_vkAcquireNextImageKHR>(device, "vkAcquireNextImageKHR");
    lock.unlock();
    uint32_t index = 0;
    const auto result = acquire(device, handle, acquire2 ? info.timeout : guest.args[2], semaphore, fence, &index);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        std::memcpy(output, &index, sizeof(index));
    return Result(result);
}

VkResult CreateInstance(const Guest& guest, VulkanObjects& objects)
{
    auto info = guest.read<VkInstanceCreateInfo>(guest.args[0]);
    if (info.sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO || info.pNext || info.flags || guest.args[1])
        return VK_ERROR_INITIALIZATION_FAILED;
    if (info.enabledLayerCount)
        return VK_ERROR_LAYER_NOT_PRESENT;
    info.ppEnabledLayerNames = nullptr;
    std::vector<const char*> extensions;
    Instance record;
    if (info.enabledExtensionCount > 2)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info.enabledExtensionCount && !HostSurfacesAvailable())
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(info.ppEnabledExtensionNames) + i * sizeof(uintptr_t);
        const auto name = guest.string(guest.read<uintptr_t>(address), VK_MAX_EXTENSION_NAME_SIZE);
        if (name == VK_KHR_SURFACE_EXTENSION_NAME && !record.surface) {
            record.surface = true;
            extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        }
        else if (name == VK_KHR_ANDROID_SURFACE_EXTENSION_NAME && !record.androidSurface) {
            record.androidSurface = true;
            extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        }
        else {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    if (record.androidSurface && !record.surface)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (TraceVulkan())
        std::fprintf(stderr, "[android-vulkan] instance Android surface: %s\n",
                     record.androidSurface ? "enabled" : "disabled");
    info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    VkApplicationInfo application{};
    std::string name, engine;
    if (info.pApplicationInfo) {
        application = guest.read<VkApplicationInfo>(reinterpret_cast<uintptr_t>(info.pApplicationInfo));
        if (application.sType != VK_STRUCTURE_TYPE_APPLICATION_INFO || application.pNext)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (TraceVulkan() && application.apiVersion) {
            const auto version = application.apiVersion;
            std::fprintf(stderr, "[android-vulkan] requested instance API %u.%u.%u\n", VK_API_VERSION_MAJOR(version),
                         VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
        }
        if (application.apiVersion &&
            (VK_API_VERSION_VARIANT(application.apiVersion) || application.apiVersion < VK_API_VERSION_1_0))
            return VK_ERROR_INCOMPATIBLE_DRIVER;
        // Let the native loader negotiate the application's target version.
        // Guest capability queries still report the supported bridge level.
        if (application.pApplicationName) {
            name = guest.string(reinterpret_cast<uintptr_t>(application.pApplicationName), 4096);
            application.pApplicationName = name.c_str();
        }
        if (application.pEngineName) {
            engine = guest.string(reinterpret_cast<uintptr_t>(application.pEngineName), 4096);
            application.pEngineName = engine.c_str();
        }
        info.pApplicationInfo = &application;
    }
    auto* output = guest.output<VkInstance>(guest.args[2]);
    VkInstance instance = VK_NULL_HANDLE;
    const auto result = Native<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance);
    if (result != VK_SUCCESS)
        return result;
    try {
        objects.instances.emplace(instance, record);
    }
    catch (...) {
        Native<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        throw;
    }
    std::memcpy(output, &instance, sizeof(instance));
    return VK_SUCCESS;
}

VkResult CreateDevice(const Guest& guest, VulkanObjects& objects, VkPhysicalDevice physical)
{
    auto info = guest.read<VkDeviceCreateInfo>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || info.pNext || info.flags || guest.args[2])
        return VK_ERROR_INITIALIZATION_FAILED;
    if (info.enabledLayerCount)
        return VK_ERROR_LAYER_NOT_PRESENT;
    const char* extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (info.enabledExtensionCount > 1)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info.enabledExtensionCount) {
        const auto address = guest.read<uintptr_t>(reinterpret_cast<uintptr_t>(info.ppEnabledExtensionNames));
        if (guest.string(address, VK_MAX_EXTENSION_NAME_SIZE) != extension ||
            !HostSwapchainAvailable(objects, physical))
            return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    info.ppEnabledLayerNames = nullptr;
    info.ppEnabledExtensionNames = info.enabledExtensionCount ? &extension : nullptr;
    const auto query = Native<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t count = 0;
    query(physical, &count, nullptr);
    if (!info.queueCreateInfoCount || info.queueCreateInfoCount > count)
        return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkQueueFamilyProperties> families(count);
    query(physical, &count, families.data());
    std::vector<VkDeviceQueueCreateInfo> queues(info.queueCreateInfoCount);
    std::vector<std::vector<float>> priorities(queues.size());
    Device record{};
    record.physical = physical;
    record.swapchainEnabled = info.enabledExtensionCount != 0;
    if (TraceVulkan())
        std::fprintf(stderr, "[android-vulkan] device swapchain: %s\n",
                     record.swapchainEnabled ? "enabled" : "disabled");
    Native<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical,
                                                                                           &record.memoryProperties);
    const auto source = reinterpret_cast<uintptr_t>(info.pQueueCreateInfos);
    const auto bytes = static_cast<unsigned>(queues.size() * sizeof(VkDeviceQueueCreateInfo));
    std::memcpy(queues.data(), guest.memory(source, bytes, false), bytes);
    for (size_t i = 0; i < queues.size(); ++i) {
        auto& queue = queues[i];
        if (queue.sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || queue.pNext || queue.flags ||
            queue.queueFamilyIndex >= count || !queue.queueCount ||
            queue.queueCount > families[queue.queueFamilyIndex].queueCount ||
            !record.queueCounts.emplace(queue.queueFamilyIndex, queue.queueCount).second)
            return VK_ERROR_INITIALIZATION_FAILED;
        auto& values = priorities[i];
        values.resize(queue.queueCount);
        const auto priorityBytes = static_cast<unsigned>(queue.queueCount * sizeof(float));
        std::memcpy(values.data(),
                    guest.memory(reinterpret_cast<uintptr_t>(queue.pQueuePriorities), priorityBytes, false),
                    priorityBytes);
        if (!std::ranges::all_of(values, [](float v) {
                return v >= 0 && v <= 1;
            }))
            return VK_ERROR_INITIALIZATION_FAILED;
        queue.pQueuePriorities = values.data();
    }
    info.pQueueCreateInfos = queues.data();
    VkPhysicalDeviceFeatures features{};
    if (info.pEnabledFeatures) {
        features = guest.read<VkPhysicalDeviceFeatures>(reinterpret_cast<uintptr_t>(info.pEnabledFeatures));
        const auto bits = std::bit_cast<std::array<VkBool32, sizeof(features) / sizeof(VkBool32)>>(features);
        if (!std::ranges::all_of(bits, [](VkBool32 flag) {
                return flag <= VK_TRUE;
            }))
            return VK_ERROR_INITIALIZATION_FAILED;
        info.pEnabledFeatures = &features;
    }
    auto* output = guest.output<VkDevice>(guest.args[3]);
    VkDevice device = VK_NULL_HANDLE;
    const auto result = Native<PFN_vkCreateDevice>("vkCreateDevice")(physical, &info, nullptr, &device);
    if (result != VK_SUCCESS)
        return result;
    try {
        objects.devices.emplace(device, std::move(record));
    }
    catch (...) {
        Native<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
        throw;
    }
    std::memcpy(output, &device, sizeof(device));
    return VK_SUCCESS;
}

VkResult CreateSurface(const Guest& guest, const std::function<std::shared_ptr<NativeWindow>(uintptr_t)>& acquireWindow)
{
    const auto info = guest.read<VkAndroidSurfaceCreateInfoKHR>(guest.args[1]);
    if (info.sType != VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR || info.pNext || info.flags || guest.args[2])
        return VK_ERROR_INITIALIZATION_FAILED;
    auto retained = acquireWindow(reinterpret_cast<uintptr_t>(info.window));
    const auto hwnd = WindowsHostWindow();
    if (!retained || retained->imageQueue || !hwnd)
        return VK_ERROR_SURFACE_LOST_KHR;
    auto* output = guest.output<VkSurfaceKHR>(guest.args[3]);
    const std::lock_guard lock(AndroidLoaderMutex());
    auto& objects = Objects();
    const auto instance = guest.handle<VkInstance>(0);
    const auto found = objects.instances.find(instance);
    if (found == objects.instances.end() || !found->second.androidSurface)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!objects.surfaces.empty())
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    const VkWin32SurfaceCreateInfoKHR nativeInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0,
                                                 GetModuleHandleW(nullptr), hwnd};
    const auto create = Native<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const auto result = create(instance, &nativeInfo, nullptr, &surface);
    if (result != VK_SUCCESS)
        return result;
    try {
        objects.surfaces.emplace(surface, Surface{instance, std::move(retained)});
    }
    catch (...) {
        Native<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(instance, surface, nullptr);
        throw;
    }
    std::memcpy(output, &surface, sizeof(surface));
    return VK_SUCCESS;
}
}

uint8_t* Arm64VulkanMappedData(uintptr_t address, unsigned size)
{
    auto& mappings = Mappings();
    if (!mappings.count.load(std::memory_order_acquire))
        return nullptr;
    const std::lock_guard lock(mappings.mutex);
    const auto above = mappings.ranges.upper_bound(address);
    if (above == mappings.ranges.begin())
        return nullptr;
    const auto& [base, length] = *std::prev(above);
    const auto offset = address - base;
    return offset < length && size <= length - offset ? reinterpret_cast<uint8_t*>(address) : nullptr;
}

bool WindowsArm64VulkanAvailable()
{
    if (!IsArm64TranslationEnabled())
        return false;
    static const bool disabled = [] {
        char value[16]{};
        const auto size = GetEnvironmentVariableA("STONE_GUEST_VULKAN", value, sizeof(value));
        return size && size < sizeof(value) && value[0] == '0';
    }();
    return !disabled && VulkanLoader() && Native<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
}

uintptr_t Libvulkan(std::string_view name)
{
    if (!WindowsArm64VulkanAvailable())
        return 0;
    const auto command = std::ranges::find(commands, name, &Command::name);
    if (command == std::end(commands))
        return 0;
    const auto native = Native<PFN_vkVoidFunction>(HostName(*command));
    return native ? Import(name, native) : 0;
}

uint64_t CallArm64Vulkan(std::string_view name, std::span<const uint64_t, 8> args, uintptr_t stack,
                         const std::function<uint8_t*(uintptr_t, unsigned, bool)>& memory,
                         const std::function<std::string(uintptr_t, unsigned)>& readString,
                         const std::function<float(unsigned)>& floatArgument,
                         const std::function<std::shared_ptr<NativeWindow>(uintptr_t)>& acquireWindow)
{
    const Guest guest{args, stack, memory, readString, floatArgument};
    if (name == "vkCreateAndroidSurfaceKHR")
        return TraceResult(name, CreateSurface(guest, acquireWindow));
    std::unique_lock lock(AndroidLoaderMutex());
    auto& objects = Objects();
    const auto instance = guest.handle<VkInstance>(0);
    const auto physical = guest.handle<VkPhysicalDevice>(0);
    const auto device = guest.handle<VkDevice>(0);

    if (name == "vkGetInstanceProcAddr" || name == "vkGetDeviceProcAddr") {
        const auto symbol = readString(args[1], VK_MAX_EXTENSION_NAME_SIZE);
        const auto command = std::ranges::find(commands, symbol, &Command::name);
        if (command == std::end(commands)) {
            if (TraceVulkan())
                std::fprintf(stderr, "[android-vulkan] unbound procedure %s\n", symbol.c_str());
            return 0;
        }
        if (name == "vkGetDeviceProcAddr") {
            const auto found = objects.devices.find(device);
            if (found == objects.devices.end() ||
                (command->scope != Scope::Device && command->scope != Scope::Resource &&
                 command->scope != Scope::Commands && command->scope != Scope::Descriptor &&
                 command->scope != Scope::Draw && command->scope != Scope::Query &&
                 !(command->scope == Scope::Swapchain && found->second.swapchainEnabled)) ||
                !Native<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr")(device, HostName(*command)))
                return 0;
        }
        else if (!instance) {
            if (command->scope != Scope::Global)
                return 0;
        }
        else if (!objects.instances.contains(instance) ||
                 !Native<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr")(instance, HostName(*command))) {
            return 0;
        }
        return Arm64VulkanSymbol(symbol);
    }
    if (const auto command = std::ranges::find(commands, name, &Command::name); command != std::end(commands)) {
        if (command->scope == Scope::Swapchain) {
            if (name == "vkAcquireNextImageKHR" || name == "vkAcquireNextImage2KHR" || name == "vkQueuePresentKHR")
                return TraceFrameCall(command->name, [&] {
                    return CallSwapchain(name, guest, objects, lock);
                });
            return CallSwapchain(name, guest, objects, lock);
        }
        if (command->scope == Scope::Commands) {
            if (name == "vkQueueSubmit")
                return TraceFrameCall(command->name, [&] {
                    return CallCommands(name, guest, objects, lock);
                });
            return CallCommands(name, guest, objects, lock);
        }
        if (command->scope == Scope::Query)
            return CallQueries(name, guest, objects, lock);
        if (command->scope == Scope::Draw) {
            const auto buffer = guest.handle<VkCommandBuffer>(0);
            const auto owner = objects.commandBuffers.find(buffer);
            if (owner == objects.commandBuffers.end())
                throw std::runtime_error("invalid ARM64 Vulkan command buffer");
            CallDraw(name, guest, buffer, objects.devices.at(owner->second.device));
            return 0;
        }
        if (command->scope == Scope::Descriptor) {
            const auto found = objects.devices.find(device);
            if (found == objects.devices.end())
                throw std::runtime_error("invalid ARM64 Vulkan device");
            return CallDescriptors(name, guest, device, found->second);
        }
        if (command->scope == Scope::Resource) {
            const auto found = objects.devices.find(device);
            if (found == objects.devices.end())
                throw std::runtime_error("invalid ARM64 Vulkan device");
            return CallResource(name, guest, device, found->second);
        }
    }
    if (name == "vkEnumerateInstanceVersion") {
        guest.write(args[0], uint32_t{VK_API_VERSION_1_0});
        return VK_SUCCESS;
    }
    if (name == "vkEnumerateInstanceExtensionProperties") {
        if (args[0])
            return Result(VK_ERROR_LAYER_NOT_PRESENT);
        static constexpr VkExtensionProperties extensions[]{
            {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
            {VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_SPEC_VERSION},
        };
        return Result(
            EnumerateResult<VkExtensionProperties>(guest, 1, [](uint32_t* count, VkExtensionProperties* values) {
                const auto available = HostSurfacesAvailable() ? static_cast<uint32_t>(std::size(extensions)) : 0u;
                if (!values) {
                    *count = available;
                    return VK_SUCCESS;
                }
                *count = std::min(*count, available);
                std::copy_n(extensions, *count, values);
                return *count < available ? VK_INCOMPLETE : VK_SUCCESS;
            }));
    }
    if (name == "vkEnumerateInstanceLayerProperties") {
        guest.write(args[0], uint32_t{0});
        return VK_SUCCESS;
    }
    if (name == "vkCreateInstance")
        return TraceResult(name, CreateInstance(guest, objects));
    if (name == "vkDestroyInstance") {
        if (!instance)
            return 0;
        if (args[1] || !objects.instances.contains(instance) ||
            std::ranges::any_of(objects.surfaces,
                                [&](const auto& item) {
                                    return item.second.instance == instance;
                                }) ||
            std::ranges::any_of(objects.devices, [&](const auto& item) {
                return objects.physicalDevices.at(item.second.physical) == instance;
            }))
            throw std::runtime_error("invalid ARM64 Vulkan instance destruction");
        Native<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        std::erase_if(objects.physicalDevices, [&](const auto& entry) {
            return entry.second == instance;
        });
        objects.instances.erase(instance);
        return 0;
    }
    if (name == "vkDestroySurfaceKHR") {
        const auto handle = guest.handle<VkSurfaceKHR>(1);
        if (!handle)
            return 0;
        const auto found = objects.surfaces.find(handle);
        if (args[2] || found == objects.surfaces.end() || found->second.instance != instance ||
            found->second.creating || found->second.swapchains)
            throw std::runtime_error("invalid ARM64 Vulkan surface destruction");
        Native<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(instance, handle, nullptr);
        objects.surfaces.erase(found);
        return 0;
    }
    if (name == "vkCreateSampler") {
        const auto found = objects.devices.find(device);
        if (found == objects.devices.end())
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        const auto info = guest.read<VkSamplerCreateInfo>(args[1]);
        if (info.sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO || info.pNext || info.flags || args[2])
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        auto* output = guest.output<VkSampler>(args[3]);
        VkSampler sampler = VK_NULL_HANDLE;
        const auto result = Native<PFN_vkCreateSampler>("vkCreateSampler")(device, &info, nullptr, &sampler);
        if (result == VK_SUCCESS) {
            try {
                ++found->second.samplers[sampler];
            }
            catch (...) {
                Native<PFN_vkDestroySampler>("vkDestroySampler")(device, sampler, nullptr);
                throw;
            }
            std::memcpy(output, &sampler, sizeof(sampler));
        }
        return TraceResult(name, result);
    }
    if (name == "vkDestroySampler") {
        const auto sampler = guest.handle<VkSampler>(1);
        if (!sampler)
            return 0;
        const auto found = objects.devices.find(device);
        if (args[2] || found == objects.devices.end())
            throw std::runtime_error("invalid ARM64 Vulkan sampler destruction");
        auto& samplers = found->second.samplers;
        const auto entry = samplers.find(sampler);
        if (entry == samplers.end())
            throw std::runtime_error("invalid ARM64 Vulkan sampler destruction");
        Native<PFN_vkDestroySampler>("vkDestroySampler")(device, sampler, nullptr);
        if (!--entry->second)
            samplers.erase(entry);
        return 0;
    }
    if (name == "vkCreateFence" || name == "vkDestroyFence" || name == "vkGetFenceStatus" || name == "vkResetFences" ||
        name == "vkWaitForFences" || name == "vkCreateSemaphore" || name == "vkDestroySemaphore") {
        const auto found = objects.devices.find(device);
        if (found == objects.devices.end())
            throw std::runtime_error("invalid ARM64 Vulkan device");
        auto& record = found->second;
        if (name == "vkCreateSemaphore")
            return Result(CreateSyncObject<VkSemaphoreCreateInfo>(
                guest, device, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, 0, record.semaphores,
                Native<PFN_vkCreateSemaphore>("vkCreateSemaphore"),
                Native<PFN_vkDestroySemaphore>("vkDestroySemaphore")));
        if (name == "vkDestroySemaphore") {
            DestroyTrackedObject(guest, device, record.semaphores,
                                 Native<PFN_vkDestroySemaphore>("vkDestroySemaphore"));
            return 0;
        }
        if (name == "vkCreateFence")
            return Result(CreateSyncObject<VkFenceCreateInfo>(
                guest, device, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, VK_FENCE_CREATE_SIGNALED_BIT, record.fences,
                Native<PFN_vkCreateFence>("vkCreateFence"), Native<PFN_vkDestroyFence>("vkDestroyFence")));
        if (name == "vkDestroyFence") {
            DestroyTrackedObject(guest, device, record.fences, Native<PFN_vkDestroyFence>("vkDestroyFence"));
            return 0;
        }
        if (name == "vkGetFenceStatus") {
            const auto handle = guest.handle<VkFence>(1);
            if (!record.fences.contains(handle))
                return Result(VK_ERROR_INITIALIZATION_FAILED);
            return Result(Native<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device, handle));
        }
        const auto count = static_cast<uint32_t>(args[1]);
        if (!count || count > record.fences.size() || count > UINT_MAX / sizeof(VkFence))
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        const auto bytes = static_cast<unsigned>(count * sizeof(VkFence));
        const auto* source = memory(args[2], bytes, false);
        std::vector<VkFence> fences(count);
        std::memcpy(fences.data(), source, bytes);
        if (!std::ranges::all_of(fences, [&](VkFence handle) {
                return record.fences.contains(handle);
            }))
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        if (name == "vkResetFences")
            return Result(Native<PFN_vkResetFences>("vkResetFences")(device, count, fences.data()));
        const auto waitAll = static_cast<VkBool32>(args[3]);
        if (waitAll > VK_TRUE)
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        // Queue submissions on another guest thread must be able to enter while the driver waits.
        lock.unlock();
        return Result(Native<PFN_vkWaitForFences>("vkWaitForFences")(device, count, fences.data(), waitAll, args[4]));
    }
    if (name == "vkEnumeratePhysicalDevices") {
        if (!objects.instances.contains(instance))
            return Result(VK_ERROR_INITIALIZATION_FAILED);
        const auto enumerate = Native<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        return Result(EnumerateResult<VkPhysicalDevice>(guest, 1, [&](uint32_t* count, VkPhysicalDevice* values) {
            const auto result = enumerate(instance, count, values);
            if (values && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
                for (uint32_t i = 0; i < *count; ++i)
                    objects.physicalDevices.emplace(values[i], instance);
            }
            return result;
        }));
    }
    if (name == "vkQueueWaitIdle") {
        const auto queue = guest.handle<VkQueue>(0);
        if (!objects.queues.contains(queue))
            throw std::runtime_error("invalid ARM64 Vulkan queue");
        lock.unlock();
        return Result(Native<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue));
    }
    if (name == "vkDestroyDevice" || name == "vkGetDeviceQueue" || name == "vkDeviceWaitIdle") {
        if (name == "vkDestroyDevice" && !device)
            return 0;
        const auto found = objects.devices.find(device);
        if (found == objects.devices.end())
            throw std::runtime_error("invalid ARM64 Vulkan device");
        if (name == "vkDeviceWaitIdle") {
            lock.unlock();
            return Result(Native<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device));
        }
        if (name == "vkDestroyDevice") {
            if (args[1])
                throw std::runtime_error("ARM64 Vulkan allocation callbacks are unsupported");
            const auto& record = found->second;
            if (!record.samplers.empty() || !record.fences.empty() || !record.semaphores.empty() ||
                !record.events.empty() || !record.images.empty() || !record.views.empty() || !record.buffers.empty() ||
                !record.memory.empty() || !record.swapchains.empty() || !record.pipelineCaches.empty() ||
                !record.commandPools.empty() || !record.renderPasses.empty() || !record.framebuffers.empty() ||
                !record.shaders.empty() || !record.bufferViews.empty() || !record.descriptorLayouts.empty() ||
                !record.pipelineLayouts.empty() || !record.descriptorPools.empty() || !record.pipelines.empty() ||
                !record.queryPools.empty())
                throw std::runtime_error("ARM64 Vulkan device still owns resources");
            Native<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
            std::erase_if(objects.queues, [device](const auto& entry) {
                return entry.second.device == device;
            });
            objects.devices.erase(found);
        }
        else {
            const auto index = static_cast<uint32_t>(args[1]), queue = static_cast<uint32_t>(args[2]);
            const auto family = found->second.queueCounts.find(index);
            if (family == found->second.queueCounts.end() || queue >= family->second)
                throw std::runtime_error("invalid ARM64 Vulkan queue index");
            VkQueue result = VK_NULL_HANDLE;
            Native<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, index, queue, &result);
            if (result)
                objects.queues.emplace(result, Queue{device, index});
            guest.write(args[3], result);
        }
        return 0;
    }
    if (!objects.physicalDevices.contains(physical))
        throw std::runtime_error("invalid ARM64 Vulkan physical device");
    if (name.starts_with("vkGetPhysicalDeviceSurface")) {
        const auto handle = guest.handle<VkSurfaceKHR>(name == "vkGetPhysicalDeviceSurfaceSupportKHR" ? 2 : 1);
        const auto surface = objects.surfaces.find(handle);
        if (surface == objects.surfaces.end() || surface->second.instance != objects.physicalDevices.at(physical))
            return Result(VK_ERROR_SURFACE_LOST_KHR);
        if (name == "vkGetPhysicalDeviceSurfaceSupportKHR") {
            uint32_t count = 0;
            Native<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(
                physical, &count, nullptr);
            const auto index = static_cast<uint32_t>(args[1]);
            if (index >= count)
                return Result(VK_ERROR_INITIALIZATION_FAILED);
            VkBool32 supported = VK_FALSE;
            const auto result = Native<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
                "vkGetPhysicalDeviceSurfaceSupportKHR")(physical, index, handle, &supported);
            if (result == VK_SUCCESS)
                guest.write(args[3], supported);
            return Result(result);
        }
        if (name == "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") {
            VkSurfaceCapabilitiesKHR caps{};
            const auto result = Native<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(physical, handle, &caps);
            if (result == VK_SUCCESS)
                guest.write(args[2], caps);
            return Result(result);
        }
        if (name == "vkGetPhysicalDeviceSurfaceFormatsKHR") {
            const auto enumerate =
                Native<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
            return Result(
                EnumerateResult<VkSurfaceFormatKHR>(guest, 2, [&](uint32_t* count, VkSurfaceFormatKHR* formats) {
                    return enumerate(physical, handle, count, formats);
                }));
        }
        const auto enumerate =
            Native<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
        return Result(EnumerateResult<VkPresentModeKHR>(guest, 2, [&](uint32_t* count, VkPresentModeKHR* modes) {
            return enumerate(physical, handle, count, modes);
        }));
    }
    if (name == "vkCreateDevice")
        return TraceResult(name, CreateDevice(guest, objects, physical));
    if (name == "vkEnumerateDeviceExtensionProperties" || name == "vkEnumerateDeviceLayerProperties") {
        const bool extensions = name == "vkEnumerateDeviceExtensionProperties";
        if (extensions && args[1])
            return Result(VK_ERROR_LAYER_NOT_PRESENT);
        if (!extensions) {
            guest.write(args[1], uint32_t{0});
            return VK_SUCCESS;
        }
        const auto available = HostSwapchainAvailable(objects, physical) ? 1u : 0u;
        return Result(EnumerateResult<VkExtensionProperties>(
            guest, 2, [available](uint32_t* count, VkExtensionProperties* values) {
                if (values) {
                    if (available && *count)
                        *values = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION};
                    *count = std::min(*count, available);
                    return *count < available ? VK_INCOMPLETE : VK_SUCCESS;
                }
                *count = available;
                return VK_SUCCESS;
            }));
    }
    if (name == "vkGetPhysicalDeviceProperties") {
        VkPhysicalDeviceProperties result{};
        Native<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical, &result);
        result.apiVersion = VK_API_VERSION_1_0;
        guest.write(args[1], result);
    }
    else if (name == "vkGetPhysicalDeviceFeatures") {
        VkPhysicalDeviceFeatures result{};
        Native<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical, &result);
        guest.write(args[1], result);
    }
    else if (name == "vkGetPhysicalDeviceFeatures2" || name == "vkGetPhysicalDeviceFeatures2KHR") {
        auto output = guest.read<VkPhysicalDeviceFeatures2>(args[1]);
        if (output.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            throw std::runtime_error("invalid ARM64 Vulkan features query");
        // The bridge implements the core feature query. Preserve extension
        // structures in the output chain without passing them to the driver.
        Native<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical, &output.features);
        guest.write(args[1], output);
    }
    else if (name == "vkGetPhysicalDeviceProperties2" || name == "vkGetPhysicalDeviceProperties2KHR") {
        auto output = guest.read<VkPhysicalDeviceProperties2>(args[1]);
        if (output.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2)
            throw std::runtime_error("invalid ARM64 Vulkan properties query");
        Native<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical, &output.properties);
        output.properties.apiVersion = VK_API_VERSION_1_0;
        guest.write(args[1], output);
    }
    else if (name == "vkGetPhysicalDeviceMemoryProperties2" || name == "vkGetPhysicalDeviceMemoryProperties2KHR") {
        auto output = guest.read<VkPhysicalDeviceMemoryProperties2>(args[1]);
        if (output.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2)
            throw std::runtime_error("invalid ARM64 Vulkan memory properties query");
        Native<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
            physical, &output.memoryProperties);
        guest.write(args[1], output);
    }
    else if (name == "vkGetPhysicalDeviceMemoryProperties") {
        VkPhysicalDeviceMemoryProperties result{};
        Native<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &result);
        guest.write(args[1], result);
    }
    else if (name == "vkGetPhysicalDeviceFormatProperties") {
        VkFormatProperties result{};
        Native<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
            physical, static_cast<VkFormat>(static_cast<uint32_t>(args[1])), &result);
        guest.write(args[2], result);
    }
    else if (name == "vkGetPhysicalDeviceImageFormatProperties") {
        VkImageFormatProperties properties{};
        const auto result =
            Native<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties")(
                physical, static_cast<VkFormat>(args[1]), static_cast<VkImageType>(args[2]),
                static_cast<VkImageTiling>(args[3]), static_cast<VkImageUsageFlags>(args[4]),
                static_cast<VkImageCreateFlags>(args[5]), &properties);
        if (result == VK_SUCCESS)
            guest.write(args[6], properties);
        return Result(result);
    }
    else if (name == "vkGetPhysicalDeviceSparseImageFormatProperties") {
        const auto query = Native<PFN_vkGetPhysicalDeviceSparseImageFormatProperties>(
            "vkGetPhysicalDeviceSparseImageFormatProperties");
        EnumerateResult<VkSparseImageFormatProperties>(
            guest, 6, [&](uint32_t* count, VkSparseImageFormatProperties* values) {
                query(physical, static_cast<VkFormat>(args[1]), static_cast<VkImageType>(args[2]),
                      static_cast<VkSampleCountFlagBits>(args[3]), static_cast<VkImageUsageFlags>(args[4]),
                      static_cast<VkImageTiling>(args[5]), count, values);
                return VK_SUCCESS;
            });
    }
    else if (name == "vkGetPhysicalDeviceQueueFamilyProperties") {
        const auto query =
            Native<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        EnumerateResult<VkQueueFamilyProperties>(guest, 1, [&](uint32_t* count, VkQueueFamilyProperties* values) {
            query(physical, count, values);
            return VK_SUCCESS;
        });
    }
    else {
        throw std::runtime_error("unsupported ARM64 Vulkan call " + std::string(name));
    }
    return 0;
}
}
