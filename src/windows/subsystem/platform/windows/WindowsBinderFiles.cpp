#include "../../../host/files/BinderHandles.h"
#include "WindowsBinderFiles.h"

#include <windows.h>

namespace {
stone::windows::GuestBinderFiles guestFiles;
constexpr uint64_t MemoryGrant = 1ULL << 48;
constexpr uint64_t SocketGrant = 1ULL << 49;

HANDLE ChildProcess(JNIEnv* env, jobject child)
{
    if (!child)
        return nullptr;
    auto type = env->FindClass("com/trebledot/stone/loader/WindowsIsolatedProcess");
    if (!type)
        return nullptr;
    if (!env->IsInstanceOf(child, type)) {
        env->DeleteLocalRef(type);
        type = env->FindClass("java/lang/ProcessImpl");
        if (!type)
            return nullptr;
        if (!env->IsInstanceOf(child, type)) {
            env->DeleteLocalRef(type);
            return nullptr;
        }
    }
    const auto field = env->GetFieldID(type, "handle", "J");
    env->DeleteLocalRef(type);
    return field ? reinterpret_cast<HANDLE>(env->GetLongField(child, field)) : nullptr;
}

}

extern "C" JNIEXPORT jlong JNICALL Java_android_os_WindowsBinderFiles_duplicateToChild(JNIEnv* env, jclass,
                                                                                       jobject child, jobject file,
                                                                                       jint peerUid)
{
    const auto target = ChildProcess(env, child);
    if (!target || !file)
        return 0;
    const auto type = env->GetObjectClass(file);
    if (!type)
        return 0;
    const auto field = env->GetFieldID(type, "handle", "J");
    const auto guestFd = field && guestFiles.duplicateDisk ? env->GetFieldID(type, "fd", "I") : nullptr;
    env->DeleteLocalRef(type);
    if (!field || env->ExceptionCheck() || env->MonitorEnter(file) != JNI_OK)
        return 0;
    auto source = reinterpret_cast<HANDLE>(env->GetLongField(file, field));
    HANDLE guestCopy = nullptr;
    unsigned metadata = 0;
    uint64_t kind = 0;
    const char* unsupported = nullptr;
    if ((source == nullptr || source == INVALID_HANDLE_VALUE) && guestFd) {
        const int fd = env->GetIntField(file, guestFd);
        if (fd >= 0) {
            if (guestFiles.duplicateReadOnlyMemory) {
                guestCopy = reinterpret_cast<HANDLE>(guestFiles.duplicateReadOnlyMemory(fd, metadata));
                if (guestCopy)
                    kind = MemoryGrant;
            }
            if (!guestCopy)
                guestCopy = reinterpret_cast<HANDLE>(guestFiles.duplicateDisk(fd));
            if (!guestCopy && guestFiles.exportSocket) {
                if (peerUid >= 0) {
                    guestCopy = reinterpret_cast<HANDLE>(guestFiles.exportSocket(
                        fd, reinterpret_cast<uintptr_t>(target), static_cast<unsigned>(peerUid), metadata));
                }
                if (guestCopy)
                    kind = SocketGrant;
            }
            if (!guestCopy && guestFiles.unsupportedFile)
                unsupported = guestFiles.unsupportedFile(fd);
        }
        source = guestCopy;
    }
    HANDLE remote = stone::windows::DuplicateWindowsBinderHandle(source, target, kind == SocketGrant);
    if (guestCopy)
        CloseHandle(guestCopy);
    env->MonitorExit(file);
    if (!remote && unsupported) {
        const auto error = env->FindClass("android/os/BadParcelableException");
        if (error)
            env->ThrowNew(error, unsupported);
        return 0;
    }
    const auto handle = reinterpret_cast<uintptr_t>(remote);
    if (handle > UINT32_MAX) {
        stone::windows::RevokeWindowsBinderHandle(target, remote);
        return 0;
    }
    return static_cast<jlong>(handle ? kind | (uint64_t{metadata} << 32) | handle : 0);
}

extern "C" JNIEXPORT void JNICALL Java_android_os_WindowsBinderFiles_revokeInChild(JNIEnv* env, jclass, jobject child,
                                                                                   jlong value)
{
    const auto target = ChildProcess(env, child);
    if (target && value > 0) {
        // A frame that was never queued cannot have exposed this handle to the peer.
        stone::windows::RevokeWindowsBinderHandle(target, reinterpret_cast<HANDLE>(value));
    }
}

