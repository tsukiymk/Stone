#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include "../../../src/windows/client/BrokerClient.h"
#include "../../../src/windows/ipc/ServiceProtocol.h"
#include "../../../src/windows/ipc/ClientSignals.h"
#include "../../../src/windows/host/CommandLine.h"
#include "../../../src/windows/host/shell/WebLinks.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <atomic>
#include <latch>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <memory>
#include <algorithm>

using namespace stone::windows;
using namespace stone::windows::ipc;
namespace fs = std::filesystem;

namespace {
void Require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

void ServiceCommand(const fs::path& directory, const std::wstring& arguments, DWORD expected = 0)
{
    const auto executable = directory / L"StoneService.exe";
    auto command = stone::session::Quote(executable.wstring()) + L" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           directory.c_str(), &startup, &process),
            "Cannot request test service shutdown");
    CloseHandle(process.hThread);
    Handle child(process.hProcess);
    Require(WaitForSingleObject(child.value, 15000) == WAIT_OBJECT_0, "Test shutdown command timed out");
    DWORD result = 1;
    Require(GetExitCodeProcess(child.value, &result) && result == expected, "Test service command failed");
}

void StopService(const fs::path& directory)
{
    ServiceCommand(directory, L"--stop-service");
}

void ManagedBroker(const fs::path& directory)
{
    ServiceCommand(directory, L"--service", ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
    ServiceCommand(directory, L"--serve --service-stop Local\\Stone.Missing." + std::to_wstring(GetCurrentProcessId()),
                   1);
    const auto event = L"Local\\Stone.ServiceTest.Stop." + std::to_wstring(GetCurrentProcessId());
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, event.c_str()));
    Require(stop.value != nullptr, "Cannot create managed broker shutdown event");
    const auto executable = directory / L"StoneService.exe";
    auto command =
        stone::session::Quote(executable.wstring()) + L" --serve --service-stop " + stone::session::Quote(event);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           directory.c_str(), &startup, &process),
            "Cannot start managed broker");
    CloseHandle(process.hThread);
    Handle broker(process.hProcess);
    try {
        Identity identity(directory);
        Response response{};
        std::wstring output;
        const auto deadline = GetTickCount64() + 15000;
        while (!Request(identity, directory, Operation::HostState, L"", response, output)) {
            Require(WaitForSingleObject(broker.value, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline,
                    "Managed broker failed to become ready");
            Sleep(20);
        }
        Require(response.result == 0 && response.process == process.dwProcessId,
                "Managed broker did not provide the user-session endpoint");
        SetEvent(stop.value);
        Require(WaitForSingleObject(broker.value, 10000) == WAIT_OBJECT_0,
                "Managed broker ignored supervisor shutdown");
        DWORD result = 1;
        Require(GetExitCodeProcess(broker.value, &result) && result == 0, "Managed broker failed graceful shutdown");
    }
    catch (...) {
        SetEvent(stop.value);
        if (WaitForSingleObject(broker.value, 10000) != WAIT_OBJECT_0)
            TerminateProcess(broker.value, ERROR_TIMEOUT);
        throw;
    }
}

template <class Predicate> void Eventually(Predicate predicate, const char* message)
{
    const auto deadline = GetTickCount64() + 20000;
    while (!predicate()) {
        Require(GetTickCount64() < deadline, message);
        Sleep(20);
    }
}

struct PowerChild {
    Handle stop, process;

    PowerChild(const fs::path& directory, int number)
    {
        const auto event =
            L"Local\\Stone.PowerTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(number);
        stop.value = CreateEventW(nullptr, TRUE, FALSE, event.c_str());
        Require(stop.value != nullptr, "Cannot create power test event");
        auto executable = directory / L"StoneClient.exe";
        auto command = stone::session::Quote(executable.wstring()) + L" --power-child " +
                       stone::session::Quote(directory.wstring()) + L" " + stone::session::Quote(event);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                               directory.c_str(), &startup, &child),
                "Cannot start power test Client");
        CloseHandle(child.hThread);
        process.value = child.hProcess;
    }

    void Exit()
    {
        SetEvent(stop.value);
        Require(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0, "Power test Client did not exit");
    }

    ~PowerChild()
    {
        SetEvent(stop.value);
        if (process.value)
            WaitForSingleObject(process.value, 5000);
    }
};

