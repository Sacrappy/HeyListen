// (HeyListen, 2026-11-14) RX_VK_LAG_LOG=1: how late the replay and the GPU are behind the game's submits, how truthful the game's
// waits are, and what the submit-time capture protects (vk_lag.cpp). A measurement: off by default; nothing changes but the
// counting and one text file, lag_log.csv, written in the working directory.
#pragma once

#include <cstdint>

namespace mkwvk {

bool lag_on();
/// The sampled capture pages' bytes against guest memory now, by what the page was bound for
struct LagCmp {
    int64_t sampled = 0, differ[4] = {0, 0, 0, 0};   // (kinds: 0 uniform, 1 vertex or index, 2 storage and other bound, 3 never bound)
    int64_t skipped = 0;                              // (late pages, a buffer already gone)
    int64_t ns = 0;                                   // (how long the comparison took)
};
/// submit_q's entry on the submitting thread (before the capture)
void lag_bq_enter();
/// the value the submit was given
void lag_submit(uint64_t value);
void lag_run_start(uint64_t value);
/// The recording of the submission is whole (not stopped at a wait): the capture check of the submit is made here
void lag_run_end(uint64_t value, uint64_t rec_seq_now, bool burst_open);
/// A submission's capture just taken (vk_memory.cpp take_capture): its sampled pages, for the checks
void lag_cap_taken(uint64_t value, uint32_t gen, const uint32_t* pages, const uint32_t* slots, size_t n, const uint8_t* late_mark);
/// Before the next recording: the captures of submits the GPU has finished, compared again
void lag_poll_late();
/// A game wait answered (back end side): fence, timeout, result, host clock before and after, the recordings' number of the
/// fence (0: none known), the path (wait_path), whether stopped work was forced
void lag_wait(uint64_t fence, uint64_t timeout_ns, int result, int64_t t0, int64_t t1, uint64_t rec_seq, int path, uint64_t done_at_exit);
void lag_finish(int64_t t0, int64_t t1, uint64_t q_values, uint64_t completed, uint64_t pushed, int blocked);
/// The GPU's counters, and the recording a submission value ended in (0: not known yet)
void lag_set_timelines(uint64_t (*completed)(), uint64_t (*rec_completed)(), uint64_t (*rec_of)(uint64_t));
/// The submission stopped at a wait for an event (the work waits for the game's own signal)
void lag_note_stop(uint64_t value, int bit);   // (bit 1: stopped itself, 2: queued behind a stopped one)
/// A recording went to the queue with this number on the recordings' timeline: the submissions whose recording was whole in a burst end in it
void lag_recording_submitted(uint64_t seq);
void lag_present();

// (vk_memory.cpp) the comparisons
void memory_lag_compare(uint32_t gen, const uint32_t* pages, const uint32_t* slots, size_t n, LagCmp& out);

}  // namespace mkwvk
