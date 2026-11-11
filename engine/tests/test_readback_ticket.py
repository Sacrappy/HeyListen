"""(TICK-059) RX_VK_READBACK_TICKET: a fast fence poll notes the sync_order_mu section in progress (a ticket), and a CPU
readback waits until that section has left: the order the poll's lock gave it, without the poll waiting.

The counted mutex and the ticket's two functions are cut out of gpu/vk/vk_replay.cpp and compiled with stubs:
- a readback after a ticket sees everything the ticket's section (and every section before it) wrote, over many random
  sections and polls on several threads (no lost wake-up: each wait ends);
- with no section in progress the ticket costs no wait;
- a ticket's wait does not wait for a later section (one taken after the poll and held).
And, by the text: the switch is read from RX_VK_READBACK_TICKET; it turns the fast poll on; the poll takes its ticket on the
fast path before answering; invalidate waits for it first thing (=1), before memory_cpu_reads and the write-back check; TICK-058's
whole-lock wait stays only for RX_VK_FENCE_POLL_FAST without the ticket; sync_order_mu is the counted mutex.

python -I -B tests/test_readback_ticket.py --cxx <compiler>
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    rp = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")

    sw = function(rp, "int readback_ticket()")
    need('getenv("RX_VK_READBACK_TICKET")' in sw, "readback_ticket does not read RX_VK_READBACK_TICKET")
    need("|| readback_ticket()" in function(rp, "bool fence_poll_fast_on()"), "the ticket does not turn the fast poll on")

    need("OrderMutex sync_order_mu;" in rp and "std::mutex sync_order_mu" not in rp, "sync_order_mu is not the counted mutex")
    om = function(rp, "struct OrderMutex {")
    lock = function(om, "void lock()")
    need(lock.index("mu.lock();") < lock.index("entered.fetch_add(1"), "a section is counted before it has the lock")
    tl = function(om, "bool try_lock()")
    need(tl.index("if (!mu.try_lock()) return false;") < tl.index("entered.fetch_add(1"), "try_lock counts a failed try")
    un = function(rp, "void OrderMutex::unlock()")
    need(un.index("left.fetch_add(1, std::memory_order_seq_cst);") < un.index("mu.unlock();"),
         "a section is counted as left after it let the lock go")

    wi = function(rp, "int wait_inner(uint64_t f, uint64_t timeout_ns) {")
    fast = wi.index("if (!timeout_ns && fence_poll_fast_on()")
    take = wi.index("if (readback_ticket()) order_ticket_take();")
    ans = wi.index("if (wait_recorded(f, 0, &ok)) return ok;")
    locked = wi.index("std::lock_guard lk(sync_order_mu);")
    need(fast < take < ans < locked, "the fast poll does not take its ticket before it answers")

    inv = function(rp, "void invalidate(uint64_t gpu, uint64_t size) {")
    w = inv.index("if (size && ticket == 1) order_ticket_wait();")
    need(w < inv.index("memory_cpu_reads(") and w < inv.index("memory_writeback_value("),
         "the readback's ticket wait is not before memory_cpu_reads and the write-back check")
    need("else if (!ticket && fence_poll_fast_on() && !async_on() && sync_waits_on() && !per_queue_fences_off()) {" in inv,
         "TICK-058's whole-lock wait is not kept for the fast poll without the ticket only")

    # (the mutex, its globals and the two functions: one stretch of the source)
    first = rp.index("struct OrderMutex {")
    last = rp.index("void order_ticket_wait()")
    parts = rp[first:last] + function(rp, "void order_ticket_wait()")
    fixture = r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
static void jt_mark(int, uint64_t) {}
''' + parts + r'''
std::atomic<uint64_t> registered{0};   // (the newest section that made its "registrations")
int main() {
    using namespace std::chrono_literals;
    // no section in progress: the ticket is what has left, no wait
    { std::lock_guard lk(sync_order_mu); registered.store(sync_order_mu.entered.load(), std::memory_order_relaxed); }
    order_ticket_take();
    CHECK(order_ticket.load() == sync_order_mu.left.load());
    order_ticket_wait();
    // a ticket's wait ends when its section leaves, not after a later one
    {
        sync_order_mu.lock();
        const uint64_t mine = sync_order_mu.entered.load();
        std::atomic<bool> done{false};
        std::thread poller([&] { order_ticket_take(); order_ticket_wait(); done = true; });
        while (order_ticket.load() < mine) std::this_thread::yield();
        std::this_thread::sleep_for(20ms);
        CHECK(!done.load());   // (its section still holds the lock)
        sync_order_mu.unlock();
        sync_order_mu.lock();   // (a later section, held)
        for (int i = 0; i < 2000 && !done.load(); i++) std::this_thread::sleep_for(1ms);
        CHECK(done.load());
        sync_order_mu.unlock();
        poller.join();
    }
    // many sections and polls: a readback after its ticket sees its section's registrations
    std::atomic<bool> stop{false};
    std::vector<std::thread> replays;
    for (int k = 0; k < 2; k++)
        replays.emplace_back([&, k] {
            std::mt19937 rng(7 + k);
            while (!stop.load()) {
                std::lock_guard lk(sync_order_mu);
                const uint64_t me = sync_order_mu.entered.load();
                for (int s = (int)(rng() % 2000); s > 0; s--) std::atomic_signal_fence(std::memory_order_seq_cst);
                registered.store(me, std::memory_order_relaxed);   // (late in the section, as a write-back is)
            }
        });
    std::vector<std::thread> polls;
    std::atomic<int64_t> waits{0};
    for (int k = 0; k < 3; k++)
        polls.emplace_back([&, k] {
            std::mt19937 rng(100 + k);
            for (int i = 0; i < 20000; i++) {
                order_ticket_take();
                const uint64_t t = order_ticket.load();
                for (int s = (int)(rng() % 500); s > 0; s--) std::atomic_signal_fence(std::memory_order_seq_cst);
                if (sync_order_mu.left.load() < t) waits++;
                order_ticket_wait();
                CHECK(sync_order_mu.left.load() >= t);
                CHECK(registered.load(std::memory_order_relaxed) >= t);
            }
        });
    for (auto& t : polls) t.join();
    stop = true;
    for (auto& t : replays) t.join();
    CHECK(sync_order_mu.entered.load() == sync_order_mu.left.load());
    std::printf("PASS the readbacks after a ticket see its section's writes (%lld of 60000 waited), no wait without one, "
                "none for a later section\n", (long long)waits.load());
    return 0;
}
'''
    work = ROOT / "work"
    work.mkdir(exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="readback-ticket-", dir=work))
    cpp, exe = out / "fixture.cpp", out / "fixture.exe"
    cpp.write_text(fixture, encoding="utf-8")
    subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Wno-unused-function", "-Werror", "-pthread", str(cpp),
                    "-o", str(exe)], check=True)
    env = dict(os.environ)
    compiler = Path(args.cxx)
    if compiler.is_file():
        env["PATH"] = str(compiler.resolve().parent) + os.pathsep + env.get("PATH", "")
    subprocess.run([str(exe)], check=True, env=env, timeout=300)
    print("Retained:", out)


if __name__ == "__main__":
    main()
