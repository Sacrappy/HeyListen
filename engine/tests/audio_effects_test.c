/* Authored parameters and samples around the actual effect code. No audio device or game is opened. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
static _Alignas(8) uint8_t guest[4096];
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static void *guest_ptr(uint64_t address) { CHECK(address < sizeof guest); return guest + address; }
#define GP(a) guest_ptr(a)
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HLE(sym) void imp_##sym(Ctx *c)
#define AUT(...) do { if (0) printf(__VA_ARGS__); } while (0)   /* (arguments used, nothing printed) */

static int lock_depth;
static void au_lock(void) { CHECK(lock_depth++ == 0); }
static void au_unlock(void) { CHECK(--lock_depth == 0); }
static const char *fixture_getenv(const char *name) { (void)name; return NULL; }
#define getenv fixture_getenv

#include "actual_audio_effects.inc"

enum { CONFIG = 0x10, EFFECT = 0x40, EFFECT2 = 0x48, MIX0 = 0x80, MIX1 = 0x88, SPLITTER = 0xa0, VOICE = 0xc0,
       PACKET = 0x200, IN = 0x300, OUT = 0x310 };
#define ADD_REVERB imp__ZN2nn5audio9AddReverbEPNS0_19AudioRendererConfigEPNS0_10ReverbTypeEPvmPNS0_10SubMixTypeEi
#define ADD_DELAY imp__ZN2nn5audio8AddDelayEPNS0_19AudioRendererConfigEPNS0_9DelayTypeEPvmPNS0_10SubMixTypeENS_8TimeSpanEi
#define ADD_BIQUAD imp__ZN2nn5audio15AddBiquadFilterEPNS0_19AudioRendererConfigEPNS0_16BiquadFilterTypeEPNS0_10SubMixTypeE

static Ctx call(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[3] = a3; c.x[4] = a4; c.x[5] = a5; c.x[6] = a6;
    return c;
}
static Ctx with_float(uint64_t obj, float f) {
    Ctx c = call(obj, 0, 0, 0, 0, 0, 0);
    memcpy(&c.vreg[0][0], &f, 4);
    return c;
}
static void reset(void) {
    memset(guest, 0, sizeof guest);
    memset(mixes, 0, sizeof mixes);
    memset(reverbs, 0, sizeof reverbs);
    for (int e = 0; e < MAX_EFFECTS; e++) free(delays[e].line);
    memset(delays, 0, sizeof delays);
    memset(mix_biquads, 0, sizeof mix_biquads);
    memset(voices, 0, sizeof voices);
    memset(splitters, 0, sizeof splitters);
    ncmds = 0;
    for (int m = 0; m < 2; m++) mixes[m].used = 1, mixes[m].buffers = 2, mixes[m].volume = 1;
    put_handle(MIX0, K_MIX, 0);
    put_handle(MIX1, K_MIX, 1);
}
static void clear_mix(int m) { memset(mixes[m].buf, 0, sizeof mixes[m].buf); }
static void finite_mix(int m) {
    for (int b = 0; b < MAX_BUF; b++)
        for (int i = 0; i < FRAME; i++) CHECK(isfinite(mixes[m].buf[b][i]));
}
static double energy(int m) {
    double e = 0;
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < FRAME; i++) e += (double)mixes[m].buf[b][i] * mixes[m].buf[b][i];
    return e;
}

