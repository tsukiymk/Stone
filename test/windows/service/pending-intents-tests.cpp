#include "PendingIntents.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace stone::windows;
namespace fs = std::filesystem;

namespace {
const std::wstring scope(32, L'a'), other(32, L'b'), id(32, L'c'), second(32, L'd');

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template <class F> void Reject(F action, const char* message)
{
    bool rejected = false;
    try {
        action();
    }
    catch (...) {
        rejected = true;
    }
    Require(rejected, message);
}
}

int main()
{
    try {
        auto directory = fs::absolute(STONE_INTENT_TEST_DIRECTORY) / std::to_wstring(GetCurrentProcessId());
        fs::create_directories(directory);
        auto file = directory / L"state.json";
        PendingIntents store(file);
        Require(!store.Put({scope, id, L"AQID", 0}, false), "Update created an unknown identity");
        Require(store.Put({scope, id, L"AQID", 0}, true), "Cannot create snapshot");
        Require(store.Put({scope, id, L"AQID", 0}, true), "Identical create retry failed");
        Reject(
            [&] {
                store.Put({other, id, L"AQID", 0}, true);
            },
            "Another scope overwrote a snapshot");
        Require(!store.Contains(other, id) && store.List(other).empty(), "Snapshot leaked into another scope");
        Reject(
            [&] {
                store.Cancel(other, id);
            },
            "Another scope canceled a snapshot");
        Reject(
            [&] {
                store.Put({scope, id, L"AQID", 0x40000000}, false);
            },
            "Identity flags changed");
        Require(store.Put({scope, id, L"BAUG", 0}, false), "Cannot update extras");
        PendingIntents restored(file);
        Require(restored.List(scope).size() == 1 && restored.List(scope)[0].payload == L"BAUG",
                "Snapshot did not survive replacement");
        Require(restored.Consume(scope, id) && restored.Consume(scope, id), "Reusable Intent consumed once");
        restored.Cancel(scope, id);
        Require(!restored.Put({scope, id, L"AQID", 0}, true) && !restored.Put({scope, id, L"AQID", 0}, false),
                "Canceled identity was resurrected");
        PendingIntents afterCancel(file);
        Require(!afterCancel.Contains(scope, id) && !afterCancel.Consume(scope, id), "Cancellation was not durable");
        Require(afterCancel.Put({scope, second, L"AQI=", 0x40000000}, true), "Cannot create one-shot");
        Require(afterCancel.Consume(scope, second) && !afterCancel.Consume(scope, second),
                "One-shot was delivered twice");
        PendingIntents afterConsume(file);
        Require(!afterConsume.Put({scope, second, L"AQI=", 0x40000000}, true),
                "One-shot create retry resurrected identity");
        auto unseen = std::wstring(32, L'e');
        afterConsume.Cancel(scope, unseen);
        Require(!afterConsume.Put({scope, unseen, L"AQID", 0}, true),
                "Cancellation preceding an uncertain create was lost");
        Reject(
            [&] {
                afterConsume.Put({scope, std::wstring(32, L'f'), L"A===", 0}, true);
            },
            "Malformed Base64 was accepted");
        auto foreign = std::wstring(32, L'1');
        Require(afterConsume.Put({other, foreign, L"AQID", 0}, true), "Cannot create independent scope");
        afterConsume.Forget(scope);
        PendingIntents removed(file);
        Require(removed.List(scope).empty() && removed.Contains(other, foreign),
                "Package cleanup crossed scope boundary");
        auto all = removed.List(other);
        Require(all.size() == 1 && removed.List(other, foreign).empty(), "Invalid page cursor");
        // Force atomic replace failure after the temp file was flushed. Both the
        // live model and a fresh reader must retain the last acknowledged state.
        HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "Cannot lock store for failure injection");
        Reject(
            [&] {
                removed.Put({other, foreign, L"BAUG", 0}, false);
            },
            "Failed replacement reported success");
        CloseHandle(locked);
        Require(removed.List(other)[0].payload == L"AQID" && PendingIntents(file).List(other)[0].payload == L"AQID",
                "Failed write changed acknowledged state");
        auto fields = std::vector<std::wstring>{L"data", L"app", L"pi-check", foreign};
        Require(ServicePendingIntentRequest(removed, other, fields) == std::vector<std::wstring>{L"1"},
                "Service query failed");
        fields[2] = L"pi-update";
        fields.push_back(L"4294967296");
        fields.push_back(L"AQID");
        Reject(
            [&] {
                ServicePendingIntentRequest(removed, other, fields);
            },
            "Oversized flags accepted");
        auto corrupt = directory / L"corrupt.json";
        {
            std::ofstream output(corrupt);
            output << "{\"format\":3,\"records\":[]}";
        }
        Reject(
            [&] {
                PendingIntents invalid(corrupt);
            },
            "Unknown format accepted");
        removed.Retain({scope});
        Require(!PendingIntents(file).Contains(other, foreign) && !removed.Next(other, L""),
                "Revoked launch profile retained snapshots");
        auto notificationFile = directory / L"notifications.json";
        PendingIntents notices(notificationFile);
        auto target = std::wstring(32, L'2');
        notices.Put({scope, target, L"AQID", 0x40000000}, true);
        PendingNotification note;
        note.scope = scope;
        note.channel = L"test";
        note.visual.key = L"+:21";
        note.visual.title = L"Persistent notice";
        note.targets = {target, L""};
        note.flags = 16;
        note = notices.Post(note);
        locked =
            CreateFileW(notificationFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "Cannot lock notification store");
        Reject(
            [&] {
                notices.Activate(note.id, -1);
            },
            "Notification accepted without a durable commit");
        CloseHandle(locked);
        Require(notices.Contains(scope, target) && !notices.Delivery(scope) &&
                    notices.Notification(note.id).id == note.id,
                "Failed acceptance consumed or queued an event");
        notices.Put({scope, target, L"BAUG", 0x40000000}, false);
        auto delivery = notices.Activate(note.id, -1);
        Require(delivery.payload == L"BAUG" && delivery.cancel && !notices.Contains(scope, target),
                "Accepted event lost latest extras or one-shot state");
        Reject(
            [&] {
                notices.Activate(note.id, -1);
            },
            "Canceled old notification route was accepted");
        notices.Cancel(scope, target);
        PendingIntents queued(notificationFile);
        Require(queued.Delivery(scope) && queued.Delivery(scope)->payload == L"BAUG" && !queued.Delivery(other),
                "Accepted snapshot did not survive Service replacement");
        Reject(
            [&] {
                queued.Claim(other, delivery.id);
            },
            "Another application claimed an event");
        Require(queued.Claim(scope, delivery.id) && !queued.Claim(scope, delivery.id) &&
                    !PendingIntents(notificationFile).Delivery(scope),
                "Claimed event was replayed");
        auto reusable = std::wstring(32, L'3');
        queued.Put({scope, reusable, L"AQID", 0}, true);
        note.targets = {reusable, L""};
        note.flags = 0;
        auto first = queued.Post(note);
        auto replacement = queued.Post(note);
        Reject(
            [&] {
                queued.Activate(first.id, -1);
            },
            "Replacement left the old route usable");
        auto event1 = queued.Activate(replacement.id, -1);
        queued.Put({scope, reusable, L"BAUG", 0}, false);
        auto event2 = queued.Activate(replacement.id, -1);
        Require(queued.Delivery(scope)->id == event1.id && event1.payload == L"AQID" && event2.payload == L"BAUG",
                "Events were reordered or changed after acceptance");
        queued.Claim(scope, event1.id);
        Require(queued.Delivery(scope)->id == event2.id, "Delivery order did not advance");
        queued.Forget(scope);
        Require(queued.Notifications().empty() && !queued.Delivery(scope),
                "Uninstall retained notification routes or events");
        std::cout << "PASS PendingIntent persistence, cancellation, one-shot, scope isolation and write failure\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
