#pragma once

#include <cstddef>
#include <cstdint>

namespace stone::windows {
struct AndroidIfaddrs {
    AndroidIfaddrs* next;
    char* name;
    unsigned flags;
    void* address;
    void* netmask;
    void* broadcast;
    void* data;
};

static_assert(sizeof(AndroidIfaddrs) == 56 && offsetof(AndroidIfaddrs, address) == 24);
int AndroidGetIfAddrs(AndroidIfaddrs** output);
void AndroidFreeIfAddrs(AndroidIfaddrs* head);
char* AndroidInterfaceName(unsigned index, char* name);
unsigned AndroidInterfaceIndex(const char* name);
bool AndroidInterfaceIoctl(unsigned long request, void* argument, int& result);
}
