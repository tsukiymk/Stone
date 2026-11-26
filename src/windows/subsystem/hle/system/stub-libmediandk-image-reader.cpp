#include "stub-libmediandk-image-reader.h"
#include "stub-libandroid-hardware-buffer.h"
#include "stub-libc.h"
#include "cpu/arm64/Arm64Initializer.h"

#include <chrono>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace stone::windows {
namespace {
constexpr int MediaUnknown = -10000, MediaUnsupported = -10002, MediaInvalidObject = -10003,
              MediaInvalidParameter = -10004;
constexpr int NoBuffer = -30001, MaxImages = -30002;
constexpr int Invalid = -22, NoMemory = -12, Busy = -16;
constexpr uint64_t CpuReadOften = 3;

struct ReleaseBuffer {
    void operator()(const AndroidHardwareBuffer* buffer) const
    {
        ReleaseAndroidHardwareBuffer(buffer);
    }
};

using Buffer = std::unique_ptr<AndroidHardwareBuffer, ReleaseBuffer>;

struct Frame {
    Buffer buffer;
    int64_t timestampNs;
};

struct Image {
    AndroidImageQueue* owner;
    Buffer buffer;
    int releaseFence = -1;
    std::unique_ptr<Image> next;

    ~Image()
    {
        if (releaseFence >= 0)
            CloseAndroidFd(releaseFence);
    }
};

struct ImageListener {
    void* context;
    uintptr_t callback;
};
}

struct AndroidImageQueue {
    AndroidImageQueue(AndroidHardwareBufferDesc description, int maxImages, std::function<void()> onFrame = {})
        : description(description), geometry((uint64_t(description.width) << 32) | description.height),
          maxImages(static_cast<size_t>(maxImages)), onFrame(std::move(onFrame))
    {
    }

    AndroidHardwareBufferDesc description;
    std::atomic<uint64_t> geometry;
    const size_t maxImages;
    const std::function<void()> onFrame;
    std::mutex mutex;
    std::condition_variable changed;
    Buffer locked;
    std::deque<Frame> pending;
    std::unordered_map<Image*, std::unique_ptr<Image>> acquired;
    std::unique_ptr<Image> releasing;
    size_t releasingCount = 0;
    ImageListener listener{};
    bool pendingNotification = false;
    bool inputEnded = false;
    bool replaceOldFrames = false;
    bool closed = false;
};

namespace {
constexpr auto FencePollInterval = std::chrono::milliseconds(4);

void ReleaseFinishedImages(AndroidImageQueue& queue, bool closing = false)
{
    auto* next = &queue.releasing;
    bool released = false;
    while (*next) {
        if (!closing && !AndroidFenceReady((*next)->releaseFence)) {
            next = &(*next)->next;
            continue;
        }
        auto finished = std::move(*next);
        *next = std::move(finished->next);
        --queue.releasingCount;
        released = true;
    }
    if (released)
        queue.changed.notify_all();
}
}

int AndroidImageQueueWidth(const AndroidImageQueue& queue)
{
    return static_cast<int>(queue.geometry.load() >> 32);
}

int AndroidImageQueueHeight(const AndroidImageQueue& queue)
{
    return static_cast<int>(queue.geometry.load());
}

int AndroidImageQueueFormat(const AndroidImageQueue& queue)
{
    return static_cast<int>(queue.description.format);
}

std::shared_ptr<AndroidImageQueue> CreateAndroidEncoderQueue(int width, int height, std::function<void()> onFrame)
{
    const AndroidHardwareBufferDesc description{static_cast<uint32_t>(width),
                                                static_cast<uint32_t>(height),
                                                1,
                                                1,
                                                CpuReadOften | AndroidBufferCpuWriteOften,
                                                0,
                                                0,
                                                0};
    if (!SupportsAndroidHardwareBuffer(description))
        return {};
    return std::make_shared<AndroidImageQueue>(description, 1, std::move(onFrame));
}

std::shared_ptr<AndroidImageQueue> CreateAndroidSoftwareSurfaceQueue(int width, int height,
                                                                     std::function<void()> onFrame)
{
    auto queue = CreateAndroidEncoderQueue(width, height, std::move(onFrame));
    if (queue)
        queue->replaceOldFrames = true;
    return queue;
}

