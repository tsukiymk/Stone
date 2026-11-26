#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace stone::windows {
// Transient Activity relationships. The broker supplies authenticated process,
// data/image scope and package identities; no Android object crosses this layer.
class ActivityRoutes {
    struct Peer {
        std::wstring scope, package;
        uint64_t generation;
    };

    struct Route {
        uint32_t source, target;
        std::wstring key, caller, payload;
        bool result, claimed = false, finished = false;
    };

    struct Event {
        uint32_t peer;
        std::vector<std::wstring> fields;
    };

    std::map<uint32_t, Peer> peers_;
    std::map<std::wstring, Route> routes_;
    std::map<uint64_t, Event> events_;
    uint64_t sequence_ = 0;
    uint64_t nextPeer_ = 0;
    std::wstring epoch_;
    void EventFor(uint32_t peer, std::vector<std::wstring> fields);
    void RemoveEvents(const std::wstring& route);
    void Cancel(std::map<std::wstring, Route>::iterator route, bool sourceGone);

public:
    ActivityRoutes();

    const std::wstring& Epoch() const
    {
        return epoch_;
    }

    void Bind(uint32_t process, const std::wstring& scope, const std::wstring& package);
    void Drop(uint32_t process);
    std::wstring Start(uint32_t source, uint32_t target, const std::wstring& key, const std::wstring& caller,
                       const std::wstring& payload, bool result);
    std::vector<std::wstring> Peek(uint32_t process) const;
    bool Claim(uint32_t process, const std::wstring& event);
    bool Finish(uint32_t process, const std::wstring& route, int32_t result, const std::wstring& payload);
    void Abandon(uint32_t process, const std::wstring& key);
    bool Busy(uint32_t process) const;
};
}