void WifiReference()
{
    HANDLE handle = nullptr;
    DWORD negotiated = 0;
    WLAN_INTERFACE_INFO_LIST* list = nullptr;
    const auto close = [&](void*) {
        if (list)
            WlanFreeMemory(list);
        if (handle)
            WlanCloseHandle(handle, nullptr);
    };
    const std::unique_ptr<void, decltype(close)> cleanup(reinterpret_cast<void*>(1), close);
    std::vector<NET_IFINDEX> connected;
    if (WlanOpenHandle(2, nullptr, &negotiated, &handle) == ERROR_SUCCESS &&
        WlanEnumInterfaces(handle, nullptr, &list) == ERROR_SUCCESS) {
        Require(list->dwNumberOfItems <= 128, "Invalid Windows WLAN reference count");
        for (DWORD at = 0; at < list->dwNumberOfItems; ++at) {
            const auto& info = list->InterfaceInfo[at];
            if (info.isState != wlan_interface_state_connected &&
                info.isState != wlan_interface_state_ad_hoc_network_formed)
                continue;
            NET_LUID luid{};
            NET_IFINDEX index = 0;
            if (ConvertInterfaceGuidToLuid(&info.InterfaceGuid, &luid) == NO_ERROR &&
                ConvertInterfaceLuidToIndex(&luid, &index) == NO_ERROR)
                connected.push_back(index);
        }
    }
    Eventually(
        [] {
            return ReadClientHostState().wifi.samples > 0;
        },
        "Service WLAN polling did not run");
    if (connected.empty()) {
        Eventually(
            [] {
                return !ReadClientHostState().wifi.connected;
            },
            "Service invented a connected WLAN adapter");
        return;
    }
    // A separate IP Helper API supplies the reference IPv4 address/netmask;
    // production samples GetAdaptersAddresses instead.
    ULONG bytes = 0;
    Require(GetIpAddrTable(nullptr, &bytes, FALSE) == ERROR_INSUFFICIENT_BUFFER,
            "Cannot size the Windows IPv4 reference");
    std::vector<uint8_t> storage(bytes);
    auto* table = reinterpret_cast<MIB_IPADDRTABLE*>(storage.data());
    Require(GetIpAddrTable(table, &bytes, FALSE) == NO_ERROR, "Cannot read the Windows IPv4 reference");
    Eventually(
        [&] {
            const auto wifi = ReadClientHostState().wifi;
            if (wifi.state != 3 || !wifi.connected ||
                std::find(connected.begin(), connected.end(), wifi.interfaceIndex) == connected.end())
                return false;
            bool hasIpv4 = false;
            for (DWORD at = 0; at < table->dwNumEntries; ++at) {
                if (table->table[at].dwIndex != wifi.interfaceIndex || !table->table[at].dwAddr)
                    continue;
                hasIpv4 = true;
                if (table->table[at].dwAddr == wifi.ipv4 && table->table[at].dwMask == wifi.netmask)
                    return true;
            }
            return !hasIpv4 && wifi.ipv4 == 0; // A real associated IPv6-only interface is allowed.
        },
        "Service WLAN snapshot does not match the independent Windows interface/address reference");
}

