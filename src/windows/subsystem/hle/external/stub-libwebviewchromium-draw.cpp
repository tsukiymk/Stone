#include "LibraryJniStubs.h"
#include "abi/AndroidLibraryImports.h"
#include "../../../host/graphics/GlSurface.h"
#include "hle/system/stub-libc.h"

#include <windows.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace stone::windows {
namespace {
// Chromium's public AwDrawFn ABI. Version 3 adds the overlay fields to the GL
// parameters and lets the provider supply its callback version at creation.
constexpr int DrawVersion = 3;

struct SyncParams {
    int version;
    bool forceDark;
};

struct DrawGlParams {
    int version, left, top, right, bottom, width, height;
    bool reserved;
    float transform[16];
    float transferFunction[7];
    float toXyzD50[9];
    int overlayMode;
    uintptr_t getOverlayParent, mergeOverlayTransaction;
};

struct DrawCallbacks {
    uintptr_t sync, contextDestroyed, destroyed, drawGl;
};

struct DrawFunctionTable {
    int version;
    uintptr_t renderMode, create, release, createWithVersion;
};

static_assert(sizeof(SyncParams) == 8 && sizeof(DrawGlParams) == 184);
static_assert(offsetof(DrawGlParams, transform) == 32 && offsetof(DrawGlParams, getOverlayParent) == 168);
static_assert(sizeof(DrawFunctionTable) == 40);

struct Functor {
    void* data;
    DrawCallbacks callbacks;
    int version;
    bool drew = false;
};

void CallFunctor(uintptr_t callback, int id, const Functor& functor, void* params = nullptr)
{
    CallAndroidCallback(callback, reinterpret_cast<void*>(static_cast<intptr_t>(id)), functor.data, params);
}

// The Android ABI keeps sync, draw and destruction on one render thread.
// Its offscreen context stays current here, including for release callbacks.
class Compositor {
public:
    void setVm(JavaVM* vm)
    {
        vm_ = vm;
    }

    int create(void* data, int version, const DrawCallbacks& callbacks)
    {
        const std::lock_guard lock(mutex_);
        if (stopped_ || nextId_ == std::numeric_limits<int>::max())
            return -1;
        const int id = nextId_++;
        functors_.emplace(id, Functor{data, callbacks, std::min(version, DrawVersion)});
        return id;
    }

    void release(int id)
    {
        post([this, id] {
            decltype(functors_)::node_type entry;
            {
                const std::lock_guard lock(mutex_);
                entry = functors_.extract(id);
            }
            if (!entry)
                return;
            auto& functor = entry.mapped();
            if (functor.drew)
                CallFunctor(functor.callbacks.contextDestroyed, id, functor);
            CallFunctor(functor.callbacks.destroyed, id, functor);
        });
    }

    std::vector<jint> render(int id, int width, int height)
    {
        using Draw = std::packaged_task<std::vector<jint>()>;
        const auto task = std::make_shared<Draw>([=, this] {
            return draw(id, width, height);
        });
        auto pixels = task->get_future();
        if (!post([task] {
                (*task)();
            }))
            return {};
        return pixels.get();
    }

