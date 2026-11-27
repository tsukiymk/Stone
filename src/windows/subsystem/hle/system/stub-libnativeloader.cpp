#include "stub-libnativeloader.h"
#include "abi/AndroidLibraryImports.h"
#include "loader/AndroidSystemElf.h"
#include "filesystem/AndroidSystemFiles.h"
#include "runtime/art/ArtJni.h"
#include "framework/ArtApplicationLauncher.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace stone::windows {
namespace {
std::atomic<bool> initialized{false};
jobject applicationLoader = nullptr;
std::string applicationDirectory;
std::string applicationDataPrefix;
jobject webViewLoader = nullptr;
std::string webViewDirectory;
std::wstring webViewSource;

void Error(char** output, const char* message)
{
    if (output)
        *output = _strdup(message ? message : "native library loading failed");
}

void* Open(JNIEnv* env, int32_t, const char* path, jobject loader, const char*, jstring, bool* bridged, char** error)
{
    if (bridged)
        *bridged = false;
    if (error)
        *error = nullptr;
    if (!initialized.load()) {
        Error(error, "Stone's ART native loader has not been initialized");
        return nullptr;
    }
    if (loader) {
        const bool webView = webViewLoader && ArtJni<&JNINativeInterface_::IsSameObject>(env, loader, webViewLoader);
        if (webView) {
            const bool platform =
                path && (std::string_view(path) == "libwebviewchromium_plat_support.so" ||
                         std::string_view(path) == "/system/lib64/libwebviewchromium_plat_support.so");
            if (!platform &&
                (!path || !std::string_view(path).starts_with(webViewDirectory + "/") ||
                 std::string_view(path).substr(webViewDirectory.size() + 1).find('/') != std::string_view::npos)) {
                Error(error, "native library path is outside the registered WebView library directory");
                return nullptr;
            }
        }
        else if (!applicationLoader || !ArtJni<&JNINativeInterface_::IsSameObject>(env, loader, applicationLoader)) {
            Error(error, "unregistered ART application native class loader");
            return nullptr;
        }
        else if (!path || (std::string_view(path).find('/') != std::string_view::npos &&
                           !std::string_view(path).starts_with(applicationDataPrefix) &&
                           (!std::string_view(path).starts_with(applicationDirectory + "/") ||
                            std::string_view(path).substr(applicationDirectory.size() + 1).find('/') !=
                                std::string_view::npos))) {
            Error(error, "native library path is outside the registered APK library directory");
            return nullptr;
        }
    }
    auto* handle = DlOpen(path, 2); // RTLD_NOW in the calling runtime's system scope.
    if (!handle)
        Error(error, DlError());
    return handle;
}

void BindWebView(JNIEnv* env, jobject loader, const std::wstring& source, const std::wstring& directory,
                 const std::wstring& dataDirectory)
{
    const std::lock_guard lock(AndroidLoaderMutex());
    if (!loader || ArtRunningPackage().empty() || dataDirectory != L"/data/data/" + ArtRunningPackage() ||
        !(directory.starts_with(dataDirectory + L"/code_cache/stone-native/") ||
          directory.starts_with(dataDirectory + L"/code_cache/stone-webview/")) ||
        source.empty() || source.front() != L'/' || source.find_first_of(L"\\:") != std::wstring::npos ||
        directory.find_first_of(L"\\:") != std::wstring::npos ||
        std::filesystem::path(source).lexically_normal().generic_wstring() != source ||
        std::filesystem::path(directory).lexically_normal().generic_wstring() != directory)
        throw std::runtime_error("invalid ART WebView native library binding");
    if (webViewLoader) {
        if (ArtJni<&JNINativeInterface_::IsSameObject>(env, loader, webViewLoader) && webViewSource == source &&
            webViewDirectory == std::filesystem::path(directory).generic_string())
            return;
        throw std::runtime_error("the ART WebView native loader is already bound");
    }
    const auto guest = AndroidSystemGuestPath(std::filesystem::path(source));
    if (!guest || !IsAndroidSystemGuestReadOnly(*guest) ||
        !(source.starts_with(L"/system/app/") || source.starts_with(L"/system/priv-app/") ||
          source.starts_with(L"/product/app/") || source.starts_with(L"/product/priv-app/") ||
          source.starts_with(L"/system_ext/app/") || source.starts_with(L"/system_ext/priv-app/")))
        throw std::runtime_error("the WebView native loader requires an immutable image APK");
    try {
        const auto file = ResolveAndroidSystemFile(*guest);
        const auto native = ResolveAndroidSystemFile(directory.substr(1));
        const auto data = ResolveAndroidSystemFile(dataDirectory.substr(1));
        const auto relative = std::filesystem::canonical(native).lexically_relative(std::filesystem::canonical(data));
        if (!std::filesystem::is_regular_file(file) || file.extension() != L".apk" ||
            !std::filesystem::is_directory(native) || relative.empty() || relative.is_absolute() ||
            *relative.begin() == L"..")
            throw std::runtime_error("the WebView APK or native library directory is missing");
        SetArtPrivateLibraryRoot(data, std::filesystem::path(dataDirectory).generic_string());
    }
    catch (const AndroidSystemPathError&) {
        throw std::runtime_error("cannot resolve WebView native library inputs");
    }
    auto registeredDirectory = std::filesystem::path(directory).generic_string();
    auto registeredSource = source;
    const auto global = ArtJni<&JNINativeInterface_::NewGlobalRef>(env, loader);
    if (!global)
        throw std::runtime_error("cannot retain the ART WebView native loader");
    webViewDirectory = std::move(registeredDirectory);
    webViewSource = std::move(registeredSource);
    webViewLoader = global;
}

bool Close(void* handle, bool bridged, char** error)
{
    if (error)
        *error = nullptr;
    if (bridged) {
        Error(error, "the ART native loader has no native-bridge namespace");
        return false;
    }
    if (DlClose(handle) == 0)
        return true;
    Error(error, DlError());
    return false;
}
}

