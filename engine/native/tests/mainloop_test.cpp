// HeyListen native engine: the game's main loop driven by the native one (hl/bridge/FrameLoop.h) run with no game, on a
// model of the framework object laid out as the guest has it. The game's functions the frame's steps call (the tasks,
// the four virtual steps, the present, the tick, the SDK's queries, a function static's guard) are played by this file;
// the frame function itself is played a second time by reference_frame, written from docs/native-engine/main-loop.md
// ("One iteration"), on a copy of the same memory with the same answers: both must leave the same bytes and make the
// same calls in the same order, frame after frame, through every mode and request.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "hl/bridge/FrameLoop.h"
#include "hl/bridge/JobGroup.h"

using namespace hl;
using bridge::GuestAddr;
namespace g = bridge::guest;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

constexpr GuestAddr kBase = 0x10000000, kSize = 1u << 20;
constexpr GuestAddr kFw = kBase + 0x1000, kVt = kBase + 0x2000, kGlobal = kBase + 0x3000, kHandle = kBase + 0x3100,
                    kScratch = kBase + 0x3200, kTasks = kBase + 0x3300, kModule = kBase + 0x4000;
constexpr GuestAddr kStepFn[4] = {0x70001000, 0x70001100, 0x70001200, 0x70001300};
constexpr GuestAddr kPresentFn = 0x70001400, kOtherFrame = 0x70001500, kPrepareFn = 0x70001600;
// (pages of the guest's globals the frame function reads)
constexpr GuestAddr kPageA = 0x84cb4000, kPageB = 0x84e41000, kPageC = 0x84d61000;

struct Model final : bridge::GuestRuntime {
    std::vector<uint8_t> memory = std::vector<uint8_t>(kSize), page_a = std::vector<uint8_t>(0x1000),
                         page_b = std::vector<uint8_t>(0x1000), page_c = std::vector<uint8_t>(0x1000);
    std::vector<std::string> calls;
    uint64_t tick = 1000;
    std::mt19937 rng{5};
    bridge::FrameLoop* loop = nullptr;   // (the module frame's observer, as the check's gate calls it)
    int calcs_in_step = 1;

