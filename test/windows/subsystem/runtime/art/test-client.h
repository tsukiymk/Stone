#pragma once

#include <optional>

namespace stone::windows::test {
// Test executables may be staged as StoneClient.exe. Peer APKs and package
// management must still use the production launch path without the probe DEX.
std::optional<int> RunArtTestClient(int argc, wchar_t* argv[]);
void ConfigureArtTestService();
}
