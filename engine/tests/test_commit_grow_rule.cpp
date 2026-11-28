// Copyright (C) 2026 Sacrappy
// The commit grow rule (gpu/vk/commit_grow_rule.h), alone: tests/test_commit_grow_rule.py compiles and runs this
#include "../gpu/vk/commit_grow_rule.h"
#include <cstdio>

using namespace commit_grow_rule;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    const uint64_t MB = 1ull << 20, R = 2560 * MB;
    const int64_t S = 1'000'000'000;
    {   // never at or above the reserve, nor with the reserve off
        State s;
        CHECK(!should_try(s, R, R, 100 * S));
        CHECK(!should_try(s, R + 1, R, 100 * S));
        CHECK(!should_try(s, 40000 * MB, R, 100 * S));
        CHECK(!should_try(s, 0, 0, 100 * S));
        CHECK(should_try(s, R - 1, R, 100 * S));
        CHECK(ask_bytes(R, R) == 0 && ask_bytes(R + 5, R) == 0 && ask_bytes(0, 0) == 0);
    }
    {   // the size: (reserve - left) + reserve, rounded up to 64 KB
        CHECK(ask_bytes(0, R) == 2 * R);
        CHECK(ask_bytes(1000 * MB, R) == (R - 1000 * MB) + R);
        CHECK(ask_bytes(R - 1, R) == R + kRound);   // R + 1 rounds up
        CHECK(ask_bytes(R - kRound, R) == R + kRound);
        CHECK(ask_bytes(R - 1000, R) % kRound == 0 && ask_bytes(R - 1000, R) >= R + 1000);
    }
    {   // one try every 10 s
        State s;
        CHECK(should_try(s, 0, R, 5 * S));
        started(s, 5 * S);
        CHECK(!should_try(s, 0, R, 5 * S));
        CHECK(!should_try(s, 0, R, 14 * S));
        CHECK(!should_try(s, 0, R, 15 * S - 1));
        CHECK(should_try(s, 0, R, 15 * S));
        CHECK(!should_try(s, R, R, 60 * S));   // above the reserve again: none
    }
    {   // stop after 6 tries in a row that did not raise the limit
        State s;
        int64_t t = 10 * S;
        for (int k = 1; k <= 6; k++) {
            CHECK(should_try(s, 0, R, t));
            started(s, t);
            CHECK(finished(s, 40000 * MB, 40000 * MB) == k);
            t += 10 * S;
        }
        CHECK(!should_try(s, 0, R, t));
        CHECK(!should_try(s, 0, R, t + 3600 * S));
    }
    {   // a rise starts the count again; a fall is not a rise
        State s;
        int64_t t = 10 * S;
        for (int k = 1; k <= 5; k++) {
            started(s, t);
            CHECK(finished(s, 40000 * MB, 40000 * MB) == k);
            t += 10 * S;
        }
        CHECK(should_try(s, 0, R, t));
        started(s, t);
        CHECK(finished(s, 40000 * MB, 43000 * MB) == 0);
        CHECK(s.failed == 0);
        t += 10 * S;
        for (int k = 1; k <= 6; k++) {
            CHECK(should_try(s, 0, R, t));
            started(s, t);
            CHECK(finished(s, 43000 * MB, k == 3 ? 42000 * MB : 43000 * MB) == k);
            t += 10 * S;
        }
        CHECK(!should_try(s, 0, R, t));
    }
    if (fails) return 1;
    std::printf("commit grow rule: ok\n");
    return 0;
}