extern "C" JNIEXPORT jobject JNICALL Java_android_os_WindowsBinderFiles_adopt(JNIEnv* env, jclass, jlong value)
{
    const auto file = reinterpret_cast<HANDLE>(value);
    if (!stone::windows::IsWindowsRegularFile(file))
        return nullptr;
    return stone::windows::AdoptWindowsHandle(env, static_cast<uintptr_t>(value));
}

jobject stone::windows::AdoptWindowsHandle(JNIEnv* env, uintptr_t value)
{
    const auto file = reinterpret_cast<HANDLE>(value);
    const auto type = env->FindClass("java/io/FileDescriptor");
    const auto field = type ? env->GetFieldID(type, "handle", "J") : nullptr;
    const auto constructor = field ? env->GetMethodID(type, "<init>", "()V") : nullptr;
    const auto cleaner = constructor ? env->FindClass("java/io/FileCleanable") : nullptr;
    const auto registerCleanup =
        cleaner ? env->GetStaticMethodID(cleaner, "register", "(Ljava/io/FileDescriptor;)V") : nullptr;
    const auto fd = registerCleanup ? env->NewObject(type, constructor) : nullptr;
    if (!fd) {
        CloseHandle(file);
        return nullptr;
    }
    env->SetLongField(fd, field, static_cast<jlong>(value));
    env->CallStaticVoidMethod(cleaner, registerCleanup, fd);
    if (env->ExceptionCheck()) {
        env->SetLongField(fd, field, -1);
        CloseHandle(file);
        return nullptr;
    }
    return fd;
}

extern "C" JNIEXPORT jlong JNICALL Java_android_os_WindowsBinderFiles_size(JNIEnv*, jclass, jlong value)
{
    return stone::windows::WindowsHandleSize(reinterpret_cast<HANDLE>(value));
}

extern "C" JNIEXPORT jint JNICALL Java_android_os_WindowsBinderFiles_adoptReadOnlyMemory(JNIEnv*, jclass, jlong value,
                                                                                         jint metadata)
{
    const auto handle = reinterpret_cast<HANDLE>(value);
    if (guestFiles.adoptReadOnlyMemory && stone::windows::IsWindowsRegularFile(handle)) {
        return guestFiles.adoptReadOnlyMemory(static_cast<uintptr_t>(value), static_cast<unsigned>(metadata));
    }
    CloseHandle(handle);
    return -1;
}

extern "C" JNIEXPORT jint JNICALL Java_android_os_WindowsBinderFiles_adoptSocket(JNIEnv*, jclass, jlong value,
                                                                                 jint flags, jint peerPid, jint peerUid)
{
    if (guestFiles.importSocket && peerPid > 0 && peerUid >= 0) {
        return guestFiles.importSocket(static_cast<uintptr_t>(value), static_cast<unsigned>(flags), peerPid,
                                       static_cast<unsigned>(peerUid));
    }
    CloseHandle(reinterpret_cast<HANDLE>(value));
    return -1;
}

bool stone::windows::RegisterWindowsBinderFiles(JNIEnv* env, GuestBinderFiles guest)
{
    const auto cls = env->FindClass("android/os/WindowsBinderFiles");
    if (!cls)
        return false;
    const JNINativeMethod methods[] = {
        {const_cast<char*>("duplicateToChild"), const_cast<char*>("(Ljava/lang/Process;Ljava/io/FileDescriptor;I)J"),
         reinterpret_cast<void*>(&Java_android_os_WindowsBinderFiles_duplicateToChild)},
        {const_cast<char*>("revokeInChild"), const_cast<char*>("(Ljava/lang/Process;J)V"),
         reinterpret_cast<void*>(&Java_android_os_WindowsBinderFiles_revokeInChild)},
        {const_cast<char*>("adopt"), const_cast<char*>("(J)Ljava/io/FileDescriptor;"),
         reinterpret_cast<void*>(&Java_android_os_WindowsBinderFiles_adopt)},
        {const_cast<char*>("size"), const_cast<char*>("(J)J"),
         reinterpret_cast<void*>(&Java_android_os_WindowsBinderFiles_size)},
        {const_cast<char*>("adoptReadOnlyMemory"), const_cast<char*>("(JI)I"),
         reinterpret_cast<void*>(&Java_android_os_WindowsBinderFiles_adoptReadOnlyMemory)},
        {const_cast<char*>("adoptSocket"), const_cast<char*>("(JIII)I"),
         reinterpret_cast<void*>(&Java_android_os_WindowsBinderFiles_adoptSocket)},
    };
    const bool ok = env->RegisterNatives(cls, methods, sizeof(methods) / sizeof(methods[0])) == JNI_OK;
    if (ok)
        guestFiles = guest;
    env->DeleteLocalRef(cls);
    return ok;
}
