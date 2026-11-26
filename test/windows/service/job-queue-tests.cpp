#include "JobQueue.h"
#include "../../../src/windows/host/CommandLine.h"
#include "../../../src/windows/ipc/ServiceProtocol.h"
#include <filesystem>
#include <cstdio>
#include <stdexcept>
using namespace stone::windows;
namespace fs = std::filesystem;
using Fields = std::vector<std::wstring>;

namespace {
void Check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

void Child(const wchar_t* executable, const std::wstring& stage, const fs::path& data, const fs::path& image,
           const std::wstring& package)
{
    auto command = stone::session::Quote(executable) + L" " + stage;
    for (const auto& value : {data.wstring(), image.wstring(), package})
        command += L" " + stone::session::Quote(value);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                         &startup, &process),
          "Cannot start cold-store check");
    CloseHandle(process.hThread);
    stone::windows::ipc::Handle child(process.hProcess);
    DWORD code = 1;
    Check(WaitForSingleObject(child.value, 15000) == WAIT_OBJECT_0 && GetExitCodeProcess(child.value, &code) &&
              code == 0,
          "Cold-store subprocess failed");
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 5) {
            fs::create_directories(argv[2]);
            fs::create_directories(argv[3]);
            Fields fields{argv[2], argv[3], argv[4]};
            HostSnapshot host;
            if (std::wstring_view(argv[1]) == L"--seed")
                fields.insert(fields.end(), {L"schedule", L"cold", L"77", L"cold-persisted", L"3600000", L"-1", L"0",
                                             L"10000", L"1", L"32", L"20", L"0", L"0"});
            else
                fields.insert(fields.end(), {L"get", L"cold", L"77"});
            auto result = ServiceJobRequest(GetCurrentProcessId(), fields, host, JobClockMillis());
            if (std::wstring_view(argv[1]) != L"--seed")
                Check(result.empty() == (std::wstring_view(argv[1]) == L"--empty"),
                      "Cold-store package/data isolation failed");
            return 0;
        }
        const auto root = fs::path(STONE_JOB_TEST_DIRECTORY) /
                          (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        const auto data = root / L"data", image = root / L"image";
        fs::create_directories(data);
        fs::create_directories(image);
        uint64_t now = JobClockMillis();
        HostSnapshot host;
        host.battery = {0, 2, 50, 0};
        host.network = 0;
        HostJobState conditions;
        auto request = [&](Fields fields) {
            Fields all{data.wstring(), image.wstring(), L"com.example.jobs"};
            all.insert(all.end(), fields.begin(), fields.end());
            return ServiceJobRequest(GetCurrentProcessId(), all, host, now, conditions);
        };
        auto poll = [&] {
            auto result = request({L"poll"});
            if (result.size() == 6 && result[0] == L"start")
                request({L"accept", result[1]});
            if (result.size() == 3 && result[0] == L"stop")
                request({L"stop-accept", result[1]});
            return result;
        };
        auto schedule = [&](int id, int flags = 0, uint64_t delay = 0, const wchar_t* deadline = L"-1",
                            uint64_t period = 0, const wchar_t* payload = L"payload", const wchar_t* name = L"") {
            request({L"schedule", name, std::to_wstring(id), payload, std::to_wstring(delay), deadline,
                     std::to_wstring(period), L"10000", L"1", std::to_wstring(flags), L"20", L"1860000", L"1860000"});
        };
        auto finish = [&](const Fields& run, bool retry = false) {
            request({L"finish", run.at(1), retry ? L"1" : L"0"});
        };
        schedule(1, 1);
        Check(poll().empty(), "Charging constraint ignored");
        Check(ServiceJobClientIdle(GetCurrentProcessId(), host, now, conditions),
              "Unmet constraint prevented idle retirement");
        host.battery = {1, 8, 50, 0};
        auto firstDelivery = request({L"poll"});
        Check(!ServiceJobClientIdle(GetCurrentProcessId(), host, now, conditions),
              "Active execution allowed Client retirement");
        auto charging = request({L"poll"});
        Check(charging == firstDelivery, "Unaccepted lease was not redelivered");
        request({L"accept", charging.at(1)});
        Check(charging.size() == 6 && charging[0] == L"start", "Charging transition did not start job");
        finish(charging);
        host.battery = {0, 2, 19, 0};
        schedule(2, 2);
        Check(poll().empty(), "Low battery allowed job");
        host.battery = {0, 255, 90, 0};
        Check(poll().empty(), "Unknown battery flags allowed job");
        host.battery = {0, 2, 30, 0};
        auto battery = poll();
        Check(battery.size() == 6, "Battery recovery did not allow job");
        finish(battery);
        schedule(3, 12);
        host.network = 3;
        Check(poll().empty(), "Metered link allowed unmetered job");
        host.network = 7;
        auto network = poll();
        Check(network.size() == 6, "Unmetered connection did not start job");
        host.network = 0;
        auto stop = request({L"poll"});
        Check(request({L"poll"}) == stop, "Unaccepted stop was not redelivered");
        request({L"stop-accept", stop.at(1)});
        Check(stop.size() == 3 && stop[0] == L"stop" && stop[2] == L"7", "Connection loss did not stop job");
        finish(network);
        schedule(4, 4, 0, L"100");
        now += 99;
        Check(poll().empty(), "Deadline fired early");
        ++now;
        auto expired = poll();
        Check(expired.size() == 6 && expired[5] == L"1", "Deadline failed to override constraint");
        Check(poll().empty(), "Expired deadline job was stopped by overridden constraint");
        finish(expired);
        schedule(5, 0, 200);
        now += 199;
        Check(poll().empty(), "Latency fired early");
        ++now;
        auto retry = poll();
        Check(retry.size() == 6, "Latency did not start job");
        finish(retry, true);
        now += 9999;
        Check(poll().empty(), "Backoff fired early");
        ++now;
        retry = poll();
        Check(retry.size() == 6, "First backoff did not resume");
        finish(retry, true);
        now += 19999;
        Check(poll().empty(), "Exponential backoff fired early");
        ++now;
        retry = poll();
        Check(retry.size() == 6, "Second backoff did not resume");
        finish(retry);
        schedule(6, 0, 0, L"-1", 900000);
        auto periodic = poll();
        Check(periodic.size() == 6, "Periodic initial run missing");
        finish(periodic);
        now += 899999;
        Check(poll().empty(), "Periodic interval shortened");
        ++now;
        periodic = poll();
        Check(periodic.size() == 6, "Periodic repeat missing");
        finish(periodic);
        request({L"cancel", L"", L"6"});
        schedule(7);
        auto old = poll();
        schedule(7, 0, 0, L"-1", 0, L"replacement");
        Check(poll().at(0) == L"stop", "Replacement did not stop old execution");
        Check(poll().empty(), "Replacement overlapped old execution");
        finish(old);
        Check(request({L"get", L"", L"7"}).at(0) == L"replacement", "Old completion erased replacement");
        auto replacement = poll();
        Check(replacement.size() == 6 && replacement[1] != old[1], "Execution lease was reused");
        finish(old);
        Check(request({L"get", L"", L"7"}).size() == 1, "Stale completion affected active replacement");
        finish(replacement);
        schedule(-3, 0, 100000, L"-1", 0, L"left", L"left");
        schedule(-3, 0, 100000, L"-1", 0, L"right", L"right");
        auto page = request({L"list", L"0"});
        Check(page.size() == 6 && page[0] == L"item", "Job listing missing");
        request({L"cancel-ns", L"left"});
        Check(request({L"get", L"right", L"-3"}).size() == 1, "Namespace cancellation escaped scope");
        Check(request({L"list", L"1", page[1]}).at(0) == L"changed", "Listing failed to detect concurrent mutation");
        fs::path file;
        for (const auto& item : fs::directory_iterator(data / L"system/stone-jobs"))
            if (item.path().extension() == L".bin")
                file = item.path();
        Check(!file.empty(), "Atomic job store was not created");
        auto temporary = file;
        temporary += L".tmp";
        fs::create_directory(temporary);
        bool failed = false;
        try {
            schedule(99, 32);
        }
        catch (const std::exception&) {
            failed = true;
        }
        Check(failed && request({L"get", L"", L"99"}).empty(), "Failed storage publication committed a job");
        fs::remove(temporary);
        request({L"cancel-all"});
        Check(request({L"list", L"0"}).at(0) == L"end", "Cancel-all left jobs");
        schedule(50, 64);
        Check(poll().empty(), "Unknown storage allowed constrained job");
        conditions.storageKnown = true;
        conditions.storageTotal = 20ULL * 1024 * 1024 * 1024;
        conditions.storageFree = 100ULL * 1024 * 1024;
        Check(poll().empty(), "Low storage allowed job");
        conditions.storageFree = 1024ULL * 1024 * 1024;
        auto storage = poll();
        Check(storage.size() == 6, "Storage recovery did not allow job");
        finish(storage);
        schedule(51, 128);
        conditions.idleMillis = 2000000;
        Check(poll().empty(), "Active user allowed idle job");
        conditions.userAbsent = true;
        conditions.idleMillis = 1859999;
        Check(poll().empty(), "Idle threshold fired early");
        conditions.idleMillis = 1860000;
        auto idle = poll();
        Check(idle.size() == 6, "Idle user did not allow job");
        conditions.userAbsent = false;
        Check(poll().at(2) == L"8", "Returning user did not stop idle job");
        finish(idle);
        bool scopeDenied = false;
        try {
            ServiceJobRequest(GetCurrentProcessId(), {data.wstring(), image.wstring(), L"com.other.app", L"list", L"0"},
                              host, now);
        }
        catch (const std::exception&) {
            scopeDenied = true;
        }
        Check(scopeDenied, "Client changed its application scope");
        Child(argv[0], L"--seed", data, root / L"image-cold", L"com.example.jobs");
        Check(!ServiceJobsReady(data, root / L"image-cold", L"com.example.jobs", host, now, conditions),
              "Cold persisted job woke early");
        Check(ServiceJobsReady(data, root / L"image-cold", L"com.example.jobs", host, now + 3600001, conditions),
              "Cold due job was not offered for wake");
        Child(argv[0], L"--seed", data, image, L"com.other.jobs");
        Child(argv[0], L"--seed", root / L"other-data", image, L"com.example.jobs");
        schedule(60);
        auto removedRun = poll();
        schedule(61, 32, 3600000);
        fs::create_directory(temporary);
        failed = false;
        try {
            RemoveServicePackageJobs(data, L"com.example.jobs", now);
        }
        catch (const std::exception&) {
            failed = true;
        }
        Check(failed && request({L"get", L"", L"61"}).size() == 1, "Failed package removal discarded queued jobs");
        fs::remove(temporary);
        RemoveServicePackageJobs(data, L"com.example.jobs", now);
        Check(request({L"list", L"0"}).at(0) == L"end", "Package removal retained jobs");
        Check(poll().at(2) == L"1", "Package removal did not cancel active execution");
        finish(removedRun, true);
        Check(request({L"list", L"0"}).at(0) == L"end", "Late completion resurrected uninstalled job");
        RemoveServicePackageJobs(data, L"com.example.jobs", now);
        Check(!ServiceJobsReady(data, root / L"image-cold", L"com.example.jobs", host, now + 3600001, conditions),
              "Removed job was offered for wake");
        Child(argv[0], L"--empty", data, root / L"image-cold", L"com.example.jobs");
        Child(argv[0], L"--present", data, image, L"com.other.jobs");
        Child(argv[0], L"--present", root / L"other-data", image, L"com.example.jobs");
        std::puts(
            "PASS Service job constraints, leases, atomic rollback and package removal across cold images with package/data isolation");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL job queue: %s\n", error.what());
        return 1;
    }
}
