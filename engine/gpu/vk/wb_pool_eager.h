// (HeyListen, issue 50) When a game pool's write-backs stop being left out (vk_memory.cpp, the lazy write-back, TICK-015).
//
// A game pool whose GPU writes are not read back by the CPU (CPU_CACHED, no invalidate yet) has them left in the mirror. The
// design says a CPU write to the pool ends that for good (a stale page copied from guest memory would go over the GPU's
// words), but the code only wrote the written page back. The cut-grass debris' pool is written by the GPU (the effect's
// particle state) and by the CPU (a batch per cut, in pages of its own): the first cut's batch pages were synced from guest
// memory, the state pages stayed left out in the mirror and reverted with it - the leaves at the first cut's place.
//
// RX_VK_WB_POOL_EAGER (on by default; =0: only the written page, as before): the first CPU write to a page of the pool makes
// the whole pool eager.
#pragma once

namespace wb_pool_eager {

/// Whether the CPU's write to a page of a pool makes the pool eager: not yet eager, and the switch on.
inline bool cpu_write_makes_eager(bool pool_eager, bool switch_on) { return !pool_eager && switch_on; }

/// Whether a pool of this size may have its GPU writes left out (RX_VK_WB_SMALL_EAGER_KB: smaller pools are always written
/// back; limit 0: any size may).
inline bool may_defer_size(unsigned long long pool_bytes, unsigned long long limit_bytes) { return pool_bytes >= limit_bytes; }

}  // namespace wb_pool_eager