    void close()
    {
        {
            const std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        ready_.notify_one();
        if (worker_.joinable())
            worker_.join();
    }

private:
    bool post(std::function<void()> task)
    {
        {
            const std::lock_guard lock(mutex_);
            if (stopped_)
                return false;
            if (!worker_.joinable())
                worker_ = std::thread([this] {
                    run();
                });
            pending_.push(std::move(task));
        }
        ready_.notify_one();
        return true;
    }

    void run()
    {
        SetThreadDescription(GetCurrentThread(), L"Stone WebView RenderThread");
        JNIEnv* env = nullptr;
        vm_->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr);
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] {
                    return stopped_ || !pending_.empty();
                });
                if (pending_.empty())
                    break;
                task = std::move(pending_.front());
                pending_.pop();
            }
            try {
                task();
            }
            catch (const std::exception& error) {
                std::fprintf(stderr, "[win32] WebView render thread: %s\n", error.what());
            }
        }
        if (hasContext_)
            CloseWindowsGl();
        vm_->DetachCurrentThread();
    }

    std::vector<jint> draw(int id, int width, int height)
    {
        Functor* functor;
        {
            const std::lock_guard lock(mutex_);
            const auto it = functors_.find(id);
            if (it == functors_.end())
                return {};
            functor = &it->second;
        }

        static const auto bind =
            reinterpret_cast<decltype(&glBindFramebuffer)>(WindowsAngleExport(false, "glBindFramebuffer"));
        static const auto viewport = reinterpret_cast<decltype(&glViewport)>(WindowsAngleExport(false, "glViewport"));
        static const auto scissor = reinterpret_cast<decltype(&glScissor)>(WindowsAngleExport(false, "glScissor"));
        static const auto enable = reinterpret_cast<decltype(&glEnable)>(WindowsAngleExport(false, "glEnable"));
        static const auto disable = reinterpret_cast<decltype(&glDisable)>(WindowsAngleExport(false, "glDisable"));
        static const auto colorMask =
            reinterpret_cast<decltype(&glColorMask)>(WindowsAngleExport(false, "glColorMask"));
        static const auto clearColor =
            reinterpret_cast<decltype(&glClearColor)>(WindowsAngleExport(false, "glClearColor"));
        static const auto clear = reinterpret_cast<decltype(&glClear)>(WindowsAngleExport(false, "glClear"));
        if (!bind || !viewport || !scissor || !enable || !disable || !colorMask || !clearColor || !clear ||
            !BeginWindowsWebViewGlFrame(width, height))
            return {};
        hasContext_ = true;

        SyncParams sync{functor->version, false};
        CallFunctor(functor->callbacks.sync, id, *functor, &sync);
        bind(GL_FRAMEBUFFER, 0);
        disable(GL_SCISSOR_TEST);
        colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        clearColor(0, 0, 0, 0);
        clear(GL_COLOR_BUFFER_BIT);
        viewport(0, 0, width, height);
        enable(GL_SCISSOR_TEST);
        scissor(0, 0, width, height);

        DrawGlParams params{functor->version,
                            0,
                            0,
                            width,
                            height,
                            width,
                            height,
                            false,
                            {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
                            {2.4f, 1.f / 1.055f, 0.055f / 1.055f, 1.f / 12.92f, 0.04045f, 0, 0},
                            {0.4360657f, 0.3851471f, 0.1430664f, 0.2224884f, 0.7168732f, 0.0606079f, 0.0139160f,
                             0.0970764f, 0.7140961f},
                            0,
                            0,
                            0};
        CallFunctor(functor->callbacks.drawGl, id, *functor, &params);
        const bool firstDraw = !functor->drew;
        functor->drew = true;
        bind(GL_FRAMEBUFFER, 0);
        std::vector<jint> pixels;
        if (!CaptureWindowsGlFrame(pixels, width, height))
            return {};
        if (firstDraw)
            std::fprintf(stderr, "[win32] captured Android WebView functor framebuffer (%d x %d)\n", width, height);
        // DrawFn's surface uses premultiplied sRGB. Canvas.drawBitmap(int[])
        // takes straight ARGB.
        for (auto& pixel : pixels) {
            const auto argb = static_cast<uint32_t>(pixel);
            const auto alpha = argb >> 24;
            if (alpha == 0)
                pixel = 0;
            else if (alpha != 255) {
                const auto channel = [argb, alpha](int shift) {
                    return std::min(255u, (((argb >> shift) & 0xff) * 255 + alpha / 2) / alpha) << shift;
                };
                pixel = static_cast<jint>((alpha << 24) | channel(16) | channel(8) | channel(0));
            }
        }
        return pixels;
    }

    JavaVM* vm_ = nullptr;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> pending_;
    std::unordered_map<int, Functor> functors_;
    std::thread worker_;
    int nextId_ = 1;
    bool stopped_ = false;
    bool hasContext_ = false;
};

Compositor& GetCompositor()
{
    static auto* compositor = new Compositor;
    return *compositor;
}

int CreateFunctor(void* data, int version, const DrawCallbacks* callbacks)
{
    if (version < 1 || !callbacks || !callbacks->sync || !callbacks->drawGl || !callbacks->contextDestroyed ||
        !callbacks->destroyed)
        return -1;
    try {
        return GetCompositor().create(data, version, *callbacks);
    }
    catch (const std::exception&) {
        return -1;
    }
}

int CreateLegacyFunctor(void* data, const DrawCallbacks* callbacks)
{
    return CreateFunctor(data, 1, callbacks);
}

void ReleaseFunctor(int id)
{
    try {
        GetCompositor().release(id);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "[win32] WebView functor release: %s\n", error.what());
    }
}

jintArray JNICALL RenderFunctor(JNIEnv* env, jclass, jint id, jint width, jint height)
{
    if (id <= 0 || width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return nullptr;
    try {
        const auto pixels = GetCompositor().render(id, width, height);
        if (pixels.empty())
            return nullptr;
        const auto count = static_cast<jsize>(pixels.size());
        const auto result = env->NewIntArray(count);
        if (result)
            env->SetIntArrayRegion(result, 0, count, pixels.data());
        return result;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "[win32] WebView functor draw: %s\n", error.what());
        return nullptr;
    }
}
}

jlong JNICALL WebViewDrawFunctionTable(JNIEnv*, jclass)
{
    const std::lock_guard lock(AndroidLoaderMutex());
    static const DrawFunctionTable table{
        DrawVersion,
        BridgeAndroidImport("AwDrawFn_QueryRenderMode", reinterpret_cast<uintptr_t>(+[] {
                                return 0;
                            })), // OpenGL ES
        BridgeAndroidImport("AwDrawFn_CreateFunctor", reinterpret_cast<uintptr_t>(&CreateLegacyFunctor)),
        BridgeAndroidImport("AwDrawFn_ReleaseFunctor", reinterpret_cast<uintptr_t>(&ReleaseFunctor)),
        BridgeAndroidImport("AwDrawFn_CreateFunctor_v3", reinterpret_cast<uintptr_t>(&CreateFunctor)),
    };
    return reinterpret_cast<jlong>(&table);
}

bool RegisterWebViewCompositor(JNIEnv* env, JavaVM* vm)
{
    const auto canvas = env->FindClass("android/graphics/Canvas");
    if (!canvas)
        return false;
    const JNINativeMethod method{const_cast<char*>("nativeRenderWebViewFunctor"), const_cast<char*>("(III)[I"),
                                 reinterpret_cast<void*>(&RenderFunctor)};
    const bool registered = env->RegisterNatives(canvas, &method, 1) == JNI_OK;
    env->DeleteLocalRef(canvas);
    if (registered)
        GetCompositor().setVm(vm);
    return registered;
}

void CloseWebViewCompositor()
{
    GetCompositor().close();
}
}
