#include "Broker.h"
#include "ApplicationLogs.h"
#include "ActivityRoutes.h"
#include "../ipc/ServiceProtocol.h"
#include "HostServices.h"
#include "../host/shell/ShellIntegration.h"
#include "../host/shell/ShellIconUpload.h"
#include "../ipc/NotificationIdentity.h"
#include "PowerRequests.h"
#include "JobQueue.h"
#include "../ipc/ClientSignals.h"
#include "JobProfiles.h"
#include "NotificationHost.h"
#include "PendingIntents.h"
#include "WindowsClipboard.h"
#include "../host/shell/WebLinks.h"
#include "../host/WindowsPath.h"
#include "../host/UserPaths.h"
#include "../host/CommandLine.h"
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <map>
#include <memory>
#include <cstdio>
#include <stdexcept>

namespace stone::windows {
using namespace ipc;

namespace {
namespace fs = std::filesystem;
using namespace ipc;

struct Child {
    Handle process;
    DWORD pid;
    std::wstring data, source, package;
    ClientSignals signals;
    bool retiring = false, starting = false;
    std::wstring wakeKey, intentScope;
    std::wstring activityScope, applicationProcess;
    std::wstring logSnapshot;
    std::wstring logData, logPackage;
    size_t logOffset = 0;
    bool logReading = false;
    ShellIconUpload icon;

    Child(HANDLE handle, DWORD id, std::wstring root, std::wstring app, const fs::path& directory)
        : process(handle), pid(id), data(std::move(root)), source(std::move(app)), signals(directory, id)
    {
    }
};

struct Foreground {
    DWORD pid;
    bool close;
};

BOOL CALLBACK VisitWindow(HWND window, LPARAM parameter)
{
    const auto& request = *reinterpret_cast<Foreground*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == request.pid && !GetWindow(window, GW_OWNER)) {
        if (request.close)
            PostMessageW(window, WM_CLOSE, 0, 0);
        else {
            ShowWindowAsync(window, SW_RESTORE);
            SetForegroundWindow(window);
        }
    }
    return TRUE;
}

std::wstring PackageUri(const std::wstring& uri)
{
    Check(uri.size() >= 8 && CompareStringOrdinal(uri.data(), 8, L"stone://", 8, TRUE) == CSTR_EQUAL,
          "Expected stone://packageName");
    auto name = uri.substr(8);
    if (name.ends_with(L'/'))
        name.pop_back();
    Check(stone::session::IsPackageName(name), "Invalid Android package URI");
    return name;
}

struct Launch {
    std::wstring command, data, source, package;
    bool background = false;
};

Launch Parse(const std::vector<std::wstring>& arguments, const fs::path& directory)
{
    if (!arguments.empty() && arguments[0] == L"/launch") {
        Check(arguments.size() == 2, "Expected /launch stone://packageName");
        const auto name = PackageUri(arguments[1]);
        auto launch = Parse(ResolveApplicationLaunch(name), directory);
        launch.package = name;
        return launch;
    }
    std::map<std::wstring, std::wstring> values;
    bool jit = false, background = false, aot = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        auto flag = arguments[index];
        if (flag == L"--background") {
            Check(!background, "Repeated background option");
            background = true;
            continue;
        }
        if (flag == L"--enable-jit") {
            Check(!jit, "Repeated JIT option");
            jit = true;
            continue;
        }
        if (flag == L"--enable-aot") {
            Check(!aot, "Repeated AOT option");
            aot = true;
            continue;
        }
        Check(flag == L"--apk" || flag == L"--package" || flag == L"--uri" || flag == L"--data-dir" ||
                  flag == L"--system-image" || flag == L"--framework-image",
              "Unknown service launch option");
        Check(++index < arguments.size() && !arguments[index].empty() && !arguments[index].starts_with(L"--"),
              "Missing service launch argument");
        auto value = arguments[index];
        if (flag == L"--uri") {
            flag = L"--package";
            value = PackageUri(value);
        }
        if (flag == L"--framework-image")
            flag = L"--system-image";
        if (flag == L"--package")
            PackageUri(L"stone://" + value);
        else
            value = fs::absolute(value).lexically_normal().wstring();
        Check(values.emplace(flag, value).second, "Repeated service launch option");
    }
    Check(values.contains(L"--apk") != values.contains(L"--package"), "Select one APK or installed package");
#ifdef _M_ARM64
    throw std::runtime_error("Windows ARM64 ART execution is not implemented");
#endif
    if (values.contains(L"--apk"))
        Check(fs::is_regular_file(ExtendedWindowsPath(values[L"--apk"])), "APK does not exist");
    if (values.contains(L"--system-image"))
        Check(fs::is_regular_file(ExtendedWindowsPath(values[L"--system-image"])), "System image does not exist");
    if (!values.contains(L"--data-dir")) {
        values[L"--data-dir"] = DefaultAndroidDataDirectory().wstring();
    }
    Launch result;
    result.background = background;
    result.command = stone::session::Quote((directory / L"StoneClient.exe").wstring());
    for (const auto& [flag, value] : values)
        result.command += L" " + flag + L" " + stone::session::Quote(value);
    if (background)
        result.command += L" --background";
    if (jit)
        result.command += L" --enable-jit";
    if (aot)
        result.command += L" --enable-aot";
    result.data = Normalize(values[L"--data-dir"]);
    result.package = values.contains(L"--package") ? values[L"--package"] : L"";
    result.source = values.contains(L"--apk") ? Normalize(values[L"--apk"]) : L"";
    return result;
}