int ResizeAndroidSoftwareSurfaceQueue(AndroidImageQueue& queue, int width, int height)
{
    if (width < 1 || height < 1 || width > 8192 || height > 8192 || int64_t(width) * height > 16777216)
        return Invalid;
    const std::lock_guard lock(queue.mutex);
    if (!queue.replaceOldFrames || queue.closed || queue.inputEnded)
        return Invalid;
    if (queue.locked)
        return Busy;
    if (queue.description.width == static_cast<uint32_t>(width) &&
        queue.description.height == static_cast<uint32_t>(height))
        return 0;
    queue.pending.clear();
    queue.description.width = width;
    queue.description.height = height;
    queue.geometry.store((uint64_t(width) << 32) | uint32_t(height));
    queue.changed.notify_all();
    return 0;
}

bool IsAndroidSoftwareSurfaceQueue(const AndroidImageQueue& queue)
{
    return queue.replaceOldFrames;
}

bool AndroidImageQueueHasFrames(AndroidImageQueue& queue)
{
    const std::lock_guard lock(queue.mutex);
    return !queue.pending.empty();
}

AndroidHardwareBuffer* PeekAndroidImageFrame(AndroidImageQueue& queue)
{
    const std::lock_guard lock(queue.mutex);
    if (queue.pending.empty())
        return nullptr;
    auto* buffer = queue.pending.back().buffer.get();
    AcquireAndroidHardwareBuffer(buffer);
    return buffer;
}

AndroidHardwareBuffer* TakeAndroidImageFrame(AndroidImageQueue& queue, int64_t& timestampNs)
{
    const std::lock_guard lock(queue.mutex);
    if (queue.pending.empty())
        return nullptr;
    auto frame = std::move(queue.pending.front());
    queue.pending.pop_front();
    timestampNs = frame.timestampNs;
    queue.changed.notify_all();
    return frame.buffer.release();
}

void EndAndroidImageQueueInput(AndroidImageQueue& queue)
{
    const std::lock_guard lock(queue.mutex);
    queue.inputEnded = true;
    queue.changed.notify_all();
}

void CloseAndroidImageQueue(AndroidImageQueue& queue)
{
    const std::lock_guard lock(queue.mutex);
    queue.closed = true;
    queue.pending.clear();
    queue.changed.notify_all();
}

int LockAndroidImageQueue(AndroidImageQueue& queue, AndroidNativeWindowBuffer& result, AndroidWindowRect* dirty)
{
    std::unique_lock lock(queue.mutex);
    if (queue.locked)
        return Busy;
    for (;;) {
        if (queue.closed || queue.inputEnded)
            return Invalid;
        ReleaseFinishedImages(queue);
        if (queue.pending.size() + queue.acquired.size() + queue.releasingCount < queue.maxImages + 2)
            break;
        // A screen producer must keep servicing its GL event queue while the
        // UI thread synchronously waits for one of those events. Screen frames
        // can be superseded; image readers and encoders retain backpressure.
        if (queue.replaceOldFrames && !queue.pending.empty()) {
            queue.pending.pop_front();
            continue;
        }
        if (queue.releasing)
            queue.changed.wait_for(lock, FencePollInterval);
        else
            queue.changed.wait(lock);
    }
    if (queue.locked)
        return Busy;
    AndroidHardwareBuffer* storage = nullptr;
    if (const int status = AllocateAndroidHardwareBuffer(&queue.description, &storage); status != 0)
        return status;
    Buffer buffer(storage);
    void* pixels = nullptr;
    if (const int status = LockAndroidHardwareBuffer(storage, AndroidBufferCpuWriteOften, -1, nullptr, &pixels);
        status != 0)
        return status;
    AndroidHardwareBufferDesc desc{};
    DescribeAndroidHardwareBuffer(storage, &desc);
    const auto width = AndroidImageQueueWidth(queue), height = AndroidImageQueueHeight(queue);
    result = {width, height, static_cast<int>(desc.stride), AndroidImageQueueFormat(queue), pixels, {}};
    if (dirty)
        *dirty = {0, 0, width, height};
    queue.locked = std::move(buffer);
    return 0;
}