    void* ptr(GuestAddr at) const override {
        if (at >= kPageA && at < kPageA + 0x1000) return const_cast<uint8_t*>(page_a.data()) + (at - kPageA);
        if (at >= kPageB && at < kPageB + 0x1000) return const_cast<uint8_t*>(page_b.data()) + (at - kPageB);
        if (at >= kPageC && at < kPageC + 0x1000) return const_cast<uint8_t*>(page_c.data()) + (at - kPageC);
        CHECK(at >= kBase && at < kBase + kSize);
        return const_cast<uint8_t*>(memory.data()) + (at - kBase);
    }
    template <class T>
    T* at(GuestAddr a) { return static_cast<T*>(ptr(a)); }
    bridge::GuestThread current() const override { return (void*)1; }
    void log(const char* line) override { std::fprintf(stderr, "%s\n", line); }
    uint64_t call(bridge::GuestThread, GuestAddr fn, uint64_t a0, uint64_t a1) override {
        const uint64_t a[4] = {a0, a1, 0, 0};
        return guest(fn, a);
    }
    uint64_t call_with(bridge::GuestThread, GuestAddr fn, const uint64_t* args, unsigned nargs, const void*, unsigned) override {
        uint64_t a[4] = {};
        for (unsigned i = 0; i < nargs; i++) a[i] = args[i];
        return guest(fn, a);
    }
    void note(const char* what, uint64_t v = 0) {
        char s[64];
        std::snprintf(s, sizeof s, "%s %llx", what, (unsigned long long)v);
        calls.push_back(s);
    }
    uint64_t guest(GuestAddr fn, const uint64_t* a) {
        switch (fn) {
        case bridge::kSystemTick: tick += 7 + rng() % 5; note("tick"); return tick;
        case bridge::kPerfQuery:
            CHECK(a[3] == kHandle);
            *at<uint8_t>(a[0]) = 1;
            *at<uint32_t>(a[1]) = 0xdead;
            *at<uint16_t>(a[2]) = (uint16_t)(rng() % 900);
            note("perf");
            return 0;
        case bridge::kPerfQuery2:
            CHECK(a[1] == kHandle);
            *at<uint16_t>(a[0]) = (uint16_t)(rng() % 70000);
            note("perf2");
            return 0;
        case bridge::kFrameTasks: CHECK(a[0] == kTasks); note("tasks"); return 0;
        case bridge::kGuardAcquire: note("guard"); return (*at<uint8_t>(a[0]) & 1) ? 0 : 1;
        case bridge::kGuardRelease: *at<uint8_t>(a[0]) = 1; note("release"); return 0;
        case bridge::kStaticMake: note("make"); return 0x5151;
        case kPresentFn: CHECK(a[0] == kFw); note("present"); return 0;
        case bridge::kFrameQuery: {
            CHECK(a[0] == kFw + 0x22c && a[1] == kFw + 0x230 && a[2] == kHandle);
            *at<uint32_t>(a[0]) = 30 + rng() % 40;
            *at<uint8_t>(a[1]) = (uint8_t)(rng() % 2);
            note("query");
            return rng() % 3 == 0;
        }
        case bridge::kFrameQuery2:
            CHECK(a[0] == kFw + 0x232 && a[1] == kHandle);
            *at<uint8_t>(a[0]) = (uint8_t)(1 + rng() % 2);
            note("query2");
            return rng() % 2;
        case kPrepareFn: CHECK(a[0] == kFw); note("prepare"); return 0;
        case bridge::kFrameRateTail: CHECK(a[0] == kFw + 0x2f8); note("rate"); return 0;
        case kOtherFrame: CHECK(a[0] == kFw); note("other frame"); return 0;
        default:
            for (int i = 0; i < 4; i++)
                if (fn == kStepFn[i]) {
                    CHECK(a[0] == kFw);
                    note("step", (uint64_t)i);
                    if (i == 1 && loop)   // (the module system's frame runs inside one of the steps)
                        for (int k = 0; k < calcs_in_step; k++) {
                            loop->calc_begin(kModule);
                            __atomic_fetch_add(&at<g::ModuleFrame>(kModule)->frames, 1, __ATOMIC_SEQ_CST);
                            loop->calc_end(kModule);
                        }
                    return 0;
                }
            CHECK(!"a guest function the model does not have");
            return 0;
        }
    }
    uint64_t vslot(uint32_t off) { return *at<GuestAddr>(*at<GuestAddr>(kFw) + off); }

    void setup() {
        g::Framework* f = at<g::Framework>(kFw);
        f->vtable = kVt;
        f->tasks = kTasks;
        f->perf_handle = kHandle;
        f->budget_ticks = 30;
        f->perf_scale = 1.75f;
        f->frame_begin = 900;
        *at<GuestAddr>(kVt + bridge::kFrameSlot) = bridge::kGameFrameFunction;
        *at<GuestAddr>(kVt + bridge::kPrepareSlot) = kPrepareFn;
        for (int i = 0; i < 4; i++) *at<GuestAddr>(kVt + bridge::FrameLoop::kSteps[i]) = kStepFn[i];
        *at<GuestAddr>(kVt + bridge::FrameLoop::kPresentSlot) = kPresentFn;
        *at<GuestAddr>(bridge::kFrameGlobal) = kGlobal;
        *at<GuestAddr>(kGlobal) = 0x4444;
    }

