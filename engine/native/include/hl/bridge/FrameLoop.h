// HeyListen native engine: the game's main loop driven by the native main loop (hl::app::MainLoop).
//
// Written from a reading of the guest's framework (docs/native-engine/main-loop.md): nothing here is game code. The
// game's main thread ends in the framework's main loop (0x809667b0), which calls the framework's frame function (its
// virtual function at +0xe0, 0x8348c680) forever. Answered here through an entry gate on the loop, the iteration and the
// order of the frame's steps are the native loop's: each step of the frame function is one phase, run in the game's
// order on the main thread; the game's own functions those steps call (the frame's tasks, the framework's four virtual
// steps, its present, the SDK's tick and performance queries) are called as they are, on the main thread's own guest
// context; the words the frame function writes between them (the framework's timing and mode fields) are written in
// place with the guest's values in the guest's order. The frame's time step stays the game's (computed inside one of
// those steps, by the module system's frame 0x80706be0) and the pacing the OS layer's (inside the present step).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hl/app/MainLoop.h"
#include "hl/bridge/JobBridge.h"

namespace hl::bridge {

/// Guest functions and data the frame's phases use (the supported build's; docs/native-engine/main-loop.md).
inline constexpr GuestAddr kMainLoop = 0x809667b0;        ///< answered: run() (the framework's main loop; never returns)
inline constexpr GuestAddr kFrameFunction = 0x8348c680;   ///< the framework's frame function, whose steps are the phases
inline constexpr GuestAddr kModuleFrame = 0x80706be0;     ///< the module system's frame (observed by the check)
inline constexpr GuestAddr kFrameTasks = 0x802eaf10;      ///< the frame's tasks (the framework's +0x38)
inline constexpr GuestAddr kSystemTick = 0x83c61fd0;      ///< the system tick
inline constexpr GuestAddr kPerfQuery = 0x83c62ad0;       ///< the SDK's two queries on the framework's +0x1a8 handle
inline constexpr GuestAddr kPerfQuery2 = 0x83c62ae0;
inline constexpr GuestAddr kStaticMake = 0x83c62ec0;      ///< what a function static of the frame function is made of
inline constexpr GuestAddr kFrameGlobal = 0x84cb4840;     ///< holds the address of a global the frame function reads
inline constexpr GuestAddr kFrameStatic = 0x84e415f0;     ///< that function static, and its guard
inline constexpr GuestAddr kFrameStaticGuard = 0x84e415f8;
inline constexpr GuestAddr kFrameTimeHolder = 0x84c9e890; ///< holds the frame-time object (+0 the step, +16 the ratio)
inline constexpr uint32_t kFrameSlot = 0xe0;              ///< the frame function's slot in the framework's virtual table

namespace guest {

/// The framework object, as its frame function uses it.
struct Framework {
    GuestAddr vtable;
    uint8_t _8[0x30];
    GuestAddr tasks;          ///< +0x38: the frame's tasks
    uint8_t _40[0x10];
    uint32_t wait_mode;       ///< +0x50: 1 becomes 2 unless the mode bits are 1
    uint8_t _54[0x56];
    uint8_t budget_flag;      ///< +0xaa
    uint8_t _ab[0x55];
    int64_t frame_ticks;      ///< +0x100: the last frame's length (ticks)
    int64_t frame_begin;      ///< +0x108: the tick the frame began at
    uint8_t _110[0x28];
    int64_t budget_ticks;     ///< +0x138
    uint8_t _140[8];
    int64_t before_present;   ///< +0x148: ticks from the frame's beginning to its present
    uint8_t _150[0x58];
    GuestAddr perf_handle;    ///< +0x1a8: the handle the two SDK queries are asked on
    uint8_t _1b0[0x80];
    uint8_t no_static;        ///< +0x230 (bit 0)
    uint8_t _231[2];
    uint8_t perf_out;         ///< +0x233: written by the first query (0 when the queries are off)
    uint8_t perf_on;          ///< +0x234: 1: the queries are asked each frame
    uint8_t _235;
    uint16_t perf_a;          ///< +0x236: written by the first query
    uint16_t perf_b;          ///< +0x238
    uint16_t perf_read;       ///< +0x23a: the second query's answer
    uint16_t perf_min;        ///< +0x23c: the smaller of +0x236 and +0x238
    int16_t perf_scaled;      ///< +0x23e: +0x23a times +0x240
    float perf_scale;         ///< +0x240
    uint8_t _244[0xa];
    uint8_t mode_request;     ///< +0x24e
    uint8_t mode;             ///< +0x24f (bits 0 and 1)
};
static_assert(offsetof(Framework, tasks) == 0x38);
static_assert(offsetof(Framework, wait_mode) == 0x50);
static_assert(offsetof(Framework, budget_flag) == 0xaa);
static_assert(offsetof(Framework, frame_ticks) == 0x100);
static_assert(offsetof(Framework, frame_begin) == 0x108);
static_assert(offsetof(Framework, budget_ticks) == 0x138);
static_assert(offsetof(Framework, before_present) == 0x148);
static_assert(offsetof(Framework, perf_handle) == 0x1a8);
static_assert(offsetof(Framework, no_static) == 0x230);
static_assert(offsetof(Framework, perf_out) == 0x233);
static_assert(offsetof(Framework, perf_on) == 0x234);
static_assert(offsetof(Framework, perf_a) == 0x236);
static_assert(offsetof(Framework, perf_b) == 0x238);
static_assert(offsetof(Framework, perf_read) == 0x23a);
static_assert(offsetof(Framework, perf_min) == 0x23c);
static_assert(offsetof(Framework, perf_scaled) == 0x23e);
static_assert(offsetof(Framework, perf_scale) == 0x240);
static_assert(offsetof(Framework, mode_request) == 0x24e);
static_assert(offsetof(Framework, mode) == 0x24f);

/// The module system's frame object, as the check reads it.
struct ModuleFrame {
    uint8_t _0[0xd50];
    int32_t frames;   ///< +0xd50: counted up once a frame (an atomic add)
};
static_assert(offsetof(ModuleFrame, frames) == 0xd50);

}   // namespace guest

class FrameLoop {
public:
    struct Options {
        bool check = false;   ///< count the invariants below
        bool log = false;     ///< one line of counters in the game's log every 10 s (from the main thread, at a frame's end)
    };
    /// The framework's virtual steps, in the frame function's order (between the frame's tasks and its present).
    static constexpr uint32_t kSteps[4] = {0xf0, 0x70, 0x120, 0xe8};
    static constexpr uint32_t kPresentSlot = 0x110;
    static constexpr int kPhases = 8;   ///< begin, tasks, the four steps, present, end
    // Counters. With `check`, the invariants (each should stay 0):
    //   out_of_order: a phase run out of the game's order, or not exactly once in a frame
    //   calc_twice: a frame in which the module system's frame ran more than once
    //   counter_bad: the module frame's counter not counted up by exactly one by its run
    // and observations: calc_skipped (frames whose steps did not reach the module system's frame), targets_changed (frames whose virtual steps resolved to other functions than the first frame's),
    // function_changed (frames in which the framework's frame function, its +0xe0, was not 0x8348c680: the native loop
    // then runs that function whole, as the game's loop would).
    struct Stats {
        std::atomic<uint64_t> frames{0}, whole_frames{0}, phase_runs{0}, out_of_order{0};
        std::atomic<uint64_t> calcs{0}, calc_skipped{0}, calc_twice{0}, counter_bad{0}, targets_changed{0};
        std::atomic<uint64_t> function_changed{0};
    };

