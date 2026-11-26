#include "BrokerClient.h"
#include "ClientApplicationLogs.h"
#include "../ipc/ServiceProtocol.h"
#include "../ipc/ClientSignals.h"
#include "../ipc/LaunchProfile.h"
#include "../host/CommandLine.h"
#include <cstdio>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <thread>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;
using namespace ipc;

struct Cache {
    std::mutex mutex;
    std::mutex awakeMutex;
    std::once_flag initialized;
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle stopped{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    fs::path directory, data, image;
    HostSnapshot snapshot;
    HostChangedCallback changed = nullptr;
    bool ready = false, shell = false, jobs = false;
    bool awake = false;
    uint32_t awakeOwner = 0;
    Handle awakeService;
    std::unique_ptr<ClientSignals> session;
    uint32_t registeredOwner = 0;
    std::wstring package;
    std::vector<std::wstring> launch;
    std::wstring expectedPackage;
    std::wstring applicationProcess;
    bool launchBound = false;
};

// Process lifetime: APK exit uses ExitProcess and must never join an IPC wait.
Cache* cache = new Cache;

HostSnapshot Fetch(const fs::path& directory)
{
    Response response{};
    std::wstring output;
    RequestWithStart(directory, Operation::HostState, L"memory-v1", response, output, true);
    if (!output.starts_with(L"v3|"))
        RequestWithStart(directory, Operation::HostState, L"wifi-v1", response, output, true);
    Check(response.result == 0, "Service host state is unavailable");
    auto snapshot = DecodeHostState(output);
    Check(snapshot.owner == response.process, "Host state provider identity mismatch");
    response = {};
    output.clear();
    RequestWithStart(directory, Operation::HostState, L"network-v1", response, output, true);
    Check(response.result == 0 && response.process == snapshot.owner, "Interface snapshot provider identity mismatch");
    snapshot.interfaces = DecodeHostInterfaces(output, snapshot.owner);
    return snapshot;
}

void Publish(HostSnapshot snapshot)
{
    HostChangedCallback changed = nullptr;
    {
        const std::lock_guard lock(cache->mutex);
        if (snapshot.ui.dark != cache->snapshot.ui.dark || snapshot.ui.languageTags != cache->snapshot.ui.languageTags)
            changed = cache->changed;
        cache->snapshot = std::move(snapshot);
        cache->ready = true;
    }
    if (changed)
        changed();
}

fs::path Directory()
{
    const std::lock_guard lock(cache->mutex);
    return cache->directory;
}

uint32_t SendSystemAwake(bool required)
{
    Response response{};
    std::wstring output;
    RequestWithStart(Directory(), Operation::SystemAwake, required ? L"1" : L"0", response, output, true);
    Check(response.result == 0 && response.process != 0, "Service could not update Windows power request");
    return response.process;
}

void RememberAwakeService(uint32_t owner)
{
    Handle process(OpenProcess(SYNCHRONIZE, FALSE, owner));
    Check(process.value != nullptr && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT,
          "Wake request Service has exited");
    // Keep the process object alive so a reused numeric PID cannot hide a restart.
    if (cache->awakeService.value)
        CloseHandle(cache->awakeService.value);
    cache->awakeService.value = process.value;
    process.value = nullptr;
    cache->awakeOwner = owner;
}

void RestoreSystemAwake(uint32_t owner)
{
    const std::lock_guard lock(cache->awakeMutex);
    if (cache->awake && (cache->awakeOwner != owner || !cache->awakeService.value ||
                         WaitForSingleObject(cache->awakeService.value, 0) != WAIT_TIMEOUT))
        RememberAwakeService(SendSystemAwake(true));
}

void ShellRequest(Operation operation, const std::vector<std::wstring>& fields)
{
    auto directory = Directory();
    if (ProcessIs(GetCurrentProcessId(), directory / L"StoneClient.exe")) {
        Response response{};
        std::wstring output;
        RequestWithStart(directory, operation, Pack(fields), response, output, true);
        Check(response.result == 0, "Service could not update Windows shell integration");
        return;
    }
    // Standalone diagnostic tools use the ordinary service CLI. They are not
    // granted the production Client's mutating IPC authority.
    auto executable = directory / L"StoneService.exe";
    auto command = stone::session::Quote(executable.wstring()) +
                   (operation == Operation::ShellProfile    ? L" --shell-profile"
                    : operation == Operation::ShellShortcut ? L" --shell-shortcut"
                                                            : L" --notification-identity");
    for (const auto& field : fields)
        command += L" " + stone::session::Quote(field);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         directory.c_str(), &startup, &process),
          "Cannot start shell service request");
    CloseHandle(process.hThread);
    Handle child(process.hProcess);
    DWORD result = 1;
    if (WaitForSingleObject(child.value, 15000) != WAIT_OBJECT_0) {
        TerminateProcess(child.value, ERROR_TIMEOUT);
        WaitForSingleObject(child.value, 1000);
        throw std::runtime_error("Shell service request timed out");
    }
    Check(GetExitCodeProcess(child.value, &result) && result == 0, "Shell service request failed");
}