void Interfaces()
{
    Eventually(
        [] {
            const auto value = ReadClientHostState().interfaces;
            return value && value->samples && !value->error;
        },
        "Service interface polling failed");
    const auto value = ReadClientHostState().interfaces;
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    Require(GetUnicastIpAddressTable(AF_UNSPEC, &table) == NO_ERROR, "Cannot read independent unicast reference");
    const std::unique_ptr<void, decltype(&FreeMibTable)> cleanup(table, &FreeMibTable);
    bool loopback = false;
    for (const auto& entry : value->entries) {
        MIB_IF_ROW2 row{};
        row.InterfaceIndex = entry.index;
        Require(GetIfEntry2(&row) == NO_ERROR, "Interface index differs from Windows reference");
        MIB_IPINTERFACE_ROW ip{};
        ip.InterfaceIndex = entry.index;
        ip.Family = AF_INET;
        auto code = GetIpInterfaceEntry(&ip);
        if (code != NO_ERROR) {
            ip = {};
            ip.InterfaceIndex = entry.index;
            ip.Family = AF_INET6;
            code = GetIpInterfaceEntry(&ip);
        }
        const bool withoutIp = code == ERROR_NOT_FOUND && entry.addresses.empty() && entry.mtu == row.Mtu;
        Require(withoutIp || (code == NO_ERROR && ip.NlMtu == entry.mtu),
                ("Interface IP MTU differs from independent Windows reference: " + entry.name +
                 " index=" + std::to_string(entry.index) + " cached=" + std::to_string(entry.mtu) +
                 " reference=" + std::to_string(ip.NlMtu) + " error=" + std::to_string(code))
                    .c_str());
        Require(((entry.flags & 1) != 0) == (row.AdminStatus == NET_IF_ADMIN_STATUS_UP) &&
                    ((entry.flags & 0x40) != 0) == (row.OperStatus == IfOperStatusUp),
                "Interface status differs from Windows reference");
        for (const auto& address : entry.addresses) {
            bool found = false;
            for (ULONG at = 0; at < table->NumEntries; ++at) {
                const auto& other = table->Table[at];
                if (other.InterfaceIndex != entry.index)
                    continue;
                const bool v4 = address.family == 2;
                if (other.Address.si_family != (v4 ? AF_INET : AF_INET6) || other.OnLinkPrefixLength != address.prefix)
                    continue;
                const void* bytes =
                    v4 ? static_cast<const void*>(&other.Address.Ipv4.sin_addr) : &other.Address.Ipv6.sin6_addr;
                if (std::memcmp(address.bytes.data(), bytes, v4 ? 4 : 16) == 0 &&
                    (v4 || address.scope == other.Address.Ipv6.sin6_scope_id))
                    found = true;
            }
            Require(found, "Interface address/prefix/scope differs from independent Windows reference");
            if ((entry.flags & 8) && address.family == 2 && address.bytes[0] == 127)
                loopback = true;
        }
    }
    Require(loopback, "Service omitted the real Windows IPv4 loopback");
}

void WebLinks()
{
    for (const auto* url :
         {L"https://github.com/al54/Bad-Pixels/issues", L"http://localhost:8080/path?q=hello%20world#part",
          L"HTTPS://example.com/", L"https://[::1]:8443/", L"https://example.com/\u4e2d\u6587"})
        ValidateWebUrl(url);
    for (const auto& url :
         {std::wstring{}, std::wstring(L"file:///C:/Windows/notepad.exe"), std::wstring(L"javascript:alert(1)"),
          std::wstring(L"ms-settings:appsfeatures"), std::wstring(L"C:\\Windows\\notepad.exe"),
          std::wstring(L"//example.com"), std::wstring(L"https:example.com"), std::wstring(L"https:///example.com"),
          std::wstring(L"https://"), std::wstring(L"https://user:password@example.com/"),
          std::wstring(L"https://@example.com/"), std::wstring(L"https://example.com:bad/"),
          std::wstring(L"https://example.com/a b"), std::wstring(L"https://example.com/\nnext"),
          std::wstring(L"https://example.com/\\path"), std::wstring(L"https://example.com/\"quoted"),
          std::wstring(L"https://example.com/\0hidden", 27), std::wstring(8193, L'a')}) {
        bool rejected = false;
        try {
            ValidateWebUrl(url);
        }
        catch (const std::exception&) {
            rejected = true;
        }
        Require(rejected, "Invalid or non-web address accepted by Windows browser policy");
    }
}

