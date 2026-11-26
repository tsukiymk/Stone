#include "PowerRequests.h"
#include "../ipc/ServiceProtocol.h"
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace stone::windows {
namespace {
using ipc::Check;

struct Request {
    ipc::Handle client, power;

    explicit Request(DWORD pid) : client(OpenProcess(SYNCHRONIZE, FALSE, pid))
    {
        Check(client.value != nullptr, "Cannot track wake lock Client");
        REASON_CONTEXT reason{};
        reason.Version = POWER_REQUEST_CONTEXT_VERSION;
        reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        wchar_t text[] = L"Stone Android application holds a partial wake lock";
        reason.Reason.SimpleReasonString = text;
        power.value = PowerCreateRequest(&reason);
        if (power.value == INVALID_HANDLE_VALUE)
            power.value = nullptr;
        Check(power.value != nullptr, "Cannot create Windows power request");
        Check(PowerSetRequest(power.value, PowerRequestSystemRequired), "Cannot hold Windows system power request");
    }

    ~Request()
    {
        if (power.value)
            PowerClearRequest(power.value, PowerRequestSystemRequired);
    }
};

struct State {
    std::mutex mutex;
    std::map<DWORD, std::unique_ptr<Request>> requests;
    std::once_flag monitor;
    bool stopping = false;
};

State* state = new State;

void Reap()
{
    for (auto it = state->requests.begin(); it != state->requests.end();) {
        if (WaitForSingleObject(it->second->client.value, 0) == WAIT_OBJECT_0)
            it = state->requests.erase(it);
        else
            ++it;
    }
}
}

void SetServiceSystemAwake(DWORD client, bool required)
{
    std::call_once(state->monitor, [] {
        std::thread([] {
            for (;;) {
                Sleep(250);
                const std::lock_guard lock(state->mutex);
                if (state->stopping)
                    return;
                Reap();
            }
        }).detach();
    });
    const std::lock_guard lock(state->mutex);
    Check(!state->stopping, "Service power manager is stopping");
    Reap();
    if (!required)
        state->requests.erase(client);
    else if (!state->requests.contains(client))
        state->requests.emplace(client, std::make_unique<Request>(client));
}

uint32_t ServiceSystemAwakeCount()
{
    const std::lock_guard lock(state->mutex);
    Reap();
    return static_cast<uint32_t>(state->requests.size());
}

void StopServicePowerRequests()
{
    const std::lock_guard lock(state->mutex);
    state->stopping = true;
    state->requests.clear();
}
}
