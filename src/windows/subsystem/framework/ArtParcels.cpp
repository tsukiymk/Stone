#include "ArtParcels.h"
#include "../runtime/art/ArtJni.h"
#include "../abi/AndroidLibraryImports.h"
#include "../abi/TypedAndroidImport.h"
#include <atomic>
#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace stone::windows {
ArtLocalBinderState& LocalArtBinderState()
{
    thread_local ArtLocalBinderState state;
    return state;
}

namespace {
// Image Parcel retains all ordinary data and file descriptors. Local Java
// Binder references use opaque integers and owned JNI references, since even
// flattening a local image BBinder opens /dev/binder. These tokens cannot be
// marshalled, imported from bytes, or consumed by native Android Binder code.
struct Reference {
    jlong token;
    jobject object;
};

std::mutex gate;
std::map<jlong, std::map<jint, Reference>> references;
std::map<jlong, size_t> requestHeaders;
std::atomic<jlong> nextToken{1};
jclass parcelClass, binderClass;
jmethodID dataPosition, writeLong, readLong;
jclass strictModeClass;
jmethodID strictModeChanged;
void Throw(JNIEnv* env, const char* message);
constexpr jint SystemParcelHeader = 0x53595354; // AOSP B_PACK_CHARS('S','Y','S','T')
constexpr jint GatherViolations = static_cast<jint>(0x80000000u);

template <class F> F ParcelFunction(const char* symbol, const char* signature)
{
    return reinterpret_cast<F>(MakeNativeAndroidThunk(AndroidRuntimeFunction("libbinder.so", symbol, true), signature));
}

jint (*parcelWriteInt)(jlong, jint);
jint (*parcelReadInt)(jlong);
jint (*parcelWriteString)(jlong, const jchar*, size_t);
const jchar* (*parcelReadString)(jlong, size_t*);
size_t (*parcelPosition)(jlong);
void (*parcelSetPosition)(jlong, size_t);

void RequestHeader(jlong pointer)
{
    const std::lock_guard lock(gate);
    requestHeaders.try_emplace(pointer, parcelPosition(pointer));
}

jint ReadWorkSource(jlong pointer)
{
    const std::lock_guard lock(gate);
    const auto it = requestHeaders.find(pointer);
    if (it == requestHeaders.end())
        return -1;
    const auto saved = parcelPosition(pointer);
    parcelSetPosition(pointer, it->second);
    const auto uid = parcelReadInt(pointer);
    parcelSetPosition(pointer, saved);
    return uid;
}

jboolean ReplaceWorkSource(jlong pointer, jint uid)
{
    const std::lock_guard lock(gate);
    const auto it = requestHeaders.find(pointer);
    if (it == requestHeaders.end())
        return false;
    const auto saved = parcelPosition(pointer);
    parcelSetPosition(pointer, it->second);
    const auto status = parcelWriteInt(pointer, uid);
    parcelSetPosition(pointer, saved);
    return status == 0;
}

void WriteToken(JNIEnv* env, jclass, jlong pointer, jstring name)
{
    if (!pointer)
        return;
    if (!name) {
        auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/NullPointerException");
        if (type) {
            ArtJni<&JNINativeInterface_::ThrowNew>(env, type, "Null interface descriptor");
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
        }
        return;
    }
    const auto* text = ArtJni<&JNINativeInterface_::GetStringChars>(env, name, nullptr);
    if (!text)
        return;
    const auto& state = LocalArtBinderState();
    jint status = parcelWriteInt(pointer, state.strictModePolicy | GatherViolations);
    if (!status) {
        RequestHeader(pointer);
        status = parcelWriteInt(pointer, state.propagateWorkSource ? state.workSource : -1);
    }
    if (!status)
        status = parcelWriteInt(pointer, SystemParcelHeader);
    if (!status)
        status = parcelWriteString(pointer, text, ArtJni<&JNINativeInterface_::GetStringLength>(env, name));
    ArtJni<&JNINativeInterface_::ReleaseStringChars>(env, name, text);
    if (status)
        Throw(env, "Cannot write interface token");
}

void EnforceToken(JNIEnv* env, jclass, jlong pointer, jstring name)
{
    bool matches = false;
    const auto oldPolicy = LocalArtBinderState().strictModePolicy;
    if (pointer && name) {
        auto& state = LocalArtBinderState();
        state.strictModePolicy = parcelReadInt(pointer);
        RequestHeader(pointer);
        state.workSource = parcelReadInt(pointer);
        state.propagateWorkSource = false;
        if (parcelReadInt(pointer) == SystemParcelHeader) {
            size_t length = 0;
            const auto* actual = parcelReadString(pointer, &length);
            if (actual && length == static_cast<size_t>(ArtJni<&JNINativeInterface_::GetStringLength>(env, name))) {
                const auto* expected = ArtJni<&JNINativeInterface_::GetStringChars>(env, name, nullptr);
                if (!expected)
                    return;
                matches = !length || std::memcmp(actual, expected, length * sizeof(jchar)) == 0;
                ArtJni<&JNINativeInterface_::ReleaseStringChars>(env, name, expected);
            }
        }
    }
    if (matches) {
        if (oldPolicy != LocalArtBinderState().strictModePolicy) {
            jvalue arguments[]{jvalue{.i = LocalArtBinderState().strictModePolicy}};
            ArtJni<&JNINativeInterface_::CallStaticVoidMethodA>(env, strictModeClass, strictModeChanged, arguments);
        }
        return;
    }
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/SecurityException");
    if (type) {
        ArtJni<&JNINativeInterface_::ThrowNew>(env, type, "Binder invocation to an incorrect interface");
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    }
}

void Throw(JNIEnv* env, const char* message)
{
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "android/os/BadParcelableException");
    if (type) {
        ArtJni<&JNINativeInterface_::ThrowNew>(env, type, message);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    }
}

