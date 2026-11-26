#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace stone::windows {
struct NativeWindow;
bool WindowsArm64VulkanAvailable();
uint8_t* Arm64VulkanMappedData(uintptr_t address, unsigned size);
uintptr_t Libvulkan(std::string_view name);
uint64_t CallArm64Vulkan(std::string_view name, std::span<const uint64_t, 8> args, uintptr_t stack,
                         const std::function<uint8_t*(uintptr_t, unsigned, bool)>& memory,
                         const std::function<std::string(uintptr_t, unsigned)>& readString,
                         const std::function<float(unsigned)>& floatArgument,
                         const std::function<std::shared_ptr<NativeWindow>(uintptr_t)>& acquireWindow);
}
