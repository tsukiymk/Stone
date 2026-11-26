#include "ActivityRoutes.h"
#include <iostream>
#include <stdexcept>
using stone::windows::ActivityRoutes;

namespace {
void Require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

template <class F> void Reject(F action)
{
    bool rejected = false;
    try {
        action();
    }
    catch (const std::exception&) {
        rejected = true;
    }
    Require(rejected, "Foreign or malformed Activity operation was accepted");
}

const std::wstring key(32, L'a'), other(32, L'b');
}

int main()
{
    try {
        ActivityRoutes routes;
        routes.Bind(1, L"data/image", L"org.source");
        routes.Bind(2, L"data/image", L"org.target");
        routes.Bind(3, L"data/image", L"org.foreign");
        routes.Bind(4, L"other/image", L"org.target");
        Reject([&] {
            routes.Start(1, 4, key, L"org.source/A", L"AQID", true);
        });
        Reject([&] {
            routes.Start(1, 2, key, L"org.foreign/A", L"AQID", true);
        });
        Reject([&] {
            routes.Start(1, 2, L"bad", L"org.source/A", L"AQID", true);
        });
        Reject([&] {
            routes.Start(1, 2, key, L"org.source/A", L"not base64!", true);
        });
        auto id = routes.Start(1, 2, key, L"org.source/Alias", L"AQID", true);
        Require(routes.Start(1, 2, key, L"org.source/Alias", L"AQID", true) == id, "Retry changed Activity route");
        Reject([&] {
            routes.Start(1, 2, key, L"org.source/Alias", L"BAUG", true);
        });
        auto launch = routes.Peek(2);
        Require(launch.size() == 5 && launch[1] == L"launch" && launch[2] == id && launch[3] == L"org.source/Alias",
                "Caller identity lost");
        Require(routes.Peek(3).empty() && routes.Peek(1).empty(), "Launch leaked to another Client");
        Reject([&] {
            routes.Claim(3, launch[0]);
        });
        Reject([&] {
            routes.Finish(2, id, -1, L"BAUG");
        });
        Require(routes.Claim(2, launch[0]) && !routes.Claim(2, launch[0]), "Launch claim replayed");
        Reject([&] {
            routes.Finish(3, id, -1, L"BAUG");
        });
        Require(routes.Finish(2, id, -1, L"BAUG") && !routes.Finish(2, id, -1, L"BAUG"), "Duplicate result delivered");
        auto result = routes.Peek(1);
        Require(result.size() == 6 && result[1] == L"result" && result[3] == key && result[4] == L"-1" &&
                    result[5] == L"BAUG",
                "Result changed");
        Reject([&] {
            routes.Claim(2, result[0]);
        });
        routes.Claim(1, result[0]);
        Require(!routes.Busy(1) && !routes.Busy(2), "Completed result retained Client");
        id = routes.Start(1, 2, key, L"org.source/A", L"AQID", false);
        Require(!routes.Busy(1) && routes.Busy(2), "Ordinary target retained its source process");
        launch = routes.Peek(2);
        Require(launch[3].empty(), "Ordinary start acquired caller identity");
        routes.Claim(2, launch[0]);
        routes.Finish(2, id, 0, L"");
        Require(routes.Peek(1).empty() && !routes.Busy(1), "Ordinary start delivered result");
        id = routes.Start(1, 2, key, L"org.source/A", L"AQID", true);
        routes.Abandon(1, key);
        Require(routes.Peek(2).empty() && !routes.Busy(2), "Canceled queued launch survived");
        id = routes.Start(1, 2, key, L"org.source/A", L"AQID", true);
        routes.Claim(2, routes.Peek(2)[0]);
        routes.Drop(1);
        auto revoke = routes.Peek(2);
        Require(revoke.size() == 3 && revoke[1] == L"revoke", "Dead caller identity not revoked");
        routes.Claim(2, revoke[0]);
        Require(!routes.Busy(2) && !routes.Finish(2, id, -1, L""), "Dead caller retained result");
        routes.Bind(1, L"data/image", L"org.source");
        auto reused = routes.Start(1, 2, key, L"org.source/A", L"AQID", true);
        Require(reused != id && !routes.Finish(2, id, -1, L""), "Reused caller PID inherited an old route");
        routes.Abandon(1, key);
        id = routes.Start(1, 2, key, L"org.source/A", L"AQID", true);
        routes.Drop(2);
        result = routes.Peek(1);
        Require(result.size() == 6 && result[4] == L"0" && result[5].empty(), "Dead target did not cancel caller");
        routes.Bind(2, L"data/image", L"org.target");
        Require(routes.Peek(2).empty(), "Reused process inherited old target work");
        routes.Claim(1, result[0]);
        id = routes.Start(1, 2, other, L"org.source/A", L"AQID", true);
        routes.Bind(2, L"new/image", L"org.target");
        Require(routes.Peek(2).empty() && routes.Peek(1)[4] == L"0", "Changed image retained launch");
        routes.Drop(1);
        Require(!routes.Busy(1) && !routes.Busy(2), "Exit leaked a route");
        ActivityRoutes replacement;
        Require(replacement.Epoch() != routes.Epoch(), "Replacement Service reused its Activity identity");
        replacement.Bind(1, L"data/image", L"org.source");
        replacement.Bind(2, L"data/image", L"org.target");
        Require(replacement.Start(1, 2, key, L"org.source/A", L"AQID", true) != id,
                "Replacement Service reused an Activity route");
        routes.Bind(10, L"data/image", L"org.same");
        routes.Bind(11, L"data/image", L"org.same");
        id = routes.Start(10, 11, key, L"org.same/Private", L"AQID", true);
        launch = routes.Peek(11);
        Require(launch[1] == L"launch-local", "Same-UID route lost its trusted transport type");
        routes.Claim(11, launch[0]);
        routes.Finish(11, id, -1, L"BAUG");
        result = routes.Peek(10);
        Require(result[1] == L"result-local" && result[5] == L"BAUG", "Same-UID result lost its transport type");
        routes.Claim(10, result[0]);
        Require(!routes.Busy(10) && !routes.Busy(11), "Private process result retained a route");
        std::cout
            << "PASS Activity routing: authenticated peers, data/image isolation, caller identity, claims, results, retries, cancellation, process exit and identity replacement\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
