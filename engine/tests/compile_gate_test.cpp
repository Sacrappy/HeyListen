// compile_gate (gpu/vk/compile_gate.h): how many of the driver's compiles run at once. The budget the memory gives, the rule
// for one more by the compiles' weights (a compute compile's and a start-up graphics pipeline's), and the gate itself under
// threads: one always let through, one more only while its weight fits and the memory is there, never more than an asked
// limit, a stop that ends a wait without a turn, a compile too heavy for the budget refused rather than run alone.
// Ported from Longshot c392df3a's tests/compute_gate_test.cpp (Tomiss), with weight_words and fits_alone added here for the
// start-up pipeline build (issue 88). No game, no GPU.
// python -I -B tests/test_compile_gate.py [--cxx clang++]
#include "../gpu/vk/compile_gate.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using compile_gate::budget_for;
using compile_gate::fits_alone;
using compile_gate::kFreeMB;
using compile_gate::kMost;
using compile_gate::kPinnedMB;
using compile_gate::kReserveMB;
using compile_gate::kUnknownMB;
using compile_gate::may_start;

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

void rules() {
    expect(compile_gate::weight(true, 0) == kPinnedMB && compile_gate::weight(false, 0) == kFreeMB && kPinnedMB > kFreeMB,
           "a compute compile held to 32 lanes weighs more");
    expect(compile_gate::weight(true, 2190) == compile_gate::kHeavyMB && compile_gate::weight(true, 1892) == kPinnedMB &&
               compile_gate::weight(false, 12329) == kFreeMB && compile_gate::kHeavyMB > kPinnedMB,
           "a pinned compute compile of 2,000 phis or more weighs the most; at the driver's lanes the phis do not count");
    expect(compile_gate::weight(true, 58, true) == compile_gate::kMeshMB && compile_gate::kMeshMB > kPinnedMB &&
               compile_gate::kHeavyMB > compile_gate::kMeshMB && compile_gate::weight(true, 80000, true) == compile_gate::kHeavyMB &&
               compile_gate::weight(false, 58, true) == kFreeMB && compile_gate::weight(true, 58) == kPinnedMB,
           "a pinned mesh fallback stage weighs more than an ordinary pinned compile, whatever its phis");
    // (a start-up graphics pipeline by its SPIR-V: the PC 3050's measured sizes, predict_limits.h. Most of the game's programs
    // are small; a few of 66-151k words took 25 s and a few hundred MB; 502k and 1014k took 11 and 29 GB)
    expect(compile_gate::weight_words(0) == compile_gate::kWordsSmallMB &&
               compile_gate::weight_words(32000) == compile_gate::kWordsSmallMB,
           "a graphics pipeline of 32k words or fewer weighs the least");
    expect(compile_gate::weight_words(66000) == compile_gate::kWordsMediumMB &&
               compile_gate::weight_words(33000) == compile_gate::kWordsMediumMB,
           "66k words, the largest the prediction spent its time on: the medium weight");
    expect(compile_gate::weight_words(151000) == compile_gate::kWordsLargeMB,
           "151k words, the other size the reporter's prediction met: the large weight");
    expect(compile_gate::weight_words(300000) == compile_gate::kWordsHugeMB &&
               compile_gate::weight_words(321000) == 321 * compile_gate::kWordsMaxMBPerKWord,
           "past RX_VK_PREDICT_MAX_KWORDS' 300k: the huge weight, then 28 MB for each thousand words");
    expect(compile_gate::weight_words(502000) == 502 * compile_gate::kWordsMaxMBPerKWord &&
               compile_gate::weight_words(1014000) == 1014 * compile_gate::kWordsMaxMBPerKWord &&
               compile_gate::weight_words(5000000) == compile_gate::kWordsMaxMB,
           "the two programs the PC 3050 measured at 11 and 29 GB: 14 and 28 GB, and never more than kWordsMaxMB");
    expect(compile_gate::kWordsSmallMB < compile_gate::kWordsMediumMB &&
               compile_gate::kWordsMediumMB < compile_gate::kWordsLargeMB &&
               compile_gate::kWordsLargeMB < compile_gate::kWordsHugeMB &&
               compile_gate::kWordsHugeMB < 502 * compile_gate::kWordsMaxMBPerKWord,
           "the graphics weights grow with the words");

    expect(budget_for(0) == 0 && budget_for(kReserveMB) == 0 && budget_for(kUnknownMB) == 0, "no budget: one at a time");
    expect(budget_for(kReserveMB + 100) == 100, "the budget: what is past the reserve");
    // (a compile heavier than the whole budget: refused rather than run alone, which would be the process's death)
    expect(!fits_alone(compile_gate::kWordsMaxMB, true, 1024), "30 GB of compile with 1 GB past the reserve: not run");
    expect(fits_alone(compile_gate::kWordsMaxMB, true, 32768), "the same with 32 GB past the reserve: run");
    expect(fits_alone(compile_gate::kWordsMaxMB, false, 0) && fits_alone(compile_gate::kWordsMaxMB, true, 0),
           "no memory rule, or no budget: nothing is refused");

    const uint64_t plenty = uint64_t(1) << 40, budget = 16 * 1024;
    // (no gate)
    expect(may_start(99, 0, kPinnedMB, 0, false, 0, 0), "no limit and no memory rule: no gate");
    // (none running: always, whatever the memory)
    expect(may_start(0, 0, kPinnedMB, 0, true, 0, 0) && may_start(0, 0, kPinnedMB, 1, false, 0, 0), "the first one always starts");
    // (an asked limit: the limit alone)
    expect(!may_start(4, 0, kFreeMB, 4, false, 0, plenty) && may_start(3, 0, kFreeMB, 4, false, 0, 0), "an asked limit alone");
    // (by memory: the weights in the budget, and the weight available now)
    expect(may_start(1, kPinnedMB, kFreeMB, 0, true, budget, plenty), "one more that fits");
    expect(!may_start(1, budget - kFreeMB + 1, kFreeMB, 0, true, budget, plenty), "not one more past the budget");
    expect(!may_start(1, kFreeMB, kFreeMB, 0, true, budget, kReserveMB + kFreeMB - 1), "not one more without the memory now");
    expect(may_start(1, kFreeMB, kFreeMB, 0, true, budget, kReserveMB + kFreeMB), "one more with exactly its memory");
    expect(!may_start(1, kFreeMB, kFreeMB, 0, true, budget, kUnknownMB), "memory unknown now: no more than one");
    expect(!may_start(kMost, 0, 0, 0, true, plenty, plenty), "never more than kMost");
    expect(!may_start(1, kFreeMB, kFreeMB, 0, true, 0, plenty), "no budget: one at a time");
    // (the PC this was measured on: about 16 GB past the reserve: 8 held to 32 lanes, or kMost others)
    expect(may_start(7, 7 * kPinnedMB, kPinnedMB, 0, true, budget, plenty) &&
               !may_start(8, 8 * kPinnedMB, kPinnedMB, 0, true, budget, plenty) &&
               may_start(kMost - 1, (kMost - 1) * kFreeMB, kFreeMB, 0, true, budget, plenty),
           "16 GB past the reserve: 8 held to 32 lanes, or 16 others");
    // (issue 88's PC: 31 GB, 27 GB past the reserve. Twelve of the small compiles fit at once; a 300k-word one (4 GB) does not
    // start beside eleven of its own; and the 1014k-word program, 28 GB by the PC 3050's measurement, does not fit the 27 GB at
    // all: it is left to the draw that needs it rather than run alone into the kernel's kill)
    {
        const uint64_t avail = 31 * 1024, b = budget_for(avail);
        expect(may_start(11, 11 * compile_gate::kWordsSmallMB, compile_gate::kWordsSmallMB, 0, true, b, avail),
               "the Arc's 31 GB: the twelfth small compile starts");
        expect(!may_start(11, 11 * compile_gate::kWordsHugeMB, compile_gate::kWordsHugeMB, 0, true, b, avail),
               "the Arc's 31 GB: not one 300k-word compile beside eleven of its own");
        expect(may_start(11, 11 * compile_gate::kWordsSmallMB, compile_gate::kWordsLargeMB, 0, true, b, avail),
               "the Arc's 31 GB: the 151k-word one beside eleven small ones");
        expect(!fits_alone(compile_gate::weight_words(1014000), true, b),
               "the Arc's 31 GB: the 1014k-word compile (28 GB) does not fit the budget");
    }
    // (the freed memory handed back: only where there is a gate, and only after a long compile)
    expect(!compile_gate::trims(false, 60000), "no gate: nothing is handed back, as before");
    expect(!compile_gate::trims(true, compile_gate::kTrimAfterMs - 1), "a short compile hands nothing back");
    expect(compile_gate::trims(true, compile_gate::kTrimAfterMs) && compile_gate::trims(true, 600000), "a long one does");

    // (RADV, an integrated GPU: #1398, a Radeon 760M with 12 GB killed for memory while its first start compiled the mesh
    // fallback's stages on 12 threads. By the phis alone, at twice what was measured on discrete cards, as the Intel weights
    // are: the 80,000-phi stages took 2.9 GB, those up to 29,400 under 700 MB)
    using compile_gate::weight_radv;
    expect(weight_radv(79947) == compile_gate::kRadvBigMB && weight_radv(compile_gate::kRadvBigPhis) == compile_gate::kRadvBigMB &&
               compile_gate::kRadvBigMB >= 2 * 2900,
           "RADV: a stage of 80,000 phis weighs less than twice its measured 2.9 GB");
    expect(weight_radv(29372) == compile_gate::kRadvMidMB && weight_radv(compile_gate::kRadvBigPhis - 1) == compile_gate::kRadvMidMB &&
               compile_gate::kRadvMidMB >= 2 * 700,
           "RADV: a stage of up to 29,400 phis weighs less than twice its measured 700 MB");
    expect(weight_radv(0) == compile_gate::kRadvMB && weight_radv(compile_gate::kRadvMidPhis - 1) == compile_gate::kRadvMB &&
               compile_gate::kRadvMB >= 2 * 200 && compile_gate::kRadvMB < compile_gate::kRadvMidMB &&
               compile_gate::kRadvMidMB < compile_gate::kRadvBigMB,
           "RADV: a small stage weighs the least, and not under twice 200 MB");
    // (the 760M's PC at its first start, say 7 GB available: 3 GB past the reserve. The large stages run alone, the small ones
    // side by side, up to what fits; with plenty of memory, as many as before)
    {
        const uint64_t avail = 7 * 1024, b = budget_for(avail);
        expect(!may_start(1, compile_gate::kRadvMB, compile_gate::kRadvBigMB, 0, true, b, avail) &&
                   may_start(0, 0, compile_gate::kRadvBigMB, 0, true, b, avail),
               "RADV, 7 GB available: a stage of 80,000 phis starts alone");
        expect(may_start(1, compile_gate::kRadvMidMB, compile_gate::kRadvMidMB, 0, true, b, avail) &&
                   !may_start(2, 2 * compile_gate::kRadvMidMB, compile_gate::kRadvMidMB, 0, true, b, avail),
               "RADV, 7 GB available: two stages of 10,000 phis or more at once");
        expect(may_start(5, 5 * compile_gate::kRadvMB, compile_gate::kRadvMB, 0, true, b, avail) &&
                   !may_start(6, 6 * compile_gate::kRadvMB, compile_gate::kRadvMB, 0, true, b, avail),
               "RADV, 7 GB available: not 6 small stages at once, or more");
        expect(!may_start(3, 3 * compile_gate::kRadvMB, compile_gate::kRadvMB, 0, true, b, kReserveMB + compile_gate::kRadvMB - 1),
               "RADV: one more starts with the memory gone meanwhile");
        const uint64_t lots = 32 * 1024;
        expect(may_start(kMost - 1, (kMost - 1) * compile_gate::kRadvMB, compile_gate::kRadvMB, 0, true, budget_for(lots), lots),
               "RADV, 32 GB available: fewer small stages at once than kMost");
    }
    // (where RADV's gate is on by default: an integrated GPU on a PC with under 13 GB of RAM, as #1398's 12 GB. The Steam Deck
    // (16 GB, about 14.5 GB the system's) and 16 GB APUs keep main's rule: nobody has run the gate there)
    {
        using compile_gate::radv_by_memory;
        expect(radv_by_memory(true, 11 * 1024) && radv_by_memory(true, 7 * 1024), "RADV, integrated, 12 or 8 GB: gated");
        expect(!radv_by_memory(true, 14848), "RADV, the Steam Deck (about 14.5 GB of RAM to the system): not gated, as on main");
        expect(!radv_by_memory(true, 15 * 1024) && !radv_by_memory(true, 32 * 1024), "RADV, integrated, 16 GB or more: not gated");
        expect(!radv_by_memory(false, 8 * 1024), "RADV, a discrete card: not gated, whatever the RAM");
        expect(!radv_by_memory(true, 0), "RADV, integrated, the RAM not known: not gated, as on main");
    }
    // (a cgroup's limit: what memory.max leaves past memory.current. None set: no bound from it)
    {
        using compile_gate::cgroup_room_mb;
        expect(cgroup_room_mb("max\n", 1ull << 30) == compile_gate::kUnknownMB, "cgroup: no limit (max): no bound");
        expect(cgroup_room_mb("", 0) == compile_gate::kUnknownMB && cgroup_room_mb(nullptr, 0) == compile_gate::kUnknownMB &&
                   cgroup_room_mb("junk", 0) == compile_gate::kUnknownMB,
               "cgroup: nothing read, or not a number: no bound");
        expect(cgroup_room_mb("17179869184\n", 12ull << 30) == 4096, "cgroup: a 16 GB limit with 12 GB in use leaves 4 GB");
        expect(cgroup_room_mb("8589934592\n", 9ull << 30) == 0, "cgroup: past its limit leaves nothing");
        expect(budget_for(std::min<uint64_t>(30 * 1024, cgroup_room_mb("17179869184", 12ull << 30))) == 0,
               "cgroup: a host with 30 GB free in a slice with 4 GB left compiles one at a time");
    }
}

