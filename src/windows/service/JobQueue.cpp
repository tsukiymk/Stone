#include "JobQueue.h"
#include "../ipc/ServiceProtocol.h"
#include "../host/CommandLine.h"
#include "../host/WindowsPath.h"
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;
using ipc::Check;
using Fields = std::vector<std::wstring>;
using Key = std::pair<std::wstring, int>;
constexpr uint32_t Charging = 1, BatteryNotLow = 2, Network = 4, Unmetered = 8, Metered = 16, Persisted = 32,
                   StorageNotLow = 64, Idle = 128;
constexpr uint64_t MaxTime = 315360000000ULL, MaxBackoff = 18000000;

struct Job {
    std::wstring blob;
    uint64_t generation = 0, due = 0, deadline = 0, period = 0, backoff = 30000;
    uint64_t idle = 1860000, stableIdle = 1860000;
    uint32_t flags = 0, warning = 20, attempts = 0, policy = 1;
    std::wstring lease;
};

struct Run {
    Key key;
    uint64_t generation, started;
    DWORD client;
    bool overrideDeadline;
    int stop = -1;
    bool delivered = false, accepted = false;
};

struct Scope {
    fs::path file;
    std::wstring identity;
    uint64_t generation = 0;
    std::map<Key, Job> jobs;
    std::map<std::wstring, Run> runs;
};

struct Session {
    ipc::Handle process;
    std::wstring scope;

    explicit Session(DWORD pid, std::wstring key) : process(OpenProcess(SYNCHRONIZE, FALSE, pid)), scope(std::move(key))
    {
        Check(process.value != nullptr, "Cannot track job Client");
    }
};

std::mutex gate;
std::map<std::wstring, Scope> scopes;
std::map<DWORD, std::unique_ptr<Session>> sessions;

uint64_t WallClock()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart / 10000 - 11644473600000ULL;
}

uint64_t Number(const std::wstring& text, uint64_t maximum = UINT64_MAX)
{
    Check(!text.empty() && text.find_first_not_of(L"0123456789") == text.npos, "Invalid job number");
    auto value = std::stoull(text);
    Check(value <= maximum, "Job number is outside its range");
    return value;
}

int JobId(const std::wstring& text)
{
    auto digits = !text.empty() && text[0] == L'-' ? text.substr(1) : text;
    Check(!digits.empty() && digits.find_first_not_of(L"0123456789") == digits.npos, "Invalid job ID");
    auto value = std::stoll(text);
    Check(value >= INT_MIN && value <= INT_MAX, "Job ID is outside its range");
    return static_cast<int>(value);
}

std::wstring Hash(const std::wstring& text)
{
    std::array<unsigned char, 32> bytes{};
    Check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(text.data())),
                     static_cast<ULONG>(text.size() * sizeof(wchar_t)), bytes.data(),
                     static_cast<ULONG>(bytes.size())) >= 0,
          "Cannot identify job store");
    std::wstring result;
    for (auto byte : bytes) {
        result += L"0123456789abcdef"[byte >> 4];
        result += L"0123456789abcdef"[byte & 15];
    }
    return result;
}

void Put(std::vector<unsigned char>& bytes, uint64_t number)
{
    for (int shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<unsigned char>(number >> shift));
}

void Put(std::vector<unsigned char>& bytes, const std::wstring& text)
{
    Put(bytes, text.size());
    auto begin = reinterpret_cast<const unsigned char*>(text.data());
    bytes.insert(bytes.end(), begin, begin + text.size() * sizeof(wchar_t));
}

struct Reader {
    std::vector<unsigned char> bytes;
    size_t at = 0;

