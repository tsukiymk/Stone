#pragma once

#include <jni.h>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace stone::windows {
class ElfImage;
// Typed execution services; guest instruction dispatch never calls arbitrary host addresses.
using Arm64PropertyGet = int (*)(const char*, char*);

struct Arm64Configuration {
    int32_t density, widthDp, heightDp;
};

struct Arm64AssetBridge {
    jobject (*manager)(JNIEnv*, jobject);
    void* (*open)(jobject, const char*, int);
    void (*close)(void*);
    int (*read)(void*, void*, size_t);
    int64_t (*seek)(void*, int64_t, int);
    int64_t (*length)(const void*);
    int64_t (*remaining)(const void*);
    const void* (*buffer)(const void*);
    int (*openFileDescriptor)(const void*, int64_t*, int64_t*);
    bool (*configuration)(jobject, Arm64Configuration&);
    void* (*openDirectory)(jobject, const char*);
    void (*closeDirectory)(void*);
    const char* (*nextFile)(void*);
    void (*rewindDirectory)(void*);
};

struct AndroidNativeWindowBuffer {
    int32_t width, height, stride, format;
    void* bits;
    uint32_t reserved[6];
};

static_assert(sizeof(AndroidNativeWindowBuffer) == 48);

struct AndroidWindowRect {
    int32_t left, top, right, bottom;
};

struct Arm64WindowBridge {
    void* (*fromSurface)(JNIEnv*, jobject);
    jobject (*toSurface)(JNIEnv*, void*);
    void (*acquire)(void*);
    void (*release)(void*);
    int (*dimension)(void*, bool);
    int (*format)(void*);
    int (*geometry)(void*, int, int, int);
    int (*lock)(void*, AndroidNativeWindowBuffer*, AndroidWindowRect*);
    int (*post)(void*);
};

struct Arm64NativeMethod {
    std::string name, signature;
    uintptr_t entry;
};

struct Arm64LoadedImage {
    std::string_view name;
    const ElfImage* image;
};

struct AndroidDlInfo {
    const char* file;
    void* base;
    const char* symbol;
    void* address;
};

static_assert(sizeof(AndroidDlInfo) == 32);

struct Arm64Jni {
    JavaVM* vm;
    jclass (*findClass)(JNIEnv*, const char*);
    jint (*registerNatives)(JNIEnv*, jclass, const ElfImage&, std::span<const Arm64NativeMethod>);
    const ElfImage* (*findCode)(uintptr_t);
    uint8_t* (*findData)(uintptr_t, unsigned, bool);
    void* (*dlopen)(const char*, int);
    void* (*dlsym)(void*, const char*);
    int (*dlclose)(void*);
    char* (*dlerror)();
    int (*dladdr)(const void*, AndroidDlInfo*);
    uintptr_t (*graphicsSymbol)(std::string_view, bool);
    Arm64AssetBridge assets;
    Arm64WindowBridge windows;
    std::vector<Arm64LoadedImage> (*loadedImages)();
};

struct Arm64Argument {
    char type;
    uint64_t bits;
};
}
