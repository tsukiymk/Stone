#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace stone::windows {
using AndroidSystemPropertyTable = std::map<std::string, std::string, std::less<>>;
void SetAndroidSystemProperties(AndroidSystemPropertyTable values);
std::string_view AndroidSystemProperty(std::string_view name);

struct AndroidSystemPropertyValue {
    std::string_view name, value;
    uint32_t serial;
};

// Handles refer to the immutable process snapshot. Validate them before use;
// translated callers must never dereference an arbitrary host map entry.
uintptr_t FindAndroidSystemProperty(std::string_view name);
bool ReadAndroidSystemProperty(uintptr_t handle, AndroidSystemPropertyValue& value);
uint32_t AndroidSystemPropertyAreaSerial();
}
