#include "PendingIntents.h"
#include "../ipc/ServiceProtocol.h"
#include "../host/WindowsPath.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
#include <bcrypt.h>
#include <climits>
#include <algorithm>

namespace stone::windows {
namespace {
using namespace winrt::Windows::Data::Json;
using ipc::Check;
constexpr size_t MaximumRecords = 8192, MaximumBytes = 32 * 1024 * 1024;
constexpr uint32_t OneShot = 0x40000000;

void Id(const std::wstring& value)
{
    Check(value.size() == 32 && value.find_first_not_of(L"0123456789abcdef") == value.npos,
          "Invalid PendingIntent identity");
}

void Payload(const std::wstring& value)
{
    Check(!value.empty() && value.size() <= 24576 && value.size() % 4 == 0, "Invalid PendingIntent payload size");
    auto padding = value.find(L'=');
    auto count = padding == value.npos ? value.size() : padding;
    Check(value.substr(0, count).find_first_not_of(
              L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") == value.npos &&
              (padding == value.npos ||
               value.size() - padding <= 2 && value.find_first_not_of(L'=', padding) == value.npos),
          "Invalid PendingIntent payload encoding");
}

uint32_t Flags(double value)
{
    Check(std::isfinite(value) && std::floor(value) == value && value >= 0 && value <= UINT32_MAX,
          "Invalid PendingIntent flags");
    return static_cast<uint32_t>(value);
}

std::wstring NewId()
{
    unsigned char bytes[16];
    Check(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
          "Cannot identify notification");
    std::wstring result;
    for (auto byte : bytes) {
        result += L"0123456789abcdef"[byte >> 4];
        result += L"0123456789abcdef"[byte & 15];
    }
    return result;
}

std::wstring Text(const JsonObject& object, const wchar_t* name, size_t maximum = 2048)
{
    std::wstring value(object.GetNamedString(name));
    Check(value.size() <= maximum && value.find(L'\0') == value.npos, "Invalid saved notification text");
    return value;
}

void Text(JsonObject& object, const wchar_t* name, const std::wstring& value)
{
    object.Insert(name, JsonValue::CreateStringValue(value));
}

void Strings(JsonObject& object, const wchar_t* name, const std::vector<std::wstring>& values)
{
    JsonArray array;
    for (const auto& value : values)
        array.Append(JsonValue::CreateStringValue(value));
    object.Insert(name, array);
}

std::vector<std::wstring> Strings(const JsonObject& object, const wchar_t* name, size_t maximum)
{
    auto array = object.GetNamedArray(name);
    Check(array.Size() <= maximum, "Too many saved notification fields");
    std::vector<std::wstring> result;
    for (const auto& item : array) {
        std::wstring value(item.GetString());
        Check(value.size() <= 256 && value.find(L'\0') == value.npos, "Invalid saved notification field");
        result.push_back(value);
    }
    return result;
}

double Number(const JsonObject& object, const wchar_t* name, double low, double high)
{
    auto value = object.GetNamedNumber(name);
    Check(std::isfinite(value) && std::floor(value) == value && value >= low && value <= high,
          "Invalid saved notification number");
    return value;
}

template <class Map> void EraseScope(Map& values, const std::wstring& scope)
{
    for (auto it = values.begin(); it != values.end();) {
        if (it->second.scope == scope)
            it = values.erase(it);
        else
            ++it;
    }
}

void Apartment()
{
    static thread_local bool ready = false;
    if (!ready) {
        winrt::init_apartment();
        ready = true;
    }
}
}

PendingIntents::PendingIntents(std::filesystem::path file) : file_(ExtendedWindowsPath(file))
{
    Apartment();
    if (!std::filesystem::exists(file_))
        return;
    auto length = std::filesystem::file_size(file_);
    Check(length <= MaximumBytes, "PendingIntent state is too large");
    std::string bytes(static_cast<size_t>(length), '\0');
    std::ifstream input(file_, std::ios::binary);
    Check(input.read(bytes.data(), bytes.size()).good(), "Cannot read PendingIntent state");
    auto root = JsonObject::Parse(winrt::to_hstring(bytes));
    auto format = root.GetNamedNumber(L"format");
    Check(format == 1 || format == 2, "Unknown PendingIntent state version");
    auto rows = root.GetNamedArray(L"records");
    Check(rows.Size() <= MaximumRecords, "Too many saved PendingIntents");
    for (const auto& row : rows) {
        auto object = row.GetObject();
        PendingIntentSnapshot value;
        value.scope = object.GetNamedString(L"scope");
        value.id = object.GetNamedString(L"id");
        value.payload = object.GetNamedString(L"payload");
        value.flags = Flags(object.GetNamedNumber(L"flags"));
        value.active = object.GetNamedBoolean(L"active");
        Id(value.scope);
        Id(value.id);
        if (value.active)
            Payload(value.payload);
        else
            Check(value.payload.empty(), "Canceled PendingIntent retains a payload");
        const auto id = value.id;
        Check(records_.emplace(id, std::move(value)).second, "Duplicate PendingIntent identity");
    }
    if (format == 1)
        return;
    auto notes = root.GetNamedArray(L"notifications"), queue = root.GetNamedArray(L"deliveries");
    Check(notes.Size() <= 4096 && queue.Size() <= 1024, "Too many saved notifications");
    for (const auto& row : notes) {
        auto object = row.GetObject();
        PendingNotification value;
        value.scope = Text(object, L"scope", 32);
        value.id = Text(object, L"id", 32);
        Id(value.scope);
        Id(value.id);
        value.channel = Text(object, L"channel", 1024);
        value.visual.key = Text(object, L"key");
        value.visual.title = Text(object, L"title", 1024);
        value.visual.text = Text(object, L"text", 8192);
        value.visual.progress = static_cast<int>(Number(object, L"progress", INT_MIN, INT_MAX));
        value.visual.maximum = static_cast<int>(Number(object, L"maximum", INT_MIN, INT_MAX));
        value.visual.indeterminate = object.GetNamedBoolean(L"indeterminate");
        value.visual.silent = object.GetNamedBoolean(L"silent");
        value.visual.suppressPopup = object.GetNamedBoolean(L"suppress");
        value.flags = Flags(object.GetNamedNumber(L"flags"));
        value.visual.actions = Strings(object, L"actions", 5);
        value.targets = Strings(object, L"targets", 7);
        Check(value.targets.size() == value.visual.actions.size() + 2, "Invalid saved notification targets");
        for (const auto& id : value.targets)
            if (!id.empty()) {
                Id(id);
                auto it = records_.find(id);
                Check(it == records_.end() || it->second.scope == value.scope, "Saved notification crosses scope");
            }
        for (const auto& [id, prior] : notifications_)
            Check(prior.scope != value.scope || prior.visual.key != value.visual.key,
                  "Duplicate saved notification key");
        Check(notifications_.emplace(value.id, value).second, "Duplicate saved notification route");
    }
    for (const auto& row : queue) {
        auto object = row.GetObject();
        PendingDelivery value{Text(object, L"scope", 32), Text(object, L"id", 32), Text(object, L"payload", 24576),
                              Text(object, L"key"), Text(object, L"route", 32)};
        Id(value.scope);
        Id(value.id);
        Id(value.route);
        Payload(value.payload);
        value.sequence = static_cast<uint64_t>(Number(object, L"sequence", 1, 9007199254740991.0));
        value.cancel = object.GetNamedBoolean(L"cancel");
        Check(deliveries_.emplace(value.id, value).second, "Duplicate saved notification delivery");
    }
}

void PendingIntents::Save(const std::map<std::wstring, PendingIntentSnapshot>& records) const
{
    Save(records, notifications_, deliveries_);
}

void PendingIntents::Save(const std::map<std::wstring, PendingIntentSnapshot>& records,
                          const std::map<std::wstring, PendingNotification>& notifications,
                          const std::map<std::wstring, PendingDelivery>& deliveries) const
{
    Check(records.size() <= MaximumRecords, "Too many saved PendingIntents");
    JsonObject root;
    JsonArray rows;
    Check(notifications.size() <= 4096 && deliveries.size() <= 1024, "Too many saved notifications");
    root.Insert(L"format", JsonValue::CreateNumberValue(2));
    for (const auto& [id, value] : records) {
        JsonObject object;
        object.Insert(L"id", JsonValue::CreateStringValue(id));
        object.Insert(L"scope", JsonValue::CreateStringValue(value.scope));
        object.Insert(L"payload", JsonValue::CreateStringValue(value.payload));
        object.Insert(L"flags", JsonValue::CreateNumberValue(value.flags));
        object.Insert(L"active", JsonValue::CreateBooleanValue(value.active));
        rows.Append(object);
    }
    root.Insert(L"records", rows);
    JsonArray notes, queue;
    for (const auto& [id, value] : notifications) {
        JsonObject object;
        Text(object, L"id", id);
        Text(object, L"scope", value.scope);
        Text(object, L"channel", value.channel);
        Text(object, L"key", value.visual.key);
        Text(object, L"title", value.visual.title);
        Text(object, L"text", value.visual.text);
        object.Insert(L"flags", JsonValue::CreateNumberValue(value.flags));
        object.Insert(L"progress", JsonValue::CreateNumberValue(value.visual.progress));
        object.Insert(L"maximum", JsonValue::CreateNumberValue(value.visual.maximum));
        object.Insert(L"indeterminate", JsonValue::CreateBooleanValue(value.visual.indeterminate));
        object.Insert(L"silent", JsonValue::CreateBooleanValue(value.visual.silent));
        object.Insert(L"suppress", JsonValue::CreateBooleanValue(value.visual.suppressPopup));
        Strings(object, L"actions", value.visual.actions);
        Strings(object, L"targets", value.targets);
        notes.Append(object);
    }
    for (const auto& [id, value] : deliveries) {
        JsonObject object;
        Text(object, L"id", id);
        Text(object, L"scope", value.scope);
        Text(object, L"payload", value.payload);
        Text(object, L"key", value.key);
        Text(object, L"route", value.route);
        object.Insert(L"sequence", JsonValue::CreateNumberValue(static_cast<double>(value.sequence)));
        object.Insert(L"cancel", JsonValue::CreateBooleanValue(value.cancel));
        queue.Append(object);
    }
    root.Insert(L"notifications", notes);
    root.Insert(L"deliveries", queue);
    auto bytes = winrt::to_string(root.Stringify());
    Check(bytes.size() <= MaximumBytes, "PendingIntent state is too large");
    std::filesystem::create_directories(file_.parent_path());
    auto temporary = file_;
    temporary += L".tmp";
    {
        ipc::Handle file(
            CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        Check(file.value && WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                  written == bytes.size() && FlushFileBuffers(file.value),
              "Cannot save PendingIntent state");
    }
    Check(MoveFileExW(temporary.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
          "Cannot publish PendingIntent state");
}

bool PendingIntents::Put(PendingIntentSnapshot value, bool create)
{
    Id(value.scope);
    Id(value.id);
    Payload(value.payload);
    Check(value.active, "Cannot publish a canceled PendingIntent");
    auto found = records_.find(value.id);
    Check(found == records_.end() || found->second.scope == value.scope, "PendingIntent scope changed");
    // Keep cancellation tombstones: a retry after an uncertain IPC result must
    // never recreate an already consumed or explicitly canceled capability.
    if (found != records_.end() && !found->second.active || !create && found == records_.end())
        return false;
    Check(found == records_.end() || found->second.flags == value.flags, "PendingIntent identity flags changed");
    if (found != records_.end() && found->second.payload == value.payload)
        return true;
    auto next = records_;
    const auto id = value.id;
    next.insert_or_assign(id, std::move(value));
    Save(next);
    records_ = std::move(next);
    return true;
}

void PendingIntents::Cancel(const std::wstring& scope, const std::wstring& id)
{
    Id(scope);
    Id(id);
    auto found = records_.find(id);
    Check(found == records_.end() || found->second.scope == scope, "PendingIntent scope changed");
    if (found != records_.end() && !found->second.active)
        return;
    auto next = records_;
    auto& value = next[id];
    value.scope = scope;
    value.id = id;
    value.payload.clear();
    value.active = false;
    Save(next);
    records_ = std::move(next);
}

bool PendingIntents::Contains(const std::wstring& scope, const std::wstring& id) const
{
    Id(scope);
    Id(id);
    auto found = records_.find(id);
    return found != records_.end() && found->second.scope == scope && found->second.active;
}

bool PendingIntents::Consume(const std::wstring& scope, const std::wstring& id)
{
    if (!Contains(scope, id))
        return false;
    if (records_.at(id).flags & OneShot)
        Cancel(scope, id);
    return true;
}

std::vector<PendingIntentSnapshot> PendingIntents::List(const std::wstring& scope, const std::wstring& after) const
{
    Id(scope);
    if (!after.empty())
        Id(after);
    std::vector<PendingIntentSnapshot> result;
    for (auto it = records_.upper_bound(after); it != records_.end(); ++it)
        if (it->second.scope == scope && it->second.active)
            result.push_back(it->second);
    return result;
}

std::optional<PendingIntentSnapshot> PendingIntents::Next(const std::wstring& scope, const std::wstring& after) const
{
    Id(scope);
    if (!after.empty())
        Id(after);
    for (auto it = records_.upper_bound(after); it != records_.end(); ++it)
        if (it->second.scope == scope && it->second.active)
            return it->second;
    return {};
}

void PendingIntents::Forget(const std::wstring& scope)
{
    Id(scope);
    auto next = records_;
    auto notes = notifications_;
    auto queue = deliveries_;
    EraseScope(next, scope);
    EraseScope(notes, scope);
    EraseScope(queue, scope);
    if (next.size() != records_.size() || notes.size() != notifications_.size() || queue.size() != deliveries_.size()) {
        Save(next, notes, queue);
        records_ = std::move(next);
        notifications_ = std::move(notes);
        deliveries_ = std::move(queue);
    }
}

void PendingIntents::Retain(const std::vector<std::wstring>& scopes)
{
    std::set<std::wstring> allowed;
    for (const auto& scope : scopes) {
        Id(scope);
        allowed.insert(scope);
    }
    auto next = records_;
    for (auto it = next.begin(); it != next.end();) {
        if (!allowed.contains(it->second.scope))
            it = next.erase(it);
        else
            ++it;
    }
    auto notes = notifications_;
    auto queue = deliveries_;
    for (auto it = notes.begin(); it != notes.end();) {
        if (!allowed.contains(it->second.scope))
            it = notes.erase(it);
        else
            ++it;
    }
    for (auto it = queue.begin(); it != queue.end();) {
        if (!allowed.contains(it->second.scope))
            it = queue.erase(it);
        else
            ++it;
    }
    if (next.size() != records_.size() || notes.size() != notifications_.size() || queue.size() != deliveries_.size()) {
        Save(next, notes, queue);
        records_ = std::move(next);
        notifications_ = std::move(notes);
        deliveries_ = std::move(queue);
    }
}

PendingNotification PendingIntents::Post(PendingNotification value)
{
    Id(value.scope);
    Check(value.targets.size() == value.visual.actions.size() + 2 && value.targets.size() <= 7,
          "Invalid notification targets");
    Check(value.visual.key.size() <= 2048 && value.visual.title.size() <= 1024 && value.visual.text.size() <= 8192 &&
              value.channel.size() <= 1024,
          "Notification text is too large");
    for (const auto& id : value.targets)
        if (!id.empty()) {
            Id(id);
            auto it = records_.find(id);
            Check(it != records_.end() && it->second.scope == value.scope, "Unknown notification target");
        }
    Check(!(value.flags & 64), "Foreground notifications require their live Client");
    value.id = NewId();
    auto notes = notifications_;
    for (auto it = notes.begin(); it != notes.end();) {
        if (it->second.scope == value.scope && it->second.visual.key == value.visual.key)
            it = notes.erase(it);
        else
            ++it;
    }
    notes.emplace(value.id, value);
    Save(records_, notes, deliveries_);
    notifications_ = std::move(notes);
    return value;
}

std::vector<PendingNotification> PendingIntents::Notifications(const std::wstring& scope) const
{
    std::vector<PendingNotification> result;
    for (const auto& [id, value] : notifications_)
        if (scope.empty() || value.scope == scope)
            result.push_back(value);
    return result;
}

PendingNotification PendingIntents::Notification(const std::wstring& route) const
{
    Id(route);
    auto it = notifications_.find(route);
    Check(it != notifications_.end(), "Notification is no longer active");
    return it->second;
}

void PendingIntents::RemoveNotification(const std::wstring& scope, const std::wstring& key)
{
    auto notes = notifications_;
    for (auto it = notes.begin(); it != notes.end();) {
        if (it->second.scope == scope && it->second.visual.key == key)
            it = notes.erase(it);
        else
            ++it;
    }
    if (notes.size() != notifications_.size()) {
        Save(records_, notes, deliveries_);
        notifications_ = std::move(notes);
    }
}

void PendingIntents::ClearNotifications(const std::wstring& scope, bool deliveries)
{
    auto notes = notifications_;
    auto queue = deliveries_;
    EraseScope(notes, scope);
    if (deliveries)
        EraseScope(queue, scope);
    if (notes.size() != notifications_.size() || queue.size() != deliveries_.size()) {
        Save(records_, notes, queue);
        notifications_ = std::move(notes);
        deliveries_ = std::move(queue);
    }
}

PendingDelivery PendingIntents::Activate(const std::wstring& route, int action)
{
    auto note = Notification(route);
    Check(action >= -2 && (action < 0 || static_cast<size_t>(action + 2) < note.targets.size()),
          "Invalid notification action");
    Check(action != -2 || !(note.flags & 2), "Ongoing notification cannot be dismissed");
    PendingDelivery delivery{note.scope, NewId(), L"", note.visual.key, route};
    delivery.cancel = action == -2 || action == -1 && (note.flags & 16);
    auto records = records_;
    auto notes = notifications_;
    auto queue = deliveries_;
    const auto& target = note.targets[action == -1 ? 0 : action == -2 ? 1 : action + 2];
    if (!target.empty()) {
        auto it = records.find(target);
        Check(it != records.end() && it->second.scope == note.scope && it->second.active,
              "Notification PendingIntent was canceled");
        delivery.payload = it->second.payload;
        for (const auto& [id, value] : queue)
            delivery.sequence = std::max(delivery.sequence, value.sequence);
        Check(delivery.sequence < 9007199254740991ULL, "Notification delivery sequence exhausted");
        ++delivery.sequence;
        if (it->second.flags & OneShot) {
            it->second.active = false;
            it->second.payload.clear();
        }
        queue.emplace(delivery.id, delivery);
    }
    if (delivery.cancel)
        notes.erase(route);
    Save(records, notes, queue);
    records_ = std::move(records);
    notifications_ = std::move(notes);
    deliveries_ = std::move(queue);
    return delivery;
}

std::optional<PendingDelivery> PendingIntents::Delivery(const std::wstring& scope) const
{
    std::optional<PendingDelivery> result;
    for (const auto& [id, value] : deliveries_)
        if (value.scope == scope && (!result || value.sequence < result->sequence))
            result = value;
    return result;
}

bool PendingIntents::Claim(const std::wstring& scope, const std::wstring& id)
{
    auto it = deliveries_.find(id);
    if (it == deliveries_.end())
        return false;
    Check(it->second.scope == scope, "Notification delivery crosses scope");
    auto queue = deliveries_;
    queue.erase(id);
    Save(records_, notifications_, queue);
    deliveries_ = std::move(queue);
    return true;
}

std::vector<std::wstring> ServicePendingIntentRequest(PendingIntents& store, const std::wstring& scope,
                                                      const std::vector<std::wstring>& fields)
{
    Check(fields.size() >= 3, "Missing PendingIntent operation");
    const auto& operation = fields[2];
    if (operation == L"pi-create" || operation == L"pi-update") {
        Check(fields.size() == 6 && !fields[4].empty() && fields[4].size() <= 10 &&
                  fields[4].find_first_not_of(L"0123456789") == fields[4].npos,
              "Invalid PendingIntent snapshot fields");
        auto flags = std::stoull(fields[4]);
        Check(flags <= UINT32_MAX, "Invalid PendingIntent flags");
        return {store.Put({scope, fields[3], fields[5], static_cast<uint32_t>(flags)}, operation == L"pi-create")
                    ? L"1"
                    : L"0"};
    }
    Check(fields.size() == 4, "Invalid PendingIntent request fields");
    if (operation == L"pi-cancel") {
        store.Cancel(scope, fields[3]);
        return {};
    }
    if (operation == L"pi-check")
        return {store.Contains(scope, fields[3]) ? L"1" : L"0"};
    if (operation == L"pi-consume")
        return {store.Consume(scope, fields[3]) ? L"1" : L"0"};
    if (operation == L"pi-delivery") {
        Check(fields[3].empty(), "Invalid delivery query");
        auto row = store.Delivery(scope);
        if (!row)
            return {};
        return {row->id, row->payload, row->key, row->route, row->cancel ? L"1" : L"0"};
    }
    if (operation == L"pi-claim")
        return {store.Claim(scope, fields[3]) ? L"1" : L"0"};
    if (operation == L"pi-list") {
        auto row = store.Next(scope, fields[3]);
        if (!row)
            return {};
        return {row->id, std::to_wstring(row->flags), row->payload};
    }
    throw std::invalid_argument("Unknown PendingIntent operation");
}
}