static void standard_reverb(void) {
    reset();
    Ctx c = call(CONFIG, EFFECT, 0, 0, MIX0, 2, 0);
    ADD_REVERB(&c);
    CHECK(c.x[0] == 0 && reverbs[0].used && reverbs[0].standard && reverbs[0].mix == 0 && reverbs[0].n == 2);
    c = call(EFFECT, 1, 0, 0, 0, 0, 0);
    imp__ZN2nn5audio16SetReverbEnabledEPNS0_10ReverbTypeEb(&c);
    c = call(EFFECT, 0, 0, 0, 0, 0, 0);
    imp__ZN2nn5audio15IsReverbEnabledEPKNS0_10ReverbTypeE(&c);
    CHECK(c.x[0] == 1);
    c = with_float(EFFECT, 0.8f);
    imp__ZN2nn5audio16SetReverbOutGainEPNS0_10ReverbTypeEf(&c);
    c = call(EFFECT, 0, 0, 0, 0, 0, 0);
    imp__ZN2nn5audio16GetReverbOutGainEPKNS0_10ReverbTypeE(&c);
    float got;
    memcpy(&got, &c.vreg[0][0], 4);
    CHECK(got == 0.8f);
    c = call(EFFECT, 2000000000ull, 0, 0, 0, 0, 0);   /* 2 s */
    imp__ZN2nn5audio18SetReverbDecayTimeEPNS0_10ReverbTypeENS_8TimeSpanE(&c);
    CHECK(reverbs[0].decay_ns == 2000000000ll);
    c = call(EFFECT, 3, 0, 0, 0, 0, 0);
    imp__ZN2nn5audio18SetReverbEarlyModeEPNS0_10ReverbTypeENS1_9EarlyModeE(&c);
    CHECK(reverbs[0].early_mode == 3);
    /* an impulse, then silence: a tail that lasts, then dies away */
    clear_mix(0);
    mixes[0].buf[0][0] = 1.0f;
    effects_frame(0);
    double early = 0, late = 0;
    for (int f = 1; f < 600; f++) {
        clear_mix(0);
        effects_frame(0);
        finite_mix(0);
        if (f >= 5 && f < 60) early += energy(0);
        if (f >= 540) late += energy(0);
    }
    CHECK(early > 1e-4);
    CHECK(late < early * 1e-3);
    /* dry only with no early reflections and no late gain: the input as it is, times the dry gain */
    reset();
    c = call(CONFIG, EFFECT, 0, 0, MIX0, 2, 0);
    ADD_REVERB(&c);
    reverbs[0].enabled = 1; reverbs[0].early_mode = 4; reverbs[0].late_gain = 0; reverbs[0].dry = 0.5f;
    for (int i = 0; i < FRAME; i++) mixes[0].buf[0][i] = (float)(i % 7) * 0.1f, mixes[0].buf[1][i] = -0.25f;
    effects_frame(0);
    for (int i = 0; i < FRAME; i++) {
        CHECK(fabsf(mixes[0].buf[0][i] - 0.05f * (float)(i % 7)) < 1e-6f);
        CHECK(fabsf(mixes[0].buf[1][i] + 0.125f) < 1e-6f);
    }
    /* the I3DL2 type cannot reach a standard reverb's setters, and a disabled one leaves the mix alone */
    reverbs[0].enabled = 0;
    mixes[0].buf[0][3] = 9;
    effects_frame(0);
    CHECK(mixes[0].buf[0][3] == 9);
}

static void delay(void) {
    reset();
    Ctx c = call(CONFIG, EFFECT, 0, 0, MIX0, 20000000, 2);   /* at most 20 ms, stereo */
    ADD_DELAY(&c);
    CHECK(c.x[0] == 0 && delays[0].used && delays[0].n == 2 && delays[0].max == 961);
    Delay *d = &delays[0];
    c = call(EFFECT, 10000000, 0, 0, 0, 0, 0);   /* 10 ms = 480 samples */
    imp__ZN2nn5audio12SetDelayTimeEPNS0_9DelayTypeENS_8TimeSpanE(&c);
    c = call(EFFECT, 0, 0, 0, 0, 0, 0);
    imp__ZN2nn5audio12GetDelayTimeEPKNS0_9DelayTypeE(&c);
    CHECK(c.x[0] == 10000000);
    c = with_float(EFFECT, 1.0f);  imp__ZN2nn5audio14SetDelayInGainEPNS0_9DelayTypeEf(&c);
    c = with_float(EFFECT, 0.5f);  imp__ZN2nn5audio20SetDelayFeedbackGainEPNS0_9DelayTypeEf(&c);
    c = with_float(EFFECT, 0.0f);  imp__ZN2nn5audio15SetDelayDryGainEPNS0_9DelayTypeEf(&c);
    c = with_float(EFFECT, 1.0f);  imp__ZN2nn5audio21SetDelayChannelSpreadEPNS0_9DelayTypeEf(&c);
    c = with_float(EFFECT, 0.0f);  imp__ZN2nn5audio21SetDelayLowPassAmountEPNS0_9DelayTypeEf(&c);
    CHECK(d->in_gain == 1 && d->feedback == 0.5f && d->dry == 0 && d->spread == 1 && d->lowpass == 0);
    /* off: the mix is left alone */
    mixes[0].buf[0][0] = 1.0f;
    effects_frame(0);
    CHECK(mixes[0].buf[0][0] == 1.0f);
    c = call(EFFECT, 1, 0, 0, 0, 0, 0);
    imp__ZN2nn5audio15SetDelayEnabledEPNS0_9DelayTypeEb(&c);
    /* an impulse on the left: the echo 480 samples on, on the left; the next one 480 after that, on the right, at
       half (the feedback crossed over by the full spread) */
    float l[FRAME * 5], r[FRAME * 5];
    for (int f = 0; f < 5; f++) {
        clear_mix(0);
        if (f == 0) mixes[0].buf[0][0] = 1.0f;
        effects_frame(0);
        memcpy(l + f * FRAME, mixes[0].buf[0], sizeof(float) * FRAME);
        memcpy(r + f * FRAME, mixes[0].buf[1], sizeof(float) * FRAME);
    }
    for (int i = 0; i < FRAME * 5; i++) {
        CHECK(fabsf(l[i] - (i == 480 ? 1.0f : 0.0f)) < 1e-6f);
        CHECK(fabsf(r[i] - (i == 960 ? 0.5f : 0.0f)) < 1e-6f);
    }
    /* swapped channels through SetDelayInputOutput, read back */
    const int8_t in[2] = {1, 0}, out[2] = {1, 0};
    memcpy(GP(IN), in, 2);
    memcpy(GP(OUT), out, 2);
    c = call(EFFECT, IN, OUT, 2, 0, 0, 0);
    imp__ZN2nn5audio19SetDelayInputOutputEPNS0_9DelayTypeEPKaS4_i(&c);
    CHECK(d->n == 2 && d->in[0] == 1 && d->out[1] == 0);
    memset(GP(IN), 0x55, 4);
    c = call(EFFECT, IN, OUT, 0x400, 8, 0, 0);
    imp__ZN2nn5audio19GetDelayInputOutputEPKNS0_9DelayTypeEPaS4_Pii(&c);
    CHECK(G32(0x400) == 2 && ((int8_t *)GP(IN))[0] == 1 && ((int8_t *)GP(IN))[1] == 0);
    /* feedback above 1 or a NaN in the input stays finite */
    d->feedback = 1.5f;
    for (int f = 0; f < 400; f++) {
        clear_mix(0);
        mixes[0].buf[1][5] = f == 0 ? NAN : 0.5f;
        effects_frame(0);
        for (int b = 0; b < 2; b++)
            for (int i = 0; i < FRAME; i++) CHECK(!isnan(mixes[0].buf[b][i]));
    }
}

