#include "LibraryJniStubs.h"
#include "abi/AndroidLibraryImports.h"

#include <algorithm>

namespace stone::windows {
namespace {
bool IsClass(JNIEnv* env, jclass type, std::string_view expected)
{
    const auto klass = env->GetObjectClass(type);
    if (!klass)
        return false;
    const auto getName = env->GetMethodID(klass, "getName", "()Ljava/lang/String;");
    env->DeleteLocalRef(klass);
    if (!getName)
        return false;
    const auto className = static_cast<jstring>(env->CallObjectMethod(type, getName));
    if (env->ExceptionCheck() || !className)
        return false;
    const auto match = Utf(env, className) == expected;
    env->DeleteLocalRef(className);
    return match;
}
}

bool IsWebViewDirectoryMethod(JNIEnv* env, jclass type, std::string_view name, std::string_view signature)
{
    if (signature != "()Ljava/lang/String;")
        return false;
    constexpr std::string_view names[]{
        "getDataDirectory",      "getCacheDirectory",           "getThumbnailCacheDirectory",
        "getDownloadsDirectory", "getExternalStorageDirectory", "getNativeLibraryDirectory"};
    return std::ranges::find(names, name) != std::end(names) && IsClass(env, type, "org.chromium.base.PathUtils");
}

bool IsWebViewDrawFunctionTable(JNIEnv* env, jclass type, std::string_view name, std::string_view signature)
{
    return name == "nativeGetFunctionTable" && signature == "()J" &&
           IsClass(env, type, "com.android.webview.chromium.DrawFunctor");
}
}
