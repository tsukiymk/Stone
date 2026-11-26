#pragma once

#include <jni.h>
#include <array>
#include <cstdint>

namespace stone::windows::arm64_abi {
enum class JniEntry : size_t {
    VmBase = sizeof(JNINativeInterface_) / sizeof(void*),
    GetEnv = static_cast<size_t>(VmBase) + offsetof(JNIInvokeInterface_, GetEnv) / sizeof(void*),
    AttachThread = static_cast<size_t>(VmBase) + offsetof(JNIInvokeInterface_, AttachCurrentThread) / sizeof(void*),
    DetachThread = static_cast<size_t>(VmBase) + offsetof(JNIInvokeInterface_, DetachCurrentThread) / sizeof(void*),
    AttachDaemon =
        static_cast<size_t>(VmBase) + offsetof(JNIInvokeInterface_, AttachCurrentThreadAsDaemon) / sizeof(void*),
    FindClass = offsetof(JNINativeInterface_, FindClass) / sizeof(void*),
    FromReflectedMethod = offsetof(JNINativeInterface_, FromReflectedMethod) / sizeof(void*),
    RegisterNatives = offsetof(JNINativeInterface_, RegisterNatives) / sizeof(void*),
    NewStringUtf = offsetof(JNINativeInterface_, NewStringUTF) / sizeof(void*),
    NewGlobalRef = offsetof(JNINativeInterface_, NewGlobalRef) / sizeof(void*),
    DeleteGlobalRef = offsetof(JNINativeInterface_, DeleteGlobalRef) / sizeof(void*),
    NewWeakGlobalRef = offsetof(JNINativeInterface_, NewWeakGlobalRef) / sizeof(void*),
    DeleteWeakGlobalRef = offsetof(JNINativeInterface_, DeleteWeakGlobalRef) / sizeof(void*),
    GetObjectRefType = offsetof(JNINativeInterface_, GetObjectRefType) / sizeof(void*),
    DeleteLocalRef = offsetof(JNINativeInterface_, DeleteLocalRef) / sizeof(void*),
    GetMethodId = offsetof(JNINativeInterface_, GetMethodID) / sizeof(void*),
    GetStaticMethodId = offsetof(JNINativeInterface_, GetStaticMethodID) / sizeof(void*),
    GetFieldId = offsetof(JNINativeInterface_, GetFieldID) / sizeof(void*),
    GetObjectField = offsetof(JNINativeInterface_, GetObjectField) / sizeof(void*),
    SetObjectField = offsetof(JNINativeInterface_, SetObjectField) / sizeof(void*),
    GetStaticFieldId = offsetof(JNINativeInterface_, GetStaticFieldID) / sizeof(void*),
    GetStaticObjectField = offsetof(JNINativeInterface_, GetStaticObjectField) / sizeof(void*),
    SetStaticObjectField = offsetof(JNINativeInterface_, SetStaticObjectField) / sizeof(void*),
    ExceptionCheck = offsetof(JNINativeInterface_, ExceptionCheck) / sizeof(void*),
    Throw = offsetof(JNINativeInterface_, Throw) / sizeof(void*),
    ExceptionClear = offsetof(JNINativeInterface_, ExceptionClear) / sizeof(void*),
    FatalError = offsetof(JNINativeInterface_, FatalError) / sizeof(void*),
    GetJavaVm = offsetof(JNINativeInterface_, GetJavaVM) / sizeof(void*),
    GetObjectClass = offsetof(JNINativeInterface_, GetObjectClass) / sizeof(void*),
    IsInstanceOf = offsetof(JNINativeInterface_, IsInstanceOf) / sizeof(void*),
    NewObject = offsetof(JNINativeInterface_, NewObject) / sizeof(void*),
    CallObject = offsetof(JNINativeInterface_, CallObjectMethod) / sizeof(void*),
    CallStaticObject = offsetof(JNINativeInterface_, CallStaticObjectMethod) / sizeof(void*),
    NewString = offsetof(JNINativeInterface_, NewString) / sizeof(void*),
    GetStringLength = offsetof(JNINativeInterface_, GetStringLength) / sizeof(void*),
    GetStringChars = offsetof(JNINativeInterface_, GetStringChars) / sizeof(void*),
    ReleaseStringChars = offsetof(JNINativeInterface_, ReleaseStringChars) / sizeof(void*),
    GetStringUtfLength = offsetof(JNINativeInterface_, GetStringUTFLength) / sizeof(void*),
    GetStringUtfChars = offsetof(JNINativeInterface_, GetStringUTFChars) / sizeof(void*),
    ReleaseStringUtfChars = offsetof(JNINativeInterface_, ReleaseStringUTFChars) / sizeof(void*),
    GetArrayLength = offsetof(JNINativeInterface_, GetArrayLength) / sizeof(void*),
    NewObjectArray = offsetof(JNINativeInterface_, NewObjectArray) / sizeof(void*),
    GetObjectArrayElement = offsetof(JNINativeInterface_, GetObjectArrayElement) / sizeof(void*),
    SetObjectArrayElement = offsetof(JNINativeInterface_, SetObjectArrayElement) / sizeof(void*),
    NewPrimitiveArray = offsetof(JNINativeInterface_, NewBooleanArray) / sizeof(void*),
    GetPrimitiveElements = offsetof(JNINativeInterface_, GetBooleanArrayElements) / sizeof(void*),
    ReleasePrimitiveElements = offsetof(JNINativeInterface_, ReleaseBooleanArrayElements) / sizeof(void*),
    GetPrimitiveRegion = offsetof(JNINativeInterface_, GetBooleanArrayRegion) / sizeof(void*),
    SetPrimitiveRegion = offsetof(JNINativeInterface_, SetBooleanArrayRegion) / sizeof(void*),
    GetPrimitiveArrayCritical = offsetof(JNINativeInterface_, GetPrimitiveArrayCritical) / sizeof(void*),
    ReleasePrimitiveArrayCritical = offsetof(JNINativeInterface_, ReleasePrimitiveArrayCritical) / sizeof(void*),
    NewDirectByteBuffer = offsetof(JNINativeInterface_, NewDirectByteBuffer) / sizeof(void*),
    GetDirectBufferAddress = offsetof(JNINativeInterface_, GetDirectBufferAddress) / sizeof(void*),
    GetDirectBufferCapacity = offsetof(JNINativeInterface_, GetDirectBufferCapacity) / sizeof(void*),
    IsSameObject = offsetof(JNINativeInterface_, IsSameObject) / sizeof(void*),
    PushLocalFrame = offsetof(JNINativeInterface_, PushLocalFrame) / sizeof(void*),
    PopLocalFrame = offsetof(JNINativeInterface_, PopLocalFrame) / sizeof(void*),
    NewLocalRef = offsetof(JNINativeInterface_, NewLocalRef) / sizeof(void*),
    EnsureLocalCapacity = offsetof(JNINativeInterface_, EnsureLocalCapacity) / sizeof(void*),
    ExceptionOccurred = offsetof(JNINativeInterface_, ExceptionOccurred) / sizeof(void*),
    ExceptionDescribe = offsetof(JNINativeInterface_, ExceptionDescribe) / sizeof(void*),
};
extern const std::array<uint8_t, static_cast<size_t>(JniEntry::VmBase) + sizeof(JNIInvokeInterface_) / sizeof(void*)>
    jniTokens;

inline uintptr_t JniToken(JniEntry entry)
{
    return reinterpret_cast<uintptr_t>(&jniTokens[static_cast<size_t>(entry)]);
}

struct GuestJniTables {
    std::array<uintptr_t, sizeof(JNIInvokeInterface_) / sizeof(void*)> vmTable{};
    std::array<uintptr_t, sizeof(JNINativeInterface_) / sizeof(void*)> envTable{};
    const uintptr_t vm = reinterpret_cast<uintptr_t>(vmTable.data());

    GuestJniTables()
    {
        for (size_t i = 3; i < vmTable.size(); ++i)
            vmTable[i] = JniToken(static_cast<JniEntry>(static_cast<size_t>(JniEntry::VmBase) + i));
        for (size_t i = 4; i < envTable.size(); ++i)
            envTable[i] = JniToken(static_cast<JniEntry>(i));
    }
};

extern const GuestJniTables guestJni;
extern thread_local uintptr_t guestJniEnv;
}