jint Position(JNIEnv* env, jlong pointer)
{
    jvalue args[]{jvalue{.j = pointer}};
    return ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, parcelClass, dataPosition, args);
}

void Clear(JNIEnv* env, jlong pointer)
{
    const std::lock_guard lock(gate);
    requestHeaders.erase(pointer);
    auto it = references.find(pointer);
    if (it == references.end())
        return;
    for (auto& [offset, reference] : it->second)
        ArtJni<&JNINativeInterface_::DeleteGlobalRef>(env, reference.object);
    references.erase(it);
}

template <class F> F Image(const char* name, const char* tail, const char* signature)
{
    std::string symbol = "android_os_Parcel_";
    symbol += name;
    symbol = "_ZN7androidL" + std::to_string(symbol.size()) + symbol + "EP7_JNIEnvP7_jclass" + tail;
    return reinterpret_cast<F>(
        MakeNativeAndroidThunk(AndroidRuntimeFunction("libandroid_runtime.so", symbol, true), signature));
}

void Write(JNIEnv* env, jclass, jlong pointer, jobject object)
{
    if (object && !ArtJni<&JNINativeInterface_::IsInstanceOf>(env, object, binderClass)) {
        Throw(env, "Only local Java Binder objects can enter a Stone Parcel");
        return;
    }
    jint offset = Position(env, pointer);
    jobject global = object ? ArtJni<&JNINativeInterface_::NewGlobalRef>(env, object) : nullptr;
    if (object && !global)
        return;
    jlong token = object ? nextToken.fetch_add(1) : 0;
    jvalue args[]{jvalue{.j = pointer}, jvalue{.j = token}};
    jint status = ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, parcelClass, writeLong, args);
    if (status || ArtJni<&JNINativeInterface_::ExceptionCheck>(env)) {
        if (global)
            ArtJni<&JNINativeInterface_::DeleteGlobalRef>(env, global);
        if (status)
            Throw(env, "Cannot write local Binder reference");
        return;
    }
    const std::lock_guard lock(gate);
    auto& entries = references[pointer];
    auto old = entries.lower_bound(offset < 7 ? 0 : offset - 7);
    while (old != entries.end() && old->first < int64_t(offset) + 8) {
        ArtJni<&JNINativeInterface_::DeleteGlobalRef>(env, old->second.object);
        old = entries.erase(old);
    }
    if (global)
        entries.emplace(offset, Reference{token, global});
    if (entries.empty())
        references.erase(pointer);
}