/// `threads` threads each take `each` turns of weight_mb at a gate; avail_mb is what the memory reads. The most that ran at once
int most_running(unsigned limit, bool by_memory, uint64_t budget_mb, uint64_t weight_mb, uint64_t avail_mb, unsigned threads,
                 int each, uint64_t* waits = nullptr) {
    compile_gate::Gate gate;
    gate.set(limit, by_memory, budget_mb);
    std::atomic<int> running{0}, most{0}, turns{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++)
        pool.emplace_back([&] {
            for (int i = 0; i < each; i++) {
                if (!gate.enter(weight_mb, [&] { return avail_mb; }, [] { return false; })) continue;
                const int now = ++running;
                int seen = most.load();
                while (now > seen && !most.compare_exchange_weak(seen, now)) {}
                // (a short spin, not a sleep: Windows' timers round a sleep up to 15 ms, which made this test 80 s long. 3 ms
                // so that two threads overlap even when the whole suite runs at once)
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(3000);
                while (std::chrono::steady_clock::now() < until) {}
                --running;
                turns++;
                gate.leave(weight_mb);
            }
        });
    for (std::thread& t : pool) t.join();
    expect(turns.load() == (int)threads * each, "every compile got its turn: none is left out");
    expect(running.load() == 0, "nothing is left running");
    unsigned gate_most = 0;
    uint64_t gate_waits = 0;
    gate.seen(&gate_most, &gate_waits);
    expect((int)gate_most == most.load(), "the gate's own count of the most at once is what ran");
    if (waits) *waits = gate_waits;
    return most.load();
}

