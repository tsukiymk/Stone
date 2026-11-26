#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace stone::windows {
// Only the isolated probe supplies diagnostic arguments. Production uses the
// sibling executable with no decision or automation arguments.
void ConfigureArtSettings(const std::filesystem::path& executable, std::wstring arguments = {},
                          uint32_t timeoutMs = 300000);
// Management-only: reuse the installer window owned by the actual settings parent.
void ConfigureArtSettingsParent(const std::filesystem::path& executable, const std::wstring& pipe, uint32_t parent);
bool ArtSettingsEnabled();
uint64_t CreateArtSettingsConfirmation(std::string json);
bool AwaitArtSettingsConfirmation(uint64_t request);
void CancelArtSettingsConfirmation(uint64_t request);
}