int PostAndroidImageQueue(AndroidImageQueue& queue, int64_t timestampNs)
{
    std::unique_lock lock(queue.mutex);
    if (!queue.locked)
        return Invalid;
    auto buffer = std::move(queue.locked);
    if (const int status = UnlockAndroidHardwareBuffer(buffer.get(), nullptr); status != 0)
        return status;
    if (queue.closed || queue.inputEnded)
        return Invalid;
    if (timestampNs < 0) {
        AndroidTimespec clock{};
        AndroidClock(1, &clock);
        timestampNs = clock.seconds * 1'000'000'000 + clock.nanos;
    }
    try {
        queue.pending.push_back({std::move(buffer), timestampNs});
    }
    catch (const std::bad_alloc&) {
        queue.changed.notify_all();
        return NoMemory;
    }
    queue.pendingNotification = true;
    queue.changed.notify_all();
    lock.unlock();
    if (queue.onFrame)
        queue.onFrame();
    return 0;
}

namespace {
struct Reader {
    std::shared_ptr<AndroidImageQueue> queue;
    void* window = nullptr;
    std::thread worker;
    bool deleteOnWorkerExit = false; // Protected by the queue mutex.

    ~Reader()
    {
        if (window)
            ReleaseAndroidImageWindow(window);
    }
};

void NotifyImages(Reader* reader)
{
    auto& queue = *reader->queue;
    std::unique_lock lock(queue.mutex);
    while (!queue.closed) {
        ReleaseFinishedImages(queue);
        if (!queue.pendingNotification || !queue.listener.callback) {
            if (queue.releasing)
                queue.changed.wait_for(lock, FencePollInterval);
            else
                queue.changed.wait(lock);
            continue;
        }
        const auto listener = queue.listener;
        queue.pendingNotification = false;
        lock.unlock();
        try {
            CallAndroidCallback(listener.callback, listener.context, reader);
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "[android-image-reader] %s\n", error.what());
        }
        lock.lock();
    }
    const bool destroy = reader->deleteOnWorkerExit;
    lock.unlock();
    if (destroy)
        delete reader;
}

int NewReader(int width, int height, int format, uint64_t usage, int maxImages, Reader** output)
{
    if (!output)
        return MediaInvalidParameter;
    *output = nullptr;
    if (width < 1 || width > INT_MAX - 15 || height < 1 || maxImages < 1)
        return MediaInvalidParameter;
    // The software producer supports packed pixel buffers. PRIVATE and planar
    // formats need a graphics or codec producer and a different buffer layout.
    if (format < 1 || format > 4 || (usage & 0xf0))
        return MediaUnsupported;
    const AndroidHardwareBufferDesc desc{static_cast<uint32_t>(width),
                                         static_cast<uint32_t>(height),
                                         1,
                                         static_cast<uint32_t>(format),
                                         usage | AndroidBufferCpuWriteOften,
                                         0,
                                         0,
                                         0};
    if (!SupportsAndroidHardwareBuffer(desc))
        return MediaUnsupported;
    try {
        auto reader = std::make_unique<Reader>();
        reader->queue = std::make_shared<AndroidImageQueue>(desc, maxImages);
        reader->window = CreateAndroidImageWindow(reader->queue);
        if (!reader->window)
            return MediaUnknown;
        reader->worker = std::thread(NotifyImages, reader.get());
        *output = reader.release();
        return 0;
    }
    catch (const std::bad_alloc&) {
        return MediaUnknown;
    }
    catch (const std::system_error&) {
        return MediaUnknown;
    }
}

void DeleteReader(Reader* reader)
{
    if (!reader)
        return;
    auto& queue = *reader->queue;
    const bool onWorker = reader->worker.get_id() == std::this_thread::get_id();
    {
        const std::lock_guard lock(queue.mutex);
        queue.closed = true;
        reader->deleteOnWorkerExit = onWorker;
        queue.changed.notify_all();
    }
    if (!onWorker)
        reader->worker.join();
    {
        const std::lock_guard lock(queue.mutex);
        queue.acquired.clear();
        queue.pending.clear();
        ReleaseFinishedImages(queue, true);
    }
    if (onWorker)
        reader->worker.detach();
    else
        delete reader;
}

int GetReaderWindow(Reader* reader, void** output)
{
    if (!reader || !output)
        return MediaInvalidParameter;
    *output = reader->window;
    return 0;
}

