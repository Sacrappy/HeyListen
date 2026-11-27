// Copyright (C) 2026 Sacrappy

// (HeyListen, JOBS1) When a large texture's BC blocks are read from the disk cache at once instead of transcoded on the GPU
// (vk_texture.cpp bcd_sync_wanted, RX_VK_BC_DISK_SYNC, RX_VK_BC_DISK_SYNC_MB). A plain function: tests/test_bc_disk_sync.py compiles
// this header and checks it.
#pragma once
#include <cstdint>
#include <cstdlib>

namespace bc_disk_sync {

/// RX_VK_BC_DISK_SYNC_MB's value as bytes: unset or not a number above 0, 16 MB
inline uint64_t min_bytes(const char* e) {
    const int mb = e && *e ? atoi(e) : 0;
    return (uint64_t)(mb > 0 ? mb : 16) << 20;
}
/// Whether a texture of `size` bytes of blocks is read at once: RX_VK_BC_DISK_SYNC unset or not 0, and at least min bytes. (The caller
/// asks only once the read-ahead pool had no blocks for it; the disk must then have its whole record, else the old path)
inline bool wanted(const char* env, uint64_t size, uint64_t min) {
    const bool on = !(env && *env) || atoi(env) != 0;
    return on && size >= min;
}

}   // namespace bc_disk_sync
