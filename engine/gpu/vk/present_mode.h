/* Which present mode the swapchain uses (vk_main.cpp create_swapchain): the Insert menu's V-Sync and V-Sync mode rows, the
   VRR row, RX_VK_FIFO and RX_VK_IMMEDIATE, and the modes the surface offers. Plain ints, no Vulkan header:
   tests/test_present_mode.cpp compiles it. */
#pragma once

namespace mkwvk::present_mode {

/// The V-Sync row's values (opts.vsync, saved as vsync). Off is what the game always did: mailbox where offered
enum Row { kOff = 0, kOn = 1 };
/// The V-Sync mode row's values (opts.vsync_mode, saved as vsync_mode), with the V-Sync row On. There is no Mailbox double:
/// Mailbox needs a spare image, so it is triple buffered by nature (with two it would behave like FIFO)
enum Buffering { kFifoDouble = 0, kFifoTriple = 1, kMailbox = 2 };
/// What is used
enum Mode { kFifo = 0, kMailboxMode = 1, kImmediateMode = 2 };
/// The modes a surface offers, as bits (FIFO is always there: the spec makes it mandatory)
enum Offered : unsigned { kHasMailbox = 1u, kHasImmediate = 2u };

struct Input {
    int row = kOff;              // opts.vsync
    int buffering = kFifoTriple; // opts.vsync_mode (only read with the row On)
    bool vrr = false;            // the VRR row resolved to On (vrr_resolve below): FIFO, whatever the V-Sync row says
    bool env_fifo = false;       // RX_VK_FIFO set
    bool env_immediate = false;  // RX_VK_IMMEDIATE set
    unsigned offered = 0;        // Offered bits
    bool windows = false;
};

/// The mode to create the swapchain with. RX_VK_FIFO wins over everything, then RX_VK_IMMEDIATE, then VRR (FIFO, whatever
/// the rows say), then the rows. Off is what it always was: mailbox when offered; with none, immediate on Windows (AMD has
/// no mailbox, and FIFO held anything under 60 fps at 30), FIFO elsewhere. On is FIFO, or Mailbox when the V-Sync mode row
/// asks for it and the surface lists it (else FIFO: the caller logs the fallback).
inline Mode pick(const Input& in) {
    if (in.env_fifo) return kFifo;
    const bool imm = (in.offered & kHasImmediate) != 0;
    if (in.env_immediate && imm) return kImmediateMode;
    if (in.vrr) return kFifo;
    if (in.row == kOn) return in.buffering == kMailbox && (in.offered & kHasMailbox) ? kMailboxMode : kFifo;
    if (in.offered & kHasMailbox) return kMailboxMode;
    return in.windows && imm ? kImmediateMode : kFifo;
}

/// A mode's name for the log and the overlay
inline const char* mode_name(Mode m) {
    return m == kMailboxMode ? "mailbox" : m == kImmediateMode ? "immediate" : "fifo";
}

/// The V-Sync mode row's value in words for the log
inline const char* buffering_name(int b) {
    return b == kFifoDouble ? "FIFO (double buffering)" : b == kMailbox ? "Mailbox (triple buffering)" : "FIFO (triple buffering)";
}

/// Whether a surface that offers `offered` can give the V-Sync mode row's value (0 offered: not asked yet)
inline bool buffering_offered(int b, unsigned offered) {
    return !offered || b != kMailbox || (offered & kHasMailbox) != 0;
}
/// Whether the V-Sync row's Off can be had here as it says: mailbox or immediate (else FIFO)
inline bool off_offered(unsigned offered) {
    return !offered || (offered & (kHasMailbox | kHasImmediate)) != 0;
}

// ---- the swapchain's images ----

/// What the V-Sync mode row comes to in swapchain images: the row's wish, backed up for frame generation or the surface's
/// minimum, held down to the surface's maximum
struct Images {
    int want = 0;                  // the row's wish
    int count = 0;                 // what to ask the driver for
    bool framegen_raised = false;  // frame generation needs more than the row asks (FIFO 3, Mailbox 4)
    bool min_raised = false;       // the surface asks for at least that many
    bool max_capped = false;       // the surface allows at most that many
};

/// The row's wish, before frame generation and the surface have their say: FIFO double 2, FIFO triple 3, Mailbox 3. Off is
/// today's: 4 with mailbox or immediate (the spare image frame interpolation wants), 3 under FIFO ("three keep one free")
inline int image_want(int row, int buffering, Mode picked) {
    if (row == kOn) {
        if (picked == kMailboxMode) return 3;
        if (picked == kImmediateMode) return 4;   // (RX_VK_IMMEDIATE: the row On's count, the mode alone changes)
        return buffering == kFifoDouble ? 2 : 3;
    }
    return picked == kFifo ? 3 : 4;
}

/// The image count to create the swapchain with. Frame generation keeps what it needs whatever the row says (a spare image
/// always free: 4 with Mailbox, 3 under FIFO), then the surface's minImageCount/maxImageCount clamp it. min_count 0: the
/// surface was not asked (a test); max_count 0: no maximum
inline Images image_plan(const Input& in, Mode picked, bool framegen, unsigned min_count, unsigned max_count) {
    Images im;
    im.want = image_want(in.row, in.buffering, picked);
    int n = im.want;
    const int floor = framegen ? (picked == kFifo ? 3 : 4) : 0;
    if (n < floor) im.framegen_raised = true, n = floor;
    if (min_count && n < (int)min_count) im.min_raised = true, n = (int)min_count;
    if (max_count && n > (int)max_count) im.max_capped = true, n = (int)max_count;
    im.count = n;
    return im;
}

// ---- VRR (G-Sync / FreeSync) ----

/// The VRR row's values (opts.vrr, saved as vrr). Off is the default: nothing changes for a player who leaves it
enum VrrRow { kVrrOff = 0, kVrrOn = 1, kVrrAuto = 2 };
/// Why the VRR row resolved as it did (logged once per change)
enum VrrWhy {
    kWhyOff,            // the row is Off
    kWhyOn,             // the row is On
    kWhyAutoDetected,   // Auto, and the display's EDID lists a refresh range wide enough to be variable
    kWhyAutoNotFound,   // Auto, and no such range (or it could not be read): off
    kWhyAutoWindowed,   // Auto and not fullscreen: off (the window cannot flip)
    kWhyForcedByEnv,    // RX_VK_FIFO / RX_VK_IMMEDIATE decide the mode: VRR leaves the presents alone
};

struct VrrInput {
    int row = kVrrOff;
    bool fullscreen = false;
    int detected = -1;           // -1 unknown, 0 not a variable-refresh display, 1 is
    int refresh_mhz = 0;         // the display's current refresh in mHz, 0 unknown
    bool env_forced = false;     // RX_VK_FIFO or RX_VK_IMMEDIATE set
};
struct VrrState {
    bool active = false;         // FIFO and the cap are used
    bool popup = false;          // the fullscreen window is a flip-eligible WS_POPUP (display.c)
    int cap_below = 0;           // frames a second under the refresh the rate is held at; 0: no cap (refresh unknown)
    VrrWhy why = kWhyOff;
};

/// What the frame rate limit is held to under VRR: NVIDIA's advice is a few fps under the refresh so the game never hits the
/// refresh and falls into V-Sync's wait
constexpr int kVrrCapBelow = 3;

inline VrrState vrr_resolve(const VrrInput& in) {
    VrrState s;
    if (in.row == kVrrOff) return s;
    if (in.env_forced) return s.why = kWhyForcedByEnv, s;
    if (in.row == kVrrAuto) {
        if (!in.fullscreen) return s.why = kWhyAutoWindowed, s;
        if (in.detected != 1) return s.why = kWhyAutoNotFound, s;
        s.why = kWhyAutoDetected;
    } else {
        s.why = kWhyOn;
    }
    s.active = true;
    s.popup = true;   // (only worn by the window while it is fullscreen)
    s.cap_below = in.refresh_mhz > 0 ? kVrrCapBelow : 0;
    return s;
}

/// The frame rate limit under VRR: the player's own limit when it is lower than the refresh - 3, else that. 0 for no cap
/// (inactive, or the refresh unknown). Only a limit above 60 is touched: the console's 60 is left as it is
inline int vrr_limit(const VrrState& s, int refresh_mhz, int limit) {
    if (!s.active || s.cap_below <= 0 || refresh_mhz <= 0 || limit <= 60) return limit;
    const int cap = (refresh_mhz + 500) / 1000 - s.cap_below;
    return cap < 60 ? 60 : (cap < limit ? cap : limit);
}

/// A monitor's refresh range from its EDID (the 0xFD display range limits descriptor): wide enough to be variable? 1 yes,
/// 0 no, -1 no range descriptor or no EDID. A fixed-rate monitor lists a narrow range (56-61 Hz, 50-75 Hz); FreeSync and G-Sync
/// Compatible ones list 48-144 and the like. A hint only: the display's own menu and the driver have the last word
inline int edid_variable(const unsigned char* edid, int size, int* min_hz = nullptr, int* max_hz = nullptr) {
    if (!edid || size < 128 || edid[0] != 0 || edid[1] != 0xFF || edid[7] != 0) return -1;
    for (int off = 54; off <= 108; off += 18) {
        const unsigned char* d = edid + off;
        if (d[0] || d[1] || d[2] || d[3] != 0xFD) continue;
        int lo = d[5], hi = d[6];
        if (d[4] & 0x02) hi += 255;   // (offsets: the vertical range limits over 255 Hz)
        if (d[4] & 0x08) lo += 255;
        if (min_hz) *min_hz = lo;
        if (max_hz) *max_hz = hi;
        return hi - lo >= 30 && hi >= 75 ? 1 : 0;
    }
    return -1;
}

} // namespace mkwvk::present_mode
