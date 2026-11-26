#include "HostServices.h"
#include "PowerRequests.h"
#include <windows.h>
#include <shellapi.h>
#include <netlistmgr.h>
#include <wrl/client.h>
#include <mutex>
#include <thread>
#include <vector>
#include <map>

namespace stone::windows {
namespace {
struct Disk {
    bool known = false;
    uint64_t free = 0, total = 0;
};

struct State {
    std::mutex mutex;
    HostSnapshot snapshot;
    HostJobState jobs;
    std::map<std::filesystem::path, Disk> disks;
};

// Service-process lifetime. Shutdown must not join a possibly stalled NLM RPC.
State* state = new State;
std::once_flag started;

bool Dark()
{
    DWORD value = 1, size = sizeof(value);
    constexpr auto key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) !=
        ERROR_SUCCESS) {
        size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) !=
            ERROR_SUCCESS)
            value = 1;
    }
    return value == 0;
}

HostUiConfiguration Ui()
{
    HostUiConfiguration result{{}, Dark()};
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size > 1 && size <= 32768) {
        std::vector<wchar_t> names(size);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.data(), &size)) {
            size_t at = 0;
            while (at < names.size() && names[at]) {
                const auto start = at;
                while (at < names.size() && names[at])
                    ++at;
                if (at == names.size())
                    break;
                if (!result.languageTags.empty())
                    result.languageTags += L',';
                result.languageTags.append(names.data() + start, at - start);
                ++at;
            }
        }
    }
    if (result.languageTags.empty()) {
        wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
        result.languageTags = GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) ? name : L"en-US";
    }
    return result;
}

void PowerAndUi()
{
    MEMORYSTATUSEX memory{sizeof(memory)};
    const bool memoryValid = GlobalMemoryStatusEx(&memory) != FALSE;
    ULONGLONG installed = 0;
    if (!GetPhysicallyInstalledSystemMemory(&installed))
        installed = 0;
    SYSTEM_POWER_STATUS power{};
    bool valid = GetSystemPowerStatus(&power) != FALSE;
    auto ui = Ui();
    LASTINPUTINFO input{sizeof(input)};
    QUERY_USER_NOTIFICATION_STATE presence{};
    const bool absent =
        GetLastInputInfo(&input) && SUCCEEDED(SHQueryUserNotificationState(&presence)) && presence == QUNS_NOT_PRESENT;
    const auto elapsed = static_cast<DWORD>(GetTickCount() - input.dwTime);
    const uint64_t idle = absent && elapsed <= INT_MAX ? elapsed : 0;
    const std::lock_guard lock(state->mutex);
    state->snapshot.ui = std::move(ui);
    if (memoryValid)
        state->snapshot.memory = {memory.ullAvailPhys, memory.ullTotalPhys, memory.ullTotalPhys / 16,
                                  installed ? installed * 1024 : memory.ullTotalPhys};
    if (valid)
        state->snapshot.battery = {power.ACLineStatus, power.BatteryFlag, power.BatteryLifePercent,
                                   power.SystemStatusFlag};
    ++state->snapshot.powerSamples;
    state->jobs.userAbsent = absent;
    state->jobs.idleMillis = idle;
}

int Network()
{
    Microsoft::WRL::ComPtr<INetworkListManager> manager;
    NLM_CONNECTIVITY connectivity{};
    if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager))) ||
        FAILED(manager->GetConnectivity(&connectivity)))
        return 0;
    constexpr auto internet = NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET;
    constexpr auto connected = internet | NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV6_SUBNET |
                               NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK;
    if (!(connectivity & connected))
        return 0;
    Microsoft::WRL::ComPtr<INetworkCostManager> costManager;
    DWORD cost = NLM_CONNECTION_COST_UNKNOWN;
    bool unmetered = SUCCEEDED(manager.As(&costManager)) && SUCCEEDED(costManager->GetCost(&cost, nullptr)) &&
                     cost == NLM_CONNECTION_COST_UNRESTRICTED;
    return 1 | ((connectivity & internet) ? 2 : 0) | (unmetered ? 4 : 0);
}
}

void StartHostServices()
{
    std::call_once(started, [] {
        state->snapshot.owner = GetCurrentProcessId();
        PowerAndUi();
        std::thread([] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;) {
                Sleep(1000);
                try {
                    PowerAndUi();
                }
                catch (...) {
                }
            }
        }).detach();
        std::thread([] {
            const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(apartment))
                return;
            for (;;) {
                int network = 0;
                try {
                    network = Network();
                }
                catch (...) {
                }
                {
                    const std::lock_guard lock(state->mutex);
                    state->snapshot.network = network;
                    ++state->snapshot.networkSamples;
                }
                Sleep(1000);
            }
        }).detach();
        std::thread([] {
            uint64_t samples = 0;
            for (;;) {
                HostWifiState wifi;
                try {
                    wifi = ReadWindowsWifi();
                }
                catch (...) {
                }
                wifi.samples = ++samples;
                {
                    const std::lock_guard lock(state->mutex);
                    state->snapshot.wifi = wifi;
                }
                Sleep(1000);
            }
        }).detach();
        std::thread([] {
            uint64_t samples = 0;
            for (;;) {
                auto interfaces = std::make_shared<HostInterfaces>();
                interfaces->error = 5;
                try {
                    *interfaces = ReadWindowsInterfaces();
                }
                catch (...) {
                }
                interfaces->samples = ++samples;
                {
                    const std::lock_guard lock(state->mutex);
                    state->snapshot.interfaces = std::move(interfaces);
                }
                Sleep(1000);
            }
        }).detach();
        std::thread([] {
            for (;;) {
                std::vector<std::filesystem::path> paths;
                {
                    const std::lock_guard lock(state->mutex);
                    for (const auto& [path, disk] : state->disks)
                        paths.push_back(path);
                }
                for (const auto& path : paths) {
                    ULARGE_INTEGER free{}, total{};
                    bool known = GetDiskFreeSpaceExW(path.c_str(), &free, &total, nullptr) != FALSE;
                    const std::lock_guard lock(state->mutex);
                    state->disks[path] = {known, free.QuadPart, total.QuadPart};
                }
                Sleep(1000);
            }
        }).detach();
    });
}

HostSnapshot ReadServiceHostState()
{
    StartHostServices();
    HostSnapshot result;
    {
        const std::lock_guard lock(state->mutex);
        result = state->snapshot;
    }
    result.systemPowerRequests = ServiceSystemAwakeCount();
    return result;
}

HostJobState ReadServiceJobState(const std::filesystem::path& data)
{
    StartHostServices();
    const std::lock_guard lock(state->mutex);
    auto& disk = state->disks[data];
    auto result = state->jobs;
    result.storageKnown = disk.known;
    result.storageFree = disk.free;
    result.storageTotal = disk.total;
    return result;
}
}
