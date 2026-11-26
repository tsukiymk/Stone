#include "ArtRuntime.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t* argv[])
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        std::cout
            << "Usage: StoneArtCompiler --system-directory <prepared-image> --data-dir <directory> -- <dex2oat-options>\n"
               "Experimental x64/translated ARM64 image dex2oat host. Uses the selected image compiler without a Windows JVM.\n";
        return 0;
    }
    if (argc < 7 || std::wstring_view(argv[1]) != L"--system-directory" ||
        std::wstring_view(argv[3]) != L"--data-dir" || std::wstring_view(argv[5]) != L"--") {
        std::cerr << "Invalid compiler arguments; use --help\n";
        return 2;
    }
    try {
        std::vector<std::string> arguments;
        for (int i = 6; i < argc; ++i) {
            const int size =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (!size)
                throw std::runtime_error("Invalid Unicode compiler argument");
            std::string value(size, '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), size, nullptr, nullptr))
                throw std::runtime_error("Could not encode compiler argument");
            value.pop_back();
            arguments.push_back(std::move(value));
        }
        return stone::windows::RunArtDex2Oat(argv[2], argv[4], std::move(arguments));
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
