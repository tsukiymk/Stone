#pragma once

#include <cstdint>

namespace stone::windows {
// Bionic LP64 layouts shared by the native and translated user database APIs.
struct AndroidPasswd {
    char* name;
    char* password;
    uint32_t uid, gid;
    char* gecos;
    char* directory;
    char* shell;
};

static_assert(sizeof(AndroidPasswd) == 48);

struct AndroidGroup {
    char* name;
    char* password;
    uint32_t gid;
    char** members;
};

static_assert(sizeof(AndroidGroup) == 32);
}