    // The frame function as main-loop.md reads it ("One iteration").
    void reference_frame() {
        g::Framework* f = at<g::Framework>(kFw);
        if (vslot(bridge::kFrameSlot) == bridge::kGameFrameFunction) {   // (the game's framework's prepare)
            const uint64_t q[4] = {kFw + 0x22c, kFw + 0x230, f->perf_handle, 0};
            if ((uint32_t)guest(bridge::kFrameQuery, q)) {
                f->no_static = 0;
                f->query_rate = 60;
            }
            const uint64_t q2[4] = {kFw + 0x232, f->perf_handle, 0, 0};
            if ((uint32_t)guest(bridge::kFrameQuery2, q2)) f->query_flag = 0;
            const uint64_t a[4] = {kFw, 0, 0, 0};
            guest(vslot(bridge::kPrepareSlot), a);
            *at<float>(bridge::kFrameRatio) = 60.0f / (float)f->nominal_rate;
            const uint64_t r[4] = {kFw + 0x2f8, 0, 0, 0};
            guest(bridge::kFrameRateTail, r);
        }
        const GuestAddr holder = *at<GuestAddr>(bridge::kFrameGlobal);
        if ((f->mode & 3) != 1 && f->wait_mode == 1) f->wait_mode = 2;
        const GuestAddr global = *at<GuestAddr>(holder);
        if (f->perf_on == 1) {
            const uint64_t q[4] = {kFw + 0x233, kScratch, kFw + 0x236, f->perf_handle};
            guest(bridge::kPerfQuery, q);
            f->perf_min = f->perf_a < f->perf_b ? f->perf_a : f->perf_b;
            const uint64_t q2[4] = {kScratch + 8, f->perf_handle, 0, 0};
            guest(bridge::kPerfQuery2, q2);
            const uint16_t v = *at<uint16_t>(kScratch + 8);
            f->perf_read = v;
            const float p = f->perf_scale * (float)v;
            f->perf_scaled = (int16_t)(p >= 2147483648.0f ? INT32_MAX : (int32_t)p);
        } else {
            f->perf_out = 0;
        }
        const uint64_t t[4] = {f->tasks, 0, 0, 0};
        guest(bridge::kFrameTasks, t);
        for (uint32_t s : bridge::FrameLoop::kSteps) {
            const uint64_t a[4] = {kFw, 0, 0, 0};
            guest(vslot(s), a);
        }
        const uint64_t none[4] = {};
        f->before_present = (int64_t)guest(bridge::kSystemTick, none) - f->frame_begin;
        const uint64_t a[4] = {kFw, 0, 0, 0};
        guest(vslot(bridge::FrameLoop::kPresentSlot), a);
        const int64_t t2 = (int64_t)guest(bridge::kSystemTick, none);
        if ((f->mode & 3) != 3) {
            const bool skip = (t2 - f->frame_begin < f->budget_ticks) ? false : (f->budget_flag & 1);
            if (global && !skip && !(f->no_static & 1))
                for (int k = 0; k < 2; k++)
                    if (!(*at<uint8_t>(bridge::kFrameStaticGuard) & 1)) {
                        const uint64_t gd[4] = {bridge::kFrameStaticGuard, 0, 0, 0};
                        if (guest(bridge::kGuardAcquire, gd)) {
                            *at<uint64_t>(bridge::kFrameStatic) = guest(bridge::kStaticMake, none);
                            guest(bridge::kGuardRelease, gd);
                        }
                    }
            f->frame_ticks = (int64_t)guest(bridge::kSystemTick, none) - f->frame_begin;
            f->frame_begin = (int64_t)guest(bridge::kSystemTick, none);
        }
        const uint8_t r = f->mode_request;
        if (r == 1) {
            if (f->mode & 1) {
                f->mode &= 0xfc;
                f->mode_request = 0;
                return;
            }
            f->mode_request = 0;
        } else if (r == 2) {
            if (!(f->mode & 1)) {
                f->mode |= 3;
                f->mode_request = 0;
                return;
            }
            f->mode_request = 0;
        } else if (r != 0) {
            f->mode_request = 0;
        }
        if (f->mode & 1) f->mode ^= 2;
    }
};

