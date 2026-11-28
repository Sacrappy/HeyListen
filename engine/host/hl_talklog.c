// Copyright (C) 2026 Sacrappy

/* (HeyListen) HOS_TALK_LOG: one log line a talk (or a message dialog opened by anything else) that says how long its text box took
   after the request, and which stages it went through: the talk request, the dialog's open request, the text thread's first
   run, and the walk-back Link makes to the sign or person (its time and updates). A tester's log then names the stage that took
   the seconds. Lift hooks at six function entries (nx2/hl_gates.py HL_TALK_HOOK: host calls with nothing but the event's
   number; the walk-back's update runs only during a walk-back); the first text box with a message text (16 bytes or more: the button prompts' boxes are shorter) drawn after the request ends the measure
   (prompts.c, hos_prompts_text_draw); after 10 s without one the line says so (the present path, nn_vi.c). The rule and the
   line are host/oot_talk_log.h. Idle: no request waiting, one load and compare at a box drawn and at a present.
   HOS_TALK_LOG=0 turns it off (on by default), =2 also measures the dialogs opened with no talk request. */
#include "hl_host.h"
#include "oot_talk_log.h"
#include <stdlib.h>
#include <string.h>

/* (two hashes each: the lift as written before native cold helpers, and with them, the default since 27 Nov) */
HL_TALK_HOOK(826df3c0, "1286cd748e7d6405,007a6a55008b1a59", 0);   /* the player's talk sequence starts: the talk request */
HL_TALK_HOOK(826cd180, "b7065d1a83f8a2d8,5cee805aa6e7914e", 1);   /* the walk-back's start */
HL_TALK_HOOK(826cdb90, "3adb5656705a31f8,23ba483a32147dc3", 2);   /* its end */
HL_TALK_HOOK(826cd9c0, "5879af12ca74f55d,dd1a73aec16dccdf", 3);   /* one of its updates */
HL_TALK_HOOK(83adc560, "1e13309df5d62b32,362228d4775c514e", 4);   /* the message dialog's open request */
HL_TALK_HOOK(80e6dfd0, "cc1163ac3cbf34f5,f092a8145d73115e", 5);   /* the text thread's handler */

volatile int hos_talk_armed;   /* a measure waits for its box: what prompts.c and nn_vi.c compare */

int64_t hos_vi_presents(void);
void hos_crash_times(double *since_play, double *since_map, char *map, size_t cap);

static OotTalkLog talk = OOT_TALK_LOG_INIT;
static volatile int talk_lock;

static int talk_level(void) {
    static int on = -1;
    /* HOS_TALK_LOG (default on): 0 turns off the "talk:" lines (how long a talk's text box took after the request, with the stages
       it went through); 2 also measures the message dialogs that open with no talk request (not by default: the open request
       fires for many things that are not messages). */
    if (on < 0) on = getenv("HOS_TALK_LOG") && *getenv("HOS_TALK_LOG") ? atoi(getenv("HOS_TALK_LOG")) : 1;
    return on;
}
static void lock(void) { while (__atomic_exchange_n(&talk_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&talk_lock, 0, __ATOMIC_RELEASE); }

static void told(int made, const char *line) {
    if (made) hos_log("%s", line);
}

/* the lift hook's call: event 0..5 (oot_talk_log.h) */
void hos_oot_talk_event(int event) {
    if (!talk_level()) return;
    if (event == OOT_TALK_DIALOG && !hos_talk_armed) {   /* (a dialog open request with no talk request waiting) */
        /* (the open request fires for many things that are not messages, and the next message box, seconds later, would end such
           a measure with a time that is nobody's wait: scripted reads 7 s apart logged "6-8 s" lines. Measured only with
           HOS_TALK_LOG=2, and never before play begins: the loading's own) */
        if (talk_level() < 2) return;
        double play, since_map;
        hos_crash_times(&play, &since_map, NULL, 0);
        if (play < 0) return;
    }
    lock();
    oot_talk_event(&talk, event, hos_now_ns(), hos_vi_presents());
    __atomic_store_n(&hos_talk_armed, talk.armed, __ATOMIC_RELAXED);
    unlock();
}

static void map_name(char *map, size_t cap) {
    double play, since_map;
    hos_crash_times(&play, &since_map, map, cap);
}

/* a text box is drawn (prompts.c, only when hos_talk_armed) */
void hos_prompts_box_desc(uint64_t box, uint32_t *len, int *codes, uint64_t *vtable);

void hos_talk_box_drawn(uint64_t box) {
    char line[400], map[100];
    uint32_t len;
    int codes;
    uint64_t vtable;
    hos_prompts_box_desc(box, &len, &codes, &vtable);
    map_name(map, sizeof map);
    lock();
    const int made = oot_talk_box(&talk, hos_now_ns(), hos_vi_presents(), map, line, sizeof line, talk_level() > 1, len);
    __atomic_store_n(&hos_talk_armed, talk.armed, __ATOMIC_RELAXED);
    unlock();
    told(made, line);
}

/* a frame is presented (nn_vi.c, only when hos_talk_armed) */
void hos_talk_poll(void) {
    char line[400], map[100];
    map_name(map, sizeof map);
    lock();
    const int made = oot_talk_poll(&talk, hos_now_ns(), hos_vi_presents(), map, line, sizeof line);
    __atomic_store_n(&hos_talk_armed, talk.armed, __ATOMIC_RELAXED);
    unlock();
    told(made, line);
}
