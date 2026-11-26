#include "Image.h"
#include <iostream>
#include <sstream>

int wmain(int argc, wchar_t** argv)
{
    using namespace stone::image;
    try {
        if (argc == 6 && std::wstring_view(argv[1]) == L"--extract-file") {
            std::istringstream parts(Utf8(argv[3]));
            for (std::string part; std::getline(parts, part, '/');)
                if (!part.empty())
                    Require(SafeName(part), "Invalid image file path");
            auto name = Utf8(argv[4]);
            Require(SafeName(name), "Invalid image filename");
            auto partition = OpenPartition(OpenSystem(OpenFile(argv[2])));
            Atomic(argv[5], [&](auto& stream) {
                Require(partition->Copy(Utf8(argv[3]), name, stream), "Missing image file: " + name);
            });
            return 0;
        }
        fs::path result;
        int first = 1;
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--output-file") {
            result = argv[2];
            first = 3;
        }
        Require(argc >= first + 2,
                "Usage: StoneImageTool [--output-file <index>] <system.img> <cache-directory> [extra-library.so ...]");
        std::vector<std::string> extras;
        for (int i = first + 2; i < argc; i++)
            extras.push_back(Utf8(argv[i]));
        auto output = Utf8(Prepare(argv[first], argv[first + 1], extras));
        if (result.empty())
            std::cout << output << '\n';
        else
            Atomic(result, [&](auto& stream) {
                stream << output;
            });
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Stone image preparation failed: " << error.what() << '\n';
        return 1;
    }
}
