#include "LibraryJniStubs.h"

namespace stone::windows {

CsoJniPaths CsoJniPathArguments(std::string_view name, std::string_view signature)
{
    if (name == "nativeInitialize" &&
        signature ==
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;Z)V") {
        return {0b1111, (1u << 4) | (1u << 6) | (1u << 7)};
    }
    return {};
}

}