void Starting(const std::vector<std::wstring>& fields)
{
    Response response{};
    std::wstring output;
    RequestWithStart(Directory(), Operation::ClientStarting, Pack(fields), response, output, true);
    Check(response.result == 0, "Service could not register starting Client");
}

void RestoreClientSession(uint32_t owner)
{
    fs::path data, image;
    std::wstring package;
    std::vector<std::wstring> launch;
    {
        const std::lock_guard lock(cache->mutex);
        if (!cache->session || !cache->shell || cache->registeredOwner == owner)
            return;
        data = cache->data;
        image = cache->image;
        package = cache->package;
        if (cache->launchBound)
            launch = cache->launch;
        if (!launch.empty() && !cache->applicationProcess.empty())
            launch.push_back(cache->applicationProcess);
    }
    if (!launch.empty()) {
        Starting({data.wstring(), launch[4], package, launch.size() == 9 ? launch[8] : L""});
        Response response{};
        std::wstring output;
        RequestWithStart(Directory(), Operation::ClientProfile, Pack(launch), response, output, true);
        Check(response.result == 0, "Cannot restore Client launch profile");
    }
    else
        ShellRequest(Operation::ShellProfile, {data.wstring(), image.wstring()});
    if (!package.empty()) {
        Response response{};
        std::wstring output;
        RequestWithStart(Directory(), Operation::Register, package, response, output, true);
        Check(response.result == 0, "Cannot restore Client package registration");
    }
    const std::lock_guard lock(cache->mutex);
    cache->registeredOwner = owner;
}
}

void ConfigureClientService(const fs::path& executable)
{
    const std::lock_guard lock(cache->mutex);
    auto directory = fs::canonical(executable).parent_path();
    if (cache->ready && directory != cache->directory)
        throw std::runtime_error("Client service cannot change after initialization");
    cache->directory = std::move(directory);
}

void InitializeClientSession()
{
    auto directory = Directory();
    auto session = std::make_unique<ClientSignals>(directory, GetCurrentProcessId());
    const std::lock_guard lock(cache->mutex);
    Check(!cache->session, "Client session is already initialized");
    cache->session = std::move(session);
    cache->registeredOwner = cache->snapshot.owner;
}

int ReadClientSessionCommands()
{
    const std::lock_guard lock(cache->mutex);
    return cache->session ? cache->session->Consume() : 0;
}

bool RetireClientSession()
{
    Response response{};
    std::wstring output;
    RequestWithStart(Directory(), Operation::RetireClient, L"", response, output, true);
    Check(response.result == 0 && (output == L"0" || output == L"1"), "Service could not retire Client");
    return output == L"1";
}