    /// `scratch`: 32 bytes of guest memory the frame's SDK queries write their answers to (only the main thread uses it).
    FrameLoop(GuestRuntime& runtime, jobs::JobSystem& jobs, GuestAddr scratch, Options options);

    /// The main loop's answer, on the main thread (`as`): frames of `framework` forever.
    [[noreturn]] void run(GuestThread as, GuestAddr framework);
    /// One frame (the loop's iteration; run() calls it forever, the test a few times).
    void frame();
    void bind(GuestThread as, GuestAddr framework);

    /// (check) the module system's frame `object` is about to run / has run, on the main thread.
    void calc_begin(GuestAddr object);
    void calc_end(GuestAddr object);

    const Stats& stats() const { return stats_; }
    int report(char* out, size_t cap) const;
    /// The phases' guest functions as last resolved: the four virtual steps and the present.
    const GuestAddr* targets() const { return targets_; }

private:
    template <class T>
    T* at(GuestAddr a) const { return static_cast<T*>(runtime_.ptr(a)); }
    uint64_t call(GuestAddr fn, uint64_t a0 = 0, uint64_t a1 = 0);
    uint64_t slot(uint32_t offset) const;
    void note(int phase);

    void begin();
    void tasks();
    void step(int i);
    void present();
    void end();
    void make_static();

    GuestRuntime& runtime_;
    app::MainLoop loop_;
    GuestAddr scratch_;
    Options options_;
    Stats stats_;
    GuestThread as_ = nullptr;
    GuestAddr fw_ = 0;
    GuestAddr global_ = 0;   // the global the frame function reads at its beginning, for its end
    int next_phase_ = 0;
    GuestAddr targets_[5] = {};
    bool targets_known_ = false;
    bool whole_ = false;   // this frame: the frame function is another one, run whole
    int64_t last_log_ns_ = 0;
    int calcs_this_frame_ = 0;
    int32_t calc_counter_ = 0;
    struct StepPhase {
        FrameLoop* loop;
        int index;
    } steps_[4];
};

}   // namespace hl::bridge