void gate() {
    const uint64_t plenty = uint64_t(1) << 30, none = 0;
    uint64_t waits = 0;
    expect(most_running(0, false, 0, kPinnedMB, none, 8, 20) > 1, "no gate: they run together, as before");
    expect(most_running(1, false, 0, kFreeMB, plenty, 8, 20, &waits) == 1 && waits > 0, "an asked 1: one at a time");
    {
        const int most = most_running(3, false, 0, kFreeMB, none, 8, 20);
        expect(most <= 3 && most >= 2, "an asked 3: up to three, whatever the memory");
    }
    {
        const int most = most_running(0, true, 3 * kPinnedMB, kPinnedMB, plenty, 8, 20);
        expect(most <= 3 && most >= 2, "a budget of three pinned compiles: up to three of them");
    }
    {
        const int most = most_running(0, true, 3 * kPinnedMB, kFreeMB, plenty, 20, 10);
        expect(most > 3 && most <= (int)kMost, "the same budget: more of the light ones, never past kMost");
    }
    // (the budget allows several, but there is not a compile's worth of memory available now: one at a time, and still every one)
    expect(most_running(0, true, 16 * 1024, kPinnedMB, none, 8, 20) == 1, "within the budget without the memory: one at a time");
    expect(most_running(0, true, 16 * 1024, kPinnedMB, kReserveMB + kPinnedMB - 1, 8, 20) == 1,
           "one MB short: one at a time");
    // (the start-up build's own case: twelve workers, the small weight, 27 GB past the reserve: all twelve at once, as before;
    // with the large weight and the memory short, only what fits)
    expect(most_running(0, true, 27 * 1024, compile_gate::kWordsSmallMB, 31 * 1024, 12, 100) == 12,
           "twelve start-up workers with small programs: twelve at once, as before");
    expect(most_running(0, true, 3 * 1024, compile_gate::kWordsLargeMB, 7 * 1024, 8, 20) == 2,
           "three GB past the reserve and 1.5 GB compiles: two at once");
}

