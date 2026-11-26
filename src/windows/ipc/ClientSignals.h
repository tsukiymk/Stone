#pragma once

#include "ServiceProtocol.h"

namespace stone::windows {
// Same-user/session/installation control; no Java object or frame crosses IPC.
class ClientSignals {
    ipc::Handle activate_, stop_;

public:
    ClientSignals(const std::filesystem::path& directory, DWORD process);
    void Activate();
    void Stop();
    bool ActivationPending() const;
    int Consume(); // Nonblocking, on the ART main thread: activate=1, stop=2.
};
}
