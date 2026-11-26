#include "NotificationHost.h"
#include "WindowsNotifications.h"
#include "../ipc/ServiceProtocol.h"
#include "../host/CommandLine.h"
#include "../host/WindowsPath.h"
#include <map>
#include <memory>
#include <cstdio>
#include <set>
#include <sstream>
#include <algorithm>
#include <deque>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;
using ipc::Check;

struct Session {
    std::map<DWORD, std::unique_ptr<ipc::Handle>> clients;
    std::map<uint64_t, DWORD> owners;
    std::map<std::wstring, uint64_t> tickets;
    std::map<DWORD, std::deque<int64_t>> events;
    fs::path data;
    std::wstring package;
    WindowsNotifications host;
    std::set<std::wstring> shown;

    Session(const fs::path& settings, fs::path directory, std::wstring name)
        : data(std::move(directory)), package(std::move(name)), host(settings, data, package)
    {
    }

    void Prune()
    {
        for (auto it = clients.begin(); it != clients.end();) {
            if (WaitForSingleObject(it->second->value, 0) == WAIT_TIMEOUT) {
                ++it;
                continue;
            }
            const auto pid = it->first;
            events.erase(pid);
            std::erase_if(owners, [&](const auto& entry) {
                return entry.second == pid;
            });
            it = clients.erase(it);
        }
        std::erase_if(tickets, [&](const auto& entry) {
            return !owners.contains(entry.second);
        });
    }

    void Forget(const std::wstring& key)
    {
        const auto found = tickets.find(key);
        if (found != tickets.end()) {
            owners.erase(found->second);
            tickets.erase(found);
        }
    }
};

// The broker serializes these operations; only each host's activation listener is concurrent.
std::map<std::wstring, std::unique_ptr<Session>> sessions;

int Number(const std::wstring& text)
{
    const auto first = !text.empty() && text[0] == L'-' ? 1u : 0u;
    Check(text.size() > first && text.find_first_not_of(L"0123456789", first) == text.npos,
          "Invalid notification integer");
    return std::stoi(text);
}

bool Flag(const std::wstring& text)
{
    Check(text == L"0" || text == L"1", "Invalid notification flag");
    return text == L"1";
}

Session& Host(const fs::path& settings, const fs::path& data, const std::wstring& package)
{
    auto key = ipc::Normalize(data) + L"\n" + package;
    auto it = sessions.find(key);
    if (it == sessions.end()) {
        Check(sessions.size() < 1024, "Too many notification scopes");
        it = sessions.emplace(key, std::make_unique<Session>(settings, data, package)).first;
    }
    return *it->second;
}
}

bool ServiceNotificationPolicy(const fs::path& data, const std::wstring& package)
{
    auto path = ExtendedWindowsPath(data / L"system/stone-notifications/policy.properties");
    ipc::Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file.value && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND))
        return false;
    LARGE_INTEGER size{};
    Check(file.value && GetFileSizeEx(file.value, &size) && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024,
          "Cannot read notification policy");
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    Check(bytes.empty() || ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                               read == bytes.size(),
          "Cannot read notification policy");
    if (bytes.starts_with("\xef\xbb\xbf"))
        bytes.erase(0, 3);
    std::istringstream input(bytes);
    std::string line;
    bool format = false, global = true, allowed = false;
    Check(stone::session::IsPackageName(package), "Invalid notification package");
    std::string keyPackage;
    for (auto ch : package)
        keyPackage += static_cast<char>(ch);
    while (std::getline(input, line)) {
        auto first = line.find_first_not_of(" \r\t"), last = line.find_last_not_of(" \r\t");
        if (first == line.npos)
            continue;
        line = line.substr(first, last - first + 1);
        if (line[0] == '#' || line[0] == '!')
            continue;
        auto equal = line.find('=');
        Check(equal != line.npos && equal > 0, "Invalid notification policy entry");
        auto key = line.substr(0, equal), value = line.substr(equal + 1);
        if (key == "format") {
            Check(value == "1", "Unknown notification policy");
            format = true;
        }
        else {
            Check(key == "global" || key.starts_with("package.") &&
                                         stone::session::IsPackageName(std::wstring(key.begin() + 8, key.end())),
                  "Unknown notification policy key");
            Check(value == "true" || value == "false", "Invalid notification grant");
            if (key == "global")
                global = value == "true";
            else if (key == "package." + keyPackage)
                allowed = value == "true";
        }
    }
    Check(format, "Notification policy has no format");
    return global && allowed;
}

