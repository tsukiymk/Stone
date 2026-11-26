#include "ServiceHost.h"
#include "../ipc/ServiceProtocol.h"
#include "../host/CommandLine.h"
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <bcrypt.h>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace stone::windows {
namespace {
using namespace ipc;
std::filesystem::path installation;
SERVICE_STATUS_HANDLE statusHandle = nullptr;
Handle stopEvent;

void Report(DWORD state, DWORD error = NO_ERROR, DWORD checkpoint = 0)
{
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = error;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwCheckPoint = checkpoint;
    status.dwWaitHint = checkpoint ? 30000 : 0;
    SetServiceStatus(statusHandle, &status);
}

void Log(const std::wstring& message)
{
    // The SCM host must not write to a user-controlled directory as LocalSystem.
    auto source = RegisterEventSourceW(nullptr, ServiceName);
    if (source) {
        const wchar_t* text = message.c_str();
        ReportEventW(source, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, &text, nullptr);
        DeregisterEventSource(source);
    }
}

DWORD WINAPI Control(DWORD control, DWORD, void*, void*)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        // Only ServiceMain publishes status, so RUNNING cannot overwrite a stop.
        SetEvent(stopEvent.value);
        return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

struct BrokerProcess {
    Handle process, stop;
    LUID login{};
    ULONGLONG stoppingAt = 0;
};

std::unique_ptr<BrokerProcess> Spawn(DWORD session, HANDLE token, LUID login, HANDLE job)
{
    auto broker = std::make_unique<BrokerProcess>();
    broker->login = login;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    Check(GetTokenInformation(token, TokenUser, user.data(), size, &size), "Cannot read session user");
    PWSTR sidText = nullptr;
    Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sidText),
          "Cannot identify session user");
    std::unique_ptr<void, decltype(&LocalFree)> sid(sidText, LocalFree);
    const auto securityText = std::wstring(L"D:P(A;;GA;;;SY)(A;;0x00100000;;;") + sidText + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(securityText.c_str(), SDDL_REVISION_1, &descriptor,
                                                               nullptr),
          "Cannot secure broker shutdown event");
    std::unique_ptr<void, decltype(&LocalFree)> security(descriptor, LocalFree);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    std::array<unsigned char, 16> random{};
    Check(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >=
              0,
          "Cannot identify broker shutdown event");
    std::wstring eventName = L"Global\\Stone.Service.Stop.";
    for (auto byte : random) {
        eventName += L"0123456789abcdef"[byte >> 4];
        eventName += L"0123456789abcdef"[byte & 15];
    }
    broker->stop.value = CreateEventW(&attributes, TRUE, FALSE, eventName.c_str());
    Check(broker->stop.value && GetLastError() != ERROR_ALREADY_EXISTS, "Cannot create broker shutdown event");

    void* environment = nullptr;
    Check(CreateEnvironmentBlock(&environment, token, FALSE), "Cannot create session environment");
    std::unique_ptr<void, decltype(&DestroyEnvironmentBlock)> block(environment, DestroyEnvironmentBlock);
    const auto executable = installation / L"StoneService.exe";
    auto command =
        stone::session::Quote(executable.wstring()) + L" --serve --service-stop " + stone::session::Quote(eventName);
    STARTUPINFOW startup{sizeof(startup)};
    wchar_t desktop[] = L"winsta0\\default";
    startup.lpDesktop = desktop;
    PROCESS_INFORMATION process{};
    Check(CreateProcessAsUserW(token, executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, environment,
                               installation.c_str(), &startup, &process),
          "Cannot start session broker");
    broker->process.value = process.hProcess;
    Handle thread(process.hThread);
    // The job owns brokers only. Their Clients silently break away and retain
    // their existing graceful shutdown and persistent-job completion paths.
    if (!AssignProcessToJobObject(job, process.hProcess) || ResumeThread(thread.value) == DWORD(-1)) {
        TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
        throw std::runtime_error("Cannot supervise session broker");
    }
    std::fprintf(stderr, "[service] session %lu broker %lu started\n", session, process.dwProcessId);
    return broker;
}

void Stop(BrokerProcess& broker)
{
    if (!broker.stoppingAt) {
        broker.stoppingAt = GetTickCount64();
        SetEvent(broker.stop.value);
    }
}

