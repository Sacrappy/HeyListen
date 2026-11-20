// Linux memory pressure (gpu/vk/linux_pressure.h): reading /proc/meminfo, the physical and commit reserves, and the texture
// budget's hold and recovery. Ported from Longshot f1b17d59's tests/linux_pressure_test.cpp (dukenheimer), whose module drop
// part is not ported (see gpu/vk/linux_pressure.h and the README's fixes line). The numbers are that PR's Deck and desktop runs.
// python -I -B tests/test_linux_pressure.py [--cxx clang++]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../gpu/vk/linux_pressure.h"

static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

namespace {

constexpr uint64_t kMB = 1ull << 20;

void test_meminfo_and_shortfall(const std::string& dir) {
    using namespace linux_pressure;

    // 1. Default reserves and switch overrides
    CHECK(avail_reserve(nullptr) == 1536 * kMB);
    CHECK(avail_reserve("") == 1536 * kMB);
    CHECK(avail_reserve("0") == 0);
    CHECK(avail_reserve("2048") == 2048 * kMB);
    CHECK(commit_reserve(nullptr) == 2560 * kMB);
    CHECK(commit_reserve("0") == 0);
    CHECK(commit_reserve("3072") == 3072 * kMB);

    // 2. Parsing /proc/meminfo snapshots
    const char* deck_kokiri =
        "MemTotal:       15120384 kB\n"
        "MemFree:          210432 kB\n"
        "MemAvailable:     409600 kB\n"
        "Buffers:           12340 kB\n"
        "Cached:          1820416 kB\n"
        "SwapTotal:       8388604 kB\n"
        "SwapFree:        2048000 kB\n";
    uint64_t avail = 0, swap_free = 0;
    CHECK(parse_meminfo(deck_kokiri, &avail, &swap_free));
    CHECK(avail == 400 * kMB);
    CHECK(swap_free == 2000 * kMB);

    // On the Deck in Kokiri (400 MB MemAvailable, 2000 MB SwapFree):
    // - avail shortfall against 1536 MB is 1136 MB
    // - commit shortfall against 2560 MB is 2560 - 2400 = 160 MB
    // shortfall takes the larger (1136 MB), well before earlyoom's ~5% line
    CHECK(shortfall(avail, swap_free) == 1136 * kMB);

    // Swap nearly full (400 MB MemAvailable, 200 MB SwapFree -> 600 MB total free):
    // - avail shortfall is 1136 MB
    // - commit shortfall is 2560 - 600 = 1960 MB
    CHECK(shortfall(400 * kMB, 200 * kMB) == 1960 * kMB);

    // Healthy 32 GB Linux PC (18 GB MemAvailable, 8 GB SwapFree): 0 shortfall
    CHECK(shortfall(18432 * kMB, 8192 * kMB) == 0);

    // Reserves disabled via 0
    CHECK(shortfall(400 * kMB, 200 * kMB, 0, 2560 * kMB) == 1960 * kMB);
    CHECK(shortfall(400 * kMB, 200 * kMB, 1536 * kMB, 0) == 1136 * kMB);
    CHECK(shortfall(400 * kMB, 200 * kMB, 0, 0) == 0);

    // import_want subtracts dead_import_bytes already queued
    CHECK(import_want(1136 * kMB, 256 * kMB) == 880 * kMB);
    CHECK(import_want(256 * kMB, 512 * kMB) == 0);

    // episode_want: over one pressure episode, no more in all than its deepest shortfall
    CHECK(episode_want(880 * kMB, 1136 * kMB, 0) == 880 * kMB);
    CHECK(episode_want(880 * kMB, 1136 * kMB, 900 * kMB) == 236 * kMB);
    CHECK(episode_want(880 * kMB, 1136 * kMB, 1136 * kMB) == 0);
    CHECK(episode_want(880 * kMB, 1136 * kMB, 2000 * kMB) == 0);

    // Missing MemAvailable is rejected; missing SwapFree defaults to 0
    CHECK(!parse_meminfo("MemTotal: 16000000 kB\nSwapFree: 1024 kB\n", &avail, &swap_free));
    CHECK(parse_meminfo("MemAvailable: 1024000 kB\n", &avail, &swap_free));
    CHECK(avail == 1000 * kMB && swap_free == 0);

    // Swapless system (SwapTotal: 0 kB): has_swap is false, so commit_reserve (2560 MB) does not trigger at 2000 MB MemAvailable
    bool has_swap = true;
    CHECK(parse_meminfo("MemTotal: 16000000 kB\nMemAvailable: 2048000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n",
                        &avail, &swap_free, nullptr, &has_swap));
    CHECK(!has_swap && avail == 2000 * kMB && swap_free == 0);
    CHECK(shortfall(avail, swap_free, kDefaultAvailReserve, kDefaultCommitReserve, has_swap) == 0);
    CHECK(shortfall(1000 * kMB, 0, kDefaultAvailReserve, kDefaultCommitReserve, has_swap) == 536 * kMB);

    // File read round-trip (including MemTotal)
    const std::string path = dir + "/meminfo.txt";
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    std::fwrite(deck_kokiri, 1, std::strlen(deck_kokiri), f);
    std::fclose(f);
    uint64_t total_ram = 0;
    CHECK(read_meminfo(&avail, &swap_free, path.c_str(), &total_ram, &has_swap));
    CHECK(has_swap && avail == 400 * kMB && swap_free == 2000 * kMB && total_ram == (15120384ull << 10));

    // A file that is not there: false, and nothing read
    CHECK(!read_meminfo(&avail, &swap_free, (dir + "/missing").c_str()));
}

void test_pressure_hold() {
    using namespace linux_pressure;
    // PressureHold: hysteresis (256 MB clear margin), baseline hold (no per-frame ratchet), 512 MB floor, and stepped recovery
    PressureHold hold;
    // Healthy (1600 MB avail >= 1536 MB reserve, 4000 MB swap): inactive
    CHECK(hold.step(2600 * kMB, 1600 * kMB, 4000 * kMB) == UINT64_MAX && !hold.active);
    // Dip to 850 MB avail (686 MB short of 1536 MB): enters pressure, cuts 2600 - 686 = 1914 MB
    CHECK(hold.step(2600 * kMB, 850 * kMB, 6000 * kMB) == 1914 * kMB && hold.active);
    // Next frame: textures shrank to 2200 MB, MemAvailable is still 850 MB while VMA compaction works:
    // held budget stays at 1914 MB (does NOT ratchet down to 2200 - 686 = 1514 MB!)
    CHECK(hold.step(2200 * kMB, 850 * kMB, 6000 * kMB) == 1914 * kMB && hold.active);
    // Deeper dip to 600 MB avail (936 MB short): steps down from the entry baseline (2600 - 936 = 1664 MB)
    CHECK(hold.step(2000 * kMB, 600 * kMB, 6000 * kMB) == 1664 * kMB && hold.active);
    // Extreme shortfall (1500 MB avail short + swap empty -> 2400 MB short): clamped at the 512 MB floor
    CHECK(hold.step(2000 * kMB, 100 * kMB, 60 * kMB) == 512 * kMB && hold.active);
    // Recovery to 1600 MB avail (past 1536 MB reserve, but below 1536 + 256 = 1792 MB clear line): stays at 512 MB
    CHECK(hold.step(1800 * kMB, 1600 * kMB, 6000 * kMB) == 512 * kMB && hold.active);
    // Recovery to 1800 MB avail and 3000 MB swap (>= 1792 MB avail and >= 2816 MB commit): steps up by 256 MB per sample
    // until reaching baseline_total (2600 MB), then leaves pressure
    for (uint64_t expected = 768 * kMB; expected < 2600 * kMB; expected += 256 * kMB)
        CHECK(hold.step(1800 * kMB, 1800 * kMB, 3000 * kMB) == expected && hold.active);
    CHECK(hold.step(1800 * kMB, 1800 * kMB, 3000 * kMB) == UINT64_MAX && !hold.active);

    // Entering pressure when total_bytes < 512 MB still honors the 512 MB floor
    PressureHold low_entry;
    CHECK(low_entry.step(300 * kMB, 850 * kMB, 6000 * kMB) == 512 * kMB && low_entry.active);

    // The reserves are the switch's: with both off the hold never enters pressure
    PressureHold off;
    CHECK(off.step(2600 * kMB, 100 * kMB, 0, 0, 0) == UINT64_MAX && !off.active);
}

} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2);
    test_meminfo_and_shortfall(argv[1]);
    test_pressure_hold();
    std::printf("linux_pressure: %d failures\n", bad);
    return bad ? 1 : 0;
}
