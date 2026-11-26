#include "ClientSignals.h"
#include <sddl.h>
#include <memory>

namespace stone::windows {
ClientSignals::ClientSignals(const std::filesystem::path& directory, DWORD process)
{
    ipc::Identity identity(directory);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ipc::Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(identity.security.c_str(), SDDL_REVISION_1,
                                                                    &descriptor, nullptr),
               "Cannot secure Client controls");
    std::unique_ptr<void, decltype(&LocalFree)> security(descriptor, LocalFree);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const auto prefix = identity.mutex + L".Client." + std::to_wstring(process);
    activate_.value = CreateEventW(&attributes, TRUE, FALSE, (prefix + L".Activate").c_str());
    stop_.value = CreateEventW(&attributes, TRUE, FALSE, (prefix + L".Stop").c_str());
    ipc::Check(activate_.value && stop_.value, "Cannot create Client controls");
}

void ClientSignals::Activate()
{
    ipc::Check(SetEvent(activate_.value), "Cannot activate Client");
}

void ClientSignals::Stop()
{
    ipc::Check(SetEvent(stop_.value), "Cannot stop Client");
}

bool ClientSignals::ActivationPending() const
{
    return WaitForSingleObject(activate_.value, 0) == WAIT_OBJECT_0;
}

int ClientSignals::Consume()
{
    if (WaitForSingleObject(stop_.value, 0) == WAIT_OBJECT_0)
        return 2;
    if (!ActivationPending())
        return 0;
    ipc::Check(ResetEvent(activate_.value), "Cannot acknowledge Client activation");
    return 1;
}
}
