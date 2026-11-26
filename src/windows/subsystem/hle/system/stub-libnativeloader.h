#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <jni.h>

namespace stone::windows {
uintptr_t Libnativeloader(std::string_view name);
uintptr_t AndroidNativeLoaderImport(std::string_view name, bool hostAbi);
void BindArtNativeLibraries(JNIEnv* env, jobject loader, const std::wstring& directory,
                            const std::wstring& dataDirectory);
}