static void biquad_packet(int on, float b0, float b1, float b2, float a1, float a2) {
    uint8_t packet[24];
    memset(packet, 0xa5, sizeof packet);
    packet[0] = (uint8_t)on;
    const float v[] = {b0, b1, b2, a1, a2};
    memcpy(packet + 4, v, sizeof v);
    memcpy(GP(PACKET), packet, sizeof packet);
}

static void mix_biquad_and_order(void) {
    reset();
    Ctx c = call(CONFIG, EFFECT, MIX0, 0, 0, 0, 0);
    ADD_BIQUAD(&c);
    CHECK(c.x[0] == 0 && mix_biquads[0].used && mix_biquads[0].n == 2);
    biquad_packet(1, 0.5f, 0, 0, 0.5f, 0);   /* one-pole low pass, unity at DC */
    c = call(EFFECT, PACKET, 0, 0, 0, 0, 0);
    imp__ZN2nn5audio24SetBiquadFilterParameterEPNS0_16BiquadFilterTypeERKNS0_22TBiquadFilterParameterIfEE(&c);
    for (int i = 0; i < FRAME; i++) mixes[0].buf[0][i] = 1.0f;
    effects_frame(0);
    CHECK(mixes[0].buf[0][0] == 1.0f);   /* not enabled yet */
    c = call(EFFECT, 1, 0, 0, 0, 0, 0);
    imp__ZN2nn5audio22SetBiquadFilterEnabledEPNS0_16BiquadFilterTypeEb(&c);
    effects_frame(0);
    CHECK(fabsf(mixes[0].buf[0][0] - 0.5f) < 1e-6f && fabsf(mixes[0].buf[0][1] - 0.75f) < 1e-6f);
    CHECK(fabsf(mixes[0].buf[0][FRAME - 1] - 1.0f) < 1e-5f);
    /* a delay added after it runs after it: the filtered signal is what the line holds */
    c = call(CONFIG, EFFECT2, 0, 0, MIX0, 20000000, 2);
    ADD_DELAY(&c);
    CHECK(delays[0].seq > mix_biquads[0].seq);
    reset();   /* fresh states, the same order */
    c = call(CONFIG, EFFECT, MIX0, 0, 0, 0, 0);
    ADD_BIQUAD(&c);
    c = call(CONFIG, EFFECT2, 0, 0, MIX0, 20000000, 2);
    ADD_DELAY(&c);
    biquad_packet(1, 0.5f, 0, 0, 0.5f, 0);
    c = call(EFFECT, PACKET, 0, 0, 0, 0, 0);
    imp__ZN2nn5audio24SetBiquadFilterParameterEPNS0_16BiquadFilterTypeERKNS0_22TBiquadFilterParameterIfEE(&c);
    mix_biquads[0].enabled = 1;
    delays[0].enabled = 1; delays[0].time_ns = 10 * 1000000000ll / RATE + 1; delays[0].in_gain = 1; delays[0].dry = 1;
    delays[0].lowpass = 0;
    clear_mix(0);
    mixes[0].buf[0][0] = 1.0f;
    effects_frame(0);
    /* filtered impulse 0.5, 0.25, ...; then the same 10 samples later added */
    CHECK(fabsf(mixes[0].buf[0][0] - 0.5f) < 1e-6f);
    CHECK(fabsf(mixes[0].buf[0][10] - (0.5f * powf(0.5f, 10) + 0.5f)) < 1e-6f);
}