jobject Read(JNIEnv* env, jclass, jlong pointer)
{
    jint offset = Position(env, pointer);
    jvalue args[]{jvalue{.j = pointer}};
    auto token = ArtJni<&JNINativeInterface_::CallStaticLongMethodA>(env, parcelClass, readLong, args);
    if (!token || ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
        return nullptr;
    const std::lock_guard lock(gate);
    auto parcel = references.find(pointer);
    if (parcel != references.end()) {
        auto ref = parcel->second.find(offset);
        if (ref != parcel->second.end() && ref->second.token == token)
            return ArtJni<&JNINativeInterface_::NewLocalRef>(env, ref->second.object);
    }
    Throw(env, "Untrusted or stale local Binder reference");
    return nullptr;
}

jboolean Has(JNIEnv*, jclass, jlong pointer)
{
    const std::lock_guard lock(gate);
    auto it = references.find(pointer);
    return it != references.end() && !it->second.empty();
}

jboolean HasRange(JNIEnv* env, jclass, jlong pointer, jint offset, jint length)
{
    if (offset < 0 || length < 0) {
        Throw(env, "Invalid Binder range");
        return false;
    }
    const std::lock_guard lock(gate);
    auto it = references.find(pointer);
    if (it == references.end())
        return false;
    for (auto& [position, ref] : it->second)
        if (position < int64_t(offset) + length && int64_t(position) + 8 > offset)
            return true;
    return false;
}

void Free(JNIEnv* env, jclass type, jlong pointer)
{
    Clear(env, pointer);
    static auto call = Image<void (*)(JNIEnv*, jclass, jlong)>("freeBuffer", "l", "(LLJ)V");
    call(env, type, pointer);
}

void Destroy(JNIEnv* env, jclass type, jlong pointer)
{
    Clear(env, pointer);
    static auto call = Image<void (*)(JNIEnv*, jclass, jlong)>("destroy", "l", "(LLJ)V");
    call(env, type, pointer);
}

void SetSize(JNIEnv* env, jclass type, jlong pointer, jint size)
{
    static auto call = Image<void (*)(JNIEnv*, jclass, jlong, jint)>("setDataSize", "li", "(LLJI)V");
    call(env, type, pointer, size);
    if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
        return;
    const std::lock_guard lock(gate);
    auto it = references.find(pointer);
    if (it == references.end())
        return;
    auto entry = it->second.lower_bound(size < 7 ? 0 : size - 7);
    while (entry != it->second.end()) {
        ArtJni<&JNINativeInterface_::DeleteGlobalRef>(env, entry->second.object);
        entry = it->second.erase(entry);
    }
    if (it->second.empty())
        references.erase(it);
}

jbyteArray Marshall(JNIEnv* env, jclass type, jlong pointer)
{
    if (Has(env, type, pointer)) {
        Throw(env, "Local Binder references cannot be marshalled");
        return nullptr;
    }
    static auto call = Image<jbyteArray (*)(JNIEnv*, jclass, jlong)>("marshall", "l", "(LLJ)L");
    return call(env, type, pointer);
}

void Unmarshall(JNIEnv* env, jclass type, jlong pointer, jbyteArray data, jint offset, jint length)
{
    static auto call =
        Image<void (*)(JNIEnv*, jclass, jlong, jbyteArray, jint, jint)>("unmarshall", "lP11_jbyteArrayii", "(LLJLII)V");
    call(env, type, pointer, data, offset, length);
    if (!ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
        Clear(env, pointer);
}

void Append(JNIEnv* env, jclass type, jlong target, jlong source, jint offset, jint length)
{
    if (offset < 0 || length < 0) {
        Throw(env, "Invalid local Parcel append range");
        return;
    }
    static auto call = Image<void (*)(JNIEnv*, jclass, jlong, jlong, jint, jint)>("appendFrom", "llii", "(LLJJII)V");
    if (!length) {
        call(env, type, target, source, offset, length);
        return;
    }
    int position = Position(env, target);
    std::map<jint, Reference> copies;
    {
        const std::lock_guard lock(gate);
        auto it = references.find(source);
        if (it != references.end())
            for (auto& [start, ref] : it->second) {
                if (start >= int64_t(offset) + length || int64_t(start) + 8 <= offset)
                    continue;
                if (start < offset || int64_t(start) + 8 > int64_t(offset) + length) {
                    Throw(env, "Partial Binder reference append");
                    return;
                }
                copies.emplace(start - offset + position, Reference{ref.token, nullptr});
            }
        for (auto& [destination, copy] : copies) {
            copy.object =
                ArtJni<&JNINativeInterface_::NewGlobalRef>(env, it->second.at(destination - position + offset).object);
            if (!copy.object) {
                for (auto& [unused, ref] : copies)
                    if (ref.object)
                        ArtJni<&JNINativeInterface_::DeleteGlobalRef>(env, ref.object);
                return;
            }
        }
    }
    call(env, type, target, source, offset, length);
    const std::lock_guard lock(gate);
    if (!ArtJni<&JNINativeInterface_::ExceptionCheck>(env)) {
        auto& entries = references[target];
        auto old = entries.lower_bound(position < 7 ? 0 : position - 7);
        while (old != entries.end() && old->first < int64_t(position) + length) {
            ArtJni<&JNINativeInterface_::DeleteGlobalRef>(env, old->second.object);
            old = entries.erase(old);
        }
        entries.merge(copies);
        if (entries.empty())
            references.erase(target);
    }
    for (auto& [unused, ref] : copies)
        ArtJni<&JNINativeInterface_::DeleteGlobalRef>(env, ref.object);
}

template <class F> JNINativeMethod Method(const char* name, const char* signature, F function)
{
    return {const_cast<char*>(name), const_cast<char*>(signature),
            reinterpret_cast<void*>(ImportAndroidScalars<F>(std::string("ART local Parcel ") + name,
                                                            reinterpret_cast<uintptr_t>(function)))};
}
}

bool RegisterArtLocalParcels(JNIEnv* env)
{
    if (IsArm64TranslationEnabled())
        return true;
    parcelWriteInt = ParcelFunction<decltype(parcelWriteInt)>("_ZN7android6Parcel10writeInt32Ei", "(LI)I");
    parcelReadInt = ParcelFunction<decltype(parcelReadInt)>("_ZNK7android6Parcel9readInt32Ev", "(L)I");
    parcelWriteString =
        ParcelFunction<decltype(parcelWriteString)>("_ZN7android6Parcel13writeString16EPKDsm", "(LLJ)I");
    parcelReadString =
        ParcelFunction<decltype(parcelReadString)>("_ZNK7android6Parcel19readString16InplaceEPm", "(LL)L");
    parcelPosition = ParcelFunction<decltype(parcelPosition)>("_ZNK7android6Parcel12dataPositionEv", "(L)J");
    parcelSetPosition = ParcelFunction<decltype(parcelSetPosition)>("_ZNK7android6Parcel15setDataPositionEm", "(LJ)V");
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "android/os/Parcel");
    auto binder = ArtJni<&JNINativeInterface_::FindClass>(env, "android/os/Binder");
    auto strictMode = ArtJni<&JNINativeInterface_::FindClass>(env, "android/os/StrictMode");
    if (!type || !binder || !strictMode)
        return false;
    strictModeClass = static_cast<jclass>(ArtJni<&JNINativeInterface_::NewGlobalRef>(env, strictMode));
    strictModeChanged =
        ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, strictMode, "onBinderStrictModePolicyChange", "(I)V");
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, strictMode);
    parcelClass = static_cast<jclass>(ArtJni<&JNINativeInterface_::NewGlobalRef>(env, type));
    binderClass = static_cast<jclass>(ArtJni<&JNINativeInterface_::NewGlobalRef>(env, binder));
    dataPosition = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "nativeDataPosition", "(J)I");
    writeLong = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "nativeWriteLong", "(JJ)I");
    readLong = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "nativeReadLong", "(J)J");
    if (!parcelClass || !binderClass || !dataPosition || !writeLong || !readLong || !strictModeClass ||
        !strictModeChanged)
        return false;
    const JNINativeMethod methods[]{
        Method("nativeWriteInterfaceToken", "(JLjava/lang/String;)V", WriteToken),
        Method("nativeEnforceInterface", "(JLjava/lang/String;)V", EnforceToken),
        Method("nativeReadCallingWorkSourceUid", "(J)I", ReadWorkSource),
        Method("nativeReplaceCallingWorkSourceUid", "(JI)Z", ReplaceWorkSource),
        Method("nativeWriteStrongBinder", "(JLandroid/os/IBinder;)V", Write),
        Method("nativeReadStrongBinder", "(J)Landroid/os/IBinder;", Read),
        Method("nativeFreeBuffer", "(J)V", Free),
        Method("nativeDestroy", "(J)V", Destroy),
        Method("nativeSetDataSize", "(JI)V", SetSize),
        Method("nativeMarshall", "(J)[B", Marshall),
        Method("nativeUnmarshall", "(J[BII)V", Unmarshall),
        Method("nativeAppendFrom", "(JJII)V", Append),
        Method("nativeHasBinders", "(J)Z", Has),
        Method("nativeHasBindersInRange", "(JII)Z", HasRange),
    };
    bool ok = ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, methods, int(std::size(methods))) == 0;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, binder);
    return ok;
}
}
