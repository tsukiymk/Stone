#include "abi/AndroidLibraryImports.h"
#include "abi/TypedAndroidImport.h"
#include "stub-libc.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>

namespace stone::windows {
int AndroidEncodeBase64(const unsigned char* input, size_t size, char* output, size_t capacity)
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (size > (SIZE_MAX - 2) / 3 || ((size + 2) / 3) > (SIZE_MAX - 1) / 4) {
        errno = ENOSPC;
        return -1;
    }
    const auto needed = ((size + 2) / 3) * 4;
    if (!output || needed >= capacity || needed > INT_MAX) {
        errno = ENOSPC;
        return -1;
    }
    size_t at = 0;
    for (size_t index = 0; index < size; index += 3) {
        const uint32_t bits = (uint32_t{input[index]} << 16) |
                              (index + 1 < size ? uint32_t{input[index + 1]} << 8 : 0) |
                              (index + 2 < size ? input[index + 2] : 0);
        output[at++] = alphabet[bits >> 18];
        output[at++] = alphabet[(bits >> 12) & 63];
        output[at++] = index + 1 < size ? alphabet[(bits >> 6) & 63] : '=';
        output[at++] = index + 2 < size ? alphabet[bits & 63] : '=';
    }
    output[at] = 0;
    return static_cast<int>(needed);
}

int AndroidMakeDnsQuery(int operation, const char* name, int family, int type, const void*, int, const void*,
                        unsigned char* output, int capacity)
{
    if (operation != 0) {
        errno = 95;
        return -1;
    } // Android ENOTSUP; standard DNS QUERY only.
    if (!name || !output || family < 0 || family > 65535 || type < 0 || type > 65535) {
        errno = EINVAL;
        return -1;
    }
    std::array<unsigned char, 272> packet{}; // Header + maximum DNS name + QTYPE/QCLASS.
    if (BCryptGenRandom(nullptr, packet.data(), 2, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        errno = EIO;
        return -1;
    }
    packet[2] = 1; // Recursion desired.
    packet[5] = 1; // One question.
    size_t at = 12;
    if (std::strcmp(name, ".") == 0)
        ++name;
    while (*name) {
        const auto lengthAt = at++;
        size_t length = 0;
        while (*name && *name != '.') {
            unsigned value = static_cast<unsigned char>(*name++);
            if (value == '\\') {
                if (!*name) {
                    errno = EINVAL;
                    return -1;
                }
                if (name[0] >= '0' && name[0] <= '9' && name[1] >= '0' && name[1] <= '9' && name[2] >= '0' &&
                    name[2] <= '9') {
                    value = (name[0] - '0') * 100 + (name[1] - '0') * 10 + name[2] - '0';
                    name += 3;
                    if (value > 255) {
                        errno = EINVAL;
                        return -1;
                    }
                }
                else
                    value = static_cast<unsigned char>(*name++);
            }
            if (++length > 63 || at >= 267) {
                errno = 90;
                return -1;
            } // Android EMSGSIZE.
            packet[at++] = static_cast<unsigned char>(value);
        }
        if (!length) {
            errno = EINVAL;
            return -1;
        }
        packet[lengthAt] = static_cast<unsigned char>(length);
        if (*name == '.')
            ++name;
    }
    if (at - 12 >= 255) {
        errno = 90;
        return -1;
    }
    packet[at++] = 0;
    packet[at++] = static_cast<unsigned char>(type >> 8);
    packet[at++] = static_cast<unsigned char>(type);
    packet[at++] = static_cast<unsigned char>(family >> 8);
    packet[at++] = static_cast<unsigned char>(family);
    if (capacity < 0 || at > static_cast<size_t>(capacity)) {
        errno = ENOSPC;
        return -1;
    }
    std::memcpy(output, packet.data(), at);
    return static_cast<int>(at);
}

uintptr_t AndroidResolverImport(std::string_view name)
{
    if (name == "__b64_ntop")
        return Import(name, &AndroidEncodeBase64);
    if (name == "res_mkquery")
        return ImportAndroidScalars<decltype(&AndroidMakeDnsQuery)>(name,
                                                                    reinterpret_cast<uintptr_t>(&AndroidMakeDnsQuery));
    return 0;
}
}
