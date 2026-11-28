// Copyright (C) 2026 Sacrappy

/* (HeyListen) The measure behind HOS_TALK_LOG: how long a talk takes to show its text box, as one log line. Plain C with no OS
   call, so that a test compiles it alone (tests/test_talk_log.py); host/hl_talklog.c is the wiring.

   Six events come from lift hooks at function entries (nx2/hl_gates.py HL_TALK_HOOK): the talk request (the player's talk
   sequence starts), the walk-back's start, one of its updates and its end, the message dialog's open request and the text
   thread's handler. A dialog open request with no talk request waiting (a chest's item text) starts the measure itself. The first text box drawn after a request ends the
   measure and the line is made; no box within OOT_TALK_GIVE_UP_MS and the line says so. Times are in ns, any one clock. */
#ifndef OOT_TALK_LOG_H
#define OOT_TALK_LOG_H
#include <stdint.h>
#include <stdio.h>

#define OOT_TALK_GIVE_UP_MS 10000   /* no box this long after the request: one line that says so */
#define OOT_TALK_MIN_LEN 16         /* a box whose text is shorter is a label (a button prompt, a counter), not the message: not the end */
#define OOT_TALK_SLOW_MS 1500       /* a talk slower than this carries its frame count and the map's name */

enum { OOT_TALK_REQUEST = 0, OOT_TALK_WALK_BEGIN = 1, OOT_TALK_WALK_END = 2, OOT_TALK_WALK_UPDATE = 3, OOT_TALK_DIALOG = 4,
       OOT_TALK_THREAD = 5 };

typedef struct {
    int armed;               /* a request (or a dialog open request) is waiting for its box */
    int from_dialog;         /* the measure began at a dialog open request: there was no talk request */
    int64_t request_ns;
    int64_t frames0;         /* the frames presented at the start */
    int64_t dialog_ns;       /* the first dialog open request after a talk request; 0: none yet */
    int64_t thread_ns;       /* the text thread's first run after the start; 0: none yet */
    int64_t walk_begin_ns;   /* 0: no walk-back began since the request */
    int64_t walk_ns;         /* its length once it ended; -1 while it has not */
    long updates;            /* its updates since the request */
} OotTalkLog;

#define OOT_TALK_LOG_INIT { 0, 0, 0, 0, 0, 0, 0, -1, 0 }

static inline void oot_talk_start(OotTalkLog *t, int64_t now, int64_t frames, int from_dialog) {
    t->armed = 1, t->from_dialog = from_dialog, t->request_ns = now, t->frames0 = frames;
    t->dialog_ns = t->thread_ns = t->walk_begin_ns = 0, t->walk_ns = -1, t->updates = 0;
}

/* one hook event; returns 1 when it started a measure (the first request while none is waiting: a second request before the box
   does not move the start; a dialog open request with none waiting starts one of its own) */
static inline int oot_talk_event(OotTalkLog *t, int event, int64_t now, int64_t frames) {
    switch (event) {
    case OOT_TALK_REQUEST:
        if (t->armed && !t->from_dialog) return 0;   /* (one that began at a dialog open request: the talk takes over) */
        oot_talk_start(t, now, frames, 0);
        return 1;
    case OOT_TALK_DIALOG:
        if (!t->armed) { oot_talk_start(t, now, frames, 1); return 1; }
        if (!t->from_dialog) t->dialog_ns = now;   /* (the last one before the box: the first is often a prompt's) */
        break;
    case OOT_TALK_THREAD:
        if (t->armed && !t->thread_ns) t->thread_ns = now;
        break;
    case OOT_TALK_WALK_BEGIN:
        if (t->armed && !t->walk_begin_ns) t->walk_begin_ns = now, t->walk_ns = -1, t->updates = 0;
        break;
    case OOT_TALK_WALK_UPDATE:
        if (t->armed && t->walk_begin_ns && t->walk_ns < 0) t->updates++;
        break;
    case OOT_TALK_WALK_END:
        if (t->armed && t->walk_begin_ns && t->walk_ns < 0) t->walk_ns = now - t->walk_begin_ns;
        break;
    }
    return 0;
}

static inline long long oot_talk_ms(int64_t from, int64_t to) { return (long long)((to - from) / 1000000); }

