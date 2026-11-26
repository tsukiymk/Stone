#include "abi/Arm64JniSignature.h"
#include <stdexcept>

namespace stone::windows {
char ReadArm64JniType(std::string_view signature, size_t& offset, bool allowVoid)
{
    const auto start = offset;
    while (offset < signature.size() && signature[offset] == '[')
        ++offset;
    if (offset == signature.size())
        throw std::runtime_error("truncated ARM64 JNI signature");
    const bool array = offset != start;
    const char type = signature[offset++];
    if (type == 'L') {
        const auto end = signature.find(';', offset);
        if (end == signature.npos || end == offset)
            throw std::runtime_error("invalid ARM64 JNI reference type");
        offset = end + 1;
    }
    else if (type == 'V' && allowVoid && !array) {
        return 'V';
    }
    else if (std::string_view("ZBCSIJFD").find(type) == std::string_view::npos) {
        throw std::runtime_error("invalid ARM64 JNI scalar type");
    }
    return array ? 'L' : type;
}

}
