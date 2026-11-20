// SPDX-License-Identifier: GPL-3.0-or-later
// (hookshot, #1001, issue 2245; the switch from Longshot 8694e3c6) How the game's task and mesh programs run: 0 Automatic
// (the GPU's own mesh shaders where it has them, the compute fallback where not, and on Linux with an RDNA 2 or RDNA 3.5 AMD GPU,
// RADV, the compute path), 1 Native (the GPU's own mesh shaders, RADV's too), 2 Compute (the fallback on every GPU). Chosen by the
// launch switch RX_VK_MESH_SHADERS=native or compute; RX_VK_FORCE_MESH_FALLBACK=1 is Compute over it. Nothing of Vulkan
// here, so tests/test_mesh_mode_switch.py compiles and runs it without a GPU or a game.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace mesh_rule {

struct Choice {
    int mode;          // 0 Automatic, 1 Native, 2 Compute
    const char* why;   // where it came from, for the log
};

inline bool same_word(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (std::tolower((unsigned char)*a) != *b) return false;
    return !*a && !*b;
}

/// env: RX_VK_MESH_SHADERS's value or null (native or compute; anything else, Automatic). force_fallback:
/// RX_VK_FORCE_MESH_FALLBACK's value or null, a test that is Compute over it when set and not 0
inline Choice choose(const char* env, const char* force_fallback) {
    if (force_fallback && std::atoi(force_fallback) != 0) return {2, "RX_VK_FORCE_MESH_FALLBACK"};
    if (env && same_word(env, "native")) return {1, "RX_VK_MESH_SHADERS"};
    if (env && same_word(env, "compute")) return {2, "RX_VK_MESH_SHADERS"};
    if (env && *env) return {0, "the default (RX_VK_MESH_SHADERS is neither native nor compute)"};
    return {0, "the default"};
}

constexpr uint32_t kVendorAmd = 0x1002;

inline bool task_mesh_fallback_default(uint32_t vendor, uint32_t device);

/// RDNA 2 parts not in task_mesh_fallback_default's Windows list: Navi 24 (0x743f RX 6400 and 6500 XT; 0x7421 to 0x7424 RX 6300,
/// Pro W6300 to W6500M) and the RDNA 2 APUs: Van Gogh (0x163f, Steam Deck), Rembrandt (0x1681), Mendocino (0x164c), Raphael's
/// (0x164e). On Linux they are RADV's RDNA 2, which hangs on the native mesh path (issue 2245)
inline bool rdna2_extra(uint32_t device) {
    switch (device) {
    case 0x743f: case 0x7421: case 0x7422: case 0x7423: case 0x7424:   // Navi 24
    case 0x163f: case 0x1681: case 0x164c: case 0x164e:                // Van Gogh, Rembrandt, Mendocino, Raphael
        return true;
    default:
        return false;
    }
}

/// RDNA 3.5 APUs (gfx1150 to gfx1152): Strix Point (0x150e: Radeon 880M, 890M), Krackan Point (0x1114: Radeon 840M, 860M)
/// and Strix Halo (0x1586: Radeon 8040S, 8050S, 8060S). On Linux they are RADV's RDNA 3.5, whose native mesh path draws foliage
/// and glows white (an 890M on SteamOS 3.9.5 and a Legion Go 2, issue 81); the compute path draws them correctly
inline bool rdna35_extra(uint32_t device) {
    switch (device) {
    case 0x150e:   // Strix Point: Radeon 880M, 890M
    case 0x1114:   // Krackan Point: Radeon 840M, 860M
    case 0x1586:   // Strix Halo: Radeon 8040S, 8050S, 8060S
        return true;
    default:
        return false;
    }
}

/// (hookshot, issue 2245) RADV's native task/mesh path deadlocks the GPU on RDNA 2: the gfx ring and RADV's gang compute ring end
/// up waiting on each other, and a mesh-only draw hangs with the compute queue empty (devcoredumps on an RX 6800: no VM fault).
/// Task + mesh draws alone on the fallback still crash, so only the full compute path is stable there; the reports span Navi 2x
/// (RX 6800, RX 6600 XT) and Van Gogh (Steam Deck). RDNA 3 and 4 on RADV run the native path without the hang (an RX 9070 XT),
/// except the RDNA 3.5 APUs (rdna35_extra, issue 81), whose native path draws foliage and glows white; RDNA 1 and older have no
/// mesh shaders on RADV. So on Linux, with an AMD GPU (RADV) of the RDNA 2 device IDs (task_mesh_fallback_default's Navi 21 to
/// 23, rdna2_extra) or the RDNA 3.5 APUs (rdna35_extra), Automatic picks Compute; every other AMD ID stays Automatic.
/// RX_VK_MESH_SHADERS=native or compute overrides either way. linux_os: the OS the game runs on (Windows' AMD driver is not RADV)
inline bool automatic_is_compute(bool linux_os, uint32_t vendor_id, uint32_t device_id) {
    return linux_os && vendor_id == kVendorAmd &&
           (task_mesh_fallback_default(vendor_id, device_id) || rdna2_extra(device_id) || rdna35_extra(device_id));
}

