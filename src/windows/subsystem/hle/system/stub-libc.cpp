#include "abi/AndroidLibraryImports.h"
#include "stub-libc.h"
#include "stub-libc-properties.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <stdexcept>

namespace stone::windows {
uintptr_t AndroidSearchImport(std::string_view name);
uintptr_t AndroidResolverImport(std::string_view name);

namespace {
using AndroidPropertyTable = std::map<std::string, std::string, std::less<>>;
using AndroidPropertyEntry = AndroidPropertyTable::value_type;
constexpr size_t PropertyValueMax = 91;
constexpr size_t LegacyPropertyNameMax = 31;
std::mutex propertyMutex;
std::condition_variable propertyChanged;
std::atomic<uint32_t> propertyAreaSerial{0};
AndroidPropertyTable properties;

bool LoadAndroidProperties()
{
    if (propertyAreaSerial.load(std::memory_order_acquire))
        return true;
    if (!AndroidHostVm())
        return false;
    const AttachedEnvironment thread;
    auto* env = thread.env;
    if (!env || env->ExceptionCheck())
        return false;
    if (env->PushLocalFrame(4) < 0) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return false;
    }
    const auto type = env->FindClass("android/os/SystemProperties");
    const auto get = type ? env->GetStaticMethodID(type, "stonePropertySnapshot", "()[Ljava/lang/String;") : nullptr;
    const auto values = get ? static_cast<jobjectArray>(env->CallStaticObjectMethod(type, get)) : nullptr;
    AndroidPropertyTable snapshot;
    bool complete = false;
    if (!env->ExceptionCheck() && values) {
        const auto count = env->GetArrayLength(values);
        try {
            for (jsize i = 0; i + 1 < count; i += 2) {
                const auto key = static_cast<jstring>(env->GetObjectArrayElement(values, i));
                if (!key)
                    break;
                const auto value = static_cast<jstring>(env->GetObjectArrayElement(values, i + 1));
                if (!value)
                    break;
                snapshot.try_emplace(Utf(env, key), Utf(env, value));
                env->DeleteLocalRef(value);
                env->DeleteLocalRef(key);
            }
            complete = count > 0 && snapshot.size() * 2 == static_cast<size_t>(count);
        }
        catch (const std::exception&) {
            // Retry the snapshot on the next read if the JVM could not supply it.
        }
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->PopLocalFrame(nullptr);
    if (complete) {
        const std::lock_guard lock(propertyMutex);
        if (!propertyAreaSerial.load(std::memory_order_relaxed)) {
            // These app-visible properties are read-only. Bionic handles remain valid for the process lifetime.
            properties.swap(snapshot);
            propertyAreaSerial.store(static_cast<uint32_t>(properties.size()), std::memory_order_release);
            propertyChanged.notify_all();
        }
    }
    return propertyAreaSerial.load(std::memory_order_acquire) != 0;
}

const AndroidPropertyEntry* FindAndroidProperty(const char* name)
{
    if (!name || !LoadAndroidProperties())
        return nullptr;
    const auto entry = properties.find(name);
    return entry == properties.end() ? nullptr : &*entry;
}

std::string_view LegacyPropertyValue(const AndroidPropertyEntry& property)
{
    if (property.second.size() <= PropertyValueMax)
        return property.second;
    // AOSP's legacy read API reports a diagnostic for long read-only values.
    return "Must use __system_property_read_callback() to read";
}

uint32_t AndroidPropertySerial(const AndroidPropertyEntry* property)
{
    if (!property)
        return UINT32_MAX;
    const uint32_t flags = property->second.size() > PropertyValueMax ? uint32_t{1} << 16 : 0;
    return (static_cast<uint32_t>(LegacyPropertyValue(*property).size()) << 24) | flags;
}

int ReadLegacyAndroidProperty(const AndroidPropertyEntry* property, char* name, char* value)
{
    if (property && name) {
        const auto size = std::min(property->first.size(), LegacyPropertyNameMax);
        std::memcpy(name, property->first.data(), size);
        name[size] = 0;
    }
    const auto text = property ? LegacyPropertyValue(*property) : std::string_view{};
    if (!text.empty())
        std::memcpy(value, text.data(), text.size());
    value[text.size()] = 0;
    return static_cast<int>(text.size());
}

}

int AndroidProperty(const char* name, char* value)
{
    return ReadLegacyAndroidProperty(FindAndroidProperty(name), nullptr, value);
}

void SetAndroidSystemProperties(AndroidSystemPropertyTable values)
{
    if (values.empty())
        throw std::invalid_argument("the Android system property table is empty");
    const std::lock_guard lock(propertyMutex);
    if (propertyAreaSerial.load(std::memory_order_relaxed))
        throw std::logic_error("Android system properties have already been published");
    properties = std::move(values);
    propertyAreaSerial.store(static_cast<uint32_t>(properties.size()), std::memory_order_release);
    propertyChanged.notify_all();
}

std::string_view AndroidSystemProperty(std::string_view name)
{
    if (!LoadAndroidProperties())
        return {};
    const auto found = properties.find(name);
    return found == properties.end() ? std::string_view{} : std::string_view(found->second);
}

uintptr_t FindAndroidSystemProperty(std::string_view name)
{
    if (!LoadAndroidProperties())
        return 0;
    const auto found = properties.find(name);
    return found == properties.end() ? 0 : reinterpret_cast<uintptr_t>(&*found);
}

bool ReadAndroidSystemProperty(uintptr_t handle, AndroidSystemPropertyValue& value)
{
    if (!handle || !LoadAndroidProperties())
        return false;
    for (const auto& entry : properties) {
        if (reinterpret_cast<uintptr_t>(&entry) == handle) {
            value = {entry.first, entry.second, AndroidPropertySerial(&entry)};
            return true;
        }
    }
    return false;
}

uint32_t AndroidSystemPropertyAreaSerial()
{
    return LoadAndroidProperties() ? propertyAreaSerial.load(std::memory_order_acquire) : UINT32_MAX;
}

namespace {
void ReadAndroidProperty(const AndroidPropertyEntry* property, uintptr_t callback, void* cookie)
{
    if (!property || !callback)
        return;
    CallAndroidCallback(callback, cookie, const_cast<char*>(property->first.c_str()),
                        const_cast<char*>(property->second.c_str()), AndroidPropertySerial(property));
}

int ForEachAndroidProperty(uintptr_t callback, void* cookie)
{
    if (!callback || !LoadAndroidProperties())
        return -1;
    for (const auto& property : properties) {
        CallAndroidCallback(callback, const_cast<AndroidPropertyEntry*>(&property), cookie);
    }
    return 0;
}

bool WaitAndroidProperty(const AndroidPropertyEntry* property, uint32_t oldSerial, uint32_t* updated,
                         const AndroidTimespec* timeout)
{
    if (!updated || (!property && !LoadAndroidProperties()))
        return false;
    std::unique_lock lock(propertyMutex);
    const auto changed = [&] {
        const auto serial =
            property ? AndroidPropertySerial(property) : propertyAreaSerial.load(std::memory_order_relaxed);
        if (serial == oldSerial)
            return false;
        *updated = serial;
        return true;
    };
    if (changed())
        return true;
    if (!timeout) {
        propertyChanged.wait(lock, changed);
        return true;
    }
    if (timeout->seconds < 0 || timeout->nanos < 0 || timeout->nanos >= 1'000'000'000)
        return false;
    auto remaining = timeout->seconds;
    while (remaining > 86400) {
        if (propertyChanged.wait_for(lock, std::chrono::hours(24), changed))
            return true;
        remaining -= 86400;
    }
    return propertyChanged.wait_for(lock, std::chrono::seconds(remaining) + std::chrono::nanoseconds(timeout->nanos),
                                    changed);
}
}

uintptr_t Libc(std::string_view name)
{
    const auto bridge = [&](auto function) {
        return Import(name, function);
    };
    if (name == "memcpy")
        return bridge(&std::memcpy);
    if (name == "memmove")
        return bridge(&std::memmove);
    if (name == "memset")
        return bridge(&std::memset);
    if (name == "memcmp")
        return bridge(&std::memcmp);
    if (name == "strlen")
        return bridge(&std::strlen);
    if (name == "strcmp")
        return bridge(&std::strcmp);
    if (name == "strncmp")
        return bridge(&std::strncmp);
    if (name == "malloc")
        return bridge(&AndroidMalloc);
    if (name == "calloc")
        return bridge(&AndroidCalloc);
    if (name == "realloc")
        return bridge(&AndroidRealloc);
    if (name == "free")
        return bridge(&AndroidFree);
    if (name == "isnanf")
        return Import(
            name, +[](float value) -> int {
                return std::isnan(value);
            });
    if (name == "ldexp")
        return Libm(name);
    if (name == "__tls_get_addr") {
        struct TlsIndex {
            const ElfImage* module;
            uint64_t offset;
        };

        return Import(
            name, +[](const TlsIndex* index) -> void* {
                return ResolveAndroidTls(index ? index->module : nullptr, index ? index->offset : 0);
            });
    }
    if (name == "fprintf")
        return reinterpret_cast<uintptr_t>(&AndroidVarargsPrint);
    if (name == "android_get_device_api_level")
        return Import(
            name, +[] {
                char value[92];
                return AndroidProperty("ro.build.version.sdk", value) ? std::atoi(value) : -1;
            });
    if (name == "__system_property_get")
        return Import(name, &AndroidProperty);
    if (name == "__system_properties_init")
        return Import(
            name, +[] {
                return LoadAndroidProperties() ? 0 : -1;
            });
    if (name == "__system_properties_zygote_reload")
        return Import(
            name, +[] {
                // The image-backed property snapshot is immutable; there is no init
                // service or forked zygote mount namespace to replace it underneath us.
                return LoadAndroidProperties() ? 0 : -1;
            });
    if (name == "__system_property_find")
        return Import(name, &FindAndroidProperty);
    if (name == "__system_property_read_callback")
        return Import(name, &ReadAndroidProperty);
    if (name == "__system_property_read")
        return Import(name, &ReadLegacyAndroidProperty);
    if (name == "__system_property_foreach")
        return Import(name, &ForEachAndroidProperty);
    if (name == "__system_property_serial")
        return Import(name, &AndroidPropertySerial);
    if (name == "__system_property_area_serial")
        return Import(
            name, +[]() -> uint32_t {
                return LoadAndroidProperties() ? propertyAreaSerial.load(std::memory_order_acquire) : UINT32_MAX;
            });
    if (name == "__system_property_wait")
        return Import(name, &WaitAndroidProperty);
    if (name == "__system_property_set")
        return Import(
            name, +[](const char*, const char*) {
                // Guest APKs have no permission to set properties owned by Android's init service.
                errno = EACCES;
                return -1;
            });
    if (!IsArm64TranslationEnabled())
        if (const auto address = AndroidSearchImport(name))
            return address;
    if (const auto address = AndroidResolverImport(name))
        return address;
    return AndroidLibcImport(name);
}
}
