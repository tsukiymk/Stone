#include "abi/AndroidLibraryImports.h"

namespace stone::windows {
uintptr_t LibdlAndroid(std::string_view name)
{
    if (name == "android_set_application_target_sdk_version")
        return Import(name, &SetAndroidTargetSdk);
    if (name == "android_get_exported_namespace")
        return Import(name, &AndroidExportedNamespace);
    if (name == "android_create_namespace")
        return Import(
            name, +[](const char*, const char*, const char*, uint64_t, const char*, void*) -> void* {
                SetDlError("Android linker namespace creation is unavailable on Windows");
                return nullptr;
            });
    if (name == "android_link_namespaces")
        return Import(
            name, +[](void*, void*, const char*) {
                SetDlError("Android linker namespace linking is unavailable on Windows");
                return false;
            });
    return 0;
}
}