void BindArtWebViewLibraries(JNIEnv* env, jobject loader, const std::wstring& source, const std::wstring& directory,
                             const std::wstring& dataDirectory)
{
    BindWebView(env, loader, source, directory, dataDirectory);
}

void BindArtNativeLibraries(JNIEnv* env, jobject loader, const std::wstring& directory,
                            const std::wstring& dataDirectory)
{
    const std::lock_guard lock(AndroidLoaderMutex());
    if (!loader || applicationLoader || !directory.starts_with(dataDirectory + L"/") || ArtRunningPackage().empty() ||
        dataDirectory != L"/data/data/" + ArtRunningPackage() || !dataDirectory.starts_with(L"/data/data/") ||
        dataDirectory.size() <= 11 || dataDirectory.substr(11).find_first_of(L"/\\:") != std::wstring::npos)
        throw std::runtime_error("invalid or repeated ART native library binding");
    // One APK and one application loader per Client. Additional class-loader
    // namespaces remain explicitly unsupported.
    const auto guestData = std::filesystem::path(dataDirectory).generic_string();
    try {
        SetArtApplicationLibraries(ResolveAndroidSystemFile(directory.substr(1)),
                                   ResolveAndroidSystemFile(dataDirectory.substr(1)), guestData);
    }
    catch (const AndroidSystemPathError&) {
        throw std::runtime_error("cannot resolve APK native library directory");
    }
    applicationDirectory = std::filesystem::path(directory).generic_string();
    applicationDataPrefix = guestData + "/";
    applicationLoader = ArtJni<&JNINativeInterface_::NewGlobalRef>(env, loader);
    if (!applicationLoader)
        throw std::runtime_error("cannot retain ART application native loader");
}

uintptr_t AndroidNativeLoaderImport(std::string_view name, bool hostAbi)
{
    const auto bind = [name, hostAbi]<typename T>(T function, void (*bridge)() = AndroidToWindows) {
        const auto address = reinterpret_cast<uintptr_t>(function);
        return hostAbi ? address : Import(name, address, bridge);
    };
    if (name == "InitializeNativeLoader")
        return bind(+[] {
            initialized.store(true);
        });
    if (name == "ResetNativeLoader")
        return bind(+[] {
            initialized.store(false);
        });
    if (name == "OpenNativeLibrary")
        return bind(&Open, AndroidToWindowsWide);
    if (name == "CloseNativeLibrary")
        return bind(&Close);
    if (name == "NativeLoaderFreeErrorMessage")
        return bind(&std::free);
    if (name == "FindNativeLoaderNamespaceByClassLoader")
        return bind(+[](JNIEnv*, jobject) -> void* {
            return nullptr;
        });
    if (name == "IsNamespaceNativeBridged")
        return bind(+[](const void*) {
            return false;
        });
    return 0;
}

uintptr_t Libnativeloader(std::string_view name)
{
    return AndroidNativeLoaderImport(name, false);
}
}
