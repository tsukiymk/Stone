#include "../../../src/windows/host/RuntimeFeatures.h"
#include <iostream>

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc != 4)
            throw std::runtime_error("Expected data root, AOT and JIT values");
        const auto features = stone::windows::RuntimeFeatures::Load(argv[1]);
        if (features.aot != (std::wstring_view(argv[2]) == L"true") ||
            features.jit != (std::wstring_view(argv[3]) == L"true"))
            throw std::runtime_error("Native launch preferences differ from UI settings");
        std::cout << "PASS: native runtime feature preferences\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