void Supervise()
{
    Handle job(CreateJobObjectW(nullptr, nullptr));
    Check(job.value, "Cannot create broker supervision job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    Check(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
          "Cannot configure broker supervision job");
    std::map<DWORD, std::unique_ptr<BrokerProcess>> brokers;
    std::map<DWORD, ULONGLONG> retryAt;
    DWORD enumerationError = ERROR_SUCCESS;
    Report(SERVICE_RUNNING);
    while (WaitForSingleObject(stopEvent.value, 0) == WAIT_TIMEOUT) {
        PWTS_SESSION_INFOW sessions = nullptr;
        DWORD count = 0;
        if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
            const auto error = GetLastError();
            if (enumerationError != error)
                Log(L"Cannot enumerate Windows sessions (Win32 " + std::to_wstring(error) + L"). Retrying.");
            enumerationError = error;
            WaitForSingleObject(stopEvent.value, 1000);
            continue; // Session services may not be ready during system startup.
        }
        enumerationError = ERROR_SUCCESS;
        std::unique_ptr<void, decltype(&WTSFreeMemory)> list(sessions, WTSFreeMemory);
        std::set<DWORD> present;
        const auto now = GetTickCount64();
        for (DWORD index = 0; index < count; ++index) {
            const auto& session = sessions[index];
            if (!session.SessionId ||
                (session.State != WTSActive && session.State != WTSDisconnected && session.State != WTSConnected))
                continue;
            present.insert(session.SessionId);
            auto existing = brokers.find(session.SessionId);
            if (existing != brokers.end() && WaitForSingleObject(existing->second->process.value, 0) == WAIT_OBJECT_0) {
                brokers.erase(existing);
                existing = brokers.end();
            }
            if (now < retryAt[session.SessionId])
                continue;
            retryAt[session.SessionId] = now + 5000;
            HANDLE userToken = nullptr;
            if (!WTSQueryUserToken(session.SessionId, &userToken))
                continue; // Sessions can be enumerated before logon has finished.
            Handle token(userToken);
            TOKEN_STATISTICS statistics{};
            DWORD size = 0;
            if (!GetTokenInformation(token.value, TokenStatistics, &statistics, sizeof(statistics), &size))
                continue;
            if (existing != brokers.end()) {
                const auto login = existing->second->login;
                if (login.LowPart != statistics.AuthenticationId.LowPart ||
                    login.HighPart != statistics.AuthenticationId.HighPart)
                    Stop(*existing->second); // Session IDs can be reused after logoff.
                continue;
            }
            try {
                brokers.emplace(session.SessionId,
                                Spawn(session.SessionId, token.value, statistics.AuthenticationId, job.value));
            }
            catch (const std::exception& error) {
                const auto code = GetLastError();
                const std::string message(error.what());
                Log(L"Session " + std::to_wstring(session.SessionId) + L": " +
                    std::wstring(message.begin(), message.end()) + L" (Win32 " + std::to_wstring(code) + L")");
            }
        }
        for (auto it = brokers.begin(); it != brokers.end();) {
            auto& broker = *it->second;
            if (!present.contains(it->first))
                Stop(broker);
            if (broker.stoppingAt && now >= broker.stoppingAt + 25000)
                TerminateProcess(broker.process.value, ERROR_TIMEOUT);
            if (WaitForSingleObject(broker.process.value, 0) == WAIT_OBJECT_0)
                it = brokers.erase(it);
            else
                ++it;
        }
        WaitForSingleObject(stopEvent.value, 1000);
    }
    Report(SERVICE_STOP_PENDING, NO_ERROR, 1);
    for (auto& [session, broker] : brokers)
        Stop(*broker);
    const auto deadline = GetTickCount64() + 25000;
    DWORD checkpoint = 2;
    while (!brokers.empty() && GetTickCount64() < deadline) {
        for (auto it = brokers.begin(); it != brokers.end();) {
            if (WaitForSingleObject(it->second->process.value, 0) == WAIT_OBJECT_0)
                it = brokers.erase(it);
            else
                ++it;
        }
        Report(SERVICE_STOP_PENDING, NO_ERROR, checkpoint++);
        if (!brokers.empty())
            Sleep(250);
    }
    if (!brokers.empty()) {
        TerminateJobObject(job.value, ERROR_TIMEOUT);
        for (const auto& [session, broker] : brokers)
            WaitForSingleObject(broker->process.value, 1000);
    }
    // Closing the job also cleans up brokers after a host crash.
}

void WINAPI ServiceMain(DWORD, PWSTR*)
{
    statusHandle = RegisterServiceCtrlHandlerExW(ServiceName, Control, nullptr);
    if (!statusHandle)
        return;
    Report(SERVICE_START_PENDING, NO_ERROR, 1);
    DWORD result = NO_ERROR;
    try {
        stopEvent.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(stopEvent.value, "Cannot create service shutdown event");
        Supervise();
    }
    catch (const std::exception& error) {
        const std::string message(error.what());
        Log(std::wstring(message.begin(), message.end()));
        result = ERROR_EXCEPTION_IN_SERVICE;
    }
    Report(SERVICE_STOPPED, result);
}
}

int RunWindowsService(const std::filesystem::path& directory)
{
    installation = directory;
    SERVICE_TABLE_ENTRYW table[]{{const_cast<PWSTR>(ipc::ServiceName), ServiceMain}, {nullptr, nullptr}};
    if (StartServiceCtrlDispatcherW(table))
        return 0;
    return static_cast<int>(GetLastError());
}
}