// What changes between frames, the same in both models: modes, requests, the queries on or off, the budget, the global,
// the static's guard taken back now and then.
void stir(Model& m, std::mt19937& rng) {
    g::Framework* f = m.at<g::Framework>(kFw);
    f->mode = (uint8_t)(rng() % 4);
    f->mode_request = (uint8_t)(rng() % 4);
    f->wait_mode = rng() % 3;
    f->perf_on = rng() % 3 == 0 ? 0 : 1;
    f->perf_b = (uint16_t)(rng() % 900);
    f->budget_flag = (uint8_t)(rng() % 2);
    f->no_static = (uint8_t)(rng() % 4 == 0);
    f->budget_ticks = (int64_t)(rng() % 60);
    f->nominal_rate = (int32_t)(rng() % 140);   // (0 too: the ratio is then infinite on both sides)
    *m.at<GuestAddr>(kVt + bridge::kFrameSlot) = rng() % 8 == 0 ? bridge::kFrameFunction : bridge::kGameFrameFunction;
    *m.at<GuestAddr>(kGlobal) = rng() % 5 == 0 ? 0 : 0x4444;
    if (rng() % 6 == 0) *m.at<uint8_t>(bridge::kFrameStaticGuard) = 0;
}

// The native frame against the reference frame, 400 frames through every mode and request.
void frames_match_the_frame_function() {
    jobs::JobSystem js({.workers = 1});
    Model native, ref;
    native.setup();
    ref.setup();
    bridge::FrameLoop loop(native, js, kScratch, {.check = true});
    loop.bind((void*)1, kFw);
    native.loop = &loop;
    std::mt19937 a(77), b(77);
    for (int frame = 0; frame < 400; frame++) {
        stir(native, a);
        stir(ref, b);
        native.calls.clear();
        ref.calls.clear();
        loop.frame();
        ref.loop = nullptr;
        ref.reference_frame();
        // (the reference runs no module frame: the native model's step 1 does; drop the counter word from the compare)
        native.at<g::ModuleFrame>(kModule)->frames = 0;
        if (native.calls != ref.calls) {
            for (size_t i = 0; i < native.calls.size() || i < ref.calls.size(); i++)
                std::fprintf(stderr, "%-12s | %s\n", i < native.calls.size() ? native.calls[i].c_str() : "",
                             i < ref.calls.size() ? ref.calls[i].c_str() : "");
        }
        CHECK(native.calls == ref.calls);
        CHECK(native.memory == ref.memory && native.page_a == ref.page_a && native.page_b == ref.page_b &&
              native.page_c == ref.page_c);
    }
    const bridge::FrameLoop::Stats& s = loop.stats();
    CHECK(s.frames == 400 && s.phase_runs == 400 * bridge::FrameLoop::kPhases && s.out_of_order == 0);
    CHECK(s.calcs == 400 && s.calc_skipped == 0 && s.calc_twice == 0 && s.counter_bad == 0);
    CHECK(s.targets_changed == 0 && s.function_changed == 0 && s.whole_frames == 0);
    CHECK(loop.targets()[0] == kStepFn[0] && loop.targets()[4] == kPresentFn);
    char line[1024];
    loop.report(line, sizeof line);
    std::printf("%s\n", line);
}

// The check's observations: a frame whose steps run the module frame twice, one with none, a step resolved to another
// function, a frame function that is another one (run whole, as the game's loop would).
void the_check_sees() {
    jobs::JobSystem js({.workers = 1});
    Model m;
    m.setup();
    bridge::FrameLoop loop(m, js, kScratch, {.check = true});
    loop.bind((void*)1, kFw);
    m.loop = &loop;
    loop.frame();
    m.calcs_in_step = 2;
    loop.frame();
    m.calcs_in_step = 0;
    loop.frame();
    m.calcs_in_step = 1;
    *m.at<GuestAddr>(kVt + bridge::FrameLoop::kSteps[2]) = kStepFn[3];
    loop.frame();
    *m.at<GuestAddr>(kVt + bridge::kFrameSlot) = kOtherFrame;
    m.calls.clear();
    loop.frame();
    CHECK(m.calls.size() == 1 && m.calls[0] == "other frame 0");
    const bridge::FrameLoop::Stats& s = loop.stats();
    CHECK(s.calc_twice == 1 && s.calc_skipped == 2 && s.targets_changed == 1 && s.function_changed == 1);
    CHECK(s.whole_frames == 1 && s.frames == 5 && s.out_of_order == 0 && s.counter_bad == 0);
}

}   // namespace

int main() {
    frames_match_the_frame_function();
    the_check_sees();
    std::puts("native engine: the main loop on a model of the game's framework: ok");
    return 0;
}
