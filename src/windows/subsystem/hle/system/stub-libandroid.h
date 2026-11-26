#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace stone::windows {
struct AndroidImageQueue;

struct NativeWindow {
    std::shared_ptr<AndroidImageQueue> imageQueue;
    std::atomic_uint references{1};
};

inline constexpr int32_t Rgba8888 = 1;

void AcquireWindow(NativeWindow* window);
void ReleaseWindow(NativeWindow* window);
int WindowWidth(NativeWindow* window);
int WindowHeight(NativeWindow* window);
int WindowFormat(NativeWindow* window);
uintptr_t AndroidSensorImport(std::string_view name, bool hostAbi);
}
