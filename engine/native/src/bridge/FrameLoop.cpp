// HeyListen native engine: the game's main loop driven by the native main loop (hl/bridge/FrameLoop.h). Each phase is one
// step of the framework's frame function 0x8348c680 (docs/native-engine/main-loop.md, "One iteration"); the guest
// instruction addresses in the comments are where that step is.
#include "hl/bridge/FrameLoop.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hl/bridge/JobGroup.h"

namespace hl::bridge {

namespace {

// (the generic forms: GCC's _n forms take integers and pointers only, and these load and store floats too; clang takes both)
template <class T>
T ld(const T* p) { T v; __atomic_load(p, &v, __ATOMIC_RELAXED); return v; }
template <class T>
void st(T* p, T v) { __atomic_store(p, &v, __ATOMIC_RELAXED); }

// The guest's float-to-int conversion toward zero (fcvtzs): saturating, NaN to 0 (x86's own gives INT32_MIN there).
int32_t to_int(float f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return INT32_MAX;
    if (f <= -2147483648.0f) return INT32_MIN;
    return (int32_t)f;
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}   // namespace

FrameLoop::FrameLoop(GuestRuntime& runtime, jobs::JobSystem& jobs, GuestAddr scratch, Options options)
    : runtime_(runtime), loop_(jobs, {.target_hz = 0, .longest_step = 1.0}), scratch_(scratch), options_(options) {
    // (the native loop keeps no time of its own for the game: no frame rate cap, its step unused; the game's step and
    // the OS layer's pacing are inside the phases)
    loop_.add_phase({"prepare", [](void* u, const app::FrameTime&, jobs::JobSystem&) { static_cast<FrameLoop*>(u)->prepare(); }, this});
    loop_.add_phase({"begin", [](void* u, const app::FrameTime&, jobs::JobSystem&) { static_cast<FrameLoop*>(u)->begin(); }, this});
    loop_.add_phase({"tasks", [](void* u, const app::FrameTime&, jobs::JobSystem&) { static_cast<FrameLoop*>(u)->tasks(); }, this});
    for (int i = 0; i < 4; i++) {
        steps_[i] = {this, i};
        loop_.add_phase({"step", [](void* u, const app::FrameTime&, jobs::JobSystem&) {
                             const StepPhase* s = static_cast<const StepPhase*>(u);
                             s->loop->step(s->index);
                         }, &steps_[i]});
    }
    loop_.add_phase({"present", [](void* u, const app::FrameTime&, jobs::JobSystem&) { static_cast<FrameLoop*>(u)->present(); }, this});
    loop_.add_phase({"end", [](void* u, const app::FrameTime&, jobs::JobSystem&) { static_cast<FrameLoop*>(u)->end(); }, this});
}

uint64_t FrameLoop::call(GuestAddr fn, uint64_t a0, uint64_t a1) { return runtime_.call(as_, fn, a0, a1); }

// The framework's virtual function at `offset`, read from its table now (the game's loop and frame function read it
// at every call).
uint64_t FrameLoop::slot(uint32_t offset) const {
    return ld(at<GuestAddr>(ld(&at<guest::Framework>(fw_)->vtable) + offset));
}

void FrameLoop::bind(GuestThread as, GuestAddr framework) {
    as_ = as;
    fw_ = framework;
}

void FrameLoop::run(GuestThread as, GuestAddr framework) {
    bind(as, framework);
    const std::atomic<bool> never{false};
    loop_.run(never);
    std::abort();   // (the loop has no end: the game's has none either; the process ends around it)
}

void FrameLoop::frame() { loop_.step(); }

// (check) Each phase once, in the order added.
void FrameLoop::note(int phase) {
    stats_.phase_runs.fetch_add(1, std::memory_order_relaxed);
    if (!options_.check) return;
    if (phase != next_phase_) stats_.out_of_order.fetch_add(1, std::memory_order_relaxed);
    next_phase_ = phase + 1;
}

// (0x83b6ea90) Which frame function the framework has now (the game's loop reads it every iteration), and the game's
// framework's prepare: two SDK queries on the +0x1a8 handle (their answers reset +0x230 and +0x22c, or +0x232), its
// virtual step +0x128, the float of 60 over the nominal rate, and 0x83be0230 on +0x2f8. Then the base frame function.
void FrameLoop::prepare() {
    note(0);
    const GuestAddr fn = slot(kFrameSlot);
    kind_ = fn == kGameFrameFunction ? Kind::Game : fn == kFrameFunction ? Kind::Base : Kind::Whole;
    if (kind_ == Kind::Whole) {   // (another frame function: the game's loop calls it; so does this one, whole)
        stats_.function_changed.fetch_add(1, std::memory_order_relaxed);
        stats_.whole_frames.fetch_add(1, std::memory_order_relaxed);
        call(fn, fw_);
        return;
    }
    if (kind_ != Kind::Game) return;
    guest::Framework* f = at<guest::Framework>(fw_);
    const uint64_t q[3] = {fw_ + offsetof(guest::Framework, query_rate), fw_ + offsetof(guest::Framework, no_static),
                           ld(&f->perf_handle)};
    if ((uint32_t)runtime_.call_with(as_, kFrameQuery, q, 3, nullptr, 0)) {
        st(&f->no_static, (uint8_t)0);
        st(&f->query_rate, 60u);
    }
    if ((uint32_t)call(kFrameQuery2, fw_ + offsetof(guest::Framework, query_flag), ld(&f->perf_handle)))
        st(&f->query_flag, (uint8_t)0);
    call(slot(kPrepareSlot), fw_);
    float ratio = 60.0f / (float)ld(&f->nominal_rate);
    if (options_.frame_seconds) {   // (the lifted divide's step hook: 60 x the frame's seconds, when the host publishes them)
        const uint32_t bits = *options_.frame_seconds;
        if (bits) {
            float secs;
            std::memcpy(&secs, &bits, 4);
            ratio = 60.0f * secs;
        }
    }
    st(at<float>(kFrameRatio), ratio);
    call(kFrameRateTail, fw_ + offsetof(guest::Framework, rate_object));
}

// (0x8348c694) The mode word, the global read for the end, and the SDK's two performance queries when they are on.
void FrameLoop::begin() {
    note(1);
    if (kind_ == Kind::Whole) return;
    guest::Framework* f = at<guest::Framework>(fw_);
    const GuestAddr holder = ld(at<GuestAddr>(kFrameGlobal));
    if ((ld(&f->mode) & 3) != 1 && ld(&f->wait_mode) == 1) st(&f->wait_mode, 2u);
    global_ = ld(at<GuestAddr>(holder));
    if (ld(&f->perf_on) == 1) {
        // (0x8348c6d4) the first query writes +0x233, a word of its own (unused) and +0x236
        const uint64_t args[4] = {fw_ + offsetof(guest::Framework, perf_out), scratch_, fw_ + offsetof(guest::Framework, perf_a),
                                  ld(&f->perf_handle)};
        runtime_.call_with(as_, kPerfQuery, args, 4, nullptr, 0);
        const uint16_t a = ld(&f->perf_a), b = ld(&f->perf_b);
        st(&f->perf_min, a < b ? a : b);
        call(kPerfQuery2, scratch_ + 8, ld(&f->perf_handle));
        const uint16_t v = ld(at<uint16_t>(scratch_ + 8));
        st(&f->perf_read, v);
        st(&f->perf_scaled, (int16_t)to_int(ld(&f->perf_scale) * (float)v));
    } else {
        st(&f->perf_out, (uint8_t)0);
    }
}

// (0x8348c724) The frame's tasks.
void FrameLoop::tasks() {
    note(2);
    if (kind_ == Kind::Whole) return;
    call(kFrameTasks, ld(&at<guest::Framework>(fw_)->tasks));
}

// (0x8348c72c to 0x8348c768) The framework's four virtual steps, each read from its table at its call.
void FrameLoop::step(int i) {
    note(3 + i);
    if (kind_ == Kind::Whole) return;
    const GuestAddr fn = slot(kSteps[i]);
    if (options_.check) {
        if (targets_known_ && targets_[i] != fn) stats_.targets_changed.fetch_add(1, std::memory_order_relaxed);
        targets_[i] = fn;
    }
    call(fn, fw_);
}

// (0x8348c76c) The ticks from the frame's beginning, then the present (the OS layer paces it).
void FrameLoop::present() {
    note(7);
    if (kind_ == Kind::Whole) return;
    guest::Framework* f = at<guest::Framework>(fw_);
    const int64_t t = (int64_t)call(kSystemTick);
    st(&f->before_present, t - ld(&f->frame_begin));
    const GuestAddr fn = slot(kPresentSlot);
    if (options_.check) {
        if (targets_known_ && targets_[4] != fn) stats_.targets_changed.fetch_add(1, std::memory_order_relaxed);
        targets_[4] = fn;
        targets_known_ = true;
    }
    call(fn, fw_);
}

// A function static of the frame function made when it is not yet (its guard taken; 0x8348c830 and 0x8348c860, the same
// static twice).
void FrameLoop::make_static() {
    if (__atomic_load_n(at<uint8_t>(kFrameStaticGuard), __ATOMIC_ACQUIRE) & 1) return;
    if (!(uint32_t)call(kGuardAcquire, kFrameStaticGuard)) return;
    st(at<uint64_t>(kFrameStatic), call(kStaticMake));
    call(kGuardRelease, kFrameStaticGuard);
}

// (0x8348c78c) The frame's length and the next frame's beginning, unless the mode bits are both set; then the mode's
// request (0x8348c7f8).
void FrameLoop::end() {
    note(8);
    if (options_.check) {
        if (next_phase_ != kPhases) stats_.out_of_order.fetch_add(1, std::memory_order_relaxed);
        next_phase_ = 0;
        if (calcs_this_frame_ == 0) stats_.calc_skipped.fetch_add(1, std::memory_order_relaxed);
        if (calcs_this_frame_ > 1) stats_.calc_twice.fetch_add(1, std::memory_order_relaxed);
        calcs_this_frame_ = 0;
    }
    stats_.frames.fetch_add(1, std::memory_order_relaxed);
    if (options_.log) {
        const int64_t now = now_ns();
        if (now - last_log_ns_ >= 10000000000LL) {
            last_log_ns_ = now;
            char line[1024];
            if (report(line, sizeof line)) runtime_.log(line);
        }
    }
    if (kind_ == Kind::Whole) return;
    guest::Framework* f = at<guest::Framework>(fw_);
    const int64_t t2 = (int64_t)call(kSystemTick);
    if ((ld(&f->mode) & 3) != 3) {
        if (global_) {
            const bool over = t2 - ld(&f->frame_begin) >= ld(&f->budget_ticks);
            if (!(over && (ld(&f->budget_flag) & 1)) && !(ld(&f->no_static) & 1)) {
                make_static();
                make_static();
            }
        }
        st(&f->frame_ticks, (int64_t)call(kSystemTick) - ld(&f->frame_begin));
        st(&f->frame_begin, (int64_t)call(kSystemTick));
    }
    const uint8_t request = ld(&f->mode_request);
    if (request == 1 && (ld(&f->mode) & 1)) {
        st(&f->mode, (uint8_t)(ld(&f->mode) & 0xfc));
        st(&f->mode_request, (uint8_t)0);
        return;
    }
    if (request == 2 && !(ld(&f->mode) & 1)) {
        st(&f->mode, (uint8_t)(ld(&f->mode) | 3));
        st(&f->mode_request, (uint8_t)0);
        return;
    }
    if (request != 0) st(&f->mode_request, (uint8_t)0);
    const uint8_t m = ld(&f->mode);
    if (m & 1) st(&f->mode, (uint8_t)(m ^ 2));
}

// ---- checking: the module system's frame, observed through its entry gate --------------------------------------------

void FrameLoop::calc_begin(GuestAddr object) {
    calc_counter_ = ld(&at<guest::ModuleFrame>(object)->frames);
}

void FrameLoop::calc_end(GuestAddr object) {
    stats_.calcs.fetch_add(1, std::memory_order_relaxed);
    calcs_this_frame_++;
    if (ld(&at<guest::ModuleFrame>(object)->frames) != calc_counter_ + 1) stats_.counter_bad.fetch_add(1, std::memory_order_relaxed);
}

int FrameLoop::report(char* out, size_t cap) const {
    const Stats& s = stats_;
    auto v = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    int n = std::snprintf(out, cap, "hl mainloop: frames %llu (run whole %llu), phases run %llu", v(s.frames), v(s.whole_frames),
                          v(s.phase_runs));
    if (n < 0) return 0;
    if ((size_t)n >= cap) return (int)cap - 1;
    if (!options_.check) return n;
    const int m = std::snprintf(out + n, cap - (size_t)n,
                                "; CHECK: out of order %llu, module frames %llu (frames with two or more %llu, its counter "
                                "not counted up by one %llu); frames with none %llu, steps' functions changed %llu, frame "
                                "function changed %llu; steps 0x%llx 0x%llx 0x%llx 0x%llx, present 0x%llx",
                                v(s.out_of_order), v(s.calcs), v(s.calc_twice), v(s.counter_bad), v(s.calc_skipped),
                                v(s.targets_changed), v(s.function_changed), (unsigned long long)targets_[0],
                                (unsigned long long)targets_[1], (unsigned long long)targets_[2],
                                (unsigned long long)targets_[3], (unsigned long long)targets_[4]);
    if (m < 0) return n;
    return (size_t)(n + m) >= cap ? (int)cap - 1 : n + m;
}

}   // namespace hl::bridge
