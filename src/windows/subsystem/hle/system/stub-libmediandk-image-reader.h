#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace stone::windows {
struct AndroidNativeWindowBuffer;
struct AndroidWindowRect;
struct AndroidImageQueue;
struct AndroidHardwareBuffer;

void* CreateAndroidImageWindow(const std::shared_ptr<AndroidImageQueue>& queue);
void ReleaseAndroidImageWindow(void* window);
int AndroidImageQueueWidth(const AndroidImageQueue& queue);
int AndroidImageQueueHeight(const AndroidImageQueue& queue);
int AndroidImageQueueFormat(const AndroidImageQueue& queue);
std::shared_ptr<AndroidImageQueue> CreateAndroidEncoderQueue(int width, int height, std::function<void()> onFrame);
std::shared_ptr<AndroidImageQueue> CreateAndroidSoftwareSurfaceQueue(int width, int height,
                                                                     std::function<void()> onFrame = {});
int ResizeAndroidSoftwareSurfaceQueue(AndroidImageQueue& queue, int width, int height);
bool IsAndroidSoftwareSurfaceQueue(const AndroidImageQueue& queue);
bool AndroidImageQueueHasFrames(AndroidImageQueue& queue);
AndroidHardwareBuffer* TakeAndroidImageFrame(AndroidImageQueue& queue, int64_t& timestampNs);
AndroidHardwareBuffer* PeekAndroidImageFrame(AndroidImageQueue& queue);
void EndAndroidImageQueueInput(AndroidImageQueue& queue);
void CloseAndroidImageQueue(AndroidImageQueue& queue);
int LockAndroidImageQueue(AndroidImageQueue& queue, AndroidNativeWindowBuffer& buffer, AndroidWindowRect* dirty);
int PostAndroidImageQueue(AndroidImageQueue& queue, int64_t timestampNs = -1);
uintptr_t AndroidImageReaderImport(std::string_view symbol, bool hostAbi = false);
}