std::wstring Status(DWORD pid, const std::map<DWORD, std::unique_ptr<Child>>& clients)
{
    std::wstring text = L"{\"service\":" + std::to_wstring(pid) + L",\"clients\":[";
    bool first = true;
    for (const auto& [id, client] : clients) {
        if (!first)
            text += L",";
        first = false;
        text += L"{\"pid\":" + std::to_wstring(id) + L",\"package\":\"" + client->package + L"\"}";
    }
    return text + L"]}";
}

std::unique_ptr<Child> Spawn(const Launch& request, const fs::path& directory)
{
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    auto executable = directory / L"StoneClient.exe";
    auto command = request.command;
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         directory.c_str(), &startup, &process),
          "Cannot start ART Client");
    CloseHandle(process.hThread);
    auto client =
        std::make_unique<Child>(process.hProcess, process.dwProcessId, request.data, request.source, directory);
    client->package = request.package;
    client->starting = true;
    return client;
}

int Serve(const Identity& identity, const fs::path& directory, HANDLE serviceStop = nullptr)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(identity.security.c_str(), SDDL_REVISION_1, &descriptor,
                                                               nullptr),
          "Cannot secure service endpoint");
    std::unique_ptr<void, decltype(&LocalFree)> security(descriptor, LocalFree);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    Handle singleton(CreateMutexW(&attributes, FALSE, identity.mutex.c_str()));
    Check(singleton.value, "Cannot create service singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    Handle pipe(CreateNamedPipeW(
        identity.pipe.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS, 1, 131072, 131072, 0, &attributes));
    Check(pipe.value, "Cannot create service pipe");
    StartHostServices();
    std::map<DWORD, std::unique_ptr<Child>> clients;
    ActivityRoutes activities;
    JobProfiles profiles(JobProfileStorePath(directory));
    PendingIntents pendingIntents(JobProfileStorePath(directory).replace_filename(L"pending-intents.json"));
    std::vector<std::wstring> intentScopes;
    for (const auto& [key, profile] : profiles.All())
        intentScopes.push_back(profile.token);
    pendingIntents.Retain(intentScopes);

    struct Wake {
        uint64_t next = 0;
        unsigned failures = 0;
        std::string error;
    };

    std::map<std::wstring, Wake> wakes;
    auto activateNotification = [&](const std::wstring& route, int action) {
        auto note = pendingIntents.Notification(route);
        const JobProfile* profile = nullptr;
        for (const auto& [key, value] : profiles.All())
            if (value.token == note.scope) {
                profile = &value;
                break;
            }
        Check(profile != nullptr, "Notification launch profile was revoked");
        AcceptServiceNotification(pendingIntents, *profile, route, action);
        wakes[profile->Key()] = {};
    };
    std::unique_ptr<WindowsClipboard> clipboard;
    bool stop = false, stopping = false;
    ULONGLONG shutdownDeadline = 0;
    auto beginStop = [&] {
        if (stopping)
            return;
        stopping = true;
        if (serviceStop)
            shutdownDeadline = GetTickCount64() + 20000;
        for (const auto& [id, client] : clients) {
            client->signals.Stop();
            Foreground request{id, true};
            EnumWindows(VisitWindow, reinterpret_cast<LPARAM>(&request));
        }
    };
    auto prune = [&] {
        for (auto it = clients.begin(); it != clients.end();) {
            if (WaitForSingleObject(it->second->process.value, 0) == WAIT_OBJECT_0) {
                activities.Drop(it->first);
                it = clients.erase(it);
            }
            else
                ++it;
        }
        if (stopping && (clients.empty() || (shutdownDeadline && GetTickCount64() >= shutdownDeadline)))
            stop = true;
    };
    uint64_t lastTick = 0;
    auto tick = [&] {
        if (serviceStop && WaitForSingleObject(serviceStop, 0) == WAIT_OBJECT_0)
            beginStop();
        auto now = JobClockMillis();
        if (now - lastTick < 1000)
            return;
        lastTick = now;
        prune();
        MaintainServiceJobs(now);
        if (stopping)
            return;
        try {
            RestoreServiceNotifications(pendingIntents, profiles, directory / L"settings/StoneSettings.exe");
            for (const auto& route : ServiceNotificationDismissals()) {
                try {
                    activateNotification(route, -2);
                }
                catch (const std::exception& error) {
                    std::fprintf(stderr, "[notifications] dismissal rejected: %s\n", error.what());
                }
            }
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "[notifications] restoration unavailable: %s\n", error.what());
        }
        size_t live = 0, started = 0;
        for (const auto& [id, client] : clients)
            if (!client->wakeKey.empty())
                ++live;
        for (const auto& [key, profile] : profiles.All()) {
            if (live >= 4 || started >= 2)
                break;
            auto& wake = wakes[key];
            if (now < wake.next)
                continue;
            bool existing = false;
            for (const auto& [id, client] : clients)
                if (client->applicationProcess.empty() && client->data == Normalize(profile.data) &&
                    (client->package == profile.package || client->starting && client->package.empty())) {
                    existing = true;
                    break;
                }
            if (existing)
                continue;
            try {
                if (!pendingIntents.Delivery(profile.token) &&
                    !ServiceJobsReady(profile.data / L"data", profile.prepared / L"system", profile.package,
                                      ReadServiceHostState(), now, ReadServiceJobState(profile.data / L"data")))
                    continue;
                profile.ValidateForLaunch();
                std::vector<std::wstring> arguments{profile.apk.empty() ? L"--package" : L"--apk",
                                                    profile.apk.empty() ? profile.package
                                                                        : ExtendedWindowsPath(profile.apk).wstring(),
                                                    L"--data-dir",
                                                    profile.data.wstring(),
                                                    L"--system-image",
                                                    profile.image.wstring(),
                                                    L"--background"};
                auto request = Parse(arguments, directory);
                request.package = profile.package;
                request.source = profile.source.empty() ? L"" : Normalize(profile.source);
                request.command += L" --prepared-system " + stone::session::Quote(profile.prepared.wstring()) +
                                   L" --expected-package " + stone::session::Quote(profile.package) +
                                   L" --image-stamp " + stone::session::Quote(profile.imageStamp) + L" --wake-token " +
                                   stone::session::Quote(profile.token);
                if (!profile.source.empty())
                    request.command += L" --origin-apk " + stone::session::Quote(profile.source.wstring());
                auto client = Spawn(request, directory);
                client->wakeKey = key;
                auto pid = client->pid;
                clients.emplace(pid, std::move(client));
                ++live;
                ++started;
                std::fprintf(stderr, "[jobs] background Client started: package=%ls pid=%lu\n", profile.package.c_str(),
                             pid);
                wake.error.clear();
            }
            catch (const std::exception& failure) {
                if (wake.error != failure.what())
                    std::fprintf(stderr, "[jobs] cannot wake %ls: %s\n", profile.package.c_str(), failure.what());
                wake.error = failure.what();
            }
            wake.failures = std::min<unsigned>(wake.failures + 1, 6);
            wake.next = now + std::min<uint64_t>(300000, 10000ULL << (wake.failures - 1));
        }
    };
    while (!stop) {
        try {
            tick();
        }
        catch (const std::exception& failure) {
            std::fprintf(stderr, "[jobs] scheduler tick failed: %s\n", failure.what());
        }
        if (stop)
            break;
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        Check(event.value, "Cannot wait for service requests");
        OVERLAPPED pending{};
        pending.hEvent = event.value;
        bool connected = ConnectNamedPipe(pipe.value, &pending) != FALSE;
        auto error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && error == ERROR_IO_PENDING) {
            DWORD wait;
            bool warned = false;
            do {
                wait = WaitForSingleObject(event.value, 1000);
                if (wait == WAIT_TIMEOUT) {
                    try {
                        tick();
                        warned = false;
                    }
                    catch (const std::exception& failure) {
                        if (!warned)
                            std::fprintf(stderr, "[jobs] maintenance failed: %s\n", failure.what());
                        warned = true;
                    }
                    prune();
                    if (stop)
                        break;
                }
            } while (wait == WAIT_TIMEOUT);
            DWORD count = 0;
            if (wait == WAIT_OBJECT_0)
                connected = GetOverlappedResult(pipe.value, &pending, &count, FALSE) != FALSE;
            else {
                CancelIoEx(pipe.value, &pending);
                GetOverlappedResult(pipe.value, &pending, &count, TRUE);
            }
        }
        else
            connected = connected || error == ERROR_PIPE_CONNECTED;
        ULONG peer = 0;
        if (connected && GetNamedPipeClientProcessId(pipe.value, &peer) && SameSession(peer)) {
            const bool clientPeer = ProcessIs(peer, directory / L"StoneClient.exe");
            const bool trusted = clientPeer || ProcessIs(peer, directory / L"StoneService.exe");
            Header header{};
            Response response{};
            std::wstring output;
            if (Transfer(pipe.value, &header, sizeof(header), false) && header.magic == Magic &&
                header.version == Version && header.length <= 32767) {
                std::wstring text(header.length, L'\0');
                bool received =
                    text.empty() || Transfer(pipe.value, text.data(), header.length * sizeof(wchar_t), false);
                if (received)
                    try {
                        // The current-user pipe/session ACL is sufficient for this
                        // read-only, unprivileged host snapshot. Mutations still
                        // require the installation's own Client or Service binary.
                        Check(trusted || header.operation == Operation::HostState, "Untrusted service mutation");
                        prune();
                        if (header.operation == Operation::HostState) {
                            const auto host = ReadServiceHostState();
                            output = text == L"network-v1"
                                         ? EncodeHostInterfaces(host.owner,
                                                                host.interfaces ? *host.interfaces : HostInterfaces{})
                                         : EncodeHostState(host, text == L"wifi-v1", text == L"memory-v1");
                            response.process = GetCurrentProcessId();
                        }
                        else if (header.operation == Operation::NotificationActivate) {
                            Check(!clientPeer && !stopping, "Invalid notification activation caller");
                            auto fields = Split(text);
                            Check(fields.size() == 2 && !fields[1].empty() &&
                                      fields[1].find_first_not_of(L"-0123456789") == fields[1].npos,
                                  "Invalid notification activation");
                            auto action = std::stoi(fields[1]);
                            Check(action >= -1 && action <= 4, "Invalid notification action");
                            activateNotification(fields[0], action);
                        }
                        else if (header.operation == Operation::PackageRemoved) {
                            Check(clientPeer, "Only a Client can remove Android package jobs");
                            auto fields = Split(text);
                            Check(fields.size() == 2, "Invalid removed package fields");
                            RemoveServicePackageJobs(fields[0], fields[1], JobClockMillis());
                            RemoveServiceNotifications(fields[0], fields[1]);
                            RemoveApplicationLogs(fields[0], fields[1]);
                            for (const auto& [key, profile] : profiles.All())
                                if (profile.package == fields[1] &&
                                    Normalize(profile.data / L"data") == Normalize(fields[0]))
                                    pendingIntents.Forget(profile.token);
                            profiles.Remove(fields[0], fields[1]);
                        }
                        else if (header.operation == Operation::OpenWebUrl) {
                            Check(clientPeer && !stopping, "Only a Client can open web links");
                            auto child = clients.find(peer);
                            auto fields = Split(text);
                            Check(child != clients.end() && !child->second->retiring && fields.size() == 3 &&
                                      fields[1] == child->second->package &&
                                      Normalize(fs::path(fields[0])) ==
                                          Normalize(fs::path(child->second->data) / L"data"),
                                  "Web link Client identity changed");
                            OpenServiceWebUrl(fields[2]);
                            response.process = GetCurrentProcessId();
                        }
                        else if (header.operation == Operation::Activities) {
                            Check(clientPeer && !stopping, "Only a live Client can route Activities");
                            auto child = clients.find(peer);
                            auto fields = Split(text);
                            Check(child != clients.end() && !child->second->retiring && !child->second->starting &&
                                      !child->second->activityScope.empty() && fields.size() >= 2 &&
                                      fields[0] == child->second->package,
                                  "Activity Client identity changed");
                            auto& from = *child->second;
                            response.process = GetCurrentProcessId();
                            if (fields[1] == L"start") {
                                const bool local = fields.size() == 8;
                                Check((fields.size() == 7 || local) && stone::session::IsPackageName(fields[4]) &&
                                          (local ? fields[4] == from.package : fields[4] != from.package) &&
                                          (fields[6] == L"0" || fields[6] == L"1"),
                                      "Invalid Activity launch fields");
                                const auto targetProcess = local ? fields[7] : L"";
                                Check(
                                    !local || targetProcess.empty() ||
                                        (targetProcess.starts_with(from.package + L":") &&
                                         targetProcess.size() > from.package.size() + 1 &&
                                         targetProcess.size() <= 255 &&
                                         targetProcess.substr(from.package.size() + 1)
                                                 .find_first_not_of(
                                                     L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") ==
                                             targetProcess.npos),
                                    "Invalid private Activity process");
                                Check(!local || targetProcess != from.applicationProcess,
                                      "Activity target is the current process");
                                const JobProfile* source = nullptr;
                                for (const auto& [key, profile] : profiles.All())
                                    if (profile.token == from.intentScope) {
                                        source = &profile;
                                        break;
                                    }
                                Check(source != nullptr, "Activity source profile was revoked");
                                source->ValidateForLaunch();
                                Child* target = nullptr;
                                for (const auto& [id, value] : clients)
                                    if (value->data == from.data && value->package == fields[4] &&
                                        value->applicationProcess == targetProcess) {
                                        if (value->retiring) {
                                            Check(WaitForSingleObject(value->process.value, 5000) == WAIT_OBJECT_0,
                                                  "Activity target is still retiring; retry launch");
                                            activities.Drop(id);
                                            clients.erase(id);
                                            break;
                                        }
                                        Check(value->activityScope.empty() ||
                                                  value->activityScope == from.activityScope,
                                              "Activity target uses another image");
                                        target = value.get();
                                        break;
                                    }
                                if (!target) {
                                    const JobProfile* saved = nullptr;
                                    for (const auto& [key, profile] : profiles.All())
                                        if (Normalize(profile.data) == from.data && profile.package == fields[4] &&
                                            Normalize(profile.image) == Normalize(source->image) &&
                                            profile.imageStamp == source->imageStamp) {
                                            saved = &profile;
                                            break;
                                        }
                                    // A previously launched APK can be an explicit --apk input.
                                    // New targets resolve only through the installed package store.
                                    std::vector<std::wstring> arguments{L"--package",      fields[4],
                                                                        L"--data-dir",     from.data,
                                                                        L"--system-image", source->image.wstring(),
                                                                        L"--background"};
                                    if (saved && !saved->apk.empty()) {
                                        saved->ValidateForLaunch();
                                        arguments[0] = L"--apk";
                                        arguments[1] = ExtendedWindowsPath(saved->apk).wstring();
                                    }
                                    auto request = Parse(arguments, directory);
                                    request.package = fields[4];
                                    // The queued route is bound to this exact prepared image.
                                    // Preparing another cache would revoke it when the target registers.
                                    request.command += L" --prepared-system " +
                                                       stone::session::Quote(source->prepared.wstring()) +
                                                       L" --image-stamp " + stone::session::Quote(source->imageStamp);
                                    if (saved && !saved->source.empty()) {
                                        request.source = Normalize(saved->source);
                                        request.command +=
                                            L" --origin-apk " + stone::session::Quote(saved->source.wstring());
                                    }
                                    if (!targetProcess.empty()) {
                                        Check(saved != nullptr, "Private process requires an existing launch profile");
                                        request.command += L" --application-process " +
                                                           stone::session::Quote(targetProcess) +
                                                           L" --expected-package " + stone::session::Quote(fields[4]) +
                                                           L" --wake-token " + stone::session::Quote(saved->token);
                                    }
                                    auto started = Spawn(request, directory);
                                    target = started.get();
                                    started->applicationProcess = targetProcess;
                                    clients.emplace(started->pid, std::move(started));
                                }
                                activities.Bind(target->pid, from.activityScope, fields[4]);
                                output = Pack({activities.Start(peer, target->pid, fields[2], fields[3], fields[5],
                                                                fields[6] == L"1")});
                                Foreground foreground{target->pid, false};
                                EnumWindows(VisitWindow, reinterpret_cast<LPARAM>(&foreground));
                            }
                            else if (fields[1] == L"poll" && fields.size() == 2)
                                output = Pack(activities.Peek(peer));
                            else if (fields[1] == L"claim" && fields.size() == 3)
                                output = Pack({activities.Claim(peer, fields[2]) ? L"1" : L"0"});
                            else if (fields[1] == L"abandon" && fields.size() == 3)
                                activities.Abandon(peer, fields[2]);
                            else {
                                Check(fields[1] == L"finish" && fields.size() == 5 && !fields[3].empty() &&
                                          fields[3].size() <= 11,
                                      "Invalid Activity result fields");
                                size_t end = 0;
                                auto result = std::stoi(fields[3], &end);
                                Check(end == fields[3].size(), "Invalid Activity result code");
                                output = Pack({activities.Finish(peer, fields[2], result, fields[4]) ? L"1" : L"0"});
                            }
                            auto values = Split(output);
                            values.insert(values.begin(), activities.Epoch());
                            output = Pack(values);
                        }
                        else if (header.operation == Operation::ApplicationLogs) {
                            // A retiring Client still drains its final log batch.
                            Check(clientPeer, "Only a Client can access application logs");
                            auto child = clients.find(peer);
                            auto fields = Split(text);
                            Check(child != clients.end() && fields.size() >= 3 && !fields[1].empty() &&
                                      fields[1] == child->second->package &&
                                      Normalize(fs::path(fields[0])) ==
                                          Normalize(fs::path(child->second->data) / L"data"),
                                  "Application log Client identity changed");
                            auto& client = *child->second;
                            if (fields.size() == 3 && fields[2] == L"begin") {
                                client.logSnapshot = ApplicationLogRequest(fields[0], fields[1], {L"dump"});
                                client.logOffset = 0;
                                client.logData = Normalize(fields[0]);
                                client.logPackage = fields[1];
                                client.logReading = !client.logSnapshot.empty();
                                output = std::to_wstring(client.logSnapshot.size());
                            }
                            else if (fields.size() == 3 && fields[2] == L"next") {
                                Check(client.logReading, "Application log snapshot is unavailable");
                                Check(client.logData == Normalize(fields[0]) && client.logPackage == fields[1],
                                      "Application log snapshot identity changed");
                                output = client.logSnapshot.substr(client.logOffset, 24000);
                                client.logOffset += output.size();
                                if (client.logOffset == client.logSnapshot.size()) {
                                    client.logSnapshot.clear();
                                    client.logReading = false;
                                }
                            }
                            else {
                                Check(fields.size() == 4 && fields[2] == L"append",
                                      "Invalid application log operation");
                                ApplicationLogRequest(fields[0], fields[1], {fields.begin() + 2, fields.end()});
                            }
                            response.process = GetCurrentProcessId();
                        }
                        else if (header.operation == Operation::ApplicationIcon) {
                            Check(clientPeer && !stopping, "Only a Client can update an application icon");
                            auto child = clients.find(peer);
                            auto fields = Split(text);
                            Check(child != clients.end() && !child->second->retiring && fields.size() >= 3 &&
                                      Normalize(fields[0]) == child->second->data &&
                                      stone::session::IsPackageName(fields[1]),
                                  "Application icon Client identity changed");
                            auto& upload = child->second->icon;
                            auto number = [](const std::wstring& value) -> size_t {
                                Check(!value.empty() && value.size() <= 10 &&
                                          value.find_first_not_of(L"0123456789") == value.npos,
                                      "Invalid application icon length");
                                return static_cast<size_t>(std::stoull(value));
                            };
                            if (fields.size() == 5 && fields[2] == L"begin") {
                                Check(fields[3].size() <= 1024, "Application icon label is too long");
                                upload.Begin(number(fields[4]));
                                upload.root = Normalize(fields[0]);
                                upload.package = fields[1];
                                upload.label = fields[3];
                            }
                            else {
                                Check(upload.root == Normalize(fields[0]) && upload.package == fields[1],
                                      "Application icon upload identity changed");
                                if (fields.size() == 5 && fields[2] == L"chunk")
                                    upload.Append(number(fields[3]), fields[4]);
                                else {
                                    Check(fields.size() == 3 && fields[2] == L"commit",
                                          "Invalid application icon operation");
                                    auto png = upload.Complete();
                                    // Shell configuration is shared by this broker thread. Re-select
                                    // the requesting profile after interleaved Clients' requests.
                                    auto root = fs::canonical(fields[0]);
                                    const bool own = child->second->package == fields[1];
                                    output = Pack(CacheApplicationIcon(root, fields[1], upload.label, png, !own));
                                }
                            }
                            response.process = GetCurrentProcessId();
                        }
                        else if (header.operation == Operation::Clipboard) {
                            Check(clientPeer && !stopping, "Only a Client can access the clipboard");
                            auto child = clients.find(peer);
                            auto fields = Split(text);
                            Check(child != clients.end() && !child->second->retiring && fields.size() >= 3 &&
                                      fields[1] == child->second->package &&
                                      Normalize(fs::path(fields[0])) ==
                                          Normalize(fs::path(child->second->data) / L"data"),
                                  "Clipboard Client identity changed");
                            if (!clipboard)
                                clipboard = std::make_unique<WindowsClipboard>();
                            output =
                                Pack(clipboard->Exchange(std::vector<std::wstring>(fields.begin() + 2, fields.end())));
                            response.process = GetCurrentProcessId();
                        }
                        else if (header.operation == Operation::Notifications) {
                            Check(clientPeer, "Only a Client can access application notifications");
                            auto child = clients.find(peer);
                            auto fields = Split(text);
                            Check(child != clients.end() && fields.size() >= 3 && fields[1] == child->second->package &&
                                      Normalize(fs::path(fields[0])) ==
                                          Normalize(fs::path(child->second->data) / L"data"),
                                  "Notification Client identity changed");
                            if (fields[2].starts_with(L"pi-")) {
                                Check(!child->second->intentScope.empty() && !child->second->retiring,
                                      "PendingIntent Client profile is unavailable");
                                Check(std::any_of(profiles.All().begin(), profiles.All().end(),
                                                  [&](const auto& entry) {
                                                      return entry.second.token == child->second->intentScope &&
                                                             entry.second.package == fields[1] &&
                                                             Normalize(entry.second.data) == child->second->data;
                                                  }),
                                      "PendingIntent Client profile was revoked");
                                if ((fields[2] == L"pi-delivery" || fields[2] == L"pi-claim") &&
                                    !ServiceNotificationPolicy(fields[0], fields[1]))
                                    pendingIntents.ClearNotifications(child->second->intentScope, true);
                                output = Pack(
                                    ServicePendingIntentRequest(pendingIntents, child->second->intentScope, fields));
                            }
                            else
                                output = Pack(
                                    ServiceNotificationRequest(peer, directory / L"settings/StoneSettings.exe", fields,
                                                               pendingIntents, child->second->intentScope));
                            response.process = GetCurrentProcessId();
                        }
                        else if (header.operation == Operation::ClientStarting) {
                            Check(clientPeer, "Only a Client can register its startup identity");
                            auto fields = Split(text);
                            Check((fields.size() == 3 || fields.size() == 4) && fs::path(fields[0]).is_absolute() &&
                                      (fields[1].empty() || fs::path(fields[1]).is_absolute()) &&
                                      (fields[2].empty() || stone::session::IsPackageName(fields[2])),
                                  "Invalid Client startup identity");
                            const auto processName = fields.size() == 4 ? fields[3] : L"";
                            Check(
                                processName.empty() ||
                                    (processName.starts_with(fields[2] + L":") && processName.size() <= 255 &&
                                     processName.size() > fields[2].size() + 1 &&
                                     processName.substr(fields[2].size() + 1)
                                             .find_first_not_of(
                                                 L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") ==
                                         processName.npos),
                                "Invalid Client application process");
                            auto child = clients.find(peer);
                            if (child == clients.end()) {
                                HANDLE process =
                                    OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peer);
                                Check(process != nullptr, "Cannot track starting Client");
                                child = clients
                                            .emplace(peer, std::make_unique<Child>(process, peer, Normalize(fields[0]),
                                                                                   L"", directory))
                                            .first;
                            }
                            Check(child->second->data == Normalize(fields[0]) &&
                                      (fields[2].empty() || child->second->package.empty() ||
                                       child->second->package == fields[2]),
                                  "Client startup identity changed");
                            Check(child->second->applicationProcess == processName ||
                                      (child->second->intentScope.empty() && child->second->activityScope.empty()),
                                  "Client process identity changed");
                            child->second->applicationProcess = processName;
                            child->second->source = fields[1].empty() ? L"" : Normalize(fields[1]);
                            if (!fields[2].empty())
                                child->second->package = fields[2];
                            child->second->starting = true;
                        }
                        else if (header.operation == Operation::ClientProfile) {
                            Check(clientPeer, "Only a Client can register an ART launch profile");
                            auto child = clients.find(peer);
                            Check(child != clients.end(), "Client is not managed by this service");
                            auto fields = Split(text);
                            Check((fields.size() == 8 || fields.size() == 9) &&
                                      Normalize(fields[0]) == child->second->data &&
                                      (child->second->package.empty() || child->second->package == fields[5]),
                                  "Client launch identity changed");
                            const auto process = fields.size() == 9 ? fields.back() : L"";
                            Check(process == child->second->applicationProcess, "Client application process changed");
                            if (fields.size() == 9) {
                                Check(!fields[7].empty(), "Private process launch was not authorized");
                                fields.pop_back();
                            }
                            auto profile = profiles.Register(fields);
                            std::vector<std::wstring> scopes;
                            for (const auto& [key, saved] : profiles.All()) {
                                scopes.push_back(saved.token);
                                if (saved.token != profile.token && saved.package == profile.package &&
                                    Normalize(saved.data) == Normalize(profile.data))
                                    pendingIntents.ClearNotifications(saved.token, true);
                            }
                            pendingIntents.Retain(scopes);
                            child->second->intentScope = profile.token;
                            child->second->activityScope = Normalize(profile.data) + L"\n" +
                                                           Normalize(profile.prepared / L"system") + L"\n" +
                                                           profile.imageStamp;
                            activities.Bind(peer, child->second->activityScope, profile.package);
                            child->second->package = profile.package;
                            child->second->starting = false;
                            child->second->source = profile.source.empty() ? L"" : Normalize(profile.source);
                            if (process.empty())
                                try {
                                    ConfigureShellIntegration(directory / L"StoneService.exe", profile.data,
                                                              profile.image);
                                    RegisterApplicationLaunch(profile.package, profile.apk.empty() ? fs::path{}
                                                                               : profile.source.empty()
                                                                                   ? profile.apk
                                                                                   : profile.source);
                                }
                                catch (const std::exception& error) {
                                    std::fprintf(stderr, "[shell] application launch registration failed: %s\n",
                                                 error.what());
                                }
                            response.process = GetCurrentProcessId();
                            output = profile.token;
                        }
                        else if (header.operation == Operation::Jobs) {
                            Check(clientPeer, "Only a Client can manage Android jobs");
                            auto fields = Split(text);
                            Check(fields.size() >= 4, "Missing job scope");
                            output = Pack(ServiceJobRequest(peer, fields, ReadServiceHostState(), JobClockMillis(),
                                                            ReadServiceJobState(fs::canonical(fields[0]))));
                            auto child = clients.find(peer);
                            if (fields[3] == L"poll" && child != clients.end() && !child->second->wakeKey.empty())
                                wakes[child->second->wakeKey] = {};
                        }
                        else if (header.operation == Operation::RetireClient) {
                            Check(clientPeer && text.empty(), "Invalid Client retirement");
                            auto client = clients.find(peer);
                            Check(client != clients.end(), "Client is not managed by this service");
                            auto& child = *client->second;
                            bool idle = !child.signals.ActivationPending() && !activities.Busy(peer) &&
                                        !pendingIntents.Delivery(child.intentScope) &&
                                        ServiceJobClientIdle(peer, ReadServiceHostState(), JobClockMillis(),
                                                             ReadServiceJobState(fs::path(child.data) / L"data"));
                            output = idle ? L"1" : L"0";
                        }
                        else if (header.operation == Operation::SystemAwake) {
                            Check(clientPeer && (text == L"0" || text == L"1"), "Invalid Client power request");
                            SetServiceSystemAwake(peer, text == L"1");
                            response.process = GetCurrentProcessId();
                        }
                        else if (header.operation == Operation::NotificationIdentity) {
                            auto fields = Split(text);
                            Check(fields.size() == 3 && (fields[2] == L"0" || fields[2] == L"1"),
                                  "Invalid notification identity fields");
                            auto settings = fs::canonical(directory / L"settings/StoneSettings.exe");
                            Check(fs::equivalent(settings, fs::path(fields[0])),
                                  "Notification settings must belong to this installation");
                            auto diagnostic = fields[1].empty() ? fs::path{} : fs::canonical(fields[1]);
                            if (fields[2] == L"1")
                                RegisterNotificationIdentity(settings, diagnostic);
                            else
                                RemoveDiagnosticNotificationIdentity(settings, diagnostic);
                        }
                        else if (header.operation == Operation::ShellProfile ||
                                 header.operation == Operation::ShellShortcut) {
                            auto fields = Split(text);
                            const bool starting = header.operation == Operation::ShellProfile && clientPeer &&
                                                  fields.size() == 3 && fields[2] == L"starting";
                            Check(fields.size() == (header.operation == Operation::ShellProfile ? 2 : 5) || starting,
                                  "Invalid shell service fields");
                            auto data = fs::canonical(fields[0]), image = fs::canonical(fields[1]);
                            Check(fs::is_directory(data) && fs::is_regular_file(image), "Invalid shell profile paths");
                            if (fields.size() == 5)
                                Check(stone::session::IsPackageName(fields[2]) && fields[3].size() <= 1024 &&
                                          (fields[4] == L"0" || fields[4] == L"1"),
                                      "Invalid shell shortcut identity");
                            ConfigureShellIntegration(directory / L"StoneService.exe", data, image);
                            if (fields.size() == 5)
                                UpdateApplicationShortcut(fields[2], fields[3], fields[4] == L"1");
                            if (clientPeer) {
                                auto client = clients.find(peer);
                                if (client == clients.end()) {
                                    HANDLE process =
                                        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peer);
                                    Check(process != nullptr, "Cannot track Client process");
                                    clients.emplace(
                                        peer, std::make_unique<Child>(process, peer, Normalize(data), L"", directory));
                                }
                                else
                                    client->second->data = Normalize(data);
                                if (starting)
                                    clients.at(peer)->starting = true;
                            }
                        }
                        else if (header.operation == Operation::Launch) {
                            Check(!stopping, "Service is stopping");
                            auto request = Parse(Split(text), directory);
                            Child* existing = nullptr;
                            for (const auto& [id, client] : clients)
                                if (client->applicationProcess.empty() && client->data == request.data &&
                                    ((!request.package.empty() && client->package == request.package) ||
                                     (!request.source.empty() && client->source == request.source))) {
                                    existing = client.get();
                                    break;
                                }
                            if (existing && existing->retiring) {
                                // Retirement is granted only after job IPC has drained.
                                // Wait for its data lock to close before a fresh launch.
                                const auto id = existing->pid;
                                Check(WaitForSingleObject(existing->process.value, 5000) == WAIT_OBJECT_0,
                                      "Client is still retiring; retry launch");
                                clients.erase(id);
                                existing = nullptr;
                            }
                            if (existing) {
                                if (!request.background) {
                                    existing->signals.Activate();
                                    Foreground foreground{existing->pid, false};
                                    EnumWindows(VisitWindow, reinterpret_cast<LPARAM>(&foreground));
                                }
                                response.process = existing->pid;
                            }
                            else {
                                auto client = Spawn(request, directory);
                                response.process = client->pid;
                                clients.emplace(client->pid, std::move(client));
                            }
                        }
                        else if (header.operation == Operation::Register) {
                            PackageUri(L"stone://" + text);
                            auto client = clients.find(peer);
                            Check(client != clients.end(), "Client is not managed by this service");
                            client->second->package = text;
                            client->second->starting = false;
                        }
                        else if (header.operation == Operation::Status)
                            output = Status(GetCurrentProcessId(), clients);
                        else if (header.operation == Operation::Stop) {
                            // Continue serving job completions while Clients shut down.
                            beginStop();
                            prune();
                        }
                        else
                            throw std::invalid_argument("Unknown service request");
                        response.result = 0;
                    }
                    catch (const std::exception& failure) {
                        if (header.operation == Operation::ClientProfile)
                            std::fprintf(stderr, "[jobs] launch profile rejected for Client %lu: %s\n", peer,
                                         failure.what());
                        if (header.operation == Operation::Activities)
                            std::fprintf(stderr, "[activities] request rejected for Client %lu: %s\n", peer,
                                         failure.what());
                        const std::string message(failure.what());
                        output.assign(message.begin(), message.end());
                    }
                response.length = static_cast<uint32_t>(output.size());
                Transfer(pipe.value, &response, sizeof(response), true);
                if (!output.empty())
                    Transfer(pipe.value, output.data(), static_cast<DWORD>(output.size() * sizeof(wchar_t)), true);
                unsigned char acknowledged = 0;
                if (Transfer(pipe.value, &acknowledged, 1, false) && acknowledged == 0xac &&
                    header.operation == Operation::RetireClient && response.result == 0 && output == L"1") {
                    // Request() returns success only after sending this ACK. A
                    // failed exchange must leave the live Client activatable.
                    auto client = clients.find(peer);
                    if (client != clients.end())
                        client->second->retiring = true;
                }
            }
        }
        DisconnectNamedPipe(pipe.value);
        prune();
    }
    try {
        MaintainServiceJobs(JobClockMillis());
    }
    catch (const std::exception& failure) {
        std::fprintf(stderr, "[jobs] shutdown maintenance failed: %s\n", failure.what());
    }
    StopServicePowerRequests();
    StopServiceNotifications();
    return 0;
}

