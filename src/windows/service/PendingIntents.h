#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "WindowsNotifications.h"

namespace stone::windows {
struct PendingIntentSnapshot {
    std::wstring scope, id, payload;
    uint32_t flags = 0;
    bool active = true;
};

struct PendingNotification {
    std::wstring scope, id, channel;
    WindowsNotification visual;
    std::vector<std::wstring> targets; // content, delete, then action buttons
    uint32_t flags = 0;
};

struct PendingDelivery {
    std::wstring scope, id, payload, key, route;
    uint64_t sequence = 0;
    bool cancel = false;
};

// Serialized by the broker. Payloads are opaque, image/code-bound ART Parcels;
// this module never loads an APK, framework, JVM or ART.
class PendingIntents {
    std::filesystem::path file_;
    std::map<std::wstring, PendingIntentSnapshot> records_;
    std::map<std::wstring, PendingNotification> notifications_;
    std::map<std::wstring, PendingDelivery> deliveries_;
    void Save(const std::map<std::wstring, PendingIntentSnapshot>& records) const;
    void Save(const std::map<std::wstring, PendingIntentSnapshot>& records,
              const std::map<std::wstring, PendingNotification>& notifications,
              const std::map<std::wstring, PendingDelivery>& deliveries) const;

public:
    explicit PendingIntents(std::filesystem::path file);
    bool Put(PendingIntentSnapshot value, bool create);
    void Cancel(const std::wstring& scope, const std::wstring& id);
    bool Contains(const std::wstring& scope, const std::wstring& id) const;
    bool Consume(const std::wstring& scope, const std::wstring& id);
    std::vector<PendingIntentSnapshot> List(const std::wstring& scope, const std::wstring& after = {}) const;
    std::optional<PendingIntentSnapshot> Next(const std::wstring& scope, const std::wstring& after) const;
    void Forget(const std::wstring& scope);
    void Retain(const std::vector<std::wstring>& scopes);
    PendingNotification Post(PendingNotification value);
    std::vector<PendingNotification> Notifications(const std::wstring& scope = {}) const;
    PendingNotification Notification(const std::wstring& route) const;
    void RemoveNotification(const std::wstring& scope, const std::wstring& key);
    void ClearNotifications(const std::wstring& scope, bool deliveries = false);
    PendingDelivery Activate(const std::wstring& route, int action);
    std::optional<PendingDelivery> Delivery(const std::wstring& scope) const;
    bool Claim(const std::wstring& scope, const std::wstring& id);
};

std::vector<std::wstring> ServicePendingIntentRequest(PendingIntents& store, const std::wstring& scope,
                                                      const std::vector<std::wstring>& fields);
}
