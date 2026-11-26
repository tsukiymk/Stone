#include "test-suite.h"

namespace stone::windows::test {
void Require(bool condition, std::string_view expected, std::source_location location)
{
    if (!condition) {
        throw std::runtime_error(std::string(location.file_name()) + ':' + std::to_string(location.line()) + ": " +
                                 std::string(expected));
    }
}
}