void Print(const std::wstring& text)
{
    int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), length, nullptr, nullptr);
    bytes += '\n';
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}
}

int RunService(const std::vector<std::wstring>& arguments, const fs::path& directory)
{
    Identity identity(directory);
    if (arguments == std::vector<std::wstring>{L"--profile-store"}) {
        Print(JobProfileStorePath(directory).wstring());
        return 0;
    }
    if (arguments == std::vector<std::wstring>{L"--serve"})
        return Serve(identity, directory);
    if (arguments.size() == 3 && arguments[0] == L"--serve" && arguments[1] == L"--service-stop") {
        Handle stop(OpenEventW(SYNCHRONIZE, FALSE, arguments[2].c_str()));
        Check(stop.value, "Cannot open SCM broker shutdown event");
        return Serve(identity, directory, stop.value);
    }
    if (arguments.empty() || arguments == std::vector<std::wstring>{L"--help"}) {
        Print(
            L"StoneService --service (Windows SCM only) | --serve (development broker)\n"
            L"StoneService /launch stone://<packageName>\n"
            L"StoneService --apk <APK> | --uri stone://<packageName> [--data-dir <directory>] [--system-image <image>] [--enable-jit] [--enable-aot]\n"
            L"StoneService --status | --stop-service | --host-state [--wifi]\nStoneClient owns each application's ART runtime and window.");
        return 0;
    }
    Operation operation = Operation::Launch;
    if (arguments == std::vector<std::wstring>{L"--status"})
        operation = Operation::Status;
    else if (arguments == std::vector<std::wstring>{L"--host-state"} ||
             arguments == std::vector<std::wstring>{L"--host-state", L"--wifi"})
        operation = Operation::HostState;
    else if (arguments == std::vector<std::wstring>{L"--stop-service"})
        operation = Operation::Stop;
    else if (arguments.size() == 3 && arguments[0] == L"--shell-profile")
        operation = Operation::ShellProfile;
    else if (arguments.size() == 6 && arguments[0] == L"--shell-shortcut")
        operation = Operation::ShellShortcut;
    else if (arguments.size() == 4 && arguments[0] == L"--notification-identity")
        operation = Operation::NotificationIdentity;
    else if (arguments.size() == 3 && arguments[0] == L"--notification-activate")
        operation = Operation::NotificationActivate;
    else
        Parse(arguments, directory); // Reject malformed paths and URI arguments before starting a service.
    std::wstring command = L"StoneClient";
    for (const auto& argument : arguments)
        command += L" " + stone::session::Quote(argument);
    if (operation == Operation::HostState)
        command = arguments.size() == 2 ? L"wifi-v1" : L"";
    if (operation == Operation::ShellProfile || operation == Operation::ShellShortcut ||
        operation == Operation::NotificationIdentity || operation == Operation::NotificationActivate)
        command = Pack(std::vector<std::wstring>(arguments.begin() + 1, arguments.end()));
    Response response{};
    std::wstring output;
    if (operation == Operation::NotificationActivate) {
        // Start through a read-only operation. Never automatically replay a
        // reusable notification click after an uncertain acceptance response.
        RequestWithStart(directory, Operation::HostState, L"", response, output, true);
        response = {};
        output.clear();
        Check(Request(identity, directory, operation, command, response, output),
              "Notification acceptance response was lost; request was not replayed");
    }
    else
        RequestWithStart(directory, operation, command, response, output,
                         operation == Operation::Launch || operation == Operation::ShellProfile ||
                             operation == Operation::ShellShortcut || operation == Operation::NotificationIdentity ||
                             operation == Operation::NotificationActivate);
    if (!output.empty())
        Print(output);
    else if (!response.result)
        Print(L"{\"client\":" + std::to_wstring(response.process) + L"}");
    return static_cast<int>(response.result);
}
}
