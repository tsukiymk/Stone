#include "LibraryJniStubs.h"

namespace stone::windows {

unsigned MmkvJniPathArguments(std::string_view name, std::string_view signature)
{
    if (name == "jniInitialize" && signature == "(Ljava/lang/String;Ljava/lang/String;IZZJ)V") {
        return 0b11; // Root directory and cache directory.
    }
    if (name == "getMMKVWithID" && signature == "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;JZIIZII)J") {
        return 1u << 3; // Root directory only.
    }
    return 0;
}

}
