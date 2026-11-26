#pragma once

#include <cstdint>
#include <span>
#include <string_view>

struct sqlite3_vfs;

namespace stone::windows {
void LoadImageSqlite(std::span<const uint8_t> bytes);
void* SqliteExport(std::string_view name, std::string_view signature);
sqlite3_vfs* WindowsSqliteVfs(sqlite3_vfs* storage = nullptr, bool useAndroidPaths = false);
}