int RetirementChild(const fs::path& directory, const fs::path& profile)
{
    ConfigureClientService(directory / L"StoneService.exe");
    InitializeClientServices();
    InitializeClientSession();
    RegisterBrokerClientPackage(L"com.example.retirement");
    auto clipboard = ClientClipboardRequest({(profile / L"data").wstring(), L"com.example.retirement", L"sequence"});
    Require(clipboard.size() == 1 && !clipboard[0].empty() &&
                clipboard[0].find_first_not_of(L"0123456789") == clipboard[0].npos,
            "Managed Client could not query clipboard sequence");
    auto description = ClientClipboardRequest({(profile / L"data").wstring(), L"com.example.retirement", L"describe"});
    Require(description.size() == 6 && description[4].empty() && (description[1] == L"0" || description[1] == L"1"),
            "Clipboard availability query exposed text or returned invalid metadata");
    {
        Identity identity(directory);
        Response response{};
        std::wstring output;
        Require(Request(identity, directory, Operation::ApplicationLogs,
                        Pack({(profile / L"data").wstring(), L"com.example.retirement", L"append",
                              L"real test Client log\n"}),
                        response, output) &&
                    response.result == 0,
                "Registered Client could not append its application log");
        Require(Request(identity, directory, Operation::ApplicationLogs,
                        Pack({(profile / L"data").wstring(), L"com.example.retirement", L"begin"}), response, output) &&
                    response.result == 0 && !output.empty(),
                "Registered Client could not snapshot its application log");
        Require(Request(identity, directory, Operation::ApplicationLogs,
                        Pack({(profile / L"data").wstring(), L"com.example.retirement", L"next"}), response, output) &&
                    response.result == 0 && output.find(L"real test Client log\n") != output.npos,
                "Registered Client could not read its application log");
        for (int i = 0; i < 6; ++i)
            Require(Request(identity, directory, Operation::ApplicationLogs,
                            Pack({(profile / L"data").wstring(), L"com.example.retirement", L"append",
                                  std::wstring(7999, L'a') + L"\n"}),
                            response, output) &&
                        response.result == 0,
                    "Client could not append a bounded log batch");
        Require(Request(identity, directory, Operation::ApplicationLogs,
                        Pack({(profile / L"data").wstring(), L"com.example.retirement", L"begin"}), response, output) &&
                    response.result == 0,
                "Client could not begin a paged log snapshot");
        const auto snapshotSize = std::stoul(output);
        std::wstring snapshot;
        auto later = L"later-log-" + std::to_wstring(GetTickCount64()) + L"\n";
        Require(Request(identity, directory, Operation::ApplicationLogs,
                        Pack({(profile / L"data").wstring(), L"com.example.retirement", L"append", later}), response,
                        output) &&
                    response.result == 0,
                "Client could not append during log export");
        while (snapshot.size() < snapshotSize) {
            Require(Request(identity, directory, Operation::ApplicationLogs,
                            Pack({(profile / L"data").wstring(), L"com.example.retirement", L"next"}), response,
                            output) &&
                        response.result == 0 && !output.empty() && output.size() <= 24000,
                    "Invalid paged application log snapshot");
            snapshot += output;
        }
        Require(snapshot.size() == snapshotSize && snapshot.size() > 24000 && snapshot.find(later) == snapshot.npos,
                "Log export was not an immutable multi-page snapshot");
        Require(Request(identity, directory, Operation::ApplicationLogs,
                        Pack({(profile / L"data").wstring(), L"com.example.retirement", L"begin"}), response, output) &&
                    response.result == 0,
                "Client could not begin an identity-bound log snapshot");
        RegisterBrokerClientPackage(L"com.example.changed");
        Require(Request(identity, directory, Operation::ApplicationLogs,
                        Pack({(profile / L"data").wstring(), L"com.example.changed", L"next"}), response, output) &&
                    response.result != 0 && output == L"Application log snapshot identity changed",
                "Client registration change exposed another package's log snapshot");
        RegisterBrokerClientPackage(L"com.example.retirement");
        for (const auto& fields :
             {std::vector<std::wstring>{(profile / L"other-data").wstring(), L"com.example.retirement", L"dump"},
              std::vector<std::wstring>{(profile / L"data").wstring(), L"com.other.app", L"dump"}})
            Require(Request(identity, directory, Operation::ApplicationLogs, Pack(fields), response, output) &&
                        response.result != 0 && output == L"Application log Client identity changed",
                    "Application log scope rejection was not enforced by Service");
        for (const auto& fields :
             {std::vector<std::wstring>{(profile / L"other-data").wstring(), L"com.example.retirement",
                                        L"https://example.com/"},
              std::vector<std::wstring>{(profile / L"data").wstring(), L"com.other.app", L"https://example.com/"}})
            Require(Request(identity, directory, Operation::OpenWebUrl, Pack(fields), response, output) &&
                        response.result != 0 && output == L"Web link Client identity changed",
                    "Web link scope rejection was not enforced by Service");
        Require(
            Request(identity, directory, Operation::OpenWebUrl,
                    Pack({(profile / L"data").wstring(), L"com.example.retirement", L"file:///C:/Windows/notepad.exe"}),
                    response, output) &&
                response.result != 0 && output == L"Invalid web URL",
            "Service forwarded a non-web URI");
    }
    {
        std::latch ready(24), start(1);
        std::atomic<unsigned> failures{0};
        std::mutex errorMutex;
        std::string firstError;
        std::vector<std::thread> readers;
        for (unsigned reader = 0; reader != 24; ++reader)
            readers.emplace_back([&] {
                ready.count_down();
                start.wait();
                for (unsigned attempt = 0; attempt != 5; ++attempt)
                    try {
                        auto sequence = ClientClipboardRequest(
                            {(profile / L"data").wstring(), L"com.example.retirement", L"sequence"});
                        Require(sequence.size() == 1 && !sequence[0].empty() &&
                                    sequence[0].find_first_not_of(L"0123456789") == sequence[0].npos,
                                "Concurrent sequence response was malformed");
                    }
                    catch (const std::exception& error) {
                        ++failures;
                        std::lock_guard lock(errorMutex);
                        if (firstError.empty())
                            firstError = error.what();
                    }
            });
        ready.wait();
        start.count_down();
        for (auto& reader : readers)
            reader.join();
        if (failures) {
            std::ofstream report(profile / L"clipboard-failure.txt");
            report << failures.load() << ": " << firstError;
            throw std::runtime_error("Concurrent clipboard metadata requests failed: " +
                                     std::to_string(failures.load()) + "; " + firstError);
        }
    }
    for (const auto& fields :
         {std::vector<std::wstring>{(profile / L"other-data").wstring(), L"com.example.retirement", L"get"},
          std::vector<std::wstring>{(profile / L"data").wstring(), L"com.other.app", L"get"}}) {
        bool rejected = false;
        try {
            ClientClipboardRequest(fields);
        }
        catch (const std::exception& error) {
            rejected = std::string_view(error.what()) ==
                       "Service clipboard operation failed: Clipboard Client identity changed";
        }
        Require(rejected, "Clipboard accepted a spoofed package or data directory");
    }
    {
        Identity identity(directory);
        Handle pipe(CreateFileW(identity.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr));
        if (!pipe.value && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(identity.pipe.c_str(), 5000))
            pipe.value = CreateFileW(identity.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe.value == INVALID_HANDLE_VALUE)
            pipe.value = nullptr;
        Require(pipe.value != nullptr, "Cannot open retirement fault pipe");
        DWORD mode = PIPE_READMODE_MESSAGE;
        Require(SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr), "Cannot set retirement pipe mode");
        Header header{Magic, Version, Operation::RetireClient, 0};
        Response response{};
        wchar_t grant = 0;
        Require(Transfer(pipe.value, &header, sizeof(header), true) &&
                    Transfer(pipe.value, &response, sizeof(response), false) && response.result == 0 &&
                    response.length == 1 && Transfer(pipe.value, &grant, sizeof(grant), false) && grant == L'1',
                "Retirement was not granted");
        // Deliberately disconnect without Request()'s ACK: this Client has not
        // committed to exit and must remain available for a foreground launch.
    }
    {
        std::ofstream ready(profile / L"retirement-ready.txt");
        ready << GetCurrentProcessId();
    }
    const auto deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        auto command = ReadClientSessionCommands();
        if (command == 1)
            return 0;
        if (command == 2)
            break;
        Sleep(20);
    }
    throw std::runtime_error("Lost retirement ACK left Client unactivatable");
}