/// The mode a device runs: the choice, with Automatic made Compute where automatic_is_compute says so. why: for the log, the
/// choice's own reason, or the rule that made Automatic Compute
inline Choice effective(Choice c, bool linux_os, uint32_t vendor_id, uint32_t device_id) {
    if (c.mode != 0 || !automatic_is_compute(linux_os, vendor_id, device_id)) return c;
    if (rdna35_extra(device_id))
        return {2, "the default on Linux AMD RDNA 3.5 (RADV's mesh shaders draw foliage and glows white, issue 81; RX_VK_MESH_SHADERS=native keeps them)"};
    return {2, "the default on Linux AMD RDNA 2 (RADV's mesh shaders stop the GPU, issue 2245; RX_VK_MESH_SHADERS=native keeps them)"};
}

/// (hookshot, https://github.com/NotRexed/Hookshot/issues/996; Hookshot d1d6caa7, 1396428d, f95d80f5, Longshot ce4deca9) The GPUs
/// whose task + mesh draws take the compute fallback by default, while their mesh-only draws stay native (a distinct mode from
/// Compute, which runs every program as compute); asked only on Windows, where AMD's driver timed out on the native task + mesh
/// path of RDNA 2: Navi 22 first (an RX 6700 XT, whose Market dog drew as a flat pale silhouette: HeyListen issue 57), then
/// Navi 21: on an RX 6800 XT (driver 0x800161) the native path lost the device within minutes at Hyrule Castle's entrance, while
/// the fallback played 25 minutes there with none. Navi 23: an RX 6600 (driver 0x800161) lost the device on the session's first
/// task + mesh draws (reported in Mako by sharkiix, no log published); another RX 6600 on the same driver played 15-20 minutes
/// on the native path and lost it only at the driver's VRAM limit, and the fallback costs VRAM of its own (fallback_buffers_of,
/// vk_draw.cpp). Navi 24 (0x743f: RX 6400, 6500 XT; 0x7421 to 0x7424: RX 6300, Pro W6300 to W6500M) is not on by default: no
/// report, 4 GB cards. RX_VK_TASK_MESH_FALLBACK=1 to try.
/// Which IDs a chip brings: all its IDs that the PCI ID database (pci-ids.ucw.cz) or libdrm's amdgpu.ids name a card for,
/// consumer, mobile and Pro alike, as they share the chip and the driver's mesh path; not its USB or audio functions, nor
/// IDs neither list names a card for (Navi 22's 0x73c3, its SR-IOV function 0x73ce). Untested ones go in with their chip
inline bool task_mesh_fallback_default(uint32_t vendor, uint32_t device) {
    if (vendor != kVendorAmd) return false;
    switch (device) {
    case 0x73a1:   // Navi 21: Radeon Pro V620
    case 0x73a2:   // Navi 21: Radeon Pro W6900X
    case 0x73a3:   // Navi 21: Radeon Pro W6800
    case 0x73ab:   // Navi 21: Radeon Pro W6800X, W6800X Duo
    case 0x73ae:   // Navi 21: Radeon Pro V620 MxGPU
    case 0x73bf:   // Navi 21: RX 6800, 6800 XT, 6900 XT
    case 0x73af:   // Navi 21: RX 6900 XT (XTXH boards)
    case 0x73a5:   // Navi 21: RX 6950 XT
    case 0x73df:   // Navi 22: RX 6700, 6700 XT, 6750 XT, 6750 GRE 12GB, 6700M, 6800M, 6850M XT
    case 0x73e0:   // Navi 23: RX 6600M
    case 0x73e1:   // Navi 23: Radeon Pro W6600M
    case 0x73e3:   // Navi 23: Radeon Pro W6600
    case 0x73ef:   // Navi 23: RX 6650 XT, 6650M, 6650M XT, 6700S, 6800S
    case 0x73ff:   // Navi 23: RX 6600, 6600 XT, 6600 LE, 6600M, 6600S
        return true;
    default:
        return false;
    }
}

/// Whether the draws that have a task stage run as compute on a GPU that has mesh shaders: the default for the device on Windows
/// (task_mesh_fallback_default), off when the choice is Native (RX_VK_MESH_SHADERS=native: the card's own mesh shaders for every
/// draw), and RX_VK_TASK_MESH_FALLBACK, when set, decides over both (0 native, anything else but 0 the fallback). The caller adds
/// that the GPU has mesh shaders and buffer device addresses. With Compute chosen the GPU has no mesh shaders here: the whole
/// compute path, which covers these draws, runs instead
inline bool task_mesh_fallback_wanted(bool windows, uint32_t vendor, uint32_t device, int choice_mode, const char* env) {
    bool on = windows && task_mesh_fallback_default(vendor, device);
    if (choice_mode == 1) on = false;
    if (env) on = std::atoi(env) != 0;
    return on;
}

}  // namespace mesh_rule
