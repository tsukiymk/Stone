#include "ArtDocumentTrees.h"
#include "platform/windows/DocumentTrees.h"
#include "filesystem/AndroidSystemFiles.h"
#include "runtime/art/ArtJni.h"
#include "abi/AndroidLibraryImports.h"
#include "abi/TypedAndroidImport.h"
#include "hle/system/stub-libc.h"
#include "../../client/Window.h"
#include <algorithm>

namespace stone::windows {
namespace {
void Failure(JNIEnv* env, const char* message)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/io/IOException");
    if (type) {
        ArtJni<&JNINativeInterface_::ThrowNew>(env, type, message);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    }
}

template <class Action> auto Call(JNIEnv* env, Action action) -> decltype(action())
{
    try {
        return action();
    }
    catch (const winrt::hresult_error&) {
        Failure(env, "Windows directory access failed");
    }
    catch (const AndroidSystemPathError&) {
        Failure(env, "Cannot resolve document grant storage");
    }
    catch (const std::exception& error) {
        Failure(env, error.what());
    }
    if constexpr (!std::is_void_v<decltype(action())>)
        return {};
}

std::wstring Text(JNIEnv* env, jstring value)
{
    if (!value)
        throw std::invalid_argument("Missing directory document identifier");
    const auto size = ArtJni<&JNINativeInterface_::GetStringLength>(env, value);
    if (size > 8192)
        throw std::invalid_argument("Directory document identifier is too long");
    const auto* chars = ArtJni<&JNINativeInterface_::GetStringChars>(env, value, nullptr);
    if (!chars)
        throw std::runtime_error("Cannot read directory document identifier");
    std::wstring text;
    try {
        text.assign(reinterpret_cast<const wchar_t*>(chars), size);
    }
    catch (...) {
        ArtJni<&JNINativeInterface_::ReleaseStringChars>(env, value, chars);
        throw;
    }
    ArtJni<&JNINativeInterface_::ReleaseStringChars>(env, value, chars);
    if (text.find(L'\0') != text.npos)
        throw std::invalid_argument("Invalid directory document identifier");
    return text;
}

jobjectArray Strings(JNIEnv* env, const std::vector<std::wstring>& values)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/String");
    if (!type)
        return nullptr;
    const auto result =
        ArtJni<&JNINativeInterface_::NewObjectArray>(env, static_cast<jsize>(values.size()), type, nullptr);
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    for (jsize index = 0; result && index < values.size(); ++index) {
        const auto& text = values[index];
        const auto value = ArtJni<&JNINativeInterface_::NewString>(env, reinterpret_cast<const jchar*>(text.data()),
                                                                   static_cast<jsize>(text.size()));
        if (!value)
            return nullptr;
        ArtJni<&JNINativeInterface_::SetObjectArrayElement>(env, result, index, value);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, value);
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            return nullptr;
    }
    return result;
}

template <class Function> JNINativeMethod Native(const char* name, const char* signature, Function function)
{
    const auto label = std::string("ART document trees ") + name;
    return {const_cast<char*>(name), const_cast<char*>(signature),
            reinterpret_cast<void*>(IsArm64TranslationEnabled()
                                        ? ImportAndroidScalars<Function>(label, reinterpret_cast<uintptr_t>(function))
                                        : Import(label, function))};
}
}

bool RegisterArtDocumentTrees(JNIEnv* env)
{
    const JNINativeMethod methods[]{
        Native(
            "initializeNative", "(Ljava/lang/String;)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring package) -> jobjectArray {
                return Call(env, [&] {
                    auto name = Text(env, package);
                    if (name.empty() || name.size() > 255 || !std::all_of(name.begin(), name.end(), [](wchar_t c) {
                            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                                   c == L'_' || c == L'.';
                        }))
                        throw std::invalid_argument("Invalid document grant owner");
                    ConfigureDocumentTrees(ResolveAndroidSystemFile(L"data/system/stone-document-trees/" + name));
                    return Strings(env, RestoreDocumentTrees());
                });
            }),
        Native(
            "createNative", "()J",
            +[](JNIEnv* env, jclass) -> jlong {
                return Call(env, [&] {
                    return static_cast<jlong>(CreateWindowDocumentTree(WindowsHostWindow()));
                });
            }),
        Native(
            "pickNative", "(J)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jlong request) -> jobjectArray {
                return Call(env, [&]() -> jobjectArray {
                    auto selected = PickWindowDocument(static_cast<uint64_t>(request));
                    if (!selected)
                        return nullptr;
                    auto values = KeepDocumentTree(std::move(*selected));
                    try {
                        auto result = Strings(env, values);
                        if (!result)
                            ReleaseDocumentTree(values[0]);
                        return result;
                    }
                    catch (...) {
                        ReleaseDocumentTree(values[0]);
                        throw;
                    }
                });
            }),
        Native(
            "discardNative", "(Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring id) {
                Call(env, [&] {
                    ReleaseDocumentTree(Text(env, id));
                });
            }),
        Native(
            "persistNative", "(Ljava/lang/String;J)V",
            +[](JNIEnv* env, jclass, jstring id, jlong time) {
                Call(env, [&] {
                    if (time < 0)
                        throw std::invalid_argument("Invalid directory grant time");
                    PersistDocumentTree(Text(env, id), static_cast<uint64_t>(time));
                });
            }),
        Native(
            "statNative", "(Ljava/lang/String;)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring id) -> jobjectArray {
                return Call(env, [&] {
                    return Strings(env, StatTreeDocument(Text(env, id)));
                });
            }),
        Native(
            "listNative", "(Ljava/lang/String;)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring id) -> jobjectArray {
                return Call(env, [&] {
                    return Strings(env, ListTreeDocuments(Text(env, id)));
                });
            }),
        Native(
            "openNative", "(Ljava/lang/String;)I",
            +[](JNIEnv* env, jclass, jstring id) -> jint {
                return Call(env, [&] {
                    auto file = OpenTreeDocument(Text(env, id));
                    int fd = ImportAndroidSocketFile({reinterpret_cast<uintptr_t>(file.detach())});
                    if (fd < 0)
                        throw std::runtime_error("Cannot import directory document descriptor");
                    return fd;
                });
            }),
    };
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/runtime/ArtDocumentTrees");
    if (!type)
        return false;
    const bool result = ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, methods,
                                                                      static_cast<jint>(std::size(methods))) == JNI_OK;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return result;
}
}
