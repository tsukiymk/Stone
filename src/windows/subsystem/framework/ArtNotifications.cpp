#include "ArtNotifications.h"
#include "../filesystem/AndroidSystemFiles.h"
#include "../../client/BrokerClient.h"
#include <memory>
#include <mutex>
#include <stdexcept>
#include <cstdio>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;
std::mutex gate;
fs::path settingsExecutable, diagnosticExecutable, data;
std::wstring package;
std::unique_ptr<WindowsNotifications> diagnosticHost;
std::exception_ptr registrationFailure;
bool remote = false;
uint32_t owner = 0;

std::vector<std::wstring> Request(std::vector<std::wstring> fields)
{
    if (fields[0] != L"init")
        ClientNotificationRequest({data.wstring(), package, L"init"}, &owner);
    fields.insert(fields.begin(), {data.wstring(), package});
    return ClientNotificationRequest(fields, &owner);
}

std::wstring One(const std::vector<std::wstring>& fields)
{
    if (fields.size() != 1)
        throw std::runtime_error("Invalid notification service response");
    return fields[0];
}
}

void ConfigureArtNotifications(const fs::path& settings, const fs::path& diagnostic)
{
    const std::lock_guard lock(gate);
    settingsExecutable = fs::absolute(settings);
    diagnosticExecutable = diagnostic.empty() ? fs::path{} : fs::canonical(diagnostic);
    registrationFailure = nullptr;
    if (fs::is_regular_file(settingsExecutable)) {
        try {
            EnsureClientNotificationIdentity(settingsExecutable, diagnosticExecutable);
        }
        catch (...) {
            registrationFailure = std::current_exception();
        }
    }
}

bool InitializeArtNotifications(const std::wstring& name)
{
    const std::lock_guard lock(gate);
    if (remote || diagnosticHost)
        return true;
    if (!fs::is_regular_file(settingsExecutable))
        return false;
    if (registrationFailure)
        std::rethrow_exception(registrationFailure);
    data = fs::canonical(ResolveAndroidSystemFile(L"data"));
    package = name;
    if (diagnosticExecutable.empty() && ClientNotificationHostEnabled()) {
        if (One(Request({L"init"})) != L"1")
            throw std::runtime_error("Service could not initialize notifications");
        remote = true;
    }
    else
        diagnosticHost =
            std::make_unique<WindowsNotifications>(settingsExecutable, data, package, diagnosticExecutable);
    return true;
}

bool ArtNotificationsAllowed()
{
    const std::lock_guard lock(gate);
    return remote ? One(Request({L"allowed"})) == L"1" : diagnosticHost && diagnosticHost->Allowed();
}

std::vector<std::wstring> PostArtNotification(const ArtNotification& notification)
{
    const std::lock_guard lock(gate);
    if (remote) {
        std::vector<std::wstring> fields{L"post",
                                         notification.key,
                                         notification.title,
                                         notification.text,
                                         std::to_wstring(notification.progress),
                                         std::to_wstring(notification.maximum),
                                         notification.indeterminate ? L"1" : L"0",
                                         notification.silent ? L"1" : L"0",
                                         notification.suppressPopup ? L"1" : L"0",
                                         std::to_wstring(notification.actions.size())};
        fields.insert(fields.end(), notification.actions.begin(), notification.actions.end());
        if (!notification.targets.empty()) {
            fields.push_back(notification.channel);
            fields.push_back(std::to_wstring(notification.flags));
            fields.insert(fields.end(), notification.targets.begin(), notification.targets.end());
        }
        auto result = Request(std::move(fields));
        if (result.size() != 2)
            throw std::runtime_error("Invalid notification receipt");
        return result;
    }
    if (!diagnosticHost)
        throw std::runtime_error("Notification backend is not initialized");
    return {std::to_wstring(diagnosticHost->Post(notification)), L""};
}

void RemoveArtNotification(const std::wstring& key)
{
    const std::lock_guard lock(gate);
    if (remote)
        Request({L"remove", key});
    else if (diagnosticHost)
        diagnosticHost->Remove(key);
}

void RemoveArtNotifications()
{
    const std::lock_guard lock(gate);
    if (remote)
        Request({L"clear"});
    else if (diagnosticHost)
        diagnosticHost->RemoveAll();
}

std::vector<int64_t> PollArtNotifications()
{
    const std::lock_guard lock(gate);
    if (!remote)
        return diagnosticHost ? diagnosticHost->Poll() : std::vector<int64_t>{};
    const auto fields = Request({L"poll"});
    std::vector<int64_t> result{0, owner};
    for (const auto& field : fields)
        result.push_back(std::stoll(field));
    return result;
}

std::vector<std::wstring> ReadArtNotificationHistory()
{
    const std::lock_guard lock(gate);
    if (!remote)
        return diagnosticHost ? diagnosticHost->History() : std::vector<std::wstring>{};
    std::vector<std::wstring> result;
    for (size_t index = 0; index < 256; ++index) {
        auto row = Request({L"history", std::to_wstring(index)});
        if (row.empty())
            break;
        result.push_back(One(row));
    }
    return result;
}

void CleanupArtNotificationTestIdentity()
{
    const std::lock_guard lock(gate);
    if (remote) {
        try {
            Request({L"clear"});
        }
        catch (...) {
            std::fprintf(stderr, "[notifications] service test history cleanup failed\n");
        }
        return;
    }
    if (diagnosticExecutable.empty())
        return;
    try {
        if (diagnosticHost)
            diagnosticHost->RemoveAll();
    }
    catch (...) {
        std::fprintf(stderr, "[notifications] test history cleanup failed\n");
    }
    diagnosticHost.reset();
    try {
        CleanupClientNotificationTestIdentity(settingsExecutable, diagnosticExecutable);
    }
    catch (...) {
        std::fprintf(stderr, "[notifications] test identity cleanup failed\n");
    }
}
}
