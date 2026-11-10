// capture_merge::merge (gpu/vk/capture_merge.h, TICK-043): a capture made of pages copied ahead of its submit and the pages
// written since holds every page written since the last submit once, each as it is at the submit. A simulation of guest
// pages, their write marks, GPU write-backs and the pre-copy thread's rounds against a plain copy at the submit. No game, no GPU.
#include "../gpu/vk/capture_merge.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

constexpr uint32_t kPages = 64;

struct World {
    std::vector<uint32_t> mem = std::vector<uint32_t>(kPages, 0);   // (a page's contents: a version)
    std::vector<uint8_t> mark = std::vector<uint8_t>(kPages, 0);
    std::vector<uint64_t> wb = std::vector<uint64_t>(kPages, 0);   // (the value of the page's last write-back, 0: none)
    std::map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> pending;   // value -> (page, version it writes)
    uint64_t done = 0, recorded = 0;
    uint32_t next_version = 1;
    void cpu_write(uint32_t p) { mem[p] = next_version++, mark[p] = 1; }   // (the store, then its mark)
    void record_writeback(uint32_t p) {
        const uint64_t v = ++recorded;
        wb[p] = v;
        pending[v].push_back({p, next_version++});
    }
    void gpu_progress() {   // (a write-back lands in guest memory, unmarked, then its value counts as done)
        if (done >= recorded) return;
        const uint64_t v = done + 1;
        for (auto [p, ver] : pending[v]) mem[p] = ver;
        pending.erase(v);
        done = v;
    }
    std::vector<uint32_t> collect() {   // (collect_in: the marked pages, their marks cleared, in page order)
        std::vector<uint32_t> out;
        for (uint32_t p = 0; p < kPages; p++)
            if (mark[p]) mark[p] = 0, out.push_back(p);
        return out;
    }
};

struct Pre {   // (the pre-copy thread's state: vk_memory.cpp PreCopy)
    std::vector<uint32_t> pages;
    std::vector<uint64_t> done;
    std::vector<uint32_t> slots;   // (the buffer: a slot's copy of its page)
    std::map<uint32_t, uint32_t> slot_of;
    void round(World& w) {
        const uint64_t d = w.done;   // (read before the marks are cleared)
        for (uint32_t p : w.collect()) {
            uint32_t s;
            if (auto it = slot_of.find(p); it != slot_of.end()) s = it->second;
            else s = (uint32_t)pages.size(), pages.push_back(p), done.push_back(0), slots.push_back(0), slot_of[p] = s;
            slots[s] = w.mem[p], done[s] = d;
        }
    }
};

void simulate(unsigned seed, bool pre_on) {
    std::mt19937 rng(seed);
    World w;
    Pre pre;
    for (int submit = 0; submit < 200; submit++) {
        std::set<uint32_t> written;   // (since the last submit: what the capture must hold)
        const int steps = 5 + (int)(rng() % 40);
        for (int i = 0; i < steps; i++) {
            const unsigned r = rng() % 100;
            if (r < 55) {
                const uint32_t p = rng() % kPages;
                w.cpu_write(p), written.insert(p);
            } else if (r < 65) {
                w.record_writeback(rng() % kPages);
            } else if (r < 80) {
                w.gpu_progress();
            } else if (pre_on) {
                pre.round(w);
            }
        }
        // the submit: what is marked now, merged with the copies made ahead
        const std::vector<uint32_t> fresh = w.collect();
        capture_merge::Merged m;
        capture_merge::merge(pre.pages.data(), pre.done.data(), pre.pages.size(), fresh.data(), fresh.size(),
                             [&](uint32_t p) { return w.wb[p]; }, m);
        std::vector<uint32_t> buf(m.pages.size());
        for (size_t s = 0; s < m.first_new; s++) buf[s] = pre.slots[s];
        for (size_t s = m.first_new; s < m.pages.size(); s++) buf[s] = w.mem[m.pages[s]];
        for (uint32_t s : m.recopy) buf[s] = w.mem[m.pages[s]];
        // every page once; exactly the written ones (and the ones taken ahead, which are among them)
        std::set<uint32_t> got(m.pages.begin(), m.pages.end());
        expect(got.size() == m.pages.size(), "a page twice in the capture");
        expect(got == written, "the capture's pages are not the pages written since the last submit");
        expect(m.first_new == pre.pages.size(), "the copied-ahead pages are not first");
        // each as it is at the submit (a page whose write-back is still on the GPU is copied again at the take: not checked)
        for (size_t s = 0; s < m.pages.size(); s++)
            if (w.wb[m.pages[s]] <= w.done) expect(buf[s] == w.mem[m.pages[s]], "a page's copy is not the page at the submit");
        // recopies only of copied-ahead slots, each once
        std::set<uint32_t> rs(m.recopy.begin(), m.recopy.end());
        expect(rs.size() == m.recopy.size(), "a slot copied again twice");
        for (uint32_t s : m.recopy) expect(s < m.first_new, "a new page in the recopies");
        pre = Pre{};
    }
}

void fixed_cases() {
    // a page copied ahead, then written again: copied again into its slot, not added
    {
        const uint32_t pre[] = {7, 3, 9};
        const uint64_t done[] = {5, 5, 5};
        const uint32_t fresh[] = {1, 3, 12};
        capture_merge::Merged m;
        capture_merge::merge(pre, done, 3, fresh, 3, [](uint32_t) { return uint64_t{0}; }, m);
        expect((m.pages == std::vector<uint32_t>{7, 3, 9, 1, 12}), "merge order");
        expect((m.recopy == std::vector<uint32_t>{1}), "the page written again is copied again");
        expect(m.first_new == 3, "first_new");
    }
    // a write-back past what the GPU had done at the copy: copied again; one it had done: not
    {
        const uint32_t pre[] = {4, 5};
        const uint64_t done[] = {10, 10};
        capture_merge::Merged m;
        capture_merge::merge(pre, done, 2, nullptr, 0, [](uint32_t p) { return p == 4 ? uint64_t{11} : uint64_t{10}; }, m);
        expect((m.recopy == std::vector<uint32_t>{0}), "write-back after the copy");
    }
    // nothing copied ahead: the fresh pages as they are
    {
        const uint32_t fresh[] = {2, 3};
        capture_merge::Merged m;
        capture_merge::merge(nullptr, nullptr, 0, fresh, 2, [](uint32_t) { return uint64_t{0}; }, m);
        expect((m.pages == std::vector<uint32_t>{2, 3}) && m.recopy.empty() && m.first_new == 0, "no pre-copy");
    }
}

}  // namespace

int main() {
    fixed_cases();
    for (unsigned seed = 1; seed <= 300; seed++) simulate(seed, true);
    for (unsigned seed = 1; seed <= 20; seed++) simulate(seed, false);
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("capture_merge: ok\n");
    return 0;
}
