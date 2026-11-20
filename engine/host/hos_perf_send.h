/* SPDX-License-Identifier: GPL-3.0-or-later */
/* (heylisten) The performance summaries' sender, the parts that decide (host/hos_crash_dialog.c hos_perf_send_run does the
   file and curl work): plain C with no system calls, for tests/test_perf_send.py.

   The renderer (gpu/vk/perf_report.cpp) writes a summary into the game folder's perf_pending/ every few minutes of play and at
   exit, only while "Send performance statistics" is on and the build has an address. The game sends what is waiting at its
   next start, oldest first, one summary a connection of its own (the system's curl), and deletes each one the server took or
   refused. Polite by default: at most HOS_PERF_MAX_SENDS a start, and a server that answers busy (429 or 5xx) is left alone
   until the time it names (Retry-After), else an hour, doubled each time it is busy again up to a day (perf_pending/.next).
   Same rules as nx2/perf_report.py of Longshot, the sender this one is ported from. */
#pragma once
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOS_PERF_PENDING "perf_pending"
#define HOS_PERF_SENT "perf_sent"
#define HOS_PERF_NEXT ".next"
enum {
    HOS_PERF_MAX_SENDS = 12,       /* a start */
    HOS_PERF_MAX_PENDING = 48,     /* waiting at most (about four hours of play): the oldest go unsent */
    HOS_PERF_MAX_BODY = 60 * 1024, /* the server's limit is 64 KiB: a bigger one is not sent */
    HOS_PERF_TIMEOUT = 10,         /* seconds a request */
    HOS_PERF_BUSY_FIRST = 3600,    /* a busy server's first wait without a Retry-After, seconds */
    HOS_PERF_BUSY_MOST = 86400
};
enum { HOS_PERF_SENT_OK, HOS_PERF_REFUSED, HOS_PERF_BUSY };

/* The pipeline state records (gpu/vk/state_report.h) go the same way from their own folders: one file a session at most
   1.5 MiB (the server's limit must be over it), 4 waiting, 2 a start, the last 3 sent kept for the player to read */
#define HOS_PIPES_PENDING "pipelines_pending"
#define HOS_PIPES_SENT "pipelines_sent"
enum { HOS_PIPES_MAX_PENDING = 4, HOS_PIPES_MAX_SENDS = 2, HOS_PIPES_KEEP_SENT = 3, HOS_PIPES_MAX_BODY = 1536 * 1024 };

/* The per-zone shader and pipeline usage (gpu/vk/zone_report.h) the same way: batches of a few hundred KB, 4 waiting,
   2 a start, the last 3 sent kept for the player to read */
#define HOS_ZONES_PENDING "zones_pending"
#define HOS_ZONES_SENT "zones_sent"
enum { HOS_ZONES_MAX_PENDING = 4, HOS_ZONES_MAX_SENDS = 2, HOS_ZONES_KEEP_SENT = 3, HOS_ZONES_MAX_BODY = 512 * 1024 };

/* The detailed performance profile (gpu/vk/profile_report.h) the same way: batches of at most 256 KiB, 8 waiting, 2 a start,
   the last 3 sent kept for the player to read */
#define HOS_PROFILE_PENDING "profile_pending"
#define HOS_PROFILE_SENT "profile_sent"
enum { HOS_PROFILE_MAX_PENDING = 8, HOS_PROFILE_MAX_SENDS = 2, HOS_PROFILE_KEEP_SENT = 3, HOS_PROFILE_MAX_BODY = 256 * 1024 };

/* A summary's name, "<unix time>-<n>.json": 1 with its time and n, 0 for any other name */
static inline int hos_perf_name(const char *name, int64_t *at, int64_t *n) {
    const char *p = name;
    int64_t a = 0, b = 0;
    size_t digits = 0;
    for (; *p >= '0' && *p <= '9' && digits < 18; p++, digits++) a = a * 10 + (*p - '0');
    if (!digits || *p++ != '-') return 0;
    digits = 0;
    for (; *p >= '0' && *p <= '9' && digits < 9; p++, digits++) b = b * 10 + (*p - '0');
    if (!digits || strcmp(p, ".json")) return 0;
    if (at) *at = a;
    if (n) *n = b;
    return 1;
}
/* Whether name sorts before other, oldest first (unix time, then n) */
static inline int hos_perf_older(const char *name, const char *other) {
    int64_t a1 = 0, n1 = 0, a2 = 0, n2 = 0;
    hos_perf_name(name, &a1, &n1);
    hos_perf_name(other, &a2, &n2);
    return a1 != a2 ? a1 < a2 : n1 < n2;
}
/* What the server's status means: sent (2xx), busy (429, 5xx), refused (any other: never better next time) */
static inline int hos_perf_answer(int status) {
    if (status >= 200 && status < 300) return HOS_PERF_SENT_OK;
    if (status == 429 || status >= 500) return HOS_PERF_BUSY;
    return HOS_PERF_REFUSED;
}
/* A Retry-After header's seconds when it is a whole number (an HTTP date is not read: -1, and the doubling wait applies) */
static inline long hos_perf_retry_after(const char *value) {
    while (value && (*value == ' ' || *value == '\t')) value++;
    if (!value || !isdigit((unsigned char)*value)) return -1;
    long n = 0;
    for (; isdigit((unsigned char)*value) && n < 100000000L; value++) n = n * 10 + (*value - '0');
    while (*value == ' ' || *value == '\t' || *value == '\r') value++;
    return *value ? -1 : n;
}
/* The Retry-After of the last answer in curl's --dump-header file, -1 without one */
static inline long hos_perf_header_retry(const char *headers) {
    long found = -1;
    for (const char *p = headers; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (!strncmp(p, "HTTP/", 5)) found = -1;   /* (a new answer: its own headers) */
        if (strncmp(p, "Retry-After:", 12) && strncmp(p, "retry-after:", 12)) continue;
        char line[64];
        size_t k = 0;
        for (const char *q = p + 12; *q && *q != '\n' && k + 1 < sizeof line; q++) line[k++] = *q;
        line[k] = 0;
        found = hos_perf_retry_after(line);
    }
    return found;
}
/* The wait after a busy answer: its Retry-After (at most a day), else an hour, doubled each time it is busy again, up to a day */
static inline long hos_perf_busy_wait(long retry_after, long before) {
    if (retry_after >= 0) return retry_after < HOS_PERF_BUSY_MOST ? retry_after : HOS_PERF_BUSY_MOST;
    const long w = before * 2 > HOS_PERF_BUSY_FIRST ? before * 2 : HOS_PERF_BUSY_FIRST;
    return w < HOS_PERF_BUSY_MOST ? w : HOS_PERF_BUSY_MOST;
}
/* perf_pending/.next, "<unix time it may be asked again> <the wait that set it>": 1 with both, 0 when it cannot be read */
static inline int hos_perf_next_parse(const char *text, int64_t *at, long *wait) {
    char *end = NULL;
    const long long a = strtoll(text, &end, 10);
    if (end == text || *end != ' ') return 0;
    const long w = strtol(end + 1, &end, 10);
    if (!end || (*end && *end != '\n' && *end != '\r')) return 0;
    *at = a, *wait = w;
    return 1;
}
/* An environment number clamped to [low, high]; the default when unset, empty or not a whole number */
static inline long hos_perf_env_number(const char *value, long def, long low, long high) {
    if (!value || !*value) return def;
    char *end = NULL;
    const long n = strtol(value, &end, 10);
    if (end == value || *end) return def;
    return n < low ? low : n > high ? high : n;
}
