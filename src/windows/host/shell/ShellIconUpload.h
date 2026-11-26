#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace stone::windows {
// One bounded, ordered upload per authenticated Client process. No filesystem
// paths come from the sender; the Service chooses and publishes the cache file.
class ShellIconUpload {
    size_t expected = 0;
    std::vector<uint8_t> bytes;

public:
    std::wstring package, label, root;

    void Begin(size_t size)
    {
        if (!size || size > 1024 * 1024)
            throw std::invalid_argument("Invalid application icon size");
        expected = size;
        bytes.clear();
        bytes.reserve(size);
    }

    void Append(size_t offset, const std::wstring& hex)
    {
        if (!expected || offset != bytes.size() || hex.empty() || hex.size() > 16000 || hex.size() % 2 ||
            hex.size() / 2 > expected - bytes.size())
            throw std::invalid_argument("Invalid application icon chunk");
        auto digit = [](wchar_t value) -> uint8_t {
            if (value >= L'0' && value <= L'9')
                return static_cast<uint8_t>(value - L'0');
            if (value >= L'a' && value <= L'f')
                return static_cast<uint8_t>(value - L'a' + 10);
            throw std::invalid_argument("Invalid application icon encoding");
        };
        std::vector<uint8_t> chunk;
        chunk.reserve(hex.size() / 2);
        for (size_t index = 0; index < hex.size(); index += 2)
            chunk.push_back((digit(hex[index]) << 4) | digit(hex[index + 1]));
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    }

    std::vector<uint8_t> Complete()
    {
        if (!expected || bytes.size() != expected)
            throw std::invalid_argument("Incomplete application icon");
        expected = 0;
        return std::move(bytes);
    }
};
}
