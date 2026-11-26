#include "ActivityRoutes.h"
#include "../ipc/ServiceProtocol.h"
#include "../host/CommandLine.h"
#include <bcrypt.h>
#include <limits>

namespace stone::windows {
using ipc::Check;

namespace {
void Key(const std::wstring& key)
{
    Check(key.size() == 32 && key.find_first_not_of(L"0123456789abcdef") == key.npos, "Invalid Activity request key");
}

void Payload(const std::wstring& payload)
{
    Check(payload.size() <= 16384 &&
              payload.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") ==
                  payload.npos,
          "Invalid Activity payload");
}
}

ActivityRoutes::ActivityRoutes()
{
    unsigned char random[16]{};
    Check(BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
          "Cannot identify Activity broker epoch");
    for (auto byte : random) {
        epoch_ += L"0123456789abcdef"[byte >> 4];
        epoch_ += L"0123456789abcdef"[byte & 15];
    }
}

void ActivityRoutes::EventFor(uint32_t peer, std::vector<std::wstring> fields)
{
    Check(events_.size() < 4096 && sequence_ != std::numeric_limits<uint64_t>::max(), "Activity event queue is full");
    events_.emplace(++sequence_, Event{peer, std::move(fields)});
}

void ActivityRoutes::RemoveEvents(const std::wstring& route)
{
    for (auto it = events_.begin(); it != events_.end();) {
        if (it->second.fields[1] == route)
            it = events_.erase(it);
        else
            ++it;
    }
}

void ActivityRoutes::Bind(uint32_t process, const std::wstring& scope, const std::wstring& package)
{
    Check(process != 0 && !scope.empty() && stone::session::IsPackageName(package), "Invalid Activity peer");
    auto found = peers_.find(process);
    if (found != peers_.end()) {
        if (found->second.scope == scope && found->second.package == package)
            return;
        Drop(process); // A changed peer identity cannot inherit old routes.
    }
    Check(nextPeer_ != std::numeric_limits<uint64_t>::max(), "Activity peer generation exhausted");
    peers_.emplace(process, Peer{scope, package, ++nextPeer_});
}

std::wstring ActivityRoutes::Start(uint32_t source, uint32_t target, const std::wstring& key,
                                   const std::wstring& caller, const std::wstring& payload, bool result)
{
    Key(key);
    Payload(payload);
    auto from = peers_.find(source), to = peers_.find(target);
    Check(from != peers_.end() && to != peers_.end() && source != target && from->second.scope == to->second.scope,
          "Activity peers do not share a data/image scope");
    Check(caller.starts_with(from->second.package + L"/") && caller.size() <= 1024 &&
              caller.size() > from->second.package.size() + 1,
          "Activity caller component is outside its package");
    const auto id = epoch_ + L"." + std::to_wstring(from->second.generation) + L":" + key;
    auto previous = routes_.find(id);
    if (previous != routes_.end()) {
        const auto& value = previous->second;
        Check(value.target == target && value.caller == caller && value.payload == payload && value.result == result,
              "Activity request identity changed");
        return id; // Explicit retries cannot enqueue another launch.
    }
    Check(routes_.size() < 1024 && events_.size() < 3072 && !payload.empty(), "Too many or empty Activity requests");
    EventFor(target, {from->second.package == to->second.package ? L"launch-local" : L"launch", id,
                      result ? caller : L"", payload});
    routes_.emplace(id, Route{source, target, key, caller, payload, result});
    return id;
}

std::vector<std::wstring> ActivityRoutes::Peek(uint32_t process) const
{
    Check(peers_.contains(process), "Unknown Activity peer");
    for (const auto& [sequence, event] : events_)
        if (event.peer == process) {
            std::vector<std::wstring> result{std::to_wstring(sequence)};
            result.insert(result.end(), event.fields.begin(), event.fields.end());
            return result;
        }
    return {};
}

bool ActivityRoutes::Claim(uint32_t process, const std::wstring& event)
{
    Check(!event.empty() && event.size() <= 20 && event.find_first_not_of(L"0123456789") == event.npos,
          "Invalid Activity event");
    auto found = events_.find(std::stoull(event));
    if (found == events_.end())
        return false;
    Check(found->second.peer == process, "Activity event belongs to another Client");
    if (found->second.fields[0] != L"revoke") {
        auto route = routes_.find(found->second.fields[1]);
        Check(route != routes_.end(), "Activity route was revoked");
        if (found->second.fields[0] == L"launch" || found->second.fields[0] == L"launch-local")
            route->second.claimed = true;
        else
            routes_.erase(route);
    }
    events_.erase(found);
    return true;
}

bool ActivityRoutes::Finish(uint32_t process, const std::wstring& id, int32_t result, const std::wstring& payload)
{
    Payload(payload);
    auto found = routes_.find(id);
    if (found == routes_.end())
        return false;
    auto& route = found->second;
    Check(route.target == process && route.claimed, "Activity result does not belong to this Client");
    if (route.finished)
        return false;
    if (route.result) {
        EventFor(route.source,
                 {peers_.at(route.source).package == peers_.at(route.target).package ? L"result-local" : L"result", id,
                  route.key, std::to_wstring(result), payload});
        route.finished = true;
    }
    else
        routes_.erase(found);
    return true;
}

void ActivityRoutes::Cancel(std::map<std::wstring, Route>::iterator found, bool sourceGone)
{
    auto id = found->first;
    auto& route = found->second;
    RemoveEvents(id);
    if (!sourceGone && route.result) {
        EventFor(route.source, {L"result", id, route.key, L"0", L""});
        route.finished = true;
    }
    else if (sourceGone && route.claimed && !route.finished && route.result) {
        EventFor(route.target, {L"revoke", id});
        routes_.erase(found);
    }
    else
        routes_.erase(found);
}

void ActivityRoutes::Drop(uint32_t process)
{
    for (auto it = routes_.begin(); it != routes_.end();) {
        auto current = it++;
        if (current->second.source == process)
            Cancel(current, true);
        else if (current->second.target == process && !current->second.finished)
            Cancel(current, false);
    }
    for (auto it = events_.begin(); it != events_.end();) {
        if (it->second.peer == process)
            it = events_.erase(it);
        else
            ++it;
    }
    peers_.erase(process);
}

void ActivityRoutes::Abandon(uint32_t process, const std::wstring& key)
{
    Key(key);
    auto peer = peers_.find(process);
    if (peer == peers_.end())
        return;
    auto found = routes_.find(epoch_ + L"." + std::to_wstring(peer->second.generation) + L":" + key);
    if (found != routes_.end())
        Cancel(found, true);
}

bool ActivityRoutes::Busy(uint32_t process) const
{
    for (const auto& [id, route] : routes_)
        if ((route.source == process && route.result) || (route.target == process && !route.finished))
            return true;
    for (const auto& [id, event] : events_)
        if (event.peer == process)
            return true;
    return false;
}
}
