#include "loader/AndroidLibraryState.h"

namespace stone::windows {
using namespace android_libraries;

namespace android_libraries {
constexpr size_t EnvironmentSlots = sizeof(JNINativeInterface_) / sizeof(void*);
std::array<void*, EnvironmentSlots> envTable{};
std::array<void*, sizeof(JNIInvokeInterface_) / sizeof(void*)> vmTable{};
thread_local GuestEnv guestEnv{envTable.data(), nullptr};
GuestVm guestVm{vmTable.data(), nullptr};
JavaVM* hostVm;

void ThrowLinkError(JNIEnv* env, const char* message)
{
    if (env->ExceptionCheck())
        return;
    if (const auto type = env->FindClass("java/lang/UnsatisfiedLinkError")) {
        env->ThrowNew(type, message);
        env->DeleteLocalRef(type);
    }
}

}

std::string Utf(JNIEnv* env, jstring value)
{
    if (!value)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars)
        throw std::runtime_error("could not read JNI string");
    const auto release = [=](const char* p) {
        env->ReleaseStringUTFChars(value, p);
    };
    const std::unique_ptr<const char, decltype(release)> guard(chars, release);
    return std::string(chars);
}

namespace android_libraries {
template <size_t Slot> uintptr_t UnavailableEnv(GuestEnv* env)
{
    char message[96];
    std::snprintf(message, sizeof(message), "Android library called unsupported JNI table slot %zu on Windows", Slot);
    ThrowLinkError(env->host, message);
    return 0;
}

template <size_t... Slots> void BindUnavailableEnv(std::index_sequence<Slots...>)
{
    const std::array handlers{&UnavailableEnv<Slots>...};
    for (size_t slot = 4; slot < handlers.size(); ++slot) {
        if (!envTable[slot])
            envTable[slot] = MakeThunk(reinterpret_cast<uintptr_t>(handlers[slot]), AndroidToWindows);
    }
}

jint UnavailableVm(GuestVm*)
{
    return JNI_ERR;
}

// The HotSpot table belongs to each thread, including when -Xcheck:jni is enabled.
// Use the member pointer to fetch its current slot at call time.
template <auto Member, size_t Slot, typename R, typename... A>
void BindSlot(R (JNICALL* JNINativeInterface_::*)(JNIEnv*, A...))
{
    static_assert(sizeof...(A) <= 5);
    static_assert((!std::is_floating_point_v<A> && ...));
    const auto handler = +[](GuestEnv* e, A... args) -> R {
        return (e->host->functions->*Member)(e->host, args...);
    };
    envTable[Slot] = MakeThunk(reinterpret_cast<uintptr_t>(handler), AndroidToWindows);
}

jclass GuestObjectClass(GuestEnv* guest, jobject object)
{
    auto* env = guest->host;
    if (env->ExceptionCheck())
        return nullptr;
    if (object)
        return env->GetObjectClass(object);
    if (const auto type = env->FindClass("java/lang/NullPointerException")) {
        env->ThrowNew(type, "GetObjectClass called with a null object");
        env->DeleteLocalRef(type);
    }
    return nullptr;
}

jclass FindGuestClassFromHost(JNIEnv* env, const char* name)
{
    const auto loader = CurrentNamespace().loader;
    if (!name || !loader)
        return env->FindClass(name);
    std::string dotted(name);
    for (auto& ch : dotted)
        if (ch == '/')
            ch = '.';
    const auto str = env->NewStringUTF(dotted.c_str());
    if (!str)
        return nullptr;
    const auto type = static_cast<jclass>(env->CallObjectMethod(loader, Process().loadClass, str));
    env->DeleteLocalRef(str);
    return type;
}

jclass FindGuestClass(GuestEnv* e, const char* name)
{
    return FindGuestClassFromHost(e->host, name);
}

template <bool Static> jmethodID GuestMethodId(GuestEnv* guest, jclass type, const char* name, const char* signature)
{
    auto* env = guest->host;
    if (!type || env->ExceptionCheck())
        return nullptr;
    const auto method =
        Static ? env->GetStaticMethodID(type, name, signature) : env->GetMethodID(type, name, signature);
    if (method) {
        try {
            auto& state = Process();
            const auto directory =
                Static && &CurrentNamespace() == &state.webView && IsWebViewDirectoryMethod(env, type, name, signature);
            if (env->ExceptionCheck())
                return nullptr;
            const std::lock_guard lock(state.signaturesMutex);
            state.signatures.try_emplace(method, signature);
            if (directory)
                state.webViewDirectoryMethods.insert(method);
        }
        catch (const std::bad_alloc&) {
            ThrowLinkError(env, "could not save Android JNI method signature");
            return nullptr;
        }
    }
    return method;
}

bool ReadJniArgs(JNIEnv* env, jmethodID method, const AndroidVaList* source, std::vector<jvalue>& result)
{
    if (env->ExceptionCheck())
        return false;
    try {
        auto& state = Process();
        const std::lock_guard lock(state.signaturesMutex);
        const auto it = state.signatures.find(method);
        if (it == state.signatures.end())
            throw std::runtime_error("unknown Android JNI method ID");
        const auto& signature = it->second;
        auto args = *source;
        for (size_t i = 1; signature[i] != ')'; ++i) {
            const bool array = signature[i] == '[';
            while (signature[i] == '[')
                ++i;
            char type = signature[i];
            if (type == 'L')
                i = signature.find(';', i);
            if (array)
                type = 'L';
            const bool fp = type == 'D' || type == 'F';
            const auto bits = args.next(fp);
            jvalue value{};
            switch (type) {
            case 'Z':
                value.z = static_cast<jboolean>(bits);
                break;
            case 'B':
                value.b = static_cast<jbyte>(bits);
                break;
            case 'C':
                value.c = static_cast<jchar>(bits);
                break;
            case 'S':
                value.s = static_cast<jshort>(bits);
                break;
            case 'I':
                value.i = static_cast<jint>(bits);
                break;
            case 'J':
                value.j = static_cast<jlong>(bits);
                break;
            case 'L':
                value.l = reinterpret_cast<jobject>(bits);
                break;
            case 'F':
                value.f = static_cast<jfloat>(std::bit_cast<double>(bits));
                break;
            case 'D':
                value.d = std::bit_cast<double>(bits);
                break;
            }
            result.push_back(value);
        }
        return true;
    }
    catch (const std::exception& error) {
        ThrowLinkError(env, error.what());
        return false;
    }
}

jobject GuestCallStaticObjectA(GuestEnv* guest, jclass receiver, jmethodID method, const jvalue* arguments)
{
    auto* env = guest->host;
    auto value = env->CallStaticObjectMethodA(receiver, method, arguments);
    if (!value || env->ExceptionCheck() || &CurrentNamespace() != &Process().webView)
        return value;
    {
        auto& state = Process();
        const std::lock_guard lock(state.signaturesMutex);
        if (!state.webViewDirectoryMethods.contains(method))
            return value;
    }
    try {
        const auto path = Utf(env, static_cast<jstring>(value));
        if (env->ExceptionCheck())
            return nullptr;
        const auto guestPath = AndroidGuestFilePath(path);
        if (path == guestPath)
            return value;
        env->DeleteLocalRef(value);
        return env->NewStringUTF(guestPath.c_str());
    }
    catch (const std::bad_alloc&) {
        ThrowLinkError(env, "could not convert WebView directory to an Android path");
        return nullptr;
    }
}

template <auto Function, typename Receiver>
auto GuestCallV(GuestEnv* guest, Receiver receiver, jmethodID method, const AndroidVaList* source)
{
    auto* env = guest->host;
    using Result = decltype((env->*Function)(receiver, method, nullptr));
    std::vector<jvalue> args;
    if (ReadJniArgs(env, method, source, args)) {
        if constexpr (std::is_same_v<decltype(Function), decltype(&JNIEnv::CallStaticObjectMethodA)>) {
            if constexpr (Function == &JNIEnv::CallStaticObjectMethodA)
                return GuestCallStaticObjectA(guest, receiver, method, args.data());
        }
        return (env->*Function)(receiver, method, args.data());
    }
    if constexpr (!std::is_void_v<Result>)
        return Result{};
}

jint GetGuestVm(GuestEnv*, GuestVm** out)
{
    if (!out)
        return JNI_ERR;
    *out = &guestVm;
    return JNI_OK;
}

jint GetGuestEnv(GuestVm* vm, void** out, jint version)
{
    if (!out)
        return JNI_ERR;
    *out = nullptr;
    JNIEnv* env = nullptr;
    const auto result = vm->host->GetEnv(reinterpret_cast<void**>(&env), version);
    if (result == JNI_OK) {
        guestEnv.host = env;
        *out = &guestEnv;
    }
    return result;
}

template <bool Daemon> jint AttachGuestThread(GuestVm* vm, void** out, void* args)
{
    if (!out)
        return JNI_ERR;
    *out = nullptr;
    void* env = nullptr;
    const jint result =
        Daemon ? vm->host->AttachCurrentThreadAsDaemon(&env, args) : vm->host->AttachCurrentThread(&env, args);
    if (result == JNI_OK) {
        guestEnv.host = static_cast<JNIEnv*>(env);
        *out = &guestEnv;
    }
    return result;
}

jint DetachGuestThread(GuestVm* vm)
{
    const auto result = vm->host->DetachCurrentThread();
    if (result == JNI_OK)
        guestEnv.host = nullptr;
    return result;
}

std::string Mangle(std::string_view modifiedUtf8)
{
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < modifiedUtf8.size();) {
        const auto lead = static_cast<uint8_t>(modifiedUtf8[i++]);
        unsigned code = lead;
        if (lead >= 0xc0 && lead <= 0xdf && i < modifiedUtf8.size()) {
            code = ((lead & 31u) << 6) | (modifiedUtf8[i++] & 63u);
        }
        else if (lead >= 0xe0 && i + 1 < modifiedUtf8.size()) {
            code = ((lead & 15u) << 12) | ((modifiedUtf8[i] & 63u) << 6) | (modifiedUtf8[i + 1] & 63u);
            i += 2;
        }
        if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9'))
            result.push_back(static_cast<char>(code));
        else if (code == '/' || code == '.')
            result += '_';
        else if (code == '_')
            result += "_1";
        else if (code == ';')
            result += "_2";
        else if (code == '[')
            result += "_3";
        else {
            result += "_0";
            for (int shift = 12; shift >= 0; shift -= 4)
                result += hex[(code >> shift) & 15];
        }
    }
    return result;
}