void stopped() {
    // (one holds the only turn; the one waiting behind it is told to stop: it gets no turn, and does not hang)
    compile_gate::Gate gate;
    gate.set(1, false);
    expect(gate.enter(kFreeMB, [] { return uint64_t(0); }, [] { return false; }), "the first turn");
    std::atomic<bool> stop{false}, returned{false}, got{true};
    std::thread waiter([&] {
        got = gate.enter(kFreeMB, [] { return uint64_t(0); }, [&] { return stop.load(); });
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    expect(!returned.load(), "the second waits while the first runs");
    stop = true;
    waiter.join();
    expect(returned.load() && !got.load(), "a stop ends the wait with no turn taken");
    gate.leave(kFreeMB);
    stop = false;
    expect(gate.enter(kFreeMB, [] { return uint64_t(0); }, [] { return true; }), "a free gate gives a turn even when stopped");
    gate.leave(kFreeMB);
}

void memory_returns() {
    // (the second waits for memory, not for a turn: it starts when the memory is back, without any leave())
    compile_gate::Gate gate;
    gate.set(0, true, 16 * 1024);
    std::atomic<uint64_t> avail{0};
    expect(gate.enter(kPinnedMB, [&] { return avail.load(); }, [] { return false; }), "the first turn");
    std::atomic<bool> entered{false};
    std::thread waiter([&] { entered = gate.enter(kPinnedMB, [&] { return avail.load(); }, [] { return false; }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    expect(!entered.load(), "no second compile while the memory is short");
    avail = kReserveMB + kPinnedMB;
    waiter.join();
    expect(entered.load(), "the second starts once the memory is available again");
    gate.leave(kPinnedMB);
    gate.leave(kPinnedMB);
}
}   // namespace

int main() {
    rules();
    gate();
    stopped();
    memory_returns();
    if (failures) return std::printf("%d checks failed\n", failures), 1;
    std::printf("compile_gate: ok\n");
    return 0;
}