std::vector<std::wstring> ServiceNotificationRequest(DWORD client, const fs::path& settings,
                                                     const std::vector<std::wstring>& fields, PendingIntents& store,
                                                     const std::wstring& scope)
{
    Check(fields.size() >= 3 && stone::session::IsPackageName(fields[1]), "Invalid notification scope");
    auto data = fs::canonical(ExtendedWindowsPath(fields[0]));
    Check(fs::is_directory(ExtendedWindowsPath(data)), "Notification data directory is unavailable");
    const auto key = ipc::Normalize(data) + L"\n" + fields[1];
    const auto& operation = fields[2];
    auto& session = Host(settings, data, fields[1]);
    session.Prune();
    if (operation == L"init") {
        Check(fields.size() == 3, "Invalid notification initialization");
        if (!session.clients.contains(client)) {
            Check(session.clients.size() < 64, "Too many notification Clients for this package");
            auto process = std::make_unique<ipc::Handle>(OpenProcess(SYNCHRONIZE, FALSE, client));
            Check(process->value != nullptr, "Cannot track notification Client");
            session.clients.emplace(client, std::move(process));
        }
        return {L"1"};
    }
    Check(session.clients.contains(client), "Notification Client scope is not initialized");
    auto& host = session.host;
    if (operation == L"allowed") {
        Check(fields.size() == 3, "Invalid notification settings query");
        return {host.Allowed() ? L"1" : L"0"};
    }
    if (operation == L"post") {
        Check(fields.size() >= 12 && fields[3].size() <= 2048 && fields[4].size() <= 1024 && fields[5].size() <= 8192,
              "Invalid notification contents");
        WindowsNotification request{fields[3], fields[4], fields[5]};
        request.progress = Number(fields[6]);
        request.maximum = Number(fields[7]);
        request.indeterminate = Flag(fields[8]);
        request.silent = Flag(fields[9]);
        request.suppressPopup = Flag(fields[10]);
        auto count = Number(fields[11]);
        Check(count >= 0 && count <= 5 && fields.size() >= 12 + count, "Invalid notification action count");
        for (size_t index = 12; index < 12 + count; ++index) {
            Check(fields[index].size() <= 256, "Notification action label is too long");
            request.actions.push_back(fields[index]);
        }
        request.recipient = client;
        size_t offset = 12 + count;
        if (fields.size() > offset) {
            Check(fields.size() == offset + 4 + count, "Invalid durable notification fields");
            const auto& flags = fields[offset + 1];
            Check(!flags.empty() && flags.size() <= 10 && flags.find_first_not_of(L"0123456789") == flags.npos,
                  "Invalid notification flags");
            auto value = std::stoull(flags);
            Check(value <= UINT32_MAX, "Invalid notification flags");
            for (const auto& prior : store.Notifications(scope))
                if (prior.visual.key == request.key)
                    session.shown.erase(prior.id);
            PendingNotification note;
            note.scope = scope;
            note.channel = fields[offset];
            note.flags = static_cast<uint32_t>(value);
            note.visual = request;
            note.targets.assign(fields.begin() + offset + 2, fields.end());
            note = store.Post(std::move(note));
            request.route = note.id;
        }
        else
            store.RemoveNotification(scope, request.key);
        Check(session.tickets.size() < 4096 || session.tickets.contains(request.key), "Too many notification tickets");
        auto ticket = host.Post(request);
        if (!request.route.empty())
            session.shown.insert(request.route);
        session.Forget(request.key);
        if (ticket) {
            session.tickets.emplace(request.key, ticket);
            session.owners.emplace(ticket, client);
        }
        return {std::to_wstring(ticket), request.route};
    }
    if (operation == L"remove") {
        Check(fields.size() == 4 && fields[3].size() <= 2048, "Invalid notification removal");
        for (const auto& note : store.Notifications(scope))
            if (note.visual.key == fields[3])
                session.shown.erase(note.id);
        store.RemoveNotification(scope, fields[3]);
        host.Remove(fields[3]);
        session.Forget(fields[3]);
        return {};
    }
    if (operation == L"clear") {
        Check(fields.size() == 3, "Invalid notification clear");
        store.ClearNotifications(scope);
        host.RemoveAll();
        session.shown.clear();
        session.owners.clear();
        session.tickets.clear();
        session.events.clear();
        return {};
    }
    if (operation == L"poll") {
        Check(fields.size() == 3, "Invalid notification event query");
        std::vector<std::wstring> result;
        const auto received = host.Poll();
        Check(received.size() % 2 == 0, "Incomplete notification event");
        for (size_t at = 0; at < received.size(); at += 2) {
            const auto owner = session.owners.find(static_cast<uint64_t>(received[at]));
            if (owner == session.owners.end() || !session.clients.contains(owner->second))
                continue;
            auto& queue = session.events[owner->second];
            if (queue.size() < 2048) {
                queue.push_back(received[at]);
                queue.push_back(received[at + 1]);
            }
        }
        auto& queue = session.events[client];
        for (size_t count = 0; count < 64 && !queue.empty(); ++count) {
            result.push_back(std::to_wstring(queue.front()));
            queue.pop_front();
        }
        return result;
    }
    if (operation == L"history") {
        Check(fields.size() == 4, "Invalid notification history query");
        auto index = Number(fields[3]);
        Check(index >= 0 && index <= 256, "Invalid history offset");
        auto history = host.History();
        return static_cast<size_t>(index) < history.size() ? std::vector<std::wstring>{history[index]}
                                                           : std::vector<std::wstring>{};
    }
    throw std::runtime_error("Unknown notification operation");
}