static void splitter_biquad(void) {
    reset();
    Splitter *s = &splitters[0];
    s->used = 1; s->dests = 2; s->dest_mix[0] = 0; s->dest_mix[1] = 1;
    s->vol[0][0][0] = s->ramp[0][0][0] = 1.0f;
    s->vol[1][0][0] = s->ramp[1][0][0] = 1.0f;
    put_handle(SPLITTER, K_SPLITTER, 0);
    Voice *v = &voices[0];
    v->used = 1; v->state = PLAY_STARTED; v->channels = 1; v->rate = RATE; v->pitch = 1; v->volume = 1;
    v->ramp_volume = 1; v->dest_kind = DEST_SPLITTER; v->dest = 0;
    fixture_src[0] = 1.0f;
    biquad_packet(1, 0.5f, 0, 0, 0.5f, 0);
    Ctx c = call(SPLITTER, 0, MIX0, 1, PACKET, 0, 0);   /* destination 0, filter 1 */
    set_split_biquad(&c);   /* (both imports call it) */
    CHECK(ncmds == 1 && !s->bq_on[0][1]);   /* queued until the update */
    memset(GP(PACKET), 0, 24);   /* (the game may reuse its packet at once) */
    commit();
    CHECK(s->bq_on[0][1] && s->bq_b[0][1][0] == 0.5f && s->bq_a[0][1][0] == 0.5f);
    render_voice(v);
    /* (the resampler gives the source one sample late) */
    CHECK(mixes[0].buf[0][0] == 0.0f && fabsf(mixes[0].buf[0][1] - 0.5f) < 1e-6f && fabsf(mixes[0].buf[0][2] - 0.75f) < 1e-6f);
    CHECK(mixes[1].buf[0][1] == 1.0f && mixes[1].buf[0][2] == 1.0f);   /* the other destination unfiltered */
    /* its state is the voice's, carried to the next frame */
    clear_mix(0);
    render_voice(v);
    CHECK(fabsf(mixes[0].buf[0][0] - 1.0f) < 1e-5f);
    /* off again */
    biquad_packet(0, 0.5f, 0, 0, 0.5f, 0);
    c = call(SPLITTER, 0, MIX0, 1, PACKET, 0, 0);
    set_split_biquad(&c);   /* (both imports call it) */
    commit();
    CHECK(!s->bq_on[0][1]);
    /* out of range filter or destination: nothing queued */
    c = call(SPLITTER, 0, MIX0, 2, PACKET, 0, 0);
    set_split_biquad(&c);   /* (both imports call it) */
    c = call(SPLITTER, MAX_SPLIT_DESTS, MIX0, 0, PACKET, 0, 0);
    set_split_biquad(&c);   /* (both imports call it) */
    CHECK(ncmds == 0);
}

static void refused(void) {
    reset();
    Ctx c = call(CONFIG, EFFECT, 0, 0, 0x90 /* no mix */, 2, 0);
    ADD_REVERB(&c);
    CHECK(c.x[0] != 0 && !reverbs[0].used);
    for (int e = 0; e < MAX_EFFECTS; e++) {
        c = call(CONFIG, EFFECT, MIX0, 0, 0, 0, 0);
        ADD_BIQUAD(&c);
        CHECK(c.x[0] == 0);
    }
    c = call(CONFIG, EFFECT2, MIX0, 0, 0, 0, 0);
    ADD_BIQUAD(&c);
    CHECK(c.x[0] != 0 && G64(EFFECT2) == 0);
}

int main(void) {
    standard_reverb();
    delay();
    mix_biquad_and_order();
    splitter_biquad();
    refused();
    puts("audio effects: ok");
    return 0;
}
