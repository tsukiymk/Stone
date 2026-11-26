#pragma once

#include <cstdint>
#include <string_view>

namespace stone::windows {
uintptr_t AndroidMediaJniImport(std::string_view name);
bool ArtMediaJniAvailable();
}
