#include "../../../host/files/FolderPicker.h"
#include "DocumentPicker.h"

#include <shobjidl.h>
#include <wrl.h>

#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace {
using Microsoft::WRL::ComPtr;

struct Request {
    jlong id;
    std::wstring initialDirectory;
};

std::mutex requestMutex;
std::optional<Request> pending;
jclass handler = nullptr;
jmethodID onPicked = nullptr;
jmethodID blockReason = nullptr;

std::wstring CopyString(JNIEnv* env, jstring value)
{
    if (!value)
        return {};
    std::wstring result(env->GetStringLength(value), L'\0');
    env->GetStringRegion(value, 0, static_cast<jsize>(result.size()), reinterpret_cast<jchar*>(result.data()));
    return result;
}

jboolean JNICALL QueueFolderPicker(JNIEnv* env, jclass, jlong parent, jstring initialDir, jlong id)
{
    const auto owner = reinterpret_cast<HWND>(parent);
    auto initial = CopyString(env, initialDir);
    if (env->ExceptionCheck())
        return JNI_FALSE;
    const std::lock_guard lock(requestMutex);
    if (pending || !owner)
        return JNI_FALSE;
    pending.emplace(Request{id, std::move(initial)});
    if (PostMessageW(owner, WindowsDocumentPickerMessage, 0, 0))
        return JNI_TRUE;
    pending.reset();
    return JNI_FALSE;
}

HRESULT PickFolder(JNIEnv* env, HWND owner, const Request& request, jstring& result)
{
    return stone::windows::PickWindowsFolder(
        owner, request.initialDirectory, [&](const wchar_t* path, std::wstring& message) -> HRESULT {
            const auto choice = env->NewString(reinterpret_cast<const jchar*>(path), static_cast<jsize>(wcslen(path)));
            if (!choice)
                return E_OUTOFMEMORY;
            const auto reason = static_cast<jstring>(env->CallStaticObjectMethod(handler, blockReason, choice));
            if (env->ExceptionCheck())
                return E_FAIL;
            if (!reason) {
                result = choice;
                return S_OK;
            }
            message = CopyString(env, reason);
            env->DeleteLocalRef(reason);
            env->DeleteLocalRef(choice);
            return env->ExceptionCheck() ? E_FAIL : S_FALSE;
        });
}

}

bool RegisterWindowsDocumentPicker(JNIEnv* env)
{
    const auto type = env->FindClass("com/trebledot/stone/loader/DocumentPickerHandler");
    if (!type)
        return false;
    handler = static_cast<jclass>(env->NewGlobalRef(type));
    onPicked = env->GetStaticMethodID(type, "stoneOnPicked", "(J[Ljava/lang/String;)V");
    blockReason = env->GetStaticMethodID(type, "stoneTreeBlockReason", "(Ljava/lang/String;)Ljava/lang/String;");
    const JNINativeMethod binding{const_cast<char*>("nativePickWindowsFolder"),
                                  const_cast<char*>("(JLjava/lang/String;J)Z"),
                                  reinterpret_cast<void*>(QueueFolderPicker)};
    const bool success = handler && onPicked && blockReason && env->RegisterNatives(type, &binding, 1) == JNI_OK;
    env->DeleteLocalRef(type);
    return success;
}

void ShowWindowsDocumentPicker(JNIEnv* env, HWND owner)
{
    Request request;
    {
        const std::lock_guard lock(requestMutex);
        if (!pending || stone::windows::WindowsFolderPickerActive())
            return;
        request = *pending;
    }
    if (env->PushLocalFrame(8) < 0) {
        const std::lock_guard lock(requestMutex);
        pending.reset();
        return;
    }
    jstring path = nullptr;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const HRESULT status = SUCCEEDED(com) ? PickFolder(env, owner, request, path) : com;
    if (SUCCEEDED(com))
        CoUninitialize();
    {
        const std::lock_guard lock(requestMutex);
        pending.reset();
    }
    if (FAILED(status) && status != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        std::fprintf(stderr, "[win32] folder picker failed (HRESULT 0x%08lX)\n", static_cast<unsigned long>(status));
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        path = nullptr;
    }
    jobjectArray paths = nullptr;
    if (path) {
        const auto type = env->FindClass("java/lang/String");
        if (type)
            paths = env->NewObjectArray(1, type, path);
    }
    if (!env->ExceptionCheck())
        env->CallStaticVoidMethod(handler, onPicked, request.id, paths);
    env->PopLocalFrame(nullptr);
}

void CloseWindowsDocumentPicker()
{
    stone::windows::CancelWindowsFolderPicker();
}
