// HeyListen native engine: which resource decodes the native path takes, and the counters of the stream module.
//
// The game's decode of a resource file (docs/native-engine/stream-boundary.md) is answered natively only on its main
// path; a request that would take a rare path is given to the translated function whole, and the choice must be made
// before anything observable is done for it (the file opened, a byte read, memory allocated). The one decision taken
// later is the chunked case: whether the file fits the decompressor's read buffer is known only after the first read,
// and the native side then closes the file and gives the request to the translated function, which opens and reads it
// again (stream-boundary.md says why that is invisible to the game).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hl::stream {

/// What the OS layer reads of a request and of the decompressor before acting on it.
struct Request {
    uint32_t size_step = 0;        // the request's size granularity (0: none)
    bool has_file = false;         // the request gives a file object (else the game opens the file by a path of its own)
    bool file_kind_ok = false;     // that object's kind flag says it opens through its own open function
    bool has_read_buffer = false;  // the decompressor has its own read buffer (else a temporary one is allocated)
    bool has_heap = false;         // the request gives a heap (else the calling thread's current heap is used)
};

enum class Reason : uint8_t {
    Native = 0,      // the native path answers
    SizeStep,        // the size granularity is not a multiple of 32: an error path
    PathOpen,        // no file object: opened by path through another helper
    FileKind,        // a file object of another kind: an error path
    NoReadBuffer,    // a temporary read buffer would be allocated from the request's heap
    NoHeap,          // no heap in the request
    Chunked,         // larger than the read buffer: decoded chunk by chunk on a second thread (decided after one read)
    Count
};

const char* reason_name(Reason r);

/// The path a request takes, decided before anything is done for it.
Reason choose(const Request& r);

/// The module's counters (any thread).
struct Counters {
    std::atomic<uint64_t> files[static_cast<int>(Reason::Count)] = {};
    std::atomic<uint64_t> bytes[static_cast<int>(Reason::Count)] = {};   // decompressed bytes, when known
    std::atomic<uint64_t> errors{0};                                       // native decodes that ended on an error path
    // the check (translated decode, then the same bytes decoded natively and compared)
    std::atomic<uint64_t> checked_files{0}, checked_bytes{0}, mismatches{0}, unchecked{0};
    std::atomic<uint64_t> checked_chunked{0};

    void add(Reason r, uint64_t decoded_bytes);
    /// One line for the log. Returns the length written.
    int report(char* out, size_t cap) const;
};

}   // namespace hl::stream