void ConfigureClientLaunchProfile(const fs::path& prepared, const fs::path& apk, const fs::path& origin,
                                  const std::wstring& expectedPackage, const std::wstring& imageStamp,
                                  const std::wstring& token, const std::wstring& process)
{
    std::vector<std::wstring> startup;
    {
        const std::lock_guard lock(cache->mutex);
        Check(cache->shell && cache->session && cache->launch.empty(),
              "Client launch profile is already configured or unavailable");
        Check(expectedPackage.empty() || stone::session::IsPackageName(expectedPackage),
              "Invalid expected application package");
        auto stamp = JobImageStamp(cache->image);
        Check(imageStamp.empty() || imageStamp == stamp, "Selected image changed before background startup");
        cache->launch = {cache->data.wstring(),
                         cache->image.wstring(),
                         fs::canonical(prepared).wstring(),
                         apk.empty() ? L"" : fs::canonical(apk).wstring(),
                         origin.empty() ? L"" : fs::absolute(origin).lexically_normal().wstring(),
                         L"",
                         stamp,
                         token};
        cache->expectedPackage = expectedPackage;
        Check(process.empty() ||
                  (process.starts_with(expectedPackage + L":") && process.size() <= 255 &&
                   process.substr(expectedPackage.size() + 1)
                           .find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") ==
                       std::wstring::npos &&
                   process.size() > expectedPackage.size() + 1 && !token.empty()),
              "Invalid private application process");
        cache->applicationProcess = process;
        startup = {cache->data.wstring(), cache->launch[4], expectedPackage, process};
    }
    Starting(startup);
}

std::wstring ClientApplicationProcess()
{
    const std::lock_guard lock(cache->mutex);
    return cache->applicationProcess;
}

void ValidateClientLaunchPackage(const std::wstring& package)
{
    const std::lock_guard lock(cache->mutex);
    Check(cache->expectedPackage.empty() || cache->expectedPackage == package,
          "Background APK package differs from its scheduled application");
}

void RegisterClientLaunchProfile(const std::wstring& package)
{
    ValidateClientLaunchPackage(package);
    std::vector<std::wstring> fields;
    {
        const std::lock_guard lock(cache->mutex);
        Check(cache->session && cache->launch.size() == 8, "Client launch profile is unavailable");
        fields = cache->launch;
    }
    fields[5] = package;
    Response response{};
    std::wstring output;
    if (!ClientApplicationProcess().empty())
        fields.push_back(ClientApplicationProcess());
    RequestWithStart(Directory(), Operation::ClientProfile, Pack(fields), response, output, true);
    Check(response.result == 0 && response.process != 0 && output.size() == 32,
          "Service could not register application launch profile");
    fields[7] = output;
    if (fields.size() == 9)
        fields.pop_back();
    const std::lock_guard lock(cache->mutex);
    cache->launch = std::move(fields);
    cache->launchBound = true;
    cache->registeredOwner = response.process;
}

bool ClientWakeRequested()
{
    const std::lock_guard lock(cache->mutex);
    return cache->launch.size() == 8 && !cache->launch[7].empty();
}

bool ClientNotificationHostEnabled()
{
    const std::lock_guard lock(cache->mutex);
    return cache->session && cache->shell && cache->launchBound;
}

std::vector<std::wstring> ClientNotificationRequest(const std::vector<std::wstring>& fields, uint32_t* owner)
{
    Check(ClientNotificationHostEnabled(), "Notification service is not configured for this Client");
    auto directory = Directory();
    uint32_t registered;
    {
        const std::lock_guard lock(cache->mutex);
        registered = cache->registeredOwner;
    }
    Response response{};
    std::wstring output;
    RequestWithStart(directory, Operation::Notifications, Pack(fields), response, output, true);
    if (response.result != 0) {
        // A replacement broker can reject the first request before the async
        // host refresh has restored this Client. Add a retry for this explicit
        // rejection only when the Service changed. One-shot consumption fails
        // closed if its commit succeeded but the response was lost.
        auto state = Fetch(directory);
        if (state.owner != registered) {
            RestoreClientSession(state.owner);
            response = {};
            output.clear();
            RequestWithStart(directory, Operation::Notifications, Pack(fields), response, output, true);
        }
    }
    Check(response.result == 0 && response.process != 0, "Service notification operation failed");
    if (owner)
        *owner = response.process;
    return Split(output);
}