void* NativeThunk(uintptr_t address, std::string_view signature)
{
    auto& state = Process();
    const std::string key = std::to_string(address) + ':' + std::string(signature);
    if (const auto it = state.nativeThunks.find(key); it != state.nativeThunks.end())
        return it->second;
    auto call = std::make_unique<Call>(address, signature, true);
    call->scope = NamespaceForCode(address);
    const auto context = reinterpret_cast<uintptr_t>(call.get());
    state.calls.push_back(std::move(call));
    void* thunk = MakeThunk(context, WindowsToAndroid);
    state.nativeThunks.emplace(key, thunk);
    return thunk;
}

jint RegisterNativeTable(JNIEnv* env, jclass type, std::span<const JNINativeMethod> methods)
{
    if (methods.empty())
        return JNI_OK;
    auto& registrations = Process().registrations;
    registrations.reserve(registrations.size() + methods.size());
    std::vector<State::Registration> staged;
    staged.reserve(methods.size());
    for (const auto& method : methods)
        staged.push_back({nullptr, method.name, method.signature});
    const auto global = static_cast<jclass>(env->NewGlobalRef(type));
    if (!global)
        return JNI_ERR;
    const auto result = env->RegisterNatives(type, methods.data(), static_cast<jint>(methods.size()));
    if (result == JNI_OK) {
        for (auto& registration : staged) {
            registration.type = global;
            registrations.push_back(std::move(registration));
        }
    }
    else {
        env->DeleteGlobalRef(global);
    }
    return result;
}