/* the walk-back as the line tells it: "none", "<ms> ms, <n> updates", or, still going, "<ms> ms so far, <n> updates, not ended" */
static inline int oot_talk_walk_text(const OotTalkLog *t, int64_t now, char *out, size_t cap) {
    if (!t->walk_begin_ns) return snprintf(out, cap, "none");
    if (t->walk_ns < 0) return snprintf(out, cap, "%lld ms so far, %ld updates, not ended", oot_talk_ms(t->walk_begin_ns, now), t->updates);
    return snprintf(out, cap, "%lld ms, %ld updates", (long long)(t->walk_ns / 1000000), t->updates);
}

/* the stages and the walk-back, in the parentheses: "dialog asked at 310 ms, text thread ran at 640 ms; walk-back: none" (a stage
   not reached: "dialog not asked", "text thread not run"; a measure from a dialog open request has no dialog stage) */
static inline int oot_talk_stages(const OotTalkLog *t, int64_t now, char *out, size_t cap) {
    char walk[96], dlg[48] = "", thr[48];
    oot_talk_walk_text(t, now, walk, sizeof walk);
    if (!t->from_dialog) {
        if (t->dialog_ns) snprintf(dlg, sizeof dlg, "dialog asked at %lld ms, ", oot_talk_ms(t->request_ns, t->dialog_ns));
        else snprintf(dlg, sizeof dlg, "dialog not asked, ");
    }
    if (t->thread_ns) snprintf(thr, sizeof thr, "text thread ran at %lld ms", oot_talk_ms(t->request_ns, t->thread_ns));
    else snprintf(thr, sizeof thr, "text thread not run");
    return snprintf(out, cap, "%s%s; walk-back: %s", dlg, thr, walk);
}

static inline int oot_talk_tail(const OotTalkLog *t, int64_t frames, const char *map, char *out, size_t cap, int n) {
    n += snprintf(out + n, cap - (size_t)n, ", %lld frames in between", (long long)(frames - t->frames0));
    if (map && map[0] && (size_t)n < cap) n += snprintf(out + n, cap - (size_t)n, ", map %s", map);
    return n;
}

/* a text box of box_len bytes of text is drawn: when a measure waits and the box is not a short label, ends it, writes the line to `out` and returns 1 (a measure that began at a dialog open request
   only when slow, or when `all`). The frames and the map are told
   after 1500 ms only (map: NULL or empty, left out) */
static inline int oot_talk_box(OotTalkLog *t, int64_t now, int64_t frames, const char *map, char *out, size_t cap, int all, uint32_t box_len) {
    char st[200];
    if (!t->armed || box_len < OOT_TALK_MIN_LEN) return 0;
    t->armed = 0;
    const long long ms = oot_talk_ms(t->request_ns, now);
    if (t->from_dialog && !all && ms <= OOT_TALK_SLOW_MS) return 0;   /* (a dialog with no talk: told when slow, or with HOS_TALK_LOG=2) */
    oot_talk_stages(t, now, st, sizeof st);
    int n = snprintf(out, cap, "talk: text box %lld ms after the %s (%s)", ms, t->from_dialog ? "dialog open request, no talk request" : "talk request", st);
    if (ms > OOT_TALK_SLOW_MS && n > 0 && (size_t)n < cap) oot_talk_tail(t, frames, map, out, cap, n);
    return 1;
}

/* the clock passes: a talk request waiting longer than the give-up time ends here with its line (returns 1), else 0; a measure that
   began at a dialog open request ends silently */
static inline int oot_talk_poll(OotTalkLog *t, int64_t now, int64_t frames, const char *map, char *out, size_t cap) {
    char st[200];
    if (!t->armed || now - t->request_ns < (int64_t)OOT_TALK_GIVE_UP_MS * 1000000) return 0;
    t->armed = 0;
    if (t->from_dialog) return 0;   /* (a dialog that opened with no talk: told only when its box comes) */
    oot_talk_stages(t, now, st, sizeof st);
    int n = snprintf(out, cap, "talk: no text box within %d s of the %s (%s)", OOT_TALK_GIVE_UP_MS / 1000,
                     t->from_dialog ? "dialog open request" : "talk request", st);
    if (n > 0 && (size_t)n < cap) oot_talk_tail(t, frames, map, out, cap, n);
    return 1;
}
#endif