void RetirementRecovery(const fs::path& directory)
{
    const auto profile = directory / (L"retirement-" + std::to_wstring(GetTickCount64()));
    fs::create_directory(profile);
    const auto apk = profile / L"test.apk", image = profile / L"system.img";
    {
        std::ofstream file(apk);
        file << "native Client test";
    }
    {
        std::ofstream file(image);
        file << "native Client test";
    }
    const auto options = L" --data-dir " + stone::session::Quote(profile.wstring()) + L" --system-image " +
                         stone::session::Quote(image.wstring());
    ServiceCommand(directory, L"--apk " + stone::session::Quote(apk.wstring()) + options);
    auto ready = profile / L"retirement-ready.txt";
    DWORD pid = 0;
    Eventually(
        [&] {
            std::ifstream file(ready);
            return static_cast<bool>(file >> pid) && pid != 0;
        },
        "Retirement fault Client did not start");
    Handle child(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    Require(child.value != nullptr, "Cannot retain retirement Client");
    ServiceCommand(directory, L"--uri stone://com.example.retirement" + options);
    DWORD result = 1;
    Require(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0 && GetExitCodeProcess(child.value, &result) &&
                result == 0,
            "Client failed activation after a lost retirement ACK");
}
}

int wmain(int argc, wchar_t** argv)
{
    fs::path directory;
    try {
        if (argc == 7 && std::wstring_view(argv[1]) == L"--apk")
            return RetirementChild(fs::canonical(argv[0]).parent_path(), argv[4]);
        if (argc == 4 && std::wstring_view(argv[1]) == L"--power-child") {
            ConfigureClientService(fs::path(argv[2]) / L"StoneService.exe");
            InitializeClientServices();
            SetClientSystemAwake(true);
            Handle stop(OpenEventW(SYNCHRONIZE, FALSE, argv[3]));
            Require(stop.value != nullptr, "Cannot open power test event");
            Require(WaitForSingleObject(stop.value, 30000) == WAIT_OBJECT_0, "Power test owner did not respond");
            // Deliberately exit while still holding the request: Service must reap it.
            return 0;
        }
        if (fs::path(argv[0]).filename() == L"StoneClient.exe")
            throw std::invalid_argument("Unexpected native test Client arguments; refusing recursive test startup");
        WebLinks();
        directory = fs::path(STONE_TEST_HOST_DIRECTORY) / std::to_wstring(GetCurrentProcessId());
        fs::create_directories(directory);
        fs::copy_file(STONE_TEST_SERVICE, directory / L"StoneService.exe", fs::copy_options::overwrite_existing);
        wchar_t self[32768]{};
        Require(GetModuleFileNameW(nullptr, self, 32768) != 0, "Cannot locate test Client");
        fs::copy_file(self, directory / L"StoneClient.exe", fs::copy_options::overwrite_existing);
        ManagedBroker(directory);
        ConfigureClientService(directory / L"StoneService.exe");
        InitializeClientServices();
        const auto initial = ReadClientHostState();
        auto provider = initial.owner;
        Require(initial.owner != 0 && initial.owner != GetCurrentProcessId(),
                "Client is providing its own host services");
        Require(ProcessIs(initial.owner, directory / L"StoneService.exe"), "Unexpected host provider executable");
        MEMORYSTATUSEX memory{sizeof(memory)};
        Require(GlobalMemoryStatusEx(&memory), "Cannot read Windows memory reference");
        Require(initial.memory[1] == memory.ullTotalPhys && initial.memory[0] <= initial.memory[1] &&
                    initial.memory[2] == initial.memory[1] / 16 && initial.memory[3] >= initial.memory[1],
                "Service memory snapshot differs from Windows");
        Handle first(OpenProcess(SYNCHRONIZE, FALSE, initial.owner));
        Require(first.value != nullptr, "Cannot retain test service");
        Eventually(
            [] {
                return ReadClientHostState().networkSamples > 0;
            },
            "Service network polling did not run");
        WifiReference();
        Interfaces();
        SYSTEM_POWER_STATUS power{};
        Require(GetSystemPowerStatus(&power), "Cannot read reference Windows power state");
        Eventually(
            [&] {
                return ReadClientHostState().battery == std::array<int, 4>{power.ACLineStatus, power.BatteryFlag,
                                                                           power.BatteryLifePercent,
                                                                           power.SystemStatusFlag};
            },
            "Service battery state does not match Windows");
        Identity identity(directory);
        Response response{};
        std::wstring output;
        Require(Request(identity, directory, Operation::HostState, L"", response, output) && response.result == 0,
                "Second host state reader failed");
        Require(DecodeHostState(output).owner == initial.owner, "Readers do not share the same Service");
        Require(Request(identity, directory, Operation::Status, L"", response, output) && response.result != 0,
                "Diagnostic peer was granted mutating service authority");
        Require(Request(identity, directory, Operation::SystemAwake, L"1", response, output) && response.result != 0,
                "Untrusted reader was granted wake request authority");
        Require(Request(identity, directory, Operation::Jobs,
                        Pack({directory.wstring(), directory.wstring(), L"com.example.jobs", L"cancel-all"}), response,
                        output) &&
                    response.result != 0,
                "Untrusted reader was granted job mutation authority");
        Require(Request(identity, directory, Operation::PackageRemoved,
                        Pack({directory.wstring(), L"com.example.jobs"}), response, output) &&
                    response.result != 0,
                "Untrusted reader was granted package cleanup authority");
        Require(Request(identity, directory, Operation::RetireClient, L"", response, output) && response.result != 0,
                "Untrusted reader was granted retirement authority");
        Require(Request(identity, directory, Operation::ClientProfile, L"", response, output) && response.result != 0,
                "Untrusted reader was granted background launch registration authority");
        Require(Request(identity, directory, Operation::ClientStarting, L"", response, output) && response.result != 0,
                "Untrusted reader was granted startup registration authority");
        Require(Request(identity, directory, Operation::Notifications, L"", response, output) && response.result != 0,
                "Untrusted reader was granted notification mutation authority");
        Require(Request(identity, directory, Operation::Clipboard,
                        Pack({directory.wstring(), L"com.example.clipboard", L"get"}), response, output) &&
                    response.result != 0,
                "Untrusted reader was granted clipboard authority");
        Require(Request(identity, directory, Operation::OpenWebUrl,
                        Pack({directory.wstring(), L"com.example.links", L"https://example.com/"}), response, output) &&
                    response.result != 0,
                "Untrusted reader was granted browser activation authority");
        {
            ClientSignals sender(directory, GetCurrentProcessId()), receiver(directory, GetCurrentProcessId());
            Require(receiver.Consume() == 0, "Spurious Client control");
            sender.Activate();
            Require(sender.ActivationPending() && receiver.Consume() == 1 && receiver.Consume() == 0,
                    "Client activation was lost or repeated");
            sender.Stop();
            Require(receiver.Consume() == 2 && receiver.Consume() == 2, "Client stop was not durable");
        }
        RetirementRecovery(directory);
        {
            PowerChild left(directory, 1), right(directory, 2);
            Eventually(
                [] {
                    return ReadClientHostState().systemPowerRequests == 2;
                },
                "Two Clients did not own independent power requests");
            StopService(directory);
            Require(WaitForSingleObject(first.value, 5000) == WAIT_OBJECT_0, "Original power provider did not exit");
            Eventually(
                [&] {
                    const auto state = ReadClientHostState();
                    return state.owner != provider && state.systemPowerRequests == 2;
                },
                "Held power requests were not restored after Service replacement");
            provider = ReadClientHostState().owner;
            CloseHandle(first.value);
            first.value = OpenProcess(SYNCHRONIZE, FALSE, provider);
            Require(first.value != nullptr, "Cannot retain restored power provider");
            left.Exit();
            Eventually(
                [] {
                    return ReadClientHostState().systemPowerRequests == 1;
                },
                "Exiting one Client removed the other's power request");
            right.Exit();
            Eventually(
                [] {
                    return ReadClientHostState().systemPowerRequests == 0;
                },
                "Exited Clients retained power requests");
        }
        StopService(directory);
        Require(WaitForSingleObject(first.value, 5000) == WAIT_OBJECT_0, "Service failed to exit");
        const auto begin = std::chrono::steady_clock::now();
        for (int index = 0; index < 10000; ++index)
            Require(ReadClientHostState().owner != 0, "Last snapshot disappeared");
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
        Require(elapsed < 500, "Cached host reads waited for IPC recovery");
        Eventually(
            [&] {
                const auto state = ReadClientHostState();
                return state.owner != provider && state.powerSamples > 0;
            },
            "Client did not reconnect to a replacement Service");
        const auto replacement = ReadClientHostState().owner;
        Handle second(OpenProcess(SYNCHRONIZE, FALSE, replacement));
        Require(second.value != nullptr, "Cannot retain replacement Service");
        Require(StopClientServices(), "Host cache worker failed to stop");
        StopService(directory);
        Require(WaitForSingleObject(second.value, 5000) == WAIT_OBJECT_0, "Replacement Service failed to exit");
        std::printf(
            "PASS managed broker shutdown, shared Service host state, web URL policy/peer authorization, 120 concurrent clipboard metadata requests, two Client wake requests/exit cleanup, recovery, 10000 cached reads in %lld ms\n",
            elapsed);
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL host services: %s\n", error.what());
        if (StopClientServices() && !directory.empty()) {
            try {
                StopService(directory);
            }
            catch (...) {
            }
        }
        return 1;
    }
}
