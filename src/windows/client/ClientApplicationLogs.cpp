#include "ClientApplicationLogs.h"
#include "BrokerClient.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace stone::windows {
namespace {
struct State {
    std::mutex gate;
    std::condition_variable changed;
    std::deque<std::pair<uint64_t, std::wstring>> records;
    uint64_t submitted = 0, completed = 0, dropped = 0, requested = 0, flushed = 0;
    size_t size = 0;
    bool started = false, stop = false, finished = false, failed = false, flush = false;
};

// A native application can exit without unwinding C++ globals.
State* state = new State;

void Run() noexcept
{
    std::unique_lock lock(state->gate);
    while (!state->stop || !state->records.empty() || state->dropped) {
        state->changed.wait_for(lock, std::chrono::milliseconds(250), [] {
            return state->stop || state->flush || state->size >= 8000;
        });
        if (state->records.empty() && !state->dropped) {
            state->flush = false;
            state->flushed = state->requested;
            state->changed.notify_all();
            continue;
        }
        const auto request = state->requested;
        std::wstring batch;
        if (state->dropped) {
            batch = L"StoneLog: dropped " + std::to_wstring(state->dropped) +
                    L" records because the Client queue was full\n";
            state->dropped = 0;
        }
        auto completed = state->completed;
        while (!state->records.empty() && batch.size() + state->records.front().second.size() <= 8000) {
            auto& record = state->records.front();
            completed = record.first;
            state->size -= record.second.size();
            batch += record.second;
            state->records.pop_front();
        }
        lock.unlock();
        bool failed = false;
        try {
            ClientApplicationLogRequest({L"append", batch});
        }
        catch (const std::exception& error) {
            failed = true;
            std::fprintf(stderr, "[logcat] log retention failed: %s\n", error.what());
        }
        catch (...) {
            failed = true;
        }
        lock.lock();
        state->completed = completed;
        state->flushed = request;
        state->failed = state->failed || failed;
        if (state->records.empty())
            state->flush = false;
        state->changed.notify_all();
    }
    state->finished = true;
    state->changed.notify_all();
}
}

void StartClientApplicationLogs()
{
    if (!ClientClipboardEnabled() || !ClientShellEnabled())
        return;
    const std::lock_guard lock(state->gate);
    if (state->started)
        return;
    std::thread worker(Run);
    worker.detach();
    state->started = true;
}

void QueueClientApplicationLog(std::wstring record) noexcept
{
    try {
        const std::lock_guard lock(state->gate);
        if (!state->started || state->stop)
            return;
        if (record.empty() || record.size() > 7900 || state->size + record.size() > 256000) {
            ++state->dropped;
            return;
        }
        state->records.emplace_back(state->submitted + 1, std::move(record));
        ++state->submitted;
        state->size += state->records.back().second.size();
        state->changed.notify_all();
    }
    catch (...) { /* Logging must not throw into Android's logger. */
    }
}

void FlushClientApplicationLogs()
{
    std::unique_lock lock(state->gate);
    if (!state->started)
        throw std::runtime_error("Application log collection is unavailable");
    auto target = state->submitted;
    auto request = ++state->requested;
    state->flush = true;
    state->changed.notify_all();
    if (!state->changed.wait_for(lock, std::chrono::seconds(20), [&] {
            return state->completed >= target && state->flushed >= request;
        }))
        throw std::runtime_error("Application log flush timed out");
    if (state->failed)
        throw std::runtime_error("Some application logs could not be retained");
}

std::wstring ReadClientApplicationLogs()
{
    static std::mutex snapshots;
    const std::lock_guard lock(snapshots);
    FlushClientApplicationLogs();
    const auto count = ClientApplicationLogRequest({L"begin"});
    if (count.empty() || count.size() > 6 || count.find_first_not_of(L"0123456789") != count.npos)
        throw std::runtime_error("Invalid application log snapshot length");
    const auto size = std::stoul(count);
    if (size > 512000)
        throw std::runtime_error("Application log snapshot exceeds its limit");
    std::wstring text;
    while (text.size() < size) {
        auto page = ClientApplicationLogRequest({L"next"});
        if (page.empty() || page.size() > 24000 || text.size() + page.size() > size)
            throw std::runtime_error("Invalid application log snapshot page");
        text += page;
    }
    return text;
}

void StopClientApplicationLogs() noexcept
{
    try {
        std::unique_lock lock(state->gate);
        if (!state->started)
            return;
        state->stop = true;
        state->changed.notify_all();
        if (!state->changed.wait_for(lock, std::chrono::seconds(20), [] {
                return state->finished;
            }))
            std::fprintf(stderr, "[logcat] application log shutdown timed out\n");
    }
    catch (...) {
    }
}
}