std::vector<std::wstring> ClientClipboardRequest(const std::vector<std::wstring>& fields)
{
    Check(ClientClipboardEnabled(), "Clipboard service is not configured for this Client");
    auto directory = Directory();
    Response response{};
    std::wstring output;
    // Copy/clear must not be automatically repeated after an uncertain reply.
    const char* failure = nullptr;
    if (!Request(Identity(directory), directory, Operation::Clipboard, Pack(fields), response, output, &failure))
        throw std::runtime_error(std::string("Service clipboard transport failed: ") +
                                 (failure ? failure : "unknown stage"));
    if (response.result != 0) {
        // Only error replies are logged; never include a successful text payload.
        std::string detail;
        for (wchar_t ch : output.substr(0, 256))
            detail += ch >= L' ' && ch <= L'~' ? static_cast<char>(ch) : '?';
        throw std::runtime_error("Service clipboard operation failed: " + detail);
    }
    Check(response.process != 0, "Clipboard service returned an invalid owner");
    return Split(output);
}

std::vector<std::wstring> ClientActivityRequest(const std::vector<std::wstring>& fields)
{
    Check(ClientNotificationHostEnabled(), "Activity service is not configured for this Client");
    Response response{};
    std::wstring output;
    const auto directory = Directory();
    // An uncertain start/claim must not be replayed after its target may have
    // already finished. The async host refresh restores a replacement broker.
    Check(Request(Identity(directory), directory, Operation::Activities, Pack(fields), response, output),
          "Activity service transport failed");
    if (response.result != 0)
        throw std::runtime_error("Service Activity operation failed: " + std::string(output.begin(), output.end()));
    Check(response.process != 0, "Service Activity identity is missing");
    auto result = Split(output);
    Check(!result.empty() && result[0].size() == 32, "Activity broker epoch is missing");
    return result;
}

bool ClientClipboardEnabled()
{
    const std::lock_guard lock(cache->mutex);
    return cache->session && !cache->package.empty();
}

std::wstring ClientApplicationLogRequest(const std::vector<std::wstring>& operation)
{
    std::vector<std::wstring> fields;
    uint32_t registered;
    {
        const std::lock_guard lock(cache->mutex);
        Check(cache->session && cache->shell && !cache->package.empty(),
              "Application logs require a registered Client");
        fields = {(cache->data / L"data").wstring(), cache->package};
        registered = cache->registeredOwner;
    }
    fields.insert(fields.end(), operation.begin(), operation.end());
    const auto directory = Directory();
    Response response{};
    std::wstring output;
    // An uncertain append reply must never cause automatic replay.
    Check(Request(Identity(directory), directory, Operation::ApplicationLogs, Pack(fields), response, output),
          "Application log transport failed");
    if (response.result != 0) {
        auto state = Fetch(directory);
        if (state.owner != registered) {
            RestoreClientSession(state.owner);
            response = {};
            output.clear();
            Check(Request(Identity(directory), directory, Operation::ApplicationLogs, Pack(fields), response, output),
                  "Application log recovery transport failed");
        }
    }
    Check(response.result == 0 && response.process != 0, "Service application log operation failed");
    return output;
}

void ClientOpenWebUrl(const std::vector<std::wstring>& fields)
{
    {
        const std::lock_guard lock(cache->mutex);
        Check(cache->session && !cache->package.empty(), "Web link service requires a registered Client");
    }
    auto directory = Directory();
    Response response{};
    std::wstring output;
    // Activation can have succeeded even if the reply was lost: do not replay it.
    Check(Request(Identity(directory), directory, Operation::OpenWebUrl, Pack(fields), response, output) &&
              response.result == 0 && response.process != 0,
          "Service web link activation failed");
}

