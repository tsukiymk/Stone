#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace stone::windows {
struct RuntimeFeatures {
    bool aot = false, jit = false;

    static RuntimeFeatures Load(const std::filesystem::path& dataRoot)
    {
        const auto path = dataRoot / L"runtime-features.properties";
        if (!std::filesystem::exists(path))
            return {};
        if (std::filesystem::file_size(path) > 256)
            throw std::runtime_error("Runtime feature settings exceed their size limit");
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("Could not read runtime feature settings");
        RuntimeFeatures result;
        bool format = false, aot = false, jit = false, executor = false;
        for (std::string line; std::getline(input, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line == "format=1" && !format)
                format = true;
            else if ((line == "aot=true" || line == "aot=false") && !aot) {
                result.aot = line == "aot=true";
                aot = true;
            }
            else if ((line == "jit=true" || line == "jit=false") && !jit) {
                result.jit = line == "jit=true";
                jit = true;
            }
            else if (line == "executor=art" && !executor) {
                executor = true;
            }
            else if (line == "executor=java-runtime")
                throw std::runtime_error(
                    "The experimental Java Runtime has been removed; remove executor=java-runtime from runtime-features.properties to use ART");
            else
                throw std::runtime_error("Invalid runtime feature settings");
        }
        if (!input.eof() || !format || !aot || !jit)
            throw std::runtime_error("Incomplete runtime feature settings");
        return result;
    }
};
}