    uint64_t Number(uint64_t maximum = UINT64_MAX)
    {
        Check(bytes.size() - at >= 8, "Truncated job store");
        uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8)
            value |= static_cast<uint64_t>(bytes[at++]) << shift;
        Check(value <= maximum, "Invalid stored job number");
        return value;
    }

    std::wstring Text(size_t maximum)
    {
        auto length = static_cast<size_t>(Number(maximum));
        Check(bytes.size() - at >= length * sizeof(wchar_t), "Truncated job string");
        std::wstring result(length, L'\0');
        std::memcpy(result.data(), bytes.data() + at, length * sizeof(wchar_t));
        at += length * sizeof(wchar_t);
        Check(result.find(L'\0') == result.npos, "Invalid stored job string");
        return result;
    }
};

void Save(const Scope& scope, uint64_t now)
{
    const auto wall = WallClock();
    std::vector<unsigned char> bytes;
    Put(bytes, 0x32424f4a);
    Put(bytes, scope.identity);
    Put(bytes, scope.generation);
    uint64_t count = 0;
    for (const auto& [key, job] : scope.jobs)
        if (job.flags & Persisted)
            ++count;
    Put(bytes, count);
    for (const auto& [key, job] : scope.jobs)
        if (job.flags & Persisted) {
            Put(bytes, key.first);
            Put(bytes, static_cast<uint64_t>(static_cast<uint32_t>(key.second)));
            Put(bytes, job.blob);
            auto due = wall + (job.due > now ? job.due - now : 0);
            auto deadline = job.deadline ? wall + (job.deadline > now ? job.deadline - now : 0) : 0;
            for (auto value : {job.generation, due, deadline, job.period, job.backoff, static_cast<uint64_t>(job.flags),
                               static_cast<uint64_t>(job.warning), static_cast<uint64_t>(job.attempts),
                               static_cast<uint64_t>(job.policy), job.idle, job.stableIdle})
                Put(bytes, value);
        }
    Check(bytes.size() <= 16777216, "Application job store is too large");
    fs::create_directories(scope.file.parent_path());
    auto temporary = scope.file;
    temporary += L".tmp";
    {
        ipc::Handle file(
            CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        Check(file.value != nullptr, "Cannot write job store");
        DWORD written = 0;
        Check(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                  written == bytes.size() && FlushFileBuffers(file.value),
              "Cannot flush job store");
    }
    Check(MoveFileExW(temporary.c_str(), scope.file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
          "Cannot publish job store");
}

Reader ReadStore(const fs::path& path)
{
    ipc::Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER length{};
    Check(file.value && GetFileSizeEx(file.value, &length) && length.QuadPart >= 0 && length.QuadPart <= 16777216,
          "Invalid job store size");
    Reader reader;
    reader.bytes.resize(static_cast<size_t>(length.QuadPart));
    DWORD read = 0;
    Check(ReadFile(file.value, reader.bytes.data(), static_cast<DWORD>(reader.bytes.size()), &read, nullptr) &&
              read == reader.bytes.size(),
          "Cannot read job store");
    return reader;
}

Scope Load(const fs::path& data, const std::wstring& identity, uint64_t now)
{
    const auto wall = WallClock();
    Scope scope;
    scope.identity = identity;
    scope.file = ExtendedWindowsPath(data / L"system/stone-jobs" / (Hash(identity) + L".bin"));
    if (!fs::exists(scope.file))
        return scope;
    auto reader = ReadStore(scope.file);
    auto version = reader.Number();
    Check((version == 0x31424f4a || version == 0x32424f4a) && reader.Text(32767) == identity,
          "Job store belongs to a different image or application");
    scope.generation = reader.Number();
    const auto count = reader.Number(512);
    for (uint64_t index = 0; index < count; ++index) {
        Key key{reader.Text(128), static_cast<int>(static_cast<uint32_t>(reader.Number(UINT32_MAX)))};
        Job job;
        job.blob = reader.Text(24576);
        job.generation = reader.Number();
        auto due = reader.Number();
        auto deadline = reader.Number();
        job.due = now + (due > wall ? std::min(MaxTime, due - wall) : 0);
        job.deadline = deadline ? now + (deadline > wall ? std::min(MaxTime, deadline - wall) : 0) : 0;
        job.period = reader.Number(MaxTime);
        job.backoff = reader.Number(MaxBackoff);
        job.flags = static_cast<uint32_t>(reader.Number(version == 0x31424f4a ? 63 : 255));
        job.warning = static_cast<uint32_t>(reader.Number(100));
        job.attempts = static_cast<uint32_t>(reader.Number(32));
        job.policy = static_cast<uint32_t>(reader.Number(1));
        if (version == 0x32424f4a) {
            job.idle = reader.Number(MaxTime);
            job.stableIdle = reader.Number(MaxTime);
        }
        Check((job.flags & Persisted) && job.generation <= scope.generation && !job.blob.empty() &&
                  job.backoff >= 10000,
              "Invalid persisted job");
        Check(scope.jobs.emplace(key, std::move(job)).second, "Duplicate persisted job");
    }
    Check(reader.at == reader.bytes.size(), "Trailing bytes in job store");
    return scope;
}

template <class Function> void Change(Scope& scope, uint64_t now, Function function)
{
    auto previous = scope;
    try {
        function();
        Check(scope.generation != UINT64_MAX, "Job revisions exhausted");
        ++scope.generation;
        Save(scope, now);
    }
    catch (...) {
        scope = std::move(previous);
        throw;
    }
}

void Finish(Scope& scope, const std::wstring& lease, bool retry, uint64_t now)
{
    auto execution = scope.runs.find(lease);
    if (execution == scope.runs.end())
        return;
    auto entry = scope.jobs.find(execution->second.key);
    if (entry != scope.jobs.end() && entry->second.generation == execution->second.generation) {
        auto& job = entry->second;
        job.lease.clear();
        if (retry) {
            job.attempts = std::min<uint32_t>(job.attempts, 31) + 1;
            auto factor = job.policy == 0 ? job.attempts : 1ULL << (job.attempts - 1);
            job.due = now + std::min<uint64_t>(MaxBackoff, job.backoff * factor);
            job.deadline = 0;
        }
        else if (job.period) {
            job.due = now + job.period;
            job.deadline = 0;
            job.attempts = 0;
        }
        else
            scope.jobs.erase(entry);
    }
    scope.runs.erase(execution);
}

void Reap(uint64_t now)
{
    for (auto it = sessions.begin(); it != sessions.end();) {
        if (WaitForSingleObject(it->second->process.value, 0) != WAIT_OBJECT_0) {
            ++it;
            continue;
        }
        auto scope = scopes.find(it->second->scope);
        if (scope != scopes.end())
            Change(scope->second, now, [&] {
                std::vector<std::wstring> dead;
                for (const auto& [lease, run] : scope->second.runs)
                    if (run.client == it->first)
                        dead.push_back(lease);
                for (const auto& lease : dead)
                    Finish(scope->second, lease, true, now);
            });
        it = sessions.erase(it);
    }
}

int Constraint(const Job& job, const HostSnapshot& host, const HostJobState& conditions)
{
    const auto& battery = host.battery;
    bool charging =
        battery[0] == 1 && battery[1] != 255 && ((battery[1] & 128) || (battery[1] & 8) || battery[2] == 100);
    if ((job.flags & Charging) && !charging)
        return 6; // STOP_REASON_CONSTRAINT_CHARGING
    bool notLow =
        battery[0] == 1 ||
        battery[1] != 255 && ((battery[1] & 128) || battery[2] <= 100 && battery[2] > static_cast<int>(job.warning));
    if ((job.flags & BatteryNotLow) && !notLow)
        return 5;
    if ((job.flags & Idle) &&
        (!conditions.userAbsent || conditions.idleMillis < (charging && notLow ? job.stableIdle : job.idle)))
        return 8;
    const auto lowStorage = std::min<uint64_t>(conditions.storageTotal / 20, 500ULL * 1024 * 1024);
    if ((job.flags & StorageNotLow) && (!conditions.storageKnown || conditions.storageFree <= lowStorage))
        return 9;
    if ((job.flags & Network) && (!(host.network & 2) || (job.flags & Unmetered) && !(host.network & 4) ||
                                  (job.flags & Metered) && (host.network & 4)))
        return 7;
    return -1;
}

void Cancel(Scope& scope, const std::wstring& name, const std::wstring& id, bool all)
{
    for (auto it = scope.jobs.begin(); it != scope.jobs.end();) {
        bool selected = all || it->first.first == name && (id.empty() || it->first.second == JobId(id));
        if (!selected) {
            ++it;
            continue;
        }
        if (!it->second.lease.empty())
            scope.runs.at(it->second.lease).stop = 1;
        it = scope.jobs.erase(it);
    }
}
}

uint64_t JobClockMillis()
{
    return GetTickCount64();
}

void MaintainServiceJobs(uint64_t now)
{
    const std::lock_guard lock(gate);
    Reap(now);
}

bool ServiceJobsReady(const fs::path& directory, const fs::path& image, const std::wstring& package,
                      const HostSnapshot& host, uint64_t now, const HostJobState& conditions)
{
    const std::lock_guard lock(gate);
    Reap(now);
    auto data = fs::canonical(ExtendedWindowsPath(directory)), system = fs::canonical(ExtendedWindowsPath(image));
    Check(fs::is_directory(ExtendedWindowsPath(data)) && fs::is_directory(ExtendedWindowsPath(system)) &&
              stone::session::IsPackageName(package),
          "Invalid background job scope");
    const auto identity = ipc::Normalize(data) + L"\n" + ipc::Normalize(system) + L"\n" + package;
    auto scope = scopes.find(identity);
    if (scope == scopes.end())
        scope = scopes.emplace(identity, Load(data, identity, now)).first;
    for (const auto& [pid, session] : sessions)
        if (session->scope == identity)
            return false;
    for (const auto& [key, job] : scope->second.jobs) {
        if (!job.lease.empty())
            continue;
        if (job.deadline && now >= job.deadline || now >= job.due && Constraint(job, host, conditions) < 0)
            return true;
    }
    return false;
}

bool ServiceJobClientIdle(DWORD client, const HostSnapshot& host, uint64_t now, const HostJobState& conditions)
{
    const std::lock_guard lock(gate);
    Reap(now);
    auto session = sessions.find(client);
    if (session == sessions.end())
        return true;
    auto found = scopes.find(session->second->scope);
    if (found == scopes.end())
        return true;
    const auto& scope = found->second;
    for (const auto& [lease, run] : scope.runs)
        if (run.client == client)
            return false;
    for (const auto& [key, job] : scope.jobs)
        if (job.lease.empty() &&
            (job.deadline && now >= job.deadline || now >= job.due && Constraint(job, host, conditions) < 0))
            return false;
    return true;
}

void RemoveServicePackageJobs(const fs::path& directory, const std::wstring& package, uint64_t now)
{
    const std::lock_guard lock(gate);
    auto data = fs::canonical(ExtendedWindowsPath(directory));
    Check(fs::is_directory(ExtendedWindowsPath(data)) && stone::session::IsPackageName(package),
          "Invalid removed package scope");
    const auto parent = ExtendedWindowsPath(data / L"system/stone-jobs");
    const auto prefix = ipc::Normalize(data) + L"\n", suffix = L"\n" + package;
    auto selected = [&](const Scope& scope) {
        return ipc::Normalize(scope.file.parent_path()) == ipc::Normalize(parent) &&
               scope.identity.starts_with(prefix) && scope.identity.ends_with(suffix);
    };
    // A package registry belongs to the data root, independently of which image
    // prepared its jobs. Include cold image stores that no Client has loaded yet.
    if (fs::is_directory(parent))
        for (const auto& entry : fs::directory_iterator(parent)) {
            if (!entry.is_regular_file() || entry.path().extension() != L".bin")
                continue;
            auto reader = ReadStore(entry.path());
            auto version = reader.Number();
            Check(version == 0x31424f4a || version == 0x32424f4a, "Invalid job store version");
            auto identity = reader.Text(32767);
            if (!identity.starts_with(prefix) || !identity.ends_with(suffix))
                continue;
            Check(entry.path().filename() == Hash(identity) + L".bin", "Invalid job store identity");
            if (!scopes.contains(identity))
                scopes.emplace(identity, Load(data, identity, now));
        }
    // Each publication is atomic; a multi-image failure is retried by the
    // package-removal transaction. Already-cleared stores are safe to revisit.
    for (auto& [identity, scope] : scopes)
        if (selected(scope) && !scope.jobs.empty())
            Change(scope, now, [&] {
                Cancel(scope, L"", L"", true);
            });
}

std::vector<std::wstring> ServiceJobRequest(DWORD client, const Fields& fields, const HostSnapshot& host, uint64_t now,
                                            const HostJobState& conditions)
{
    const std::lock_guard lock(gate);
    Check(fields.size() >= 4, "Missing job scope");
    Reap(now);
    auto data = fs::canonical(ExtendedWindowsPath(fields[0])), system = fs::canonical(ExtendedWindowsPath(fields[1]));
    const auto& package = fields[2];
    const auto& operation = fields[3];
    Check(fs::is_directory(ExtendedWindowsPath(data)) && fs::is_directory(ExtendedWindowsPath(system)) &&
              stone::session::IsPackageName(package),
          "Invalid job scope");
    const auto identity = ipc::Normalize(data) + L"\n" + ipc::Normalize(system) + L"\n" + package;
    auto session = sessions.find(client);
    if (session == sessions.end())
        sessions.emplace(client, std::make_unique<Session>(client, identity));
    else
        Check(session->second->scope == identity, "Client cannot switch job application or image");
    auto scopeEntry = scopes.find(identity);
    if (scopeEntry == scopes.end())
        scopeEntry = scopes.emplace(identity, Load(data, identity, now)).first;
    auto& scope = scopeEntry->second;
    if (operation == L"list") {
        Check(fields.size() == 5 || fields.size() == 6, "Invalid job listing");
        auto offset = Number(fields[4], 512);
        if (fields.size() == 6 && Number(fields[5]) != scope.generation)
            return {L"changed"};
        auto it = scope.jobs.begin();
        for (uint64_t index = 0; index < offset && it != scope.jobs.end(); ++index)
            ++it;
        if (it == scope.jobs.end())
            return {L"end"};
        return {L"item",         std::to_wstring(scope.generation), std::to_wstring(offset + 1),
                it->first.first, std::to_wstring(it->first.second), it->second.blob};
    }
    if (operation == L"get") {
        Check(fields.size() == 6, "Invalid job lookup");
        auto it = scope.jobs.find({fields[4], JobId(fields[5])});
        return it == scope.jobs.end() ? Fields{} : Fields{it->second.blob};
    }
    if (operation == L"schedule") {
        Check(fields.size() == 16 && fields[4].size() <= 128 && !fields[6].empty() && fields[6].size() <= 24576,
              "Invalid job submission");
        Key key{fields[4], JobId(fields[5])};
        Job job;
        job.blob = fields[6];
        job.due = now + Number(fields[7], MaxTime);
        job.deadline = fields[8] == L"-1" ? 0 : now + Number(fields[8], MaxTime);
        job.period = Number(fields[9], MaxTime);
        job.backoff = Number(fields[10], MaxBackoff);
        Check(job.backoff >= 10000, "Job retry delay is too short");
        job.policy = static_cast<uint32_t>(Number(fields[11], 1));
        job.flags = static_cast<uint32_t>(Number(fields[12], 255));
        job.warning = static_cast<uint32_t>(Number(fields[13], 100));
        job.idle = Number(fields[14], MaxTime);
        job.stableIdle = Number(fields[15], MaxTime);
        Check(scope.jobs.contains(key) || scope.jobs.size() < 512, "Application job quota exceeded");
        Change(scope, now, [&] {
            auto previous = scope.jobs.find(key);
            if (previous != scope.jobs.end() && !previous->second.lease.empty())
                scope.runs.at(previous->second.lease).stop = 1;
            Check(scope.generation != UINT64_MAX, "Job generations exhausted");
            job.generation = ++scope.generation;
            scope.jobs.insert_or_assign(key, std::move(job));
        });
        return {};
    }
    if (operation == L"cancel" || operation == L"cancel-ns" || operation == L"cancel-all") {
        Check(fields.size() == (operation == L"cancel"      ? 6
                                : operation == L"cancel-ns" ? 5
                                                            : 4),
              "Invalid job cancellation");
        Change(scope, now, [&] {
            Cancel(scope, fields.size() >= 5 ? fields[4] : L"", fields.size() >= 6 ? fields[5] : L"",
                   operation == L"cancel-all");
        });
        return {};
    }
    if (operation == L"accept" || operation == L"stop-accept") {
        Check(fields.size() == 5, "Invalid job acceptance");
        auto run = scope.runs.find(fields[4]);
        if (run == scope.runs.end())
            return {};
        Check(run->second.client == client, "Job acceptance belongs to another Client");
        if (operation == L"accept")
            run->second.accepted = true;
        else
            run->second.delivered = true;
        return {};
    }
    if (operation == L"finish") {
        Check(fields.size() == 6 && (fields[5] == L"0" || fields[5] == L"1"), "Invalid job completion");
        auto run = scope.runs.find(fields[4]);
        if (run == scope.runs.end())
            return {};
        Check(run->second.client == client, "Job completion belongs to another Client");
        Change(scope, now, [&] {
            Finish(scope, fields[4], fields[5] == L"1", now);
        });
        return {};
    }
    if (operation == L"poll") {
        Check(fields.size() == 4, "Invalid job poll");
        size_t active = 0;
        for (auto& [lease, run] : scope.runs)
            if (run.client == client) {
                ++active;
                auto job = scope.jobs.find(run.key);
                if (run.stop < 0 && !run.overrideDeadline && job != scope.jobs.end() &&
                    job->second.generation == run.generation)
                    run.stop = Constraint(job->second, host, conditions);
                if (run.stop < 0 && now >= run.started && now - run.started >= 600000)
                    run.stop = 3;
                if (run.stop >= 0 && !run.delivered)
                    return {L"stop", lease, std::to_wstring(run.stop)};
                if (run.stop < 0 && !run.accepted && job != scope.jobs.end() &&
                    job->second.generation == run.generation)
                    return {L"start",         lease,
                            run.key.first,    std::to_wstring(run.key.second),
                            job->second.blob, run.overrideDeadline ? L"1" : L"0"};
            }
        if (active >= 4)
            return {};
        for (auto& [key, job] : scope.jobs) {
            if (!job.lease.empty())
                continue;
            bool oldExecution = false;
            for (const auto& [lease, run] : scope.runs)
                if (run.key == key) {
                    oldExecution = true;
                    break;
                }
            if (oldExecution)
                continue;
            bool expired = job.deadline && now >= job.deadline;
            if (!expired && (now < job.due || Constraint(job, host, conditions) >= 0))
                continue;
            uint64_t token = 0;
            do {
                Check(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&token), sizeof(token),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
                      "Cannot identify job execution");
            } while (!token || scope.runs.contains(std::to_wstring(token)));
            job.lease = std::to_wstring(token);
            scope.runs.emplace(job.lease, Run{key, job.generation, now, client, expired});
            return {L"start", job.lease, key.first, std::to_wstring(key.second), job.blob, expired ? L"1" : L"0"};
        }
        return {};
    }
    throw std::invalid_argument("Unknown job operation");
}
}