void InitializeClientServices()
{
    std::call_once(cache->initialized, [] {
        auto directory = Directory();
        Check(!directory.empty(), "Client service is not configured");
        Check(cache->stop.value && cache->stopped.value, "Cannot create Client service shutdown events");
        Publish(Fetch(directory));
        std::thread([directory] {
            bool failed = false, profileFailed = false;
            while (WaitForSingleObject(cache->stop.value, 1000) == WAIT_TIMEOUT) {
                try {
                    auto state = Fetch(directory);
                    RestoreSystemAwake(state.owner);
                    try {
                        RestoreClientSession(state.owner);
                        profileFailed = false;
                    }
                    catch (const std::exception& error) {
                        if (!profileFailed)
                            std::fprintf(stderr, "[service] launch profile recovery failed: %s\n", error.what());
                        profileFailed = true;
                    }
                    Publish(std::move(state));
                    failed = false;
                }
                catch (const std::exception& error) {
                    if (!failed)
                        std::fprintf(stderr, "[service] host snapshot refresh failed: %s\n", error.what());
                    failed = true; // Keep the last snapshot; Windows UI callbacks never wait for recovery.
                }
            }
            SetEvent(cache->stopped.value);
        }).detach();
    });
}

bool StopClientServices() noexcept
{
    StopClientApplicationLogs();
    {
        const std::lock_guard lock(cache->mutex);
        if (!cache->ready)
            return true;
    }
    return SetEvent(cache->stop.value) && WaitForSingleObject(cache->stopped.value, 30000) == WAIT_OBJECT_0;
}

HostSnapshot ReadClientHostState()
{
    const std::lock_guard lock(cache->mutex);
    return cache->snapshot;
}

void SetClientSystemAwake(bool required)
{
    const std::lock_guard lock(cache->awakeMutex);
    auto owner = SendSystemAwake(required);
    if (required) {
        try {
            RememberAwakeService(owner);
        }
        catch (...) {
            try {
                SendSystemAwake(false);
            }
            catch (...) {
            }
            throw;
        }
    }
    else if (cache->awakeService.value) {
        CloseHandle(cache->awakeService.value);
        cache->awakeService.value = nullptr;
    }
    cache->awake = required;
}

std::vector<std::wstring> ClientJobRequest(const std::vector<std::wstring>& fields)
{
    Check(ClientJobsEnabled(), "Android jobs require an installation-local Client profile");
    Response response{};
    std::wstring output;
    RequestWithStart(Directory(), Operation::Jobs, Pack(fields), response, output, true);
    Check(response.result == 0, "Android job Service request failed");
    return Split(output);
}

bool ClientJobsEnabled()
{
    bool enabled;
    {
        const std::lock_guard lock(cache->mutex);
        enabled = cache->jobs;
    }
    return enabled && ProcessIs(GetCurrentProcessId(), Directory() / L"StoneClient.exe");
}

void RemoveClientPackageJobs(const fs::path& data, const std::wstring& packageName)
{
    Check(ClientJobsEnabled(), "Package job removal requires an installation-local Client profile");
    Response response{};
    std::wstring output;
    RequestWithStart(Directory(), Operation::PackageRemoved, Pack({fs::canonical(data).wstring(), packageName}),
                     response, output, true);
    Check(response.result == 0, "Service could not remove package jobs");
}

void EnableClientJobs()
{
    Check(ProcessIs(GetCurrentProcessId(), Directory() / L"StoneClient.exe"),
          "Jobs require this installation's Client executable");
    const std::lock_guard lock(cache->mutex);
    cache->jobs = true;
}

void SetClientHostChangedCallback(HostChangedCallback callback)
{
    const std::lock_guard lock(cache->mutex);
    cache->changed = callback;
}