jint RegisterGuestNatives(GuestEnv* e, jclass type, const JNINativeMethod* input, jint count)
{
    try {
        if (count < 0 || count > 4096 || (count && !input))
            throw std::runtime_error("invalid JNI registration count");
        const std::lock_guard lock(Process().mutex);
        std::vector<JNINativeMethod> methods;
        for (jint i = 0; i < count; ++i) {
            if (!input[i].name || !input[i].signature || !input[i].fnPtr)
                throw std::runtime_error("invalid JNI registration entry");
            const bool drawTable = &CurrentNamespace() == &Process().webView &&
                                   IsWebViewDrawFunctionTable(e->host, type, input[i].name, input[i].signature);
            const auto thunk = drawTable ? reinterpret_cast<void*>(&WebViewDrawFunctionTable)
                                         : NativeThunk(reinterpret_cast<uintptr_t>(input[i].fnPtr), input[i].signature);
            methods.push_back({input[i].name, input[i].signature, thunk});
        }
        return RegisterNativeTable(e->host, type, methods);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "[android-native] RegisterNatives: %s\n", error.what());
        ThrowLinkError(e->host, error.what());
        return JNI_ERR;
    }
}

void BindTables()
{
    envTable.fill(nullptr);
    vmTable.fill(MakeThunk(reinterpret_cast<uintptr_t>(&UnavailableVm), AndroidToWindows));
    for (size_t i = 0; i < 3; ++i)
        vmTable[i] = nullptr;
#define SLOT(member) (offsetof(JNINativeInterface_, member) / sizeof(void*))
#define HOST(member) BindSlot<&JNINativeInterface_::member, SLOT(member)>(&JNINativeInterface_::member)
#define ENV(member, fn) envTable[SLOT(member)] = MakeThunk(reinterpret_cast<uintptr_t>(&fn), AndroidToWindows)
#define VAR(member, fn) envTable[SLOT(member)] = MakeThunk(reinterpret_cast<uintptr_t>(&fn), AndroidJniVarargs)
#define VM(member, fn)                                                                                                 \
    vmTable[offsetof(JNIInvokeInterface_, member) / sizeof(void*)] =                                                   \
        MakeThunk(reinterpret_cast<uintptr_t>(&fn), AndroidToWindows)
    ENV(FindClass, FindGuestClass);
    ENV(RegisterNatives, RegisterGuestNatives);
    ENV(GetJavaVM, GetGuestVm);
    VM(GetEnv, GetGuestEnv);
    VM(AttachCurrentThread, AttachGuestThread<false>);
    VM(AttachCurrentThreadAsDaemon, AttachGuestThread<true>);
    VM(DetachCurrentThread, DetachGuestThread);
    HOST(GetVersion);
    HOST(Throw);
    HOST(ThrowNew);
    HOST(ExceptionOccurred);
    HOST(ExceptionDescribe);
    HOST(ExceptionClear);
    HOST(ExceptionCheck);
    HOST(PushLocalFrame);
    HOST(PopLocalFrame);
    HOST(NewGlobalRef);
    HOST(DeleteGlobalRef);
    HOST(DeleteLocalRef);
    HOST(NewLocalRef);
    HOST(IsSameObject);
    HOST(EnsureLocalCapacity);
    HOST(NewWeakGlobalRef);
    HOST(DeleteWeakGlobalRef);
    HOST(GetObjectRefType);
    ENV(GetObjectClass, GuestObjectClass);
    HOST(IsInstanceOf);
    HOST(GetSuperclass);
    HOST(IsAssignableFrom);
    ENV(GetMethodID, GuestMethodId<false>);
    ENV(GetStaticMethodID, GuestMethodId<true>);
#define CALL_VARIANTS(member, target, receiver)                                                                        \
    ENV(member##V, (GuestCallV<&JNIEnv::target, receiver>));                                                           \
    VAR(member, (GuestCallV<&JNIEnv::target, receiver>))
    CALL_VARIANTS(NewObject, NewObjectA, jclass);
    HOST(NewObjectA);
    HOST(AllocObject);
#define VCALL(kind, receiver)                                                                                          \
    CALL_VARIANTS(kind##ObjectMethod, kind##ObjectMethodA, receiver);                                                  \
    CALL_VARIANTS(kind##BooleanMethod, kind##BooleanMethodA, receiver);                                                \
    CALL_VARIANTS(kind##ByteMethod, kind##ByteMethodA, receiver);                                                      \
    CALL_VARIANTS(kind##CharMethod, kind##CharMethodA, receiver);                                                      \
    CALL_VARIANTS(kind##ShortMethod, kind##ShortMethodA, receiver);                                                    \
    CALL_VARIANTS(kind##IntMethod, kind##IntMethodA, receiver);                                                        \
    CALL_VARIANTS(kind##LongMethod, kind##LongMethodA, receiver);                                                      \
    CALL_VARIANTS(kind##FloatMethod, kind##FloatMethodA, receiver);                                                    \
    CALL_VARIANTS(kind##DoubleMethod, kind##DoubleMethodA, receiver);                                                  \
    CALL_VARIANTS(kind##VoidMethod, kind##VoidMethodA, receiver)
    VCALL(Call, jobject);
    VCALL(CallStatic, jclass);
#undef VCALL
#undef CALL_VARIANTS
    HOST(CallObjectMethodA);
    HOST(CallBooleanMethodA);
    HOST(CallByteMethodA);
    HOST(CallCharMethodA);
    HOST(CallShortMethodA);
    HOST(CallIntMethodA);
    HOST(CallLongMethodA);
    HOST(CallFloatMethodA);
    HOST(CallDoubleMethodA);
    HOST(CallVoidMethodA);
    ENV(CallStaticObjectMethodA, GuestCallStaticObjectA);
    HOST(CallStaticBooleanMethodA);
    HOST(CallStaticByteMethodA);
    HOST(CallStaticCharMethodA);
    HOST(CallStaticShortMethodA);
    HOST(CallStaticIntMethodA);
    HOST(CallStaticLongMethodA);
    HOST(CallStaticFloatMethodA);
    HOST(CallStaticDoubleMethodA);
    HOST(CallStaticVoidMethodA);
    HOST(CallNonvirtualObjectMethodA);
    HOST(CallNonvirtualBooleanMethodA);
    HOST(CallNonvirtualByteMethodA);
    HOST(CallNonvirtualCharMethodA);
    HOST(CallNonvirtualShortMethodA);
    HOST(CallNonvirtualIntMethodA);
    HOST(CallNonvirtualLongMethodA);
    HOST(CallNonvirtualFloatMethodA);
    HOST(CallNonvirtualDoubleMethodA);
    HOST(CallNonvirtualVoidMethodA);
    HOST(GetFieldID);
    HOST(GetStaticFieldID);
#define FIELDS(type)                                                                                                   \
    HOST(Get##type##Field);                                                                                            \
    HOST(GetStatic##type##Field)
#define SET_FIELDS(type)                                                                                               \
    HOST(Set##type##Field);                                                                                            \
    HOST(SetStatic##type##Field)
    FIELDS(Object);
    FIELDS(Boolean);
    FIELDS(Byte);
    FIELDS(Char);
    FIELDS(Short);
    FIELDS(Int);
    FIELDS(Long);
    FIELDS(Float);
    FIELDS(Double);
    SET_FIELDS(Object);
    SET_FIELDS(Boolean);
    SET_FIELDS(Byte);
    SET_FIELDS(Char);
    SET_FIELDS(Short);
    SET_FIELDS(Int);
    SET_FIELDS(Long);
#undef SET_FIELDS
#undef FIELDS
    HOST(NewStringUTF);
    HOST(GetStringUTFLength);
    HOST(GetStringUTFChars);
    HOST(ReleaseStringUTFChars);
    HOST(NewString);
    HOST(GetStringLength);
    HOST(GetStringChars);
    HOST(ReleaseStringChars);
    HOST(GetStringRegion);
    HOST(GetStringUTFRegion);
    HOST(GetArrayLength);
    HOST(NewObjectArray);
    HOST(GetObjectArrayElement);
    HOST(SetObjectArrayElement);
    HOST(GetPrimitiveArrayCritical);
    HOST(ReleasePrimitiveArrayCritical);
    HOST(GetStringCritical);
    HOST(ReleaseStringCritical);
#define ARRAYS(type)                                                                                                   \
    HOST(New##type##Array);                                                                                            \
    HOST(Get##type##ArrayElements);                                                                                    \
    HOST(Release##type##ArrayElements);                                                                                \
    HOST(Get##type##ArrayRegion);                                                                                      \
    HOST(Set##type##ArrayRegion)
    ARRAYS(Boolean);
    ARRAYS(Byte);
    ARRAYS(Char);
    ARRAYS(Short);
    ARRAYS(Int);
    ARRAYS(Long);
    ARRAYS(Float);
    ARRAYS(Double);
#undef ARRAYS
    HOST(NewDirectByteBuffer);
    HOST(GetDirectBufferAddress);
    HOST(GetDirectBufferCapacity);
    HOST(UnregisterNatives);
    BindUnavailableEnv(std::make_index_sequence<EnvironmentSlots>{});
#undef VM
#undef VAR
#undef ENV
#undef HOST
#undef SLOT
}

}

extern "C" void* StoneGuestEnv(JNIEnv* host)
{
    guestEnv.host = host;
    return &guestEnv;
}

void DetachAndroidNativeThread()
{
    if (hostVm)
        DetachGuestThread(&guestVm);
}

}
