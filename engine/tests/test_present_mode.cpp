// gpu/vk/present_mode.h: which present mode the swapchain gets from the V-Sync and V-Sync mode rows, VRR, the switches and
// the surface; and the images each choice asks for, with the surface's clamp
#include <cstdio>

#include "../gpu/vk/present_mode.h"

using namespace mkwvk::present_mode;

static int bad;
#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); bad++; } \
    } while (0)

static Mode pick_with(int row, unsigned offered, bool windows, bool fifo = false, bool imm = false, bool vrr = false,
                      int buffering = kFifoTriple) {
    Input in;
    in.row = row, in.buffering = buffering, in.offered = offered, in.windows = windows, in.env_fifo = fifo, in.env_immediate = imm,
    in.vrr = vrr;
    return pick(in);
}
static Images plan_with(int row, int buffering, Mode picked, bool framegen, unsigned min_count = 0, unsigned max_count = 0) {
    Input in;
    in.row = row, in.buffering = buffering;
    return image_plan(in, picked, framegen, min_count, max_count);
}

int main() {
    const unsigned all = kHasMailbox | kHasImmediate;
    // (Off, the default: as it always was)
    EXPECT(pick_with(kOff, all, true) == kMailboxMode);
    EXPECT(pick_with(kOff, all, false) == kMailboxMode);
    EXPECT(pick_with(kOff, kHasMailbox, true) == kMailboxMode);
    EXPECT(pick_with(kOff, kHasImmediate, true) == kImmediateMode);   // AMD on Windows: no mailbox, immediate
    EXPECT(pick_with(kOff, kHasImmediate, false) == kFifo);           // Linux: FIFO
    EXPECT(pick_with(kOff, 0, true) == kFifo);
    // (On: FIFO, or Mailbox when the mode row asks for it and the surface lists it)
    EXPECT(pick_with(kOn, all, true, false, false, false, kFifoDouble) == kFifo);
    EXPECT(pick_with(kOn, all, true, false, false, false, kFifoTriple) == kFifo);
    EXPECT(pick_with(kOn, all, true, false, false, false, kMailbox) == kMailboxMode);
    EXPECT(pick_with(kOn, kHasImmediate, true, false, false, false, kMailbox) == kFifo);   // no Mailbox here: FIFO (triple)
    EXPECT(pick_with(kOn, 0, true, false, false, false, kMailbox) == kFifo);
    // (the switches win over the rows; RX_VK_FIFO over RX_VK_IMMEDIATE)
    EXPECT(pick_with(kOff, all, true, true) == kFifo);
    EXPECT(pick_with(kOn, all, true, true, false, false, kMailbox) == kFifo);
    EXPECT(pick_with(kOff, all, true, false, true) == kImmediateMode);
    EXPECT(pick_with(kOn, all, false, false, true) == kImmediateMode);
    EXPECT(pick_with(kOff, all, true, true, true) == kFifo);
    EXPECT(pick_with(kOff, kHasMailbox, false, false, true) == kMailboxMode);   // asked, not offered
    // (VRR: FIFO whatever the rows say; the switches still win)
    EXPECT(pick_with(kOff, all, true, false, false, true) == kFifo);
    EXPECT(pick_with(kOn, all, true, false, false, true) == kFifo);
    EXPECT(pick_with(kOn, all, true, false, false, true, kMailbox) == kFifo);
    EXPECT(pick_with(kOn, all, true, false, false, true, kFifoDouble) == kFifo);
    EXPECT(pick_with(kOff, all, true, false, true, true) == kImmediateMode);
    // (an unknown row value is Off)
    EXPECT(pick_with(7, all, true) == kMailboxMode);
    // (what the surface can give: Mailbox only where it is listed, Off needs mailbox or immediate; 0 offered: not asked)
    EXPECT(buffering_offered(kFifoDouble, 0) && buffering_offered(kMailbox, 0));   // (surface not asked yet)
    EXPECT(buffering_offered(kMailbox, all) && !buffering_offered(kMailbox, kHasImmediate));
    EXPECT(off_offered(0) && off_offered(kHasMailbox) && off_offered(kHasImmediate));

    // (the images each choice asks for)
    EXPECT(plan_with(kOn, kFifoDouble, kFifo, false).count == 2);    // FIFO (double buffering)
    EXPECT(plan_with(kOn, kFifoTriple, kFifo, false).count == 3);    // FIFO (triple buffering), today's On
    EXPECT(plan_with(kOn, kMailbox, kMailboxMode, false).count == 3);   // Mailbox is triple buffered by nature
    EXPECT(plan_with(kOn, kMailbox, kFifo, false).count == 3);          // (not offered: FIFO (triple buffering))
    EXPECT(plan_with(kOn, kMailbox, kFifo, false).want == 3);
    EXPECT(plan_with(kOff, kFifoTriple, kMailboxMode, false).count == 4);   // (Off, as before: the spare image)
    EXPECT(plan_with(kOff, kFifoTriple, kImmediateMode, false).count == 4);
    EXPECT(plan_with(kOff, kFifoTriple, kFifo, false).count == 3);
    // (frame generation keeps the images it needs: the row cannot lower it)
    Images p = plan_with(kOn, kFifoDouble, kFifo, true);
    EXPECT(p.count == 3 && p.framegen_raised && p.want == 2 && !p.min_raised);
    p = plan_with(kOn, kFifoTriple, kFifo, true);
    EXPECT(p.count == 3 && !p.framegen_raised);
    p = plan_with(kOn, kMailbox, kMailboxMode, true);
    EXPECT(p.count == 4 && p.framegen_raised && p.want == 3);
    // (the surface's minImageCount / maxImageCount clamp it)
    p = plan_with(kOn, kFifoDouble, kFifo, false, 3, 0);
    EXPECT(p.count == 3 && p.min_raised && p.want == 2);       // min 3: FIFO double is not possible
    p = plan_with(kOn, kFifoTriple, kFifo, false, 0, 2);
    EXPECT(p.count == 2 && p.max_capped);
    p = plan_with(kOn, kMailbox, kMailboxMode, true, 0, 3);
    EXPECT(p.count == 3 && p.max_capped && p.framegen_raised);
    p = plan_with(kOff, kFifoTriple, kMailboxMode, false, 5, 0);
    EXPECT(p.count == 5 && p.min_raised);
    p = plan_with(kOn, kFifoDouble, kFifo, true, 2, 4);
    EXPECT(p.count == 3 && p.framegen_raised && !p.min_raised && !p.max_capped);

    // (VRR: Off changes nothing at all)
    {
        VrrInput v;
        v.fullscreen = true, v.refresh_mhz = 144000, v.detected = 1;
        const VrrState s = vrr_resolve(v);
        EXPECT(!s.active && !s.popup && s.cap_below == 0 && s.why == kWhyOff);
        EXPECT(vrr_limit(s, 144000, 240) == 240);
        v.env_forced = true;
        EXPECT(!vrr_resolve(v).active);
    }
    // (On: active in a window too, the popup style only worn in fullscreen; the cap needs the refresh)
    {
        VrrInput v;
        v.row = kVrrOn, v.refresh_mhz = 144000;
        VrrState s = vrr_resolve(v);
        EXPECT(s.active && s.popup && s.cap_below == 3 && s.why == kWhyOn);
        EXPECT(vrr_limit(s, 144000, 240) == 141);   // the refresh - 3 when the limit is above it
        EXPECT(vrr_limit(s, 144000, 144) == 141);
        EXPECT(vrr_limit(s, 144000, 120) == 120);   // the player's own lower limit stays
        EXPECT(vrr_limit(s, 144000, 90) == 90);
        EXPECT(vrr_limit(s, 144000, 60) == 60);     // the console's 60 is not touched
        EXPECT(vrr_limit(s, 59940, 90) == 60);      // a 60 Hz screen: not under 60
        EXPECT(vrr_limit(s, 165000, 165) == 162);
        v.refresh_mhz = 0;                          // unknown: no cap, said in the log
        s = vrr_resolve(v);
        EXPECT(s.active && s.cap_below == 0);
        EXPECT(vrr_limit(s, 0, 240) == 240);
        v.env_forced = true;                        // RX_VK_FIFO / RX_VK_IMMEDIATE: VRR leaves the presents alone
        s = vrr_resolve(v);
        EXPECT(!s.active && !s.popup && s.why == kWhyForcedByEnv);
    }
    // (Auto: only fullscreen and a display that lists a variable range)
    {
        VrrInput v;
        v.row = kVrrAuto, v.refresh_mhz = 144000;
        v.fullscreen = false, v.detected = 1;
        EXPECT(!vrr_resolve(v).active && vrr_resolve(v).why == kWhyAutoWindowed);
        v.fullscreen = true, v.detected = -1;
        EXPECT(!vrr_resolve(v).active && vrr_resolve(v).why == kWhyAutoNotFound);
        v.detected = 0;
        EXPECT(!vrr_resolve(v).active);
        v.detected = 1;
        EXPECT(vrr_resolve(v).active && vrr_resolve(v).why == kWhyAutoDetected);
    }
    // (the EDID's range limits)
    {
        unsigned char e[128] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
        unsigned char* d = e + 72;   // (the second descriptor)
        d[3] = 0xFD, d[5] = 48, d[6] = 144;
        int lo = 0, hi = 0;
        EXPECT(edid_variable(e, 128, &lo, &hi) == 1 && lo == 48 && hi == 144);
        d[5] = 56, d[6] = 61;
        EXPECT(edid_variable(e, 128) == 0);                  // a fixed 60 Hz screen
        d[5] = 50, d[6] = 75;
        EXPECT(edid_variable(e, 128) == 0);
        d[3] = 0xFC;                                         // no range descriptor
        EXPECT(edid_variable(e, 128) == -1);
        EXPECT(edid_variable(e, 64) == -1 && edid_variable(nullptr, 0) == -1);
        d[3] = 0xFD, d[4] = 0x02, d[5] = 48, d[6] = 20;      // (max offset +255: 275 Hz)
        EXPECT(edid_variable(e, 128, &lo, &hi) == 1 && hi == 275);
    }
    if (!bad) std::puts("present_mode: ok");
    return bad ? 1 : 0;
}