void RestoreServiceNotifications(PendingIntents& store, const JobProfiles& profiles, const fs::path& settings)
{
    auto notes = store.Notifications();
    std::vector<std::wstring> active;
    for (const auto& note : notes)
        active.push_back(note.id);
    for (auto& [key, session] : sessions) {
        try {
            session->host.SynchronizeRoutes(active);
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "[notifications] history reconciliation failed: %s\n", error.what());
        }
        for (auto it = session->shown.begin(); it != session->shown.end();) {
            if (std::find(active.begin(), active.end(), *it) == active.end())
                it = session->shown.erase(it);
            else
                ++it;
        }
    }
    std::set<std::wstring> denied;
    size_t restored = 0;
    for (const auto& note : notes) {
        if (denied.contains(note.scope))
            continue;
        try {
            const JobProfile* profile = nullptr;
            for (const auto& [key, value] : profiles.All())
                if (value.token == note.scope) {
                    profile = &value;
                    break;
                }
            if (!profile)
                continue;
            auto& session = Host(settings, profile->data / L"data", profile->package);
            if (!ServiceNotificationPolicy(session.data, session.package)) {
                store.ClearNotifications(note.scope, true);
                session.host.RemoveAll();
                session.shown.clear();
                denied.insert(note.scope);
                continue;
            }
            if (!session.shown.contains(note.id)) {
                if (restored >= 8)
                    continue;
                profile->ValidateForLaunch();
                auto visual = note.visual;
                visual.route = note.id;
                visual.silent = visual.suppressPopup = true;
                visual.recipient = 0;
                session.host.Post(visual);
                session.shown.insert(note.id);
                ++restored;
            }
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "[notifications] cannot restore route: %s\n", error.what());
        }
    }
}

std::vector<std::wstring> ServiceNotificationDismissals()
{
    std::vector<std::wstring> result;
    for (const auto& [key, session] : sessions) {
        auto events = session->host.PollRoutes();
        result.insert(result.end(), events.begin(), events.end());
    }
    return result;
}

void AcceptServiceNotification(PendingIntents& store, const JobProfile& profile, const std::wstring& route, int action)
{
    Check(ServiceNotificationPolicy(profile.data / L"data", profile.package), "Notification permission was revoked");
    auto key = ipc::Normalize(profile.data / L"data") + L"\n" + profile.package;
    auto found = sessions.find(key);
    Check(found != sessions.end() && found->second->host.Allowed(), "Windows notifications are disabled");
    profile.ValidateForLaunch();
    auto delivery = store.Activate(route, action);
    if (delivery.cancel) {
        found->second->host.Remove(delivery.key);
        found->second->shown.erase(route);
    }
}

void RemoveServiceNotifications(const fs::path& data, const std::wstring& package)
{
    for (auto it = sessions.begin(); it != sessions.end();) {
        if (it->second->package == package && ipc::Normalize(it->second->data) == ipc::Normalize(data)) {
            try {
                it->second->host.RemoveAll();
            }
            catch (const std::exception& error) {
                std::fprintf(stderr, "[notifications] package history cleanup failed: %s\n", error.what());
            }
            it = sessions.erase(it);
        }
        else
            ++it;
    }
}

void StopServiceNotifications() noexcept
{
    sessions.clear();
}
}