void ConfigureClientShell(const fs::path& executable, const fs::path& data, const fs::path& image, bool application)
{
    ConfigureClientService(executable);
    InitializeClientServices();
    auto root = fs::canonical(data), system = fs::canonical(image);
    std::vector<std::wstring> fields{root.wstring(), system.wstring()};
    if (application)
        fields.push_back(L"starting");
    ShellRequest(Operation::ShellProfile, fields);
    const std::lock_guard lock(cache->mutex);
    cache->data = root;
    cache->image = system;
    cache->shell = true;
    cache->jobs = true;
}

bool ClientShellEnabled()
{
    const std::lock_guard lock(cache->mutex);
    return cache->shell;
}

void UpdateClientShortcut(const std::wstring& name, const std::wstring& label, bool installed)
{
    std::vector<std::wstring> fields;
    {
        const std::lock_guard lock(cache->mutex);
        Check(cache->shell, "Client shell profile is not configured");
        fields = {cache->data.wstring(), cache->image.wstring(), name, label, installed ? L"1" : L"0"};
    }
    ShellRequest(Operation::ShellShortcut, fields);
}

std::vector<std::wstring> UpdateClientApplicationIcon(const std::wstring& name, const std::wstring& label,
                                                      std::span<const uint8_t> png)
{
    static std::mutex uploads;
    const std::lock_guard upload(uploads);
    std::wstring root, image;
    {
        const std::lock_guard lock(cache->mutex);
        Check(cache->shell, "Client shell profile is not configured");
        root = cache->data.wstring();
        image = cache->image.wstring();
    }
    Check(!png.empty() && png.size() <= 1024 * 1024, "Invalid application icon size");
    const auto directory = Directory();
    Check(ProcessIs(GetCurrentProcessId(), directory / L"StoneClient.exe"),
          "Application icons require this installation's Client");
    // Re-register after a Service replacement; the upload itself never starts
    // a new broker midway through a transaction.
    ShellRequest(Operation::ShellProfile, {root, image});
    auto send = [&](std::vector<std::wstring> fields) {
        Response response{};
        std::wstring output;
        Check(Request(Identity(directory), directory, Operation::ApplicationIcon, Pack(fields), response, output),
              "Application icon transport failed");
        Check(response.result == 0, "Service could not update the application icon");
        return output;
    };
    send({root, name, L"begin", label, std::to_wstring(png.size())});
    for (size_t offset = 0; offset < png.size();) {
        size_t count = std::min<size_t>(8000, png.size() - offset);
        std::wstring hex;
        hex.reserve(count * 2);
        for (auto value : png.subspan(offset, count)) {
            hex += L"0123456789abcdef"[value >> 4];
            hex += L"0123456789abcdef"[value & 15];
        }
        send({root, name, L"chunk", std::to_wstring(offset), std::move(hex)});
        offset += count;
    }
    auto result = Split(send({root, name, L"commit"}));
    Check(result.size() == 2, "Invalid application appearance response");
    return result;
}

void EnsureClientNotificationIdentity(const fs::path& settings, const fs::path& diagnostic)
{
    // Called during host preparation, before ART or its Android UI thread exists.
    ConfigureClientService(fs::absolute(settings).parent_path().parent_path() / L"StoneService.exe");
    ShellRequest(Operation::NotificationIdentity,
                 {fs::canonical(settings).wstring(), diagnostic.empty() ? L"" : diagnostic.wstring(), L"1"});
}

void CleanupClientNotificationTestIdentity(const fs::path& settings, const fs::path& diagnostic)
{
    Check(!diagnostic.empty(), "Diagnostic notification executable is required");
    ShellRequest(Operation::NotificationIdentity, {fs::canonical(settings).wstring(), diagnostic.wstring(), L"0"});
}

void RegisterBrokerClientPackage(const std::wstring& packageName) noexcept
{
    try {
        {
            const std::lock_guard lock(cache->mutex);
            cache->package = packageName;
        }
        auto directory = Directory();
        if (directory.empty())
            directory = OwnDirectory();
        Identity identity(directory);
        Response response{};
        std::wstring output;
        Request(identity, directory, Operation::Register, packageName, response, output);
    }
    catch (...) {
    }
}
}
