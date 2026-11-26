#include "abi/AndroidLibraryImports.h"
#include "stub-libdl.h"
#include <cstdlib>
#include <cstring>

namespace stone::windows {
std::string_view AndroidLibrarySearchPath()
{
    return "/apex/com.android.art/lib64:/apex/com.android.i18n/lib64:/system/lib64";
}

uintptr_t Libdl(std::string_view name)
{
    if (name == "android_get_LD_LIBRARY_PATH")
        return Import(
            name, +[](char* output, size_t capacity) {
                const auto paths = AndroidLibrarySearchPath();
                if (!output || capacity <= paths.size())
                    std::abort();
                std::memcpy(output, paths.data(), paths.size());
                output[paths.size()] = 0;
            });
    if (name == "dlopen")
        return Import(name, &DlOpen);
    if (name == "android_dlopen_ext")
        return Import(name, &AndroidDlOpenExt);
    if (name == "android_get_application_target_sdk_version")
        return Import(
            name, +[] {
                return AndroidTargetSdk();
            });
    if (name == "dlsym")
        return Import(name, &DlSym);
    if (name == "dlclose")
        return Import(name, &DlClose);
    if (name == "dlerror")
        return Import(name, &DlError);
    if (name == "android_dlwarning")
        return Import(
            name, +[](void*, uintptr_t) {
                // Required bindings fail the load. This linker has no deferred
                // compatibility warnings to report through the optional callback.
            });
    if (name == "dladdr")
        return Import(name, &DlAddr);
    if (name == "dl_iterate_phdr")
        return Import(
            name, +[](uintptr_t callback, void* data) {
                return DlIterateProgramHeaders(callback, data, false);
            });
    if (name == "__cfi_slowpath")
        return Import(
            name, +[](uint64_t typeId, void* target) {
                DlCfiSlowpath(typeId, target, nullptr);
            });
    if (name == "__cfi_slowpath_diag")
        return Import(name, &DlCfiSlowpath);
    return 0;
}
}
