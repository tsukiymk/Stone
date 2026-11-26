#include "../host/ProcessLog.h"
#include "LaunchScreen.h"
#ifdef _M_X64
#include "ArtLaunch.h"
#endif

#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;

fs::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size())
        throw std::runtime_error("Could not resolve the Stone runtime directory");
    path.resize(size);
    return fs::path(path).parent_path();
}

int Launch(int argc, wchar_t* argv[], const fs::path& directory)
{
    stone::windows::SetProcessLogOperation(L"arguments");
    constexpr auto help =
        L"Usage: StoneClient --apk <file.apk> [--data-dir <directory>] [--system-image <file.img>] [--enable-jit] [--enable-aot]\n"
        L"       StoneClient --package <packageName> [--data-dir <directory>] [--system-image <file.img>]\n"
        L"       StoneClient --manage-package <list|install|uninstall> [--target <APK-or-package>] [--data-dir <directory>] [--system-image <file.img>]\n"
        L"Stone executes the original APK on image ART in its Win32 compatibility layer.\n"
        L"--system-image selects ART and system resources (defaults to the remembered image or adjacent system.img).\n"
        L"Windows x64 APK execution is implemented; Windows ARM64 ART execution is pending.\n"
        L"--background starts Application and services without opening the launcher Activity.\n"
        L"--enable-jit enables experimental x64 ART Java JIT for this launch.\n"
        L"--enable-aot compiles the selected APK with image dex2oat and reuses validated disk artifacts (x86_64 or experimental ARM64 images).\n"
        L"Saved experimental AOT/JIT settings apply to ART application launches in the selected data directory; explicit flags also enable them.\n"
        L"ARM64 images on Windows x64 request JIT automatically; ART startup remains experimental.\n";
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        stone::windows::SetProcessLogOperation(L"help");
        std::wcout << help;
        return 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--stone-isolated-worker") {
        stone::windows::SetProcessLogOperation(L"isolated-worker");
        std::cerr << "Stone: ART isolated renderer workers are not yet supported\n";
        return 2;
    }
    bool apk = false, package = false, data = false, image = false, jit = false, management = false, target = false,
         background = false;
    bool prepared = false, origin = false, expected = false, stamp = false, wake = false, aot = false, process = false;
    bool confirmationPipe = false, confirmationParent = false;
    std::wstring operation;
    std::wstring logPackage;
    std::vector<std::wstring> arguments;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view flag(argv[i]);
        if (flag == L"--background" && !background) {
            background = true;
            continue;
        }
        if (flag == L"--enable-jit" && !jit) {
            jit = true;
            continue;
        }
        if (flag == L"--enable-aot" && !aot) {
            aot = true;
            continue;
        }
        bool* supplied = flag == L"--apk"                                              ? &apk
                         : flag == L"--package"                                        ? &package
                         : flag == L"--data-dir"                                       ? &data
                         : (flag == L"--system-image" || flag == L"--framework-image") ? &image
                         : flag == L"--prepared-system"                                ? &prepared
                         : flag == L"--origin-apk"                                     ? &origin
                         : flag == L"--expected-package"                               ? &expected
                         : flag == L"--image-stamp"                                    ? &stamp
                         : flag == L"--wake-token"                                     ? &wake
                         : flag == L"--application-process"                            ? &process
                         : flag == L"--confirmation-host-pipe"                         ? &confirmationPipe
                         : flag == L"--confirmation-host-parent"                       ? &confirmationParent
                         : flag == L"--manage-package"                                 ? &management
                         : flag == L"--target"                                         ? &target
                                                                                       : nullptr;
        if (!supplied || *supplied || i + 1 >= argc || !*argv[i + 1] ||
            std::wstring_view(argv[i + 1]).starts_with(L"--")) {
            std::wcerr << help;
            return 2;
        }
        *supplied = true;
        arguments.emplace_back(flag == L"--framework-image" ? L"--system-image" : flag);
        const auto value = argv[++i];
        if (flag == L"--package")
            logPackage = value;
        if (flag == L"--manage-package")
            operation = value;
        arguments.push_back(flag == L"--manage-package" || flag == L"--target" || flag == L"--package" ||
                                    flag == L"--expected-package" || flag == L"--image-stamp" ||
                                    flag == L"--wake-token" || flag == L"--application-process"
                                ? std::wstring(value)
                            : flag == L"--confirmation-host-pipe" || flag == L"--confirmation-host-parent"
                                ? std::wstring(value)
                                : fs::absolute(value).lexically_normal().wstring());
    }
    const bool validOperation = operation == L"list" || operation == L"install" || operation == L"uninstall";
    const bool validTarget = operation == L"list" ? !target : target;
    if (static_cast<int>(apk) + static_cast<int>(package) + static_cast<int>(management) != 1 ||
        (!management && target) ||
        ((prepared || origin || expected || stamp || wake) && (!background || !image || management)) ||
        (management && (jit || aot || background || !validOperation || !validTarget))) {
        std::wcerr << help;
        return 2;
    }
    if (process && (!background || !prepared || !expected || !stamp || !wake)) {
        std::wcerr << help;
        return 2;
    }
    if (confirmationPipe != confirmationParent || (confirmationPipe && (!management || operation != L"install"))) {
        std::wcerr << help;
        return 2;
    }
    if (!stone::windows::SetProcessLogOperation(management ? L"package-" + operation
                                                : apk      ? L"launch-apk"
                                                           : L"launch-package"))
        std::cerr << "Stone: could not name the process operation log\n";
    if (!logPackage.empty() && !stone::windows::SetProcessLogPackage(logPackage))
        std::cerr << "Stone: could not attach the package name to the process log\n";
#ifdef _M_X64
    if (management)
        return stone::windows::LaunchArtPackageManagement(arguments, directory);
    if (!background) {
        std::wstring label;
        for (size_t i = 0; i + 1 < arguments.size(); i += 2) {
            if (arguments[i] == L"--package")
                label = arguments[i + 1];
            else if (arguments[i] == L"--apk")
                label = fs::path(arguments[i + 1]).stem().wstring();
        }
        stone::windows::StartLaunchScreen(label);
    }
    return stone::windows::LaunchArtApplication(arguments, directory, jit, background, aot);
#else
    std::cerr
        << "Stone: APK execution and package management require the Windows x64 ART backend. Windows ARM64 ART execution is not implemented.\n";
    return 1;
#endif
}
}

int wmain(int argc, wchar_t* argv[])
{
    try {
        const auto directory = ExecutableDirectory();
        if (!stone::windows::StartProcessLog(L"startup"))
            std::cerr << "Stone: could not create a process log in the user Logs directory\n";
        return Launch(argc, argv, directory);
    }
    catch (const std::exception& error) {
        std::cerr << "Stone: " << error.what() << '\n';
        stone::windows::FailLaunchScreen(error.what());
        return 1;
    }
}
