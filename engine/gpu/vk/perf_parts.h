/* (heylisten) A frame's parts on the thread that presents (perf_report.cpp's parts_ms and its hitch causes): what the
   renderer measures itself. The parts are wall-clock times on the presenting thread, so they add up to the frame:

     game     the guest's own step and any gap with no work (the frame's rest: the presenting thread outside the renderer
              and the pacing sleep; its CPU-at-work share is the game_cpu cause, the rest is os_wait)
     render   the renderer's own work: submit_q / present / the replay thread's jobs (RX_VK_ASYNC=1), the waits below cut out
     gpu_wait the waits for the GPU inside it: a recording slot come round again (recording_wait), the swapchain's image
              (present_image), and the game's CPU read-backs of GPU results (vk_replay.cpp invalidate, all threads)
     pace     the pacing sleep before the frame (nn_vi.c hos_vi_native_acquire), pushed by host/nvn_native.c at the present

   Only scopes on per-frame paths (nothing per draw); each is two prof_now() reads, and nothing at all while the
   performance summary is off (perf_parts::on, one relaxed load a scope). The read-backs' total is a counter accumulated
   where the wait already was (its own block in invalidate), and the pacing sleep and the loading flag arrive through
   option_set (HOS_OPT_PERF_PACE / HOS_OPT_PERF_LOADING, as the Cheats page's counts do: host/nvn_native.c, host/hos_main.c). */
#pragma once

#include <atomic>
#include <cstdint>

#include "vk_internal.h"

namespace mkwvk::perf_parts {

/// (perf_report.cpp turns it with the row) the summary is measuring: the scopes below then cost two prof_now() reads each
inline std::atomic<bool> on{false};

/// The frame's pacing sleep in us (host/nn_vi.c through nvn_native.c's push at each present) and whether the game is
/// loading a map (host/hos_main.c, pushed when it changes): the pace part and the hitch rows' load flag
enum { kOptPace = 90, kOptLoading = 91 };   // (option_set ids: the same literals are in host/nvn_native.c and host/hos_main.c)
inline std::atomic<int64_t> pace_us{0};   // (this frame's, replaced at each present)
inline std::atomic<int> loading{0};

/// This thread's accumulated parts, ns: the renderer's own work, the GPU waits inside it, and the swapchain's block
/// (vkQueuePresentKHR, which can hold the presenting thread). Read as deltas by perf_frame_shown, on this thread
inline thread_local int64_t t_render = 0, t_wait = 0, t_present = 0;

/// A measured span on the calling thread, into `sum`: nothing while the summary is off
struct Scope {
    int64_t* sum;
    int64_t t0;
    explicit Scope(int64_t& s) : sum(on.load(std::memory_order_relaxed) ? &s : nullptr), t0(0) {
        if (sum) t0 = prof_now();
    }
    ~Scope() {
        if (sum) *sum += prof_now() - t0;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

/// (host/nvn_native.c's push) this frame's pacing sleep, and the option ids both sides know
inline void note_pace(int us) { pace_us.store(us, std::memory_order_relaxed); }
/// (host/hos_main.c's push) a map is loading (an area change): the summary's hitch rows carry it
inline void note_loading(int v) { loading.store(v ? 1 : 0, std::memory_order_relaxed); }

}   // namespace mkwvk::perf_parts
