#include "loader/AndroidLibraryState.h"

namespace stone::windows::android_libraries {
std::vector<uint8_t> CopyLibraryBytes(JNIEnv* env, jbyteArray bytes, std::string_view name)
{
    if (!bytes)
        return {};
    std::vector<uint8_t> file(static_cast<size_t>(env->GetArrayLength(bytes)));
    env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(file.size()), reinterpret_cast<jbyte*>(file.data()));
    if (env->ExceptionCheck())
        throw std::runtime_error("could not copy Android library " + std::string(name));
    return file;
}

std::vector<uint8_t> ReadLibrary(JNIEnv* env, jobject loader, const std::string& name)
{
    const auto key = env->NewStringUTF(name.c_str());
    if (!key)
        throw std::runtime_error("could not allocate library name");
    const auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(loader, Process().readLibrary, key));
    env->DeleteLocalRef(key);
    if (env->ExceptionCheck())
        throw std::runtime_error("could not read Android library " + name);
    const auto release = [env](jbyteArray value) {
        env->DeleteLocalRef(value);
    };
    const std::unique_ptr<_jbyteArray, decltype(release)> local(bytes, release);
    return CopyLibraryBytes(env, bytes, name);
}
}
