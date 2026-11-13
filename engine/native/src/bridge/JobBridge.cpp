// HeyListen native engine: the job system's boundary with the gameplay code (hl/bridge/JobBridge.h).
#include "hl/bridge/JobBridge.h"

namespace hl::bridge {

namespace {
// (the supported build's addresses; docs/native-engine/jobs-boundary.md)
constexpr GuestEntry kEntries[] = {
    {0x80b5e9e0, EntryRole::Setup, "the job system made: its schedulers and worker sets"},
    {0x80b5ef60, EntryRole::Setup, "a scheduler made (its list of contexts, its queues)"},
    {0x8069b020, EntryRole::Setup, "a worker set made: one worker thread a core"},
    {0x806af9d0, EntryRole::Stage, "a stage begun on a set of cores"},
    {0x806afae0, EntryRole::Stage, "a stage begun on a set of cores (second form)"},
    {0x806afe70, EntryRole::Stage, "a stage begun on a set of cores (third form)"},
    {0x806b0260, EntryRole::Stage, "the stage's workers registered with its scheduler and sent their message"},
    {0x806afc40, EntryRole::Stage, "the stage waited for, its owner running jobs meanwhile"},
    {0x83acfb90, EntryRole::Stage, "the frame's last stage: each owner thread starts the executor of its core and waits"},
    {0x8004aae0, EntryRole::Worker, "a worker's message: run the dispatcher, then signal the end"},
    {0x80df2680, EntryRole::Worker, "a job thread's loop (message to job)"},
    {0x80df25c0, EntryRole::Worker, "a job thread's loop (inner)"},
    {0x834a2e50, EntryRole::Worker, "a job thread's loop (outer)"},
    {0x8004ab40, EntryRole::Dispatch, "the dispatcher: takes jobs from a scheduler's queues and runs them"},
    {0x8004fa40, EntryRole::Dispatch, "a job pushed to a scheduler"},
    {0x8007fe60, EntryRole::Group, "a job group run and waited for (the main thread's long wait of the frame)"},
    {0x80706be0, EntryRole::Frame, "the frame's time step read"},
    {0x8022e220, EntryRole::Consumer, "an actor link followed: reads the identity table (one of hundreds of inlined readers)"},
};
}   // namespace

std::span<const GuestEntry> job_entries() { return kEntries; }

Readiness ready() {
    return {false,
            "the guest runtime (bind a native thread to an executor's guest context) is not written\n"
            "the entry points are named, not answered: each needs its native handler and its arguments' meaning checked\n"
            "the identity table and the lane status word are not maintained from the native side yet\n"
            "the job objects' layout (function, object, dependencies) is read only as far as the dispatcher needs\n"
            "no differential run against the translated job system has been made"};
}

}   // namespace hl::bridge