template <bool Latest> int AcquireImage(Reader* reader, Image** output, int* fence)
{
    if (!reader || !output)
        return MediaInvalidParameter;
    *output = nullptr;
    if (fence)
        *fence = -1;
    auto& queue = *reader->queue;
    const std::lock_guard lock(queue.mutex);
    if (queue.closed)
        return MediaInvalidObject;
    if (queue.acquired.size() >= queue.maxImages)
        return MaxImages;
    if (queue.pending.empty())
        return NoBuffer;
    try {
        auto image = std::make_unique<Image>();
        image->owner = &queue;
        auto* result = image.get();
        queue.acquired.emplace(result, std::move(image));
        if constexpr (Latest) {
            // With only one acquisition slot left, the NDK cannot acquire a
            // replacement while discarding the older image.
            if (queue.acquired.size() < queue.maxImages) {
                while (queue.pending.size() > 1)
                    queue.pending.pop_front();
            }
        }
        result->buffer = std::move(queue.pending.front().buffer);
        queue.pending.pop_front();
        *output = result;
        queue.changed.notify_all();
        return 0;
    }
    catch (const std::bad_alloc&) {
        return MediaUnknown;
    }
}

int SetImageListener(Reader* reader, const ImageListener* listener)
{
    if (!reader)
        return MediaInvalidParameter;
    auto& queue = *reader->queue;
    const std::lock_guard lock(queue.mutex);
    if (queue.closed)
        return MediaInvalidObject;
    queue.listener = listener ? *listener : ImageListener{};
    queue.changed.notify_all();
    return 0;
}

void DeleteImageAsync(Image* image, int fence)
{
    if (!image)
        return;
    auto& queue = *image->owner;
    const std::lock_guard lock(queue.mutex);
    const auto found = queue.acquired.find(image);
    if (found == queue.acquired.end())
        return;
    auto released = std::move(found->second);
    queue.acquired.erase(found);
    released->releaseFence = fence;
    if (!queue.closed && fence >= 0) {
        released->next = std::move(queue.releasing);
        queue.releasing = std::move(released);
        ++queue.releasingCount;
    }
    queue.changed.notify_all();
}
}

uintptr_t AndroidImageReaderImport(std::string_view symbol, bool hostAbi)
{
#define BIND(name, ...)                                                                                                \
    if (symbol == #name) {                                                                                             \
        const auto target = reinterpret_cast<uintptr_t>(__VA_ARGS__);                                                  \
        return hostAbi ? target : BridgeAndroidImport(symbol, target);                                                 \
    }
    BIND(AImageReader_newWithUsage, &NewReader);
    BIND(
        AImageReader_new, +[](int width, int height, int format, int maxImages, Reader** reader) {
            return NewReader(width, height, format, CpuReadOften, maxImages, reader);
        });
    BIND(AImageReader_delete, &DeleteReader);
    BIND(AImageReader_getWindow, &GetReaderWindow);
    BIND(AImageReader_acquireNextImageAsync, &AcquireImage<false>);
    BIND(AImageReader_acquireLatestImageAsync, &AcquireImage<true>);
    BIND(
        AImageReader_acquireNextImage, +[](Reader* reader, Image** image) {
            return AcquireImage<false>(reader, image, nullptr);
        });
    BIND(
        AImageReader_acquireLatestImage, +[](Reader* reader, Image** image) {
            return AcquireImage<true>(reader, image, nullptr);
        });
    BIND(AImageReader_setImageListener, &SetImageListener);
    BIND(
        AImage_delete, +[](Image* image) {
            DeleteImageAsync(image, -1);
        });
    BIND(AImage_deleteAsync, &DeleteImageAsync);
    BIND(
        AImage_getWidth, +[](const Image* image, int* width) {
            if (!image || !width)
                return MediaInvalidParameter;
            *width = AndroidImageQueueWidth(*image->owner);
            return 0;
        });
    BIND(
        AImage_getHeight, +[](const Image* image, int* height) {
            if (!image || !height)
                return MediaInvalidParameter;
            *height = AndroidImageQueueHeight(*image->owner);
            return 0;
        });
    BIND(
        AImage_getCropRect, +[](const Image* image, AndroidWindowRect* rect) {
            if (!image || !rect)
                return MediaInvalidParameter;
            *rect = {0, 0, AndroidImageQueueWidth(*image->owner), AndroidImageQueueHeight(*image->owner)};
            return 0;
        });
    BIND(
        AImage_getHardwareBuffer, +[](const Image* image, AndroidHardwareBuffer** buffer) {
            if (!image || !buffer)
                return MediaInvalidParameter;
            *buffer = image->buffer.get();
            return 0;
        });
#undef BIND
    return 0;
}
}
