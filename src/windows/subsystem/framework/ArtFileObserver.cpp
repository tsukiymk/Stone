#include "ArtFileObserver.h"
#include "filesystem/AndroidFileObserver.h"
#include "runtime/art/ArtJni.h"
#include "abi/AndroidLibraryImports.h"
#include "abi/TypedAndroidImport.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace stone::windows {
namespace {
struct Event {
    int watch, mask;
    std::wstring name;
};

struct Observer {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Event> events;
};

Observer& State()
{
    static auto* observer = new Observer;
    return *observer;
}

void Enqueue(int watch, int mask, const std::wstring& name)
{
    auto& state = State();
    const std::lock_guard lock(state.mutex);
    if (state.events.size() >= 8192) {
        state.events.clear();
        state.events.push_back({-1, 0x4000, {}}); // IN_Q_OVERFLOW
    }
    state.events.push_back({watch, mask, name});
    state.ready.notify_one();
}

jint Init(JNIEnv*, jobject)
{
    return 1;
} // Private token used only by the image daemon.

void Observe(JNIEnv* env, jobject self, jint token)
{
    if (token != 1)
        return;
    const auto type = ArtJni<&JNINativeInterface_::GetObjectClass>(env, self);
    const auto callback = ArtJni<&JNINativeInterface_::GetMethodID>(env, type, "onEvent", "(IILjava/lang/String;)V");
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    if (!callback)
        return;
    auto& state = State();
    for (;;) {
        Event event;
        {
            std::unique_lock lock(state.mutex);
            state.ready.wait(lock, [&] {
                return !state.events.empty();
            });
            event = std::move(state.events.front());
            state.events.pop_front();
        }
        const auto name = ArtJni<&JNINativeInterface_::NewString>(
            env, reinterpret_cast<const jchar*>(event.name.data()), static_cast<jsize>(event.name.size()));
        if (!name)
            return;
        const jvalue arguments[]{jvalue{.i = event.watch}, jvalue{.i = event.mask}, jvalue{.l = name}};
        ArtJni<&JNINativeInterface_::CallVoidMethodA>(env, self, callback, arguments);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, name);
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            return;
    }
}

void Start(JNIEnv* env, jobject, jint token, jobjectArray paths, jint mask, jintArray descriptors)
{
    if (!paths || !descriptors)
        return;
    const auto count = ArtJni<&JNINativeInterface_::GetArrayLength>(env, paths);
    if (count != ArtJni<&JNINativeInterface_::GetArrayLength>(env, descriptors))
        return;
    for (jsize index = 0; index < count; ++index) {
        jint id = -1;
        const auto path = static_cast<jstring>(ArtJni<&JNINativeInterface_::GetObjectArrayElement>(env, paths, index));
        if (path && token == 1) {
            const auto size = ArtJni<&JNINativeInterface_::GetStringLength>(env, path);
            const auto* chars = ArtJni<&JNINativeInterface_::GetStringChars>(env, path, nullptr);
            if (chars) {
                const std::wstring name(reinterpret_cast<const wchar_t*>(chars), size);
                ArtJni<&JNINativeInterface_::ReleaseStringChars>(env, path, chars);
                if (name.find(L'\0') == name.npos)
                    id = WatchAndroidDirectory(name, mask, Enqueue);
            }
        }
        if (path)
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, path);
        ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, descriptors, index, 1, &id);
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            return;
    }
}

void Stop(JNIEnv* env, jobject, jint token, jintArray descriptors)
{
    if (token != 1 || !descriptors)
        return;
    const auto count = ArtJni<&JNINativeInterface_::GetArrayLength>(env, descriptors);
    std::vector<jint> ids(count);
    ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, descriptors, 0, count, ids.data());
    if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
        return;
    for (const auto id : ids)
        StopAndroidDirectoryWatch(id);
}

template <class Function> JNINativeMethod Native(const char* name, const char* signature, Function function)
{
    const auto label = std::string("ART FileObserver ") + name;
    return {const_cast<char*>(name), const_cast<char*>(signature),
            reinterpret_cast<void*>(IsArm64TranslationEnabled()
                                        ? ImportAndroidScalars<Function>(label, reinterpret_cast<uintptr_t>(function))
                                        : Import(label, function))};
}
}

bool RegisterArtFileObserver(JNIEnv* env)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "android/os/FileObserver$ObserverThread");
    if (!type)
        return false;
    const JNINativeMethod methods[]{Native("init", "()I", Init), Native("observe", "(I)V", Observe),
                                    Native("startWatching", "(I[Ljava/lang/String;I[I)V", Start),
                                    Native("stopWatching", "(I[I)V", Stop)};
    const bool result = ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, methods, 4) == JNI_OK;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return result;
}
}
