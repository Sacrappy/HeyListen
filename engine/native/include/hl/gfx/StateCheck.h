// HeyListen native engine: the call-by-call check of a native function of the draw path against its translation
// (docs/native-engine/gfx-record-job.md, section 8).
//
// Unlike the whole recording pass (PassCheck.h), a function the game calls directly can be compared exactly: at each
// call the native version runs first into a recording (no NVN procedure called, nothing written to the game's memory:
// what it would write is kept aside as the bytes the block should hold after the call), then the translated body runs
// for real with the OS layer's NVN tap recording its calls. The check compares the two lists of calls entry by entry
// (the procedure and each of its argument registers; guest pointers as values, since both versions run on the same
// state) and the block's bytes after the translated body against the native version's.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "hl/gfx/Commands.h"

namespace hl::gfx {

class StateCheck {
public:
    struct Report {
        uint64_t calls = 0;               ///< calls compared
        uint64_t commands = 0;            ///< NVN calls compared (the translated side's)
        uint64_t mismatched_calls = 0;    ///< calls with any difference
        uint64_t command_mismatches = 0;  ///< of them, a different list of NVN calls
        uint64_t block_mismatches = 0;    ///< of them, different bytes in the block after the call
        uint64_t deferred = 0;            ///< calls the native version left to the translation (not compared)
    };
    /// Where the first mismatches are written in full, one line each.
    using Log = void (*)(void* user, const char* line);

    /// `name` begins every line it logs ("render state").
    StateCheck(const char* name, Log log, void* user, uint32_t most_logged = 8)
        : name_(name), log_(log), user_(user), most_logged_(most_logged) {}

    /// One call: the native version's commands and the block it would leave (expected), the translated body's commands
    /// and the block it left (actual). Returns true when they are the same. `path_differs`: the native version's
    /// decision of which way the call goes no longer holds (a mismatch of the calls whatever they are).
    bool compare(uint64_t block, uint32_t key, const std::vector<Command>& native,
                 const std::vector<Command>& translated, const uint8_t* expected, const uint8_t* actual, size_t size,
                 bool path_differs = false);
    void deferred() { deferred_.fetch_add(1, std::memory_order_relaxed); }
    Report report() const;

private:
    const char* name_;
    Log log_;
    void* user_;
    uint32_t most_logged_;
    std::atomic<uint64_t> calls_{0}, commands_{0}, mismatched_{0}, command_mismatches_{0}, block_mismatches_{0},
        deferred_{0};
    std::mutex log_lock_;
    uint32_t logged_ = 0;
};

}   // namespace hl::gfx
