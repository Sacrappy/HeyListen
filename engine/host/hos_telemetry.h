/* SPDX-License-Identifier: GPL-3.0-or-later */
/* (heylisten) Where the game's opt-in reports go: the one rule for the crash window (host/hos_crash_dialog.c), the performance
   summaries' sender (host/perf_send.c) and the menu's two rows (gpu/vk/vk_menu.cpp). Plain C that C++ can include, no system
   calls, for tests/test_telemetry.py.

   The address is the build's: nx2/addresses.ini's telemetry=, which nx2/make_game.py stamps into the game folder's
   hookshot_version.txt. EMPTY by default: with no address nothing can be sent, and the rows say "not available in this build".
   HL_TELEMETRY_URL in the environment (never hookshot_env.txt: host/hos_main.c env_line_allowed) replaces it for a test against
   a local server. Only https:// is taken; the one exception is explicit and for tests: with HL_TELEMETRY_TEST_HTTP=1 as well,
   a plain http:// address on this PC itself (127.0.0.1, localhost or [::1]) is taken, so a test server needs no certificate.
   Nothing else is taken, and a bad address (not https, spaces, a login in it) is no address: nothing is sent. */
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HOS_TELEMETRY_ENV "HL_TELEMETRY_URL"
#define HOS_TELEMETRY_TEST_HTTP_ENV "HL_TELEMETRY_TEST_HTTP"
#define HOS_TELEMETRY_CRASH_PATH "/telemetry/crash"
#define HOS_TELEMETRY_PERF_PATH "/telemetry/perf"
#define HOS_TELEMETRY_PIPELINES_PATH "/telemetry/pipelines"
#define HOS_TELEMETRY_ZONES_PATH "/telemetry/zones"
#define HOS_TELEMETRY_PROFILE_PATH "/telemetry/profile"
/* HL_TELEMETRY_PIPELINES=0 turns the pipeline state records off by themselves (the performance summaries stay as the row says) */
#define HOS_TELEMETRY_PIPELINES_ENV "HL_TELEMETRY_PIPELINES"
/* HL_TELEMETRY_ZONES=0 turns the per-zone shader and pipeline usage off by itself (the performance summaries stay as the row says) */
#define HOS_TELEMETRY_ZONES_ENV "HL_TELEMETRY_ZONES"
/* HL_TELEMETRY_PROFILE=0 turns the detailed performance profile (where the CPU and GPU time goes, per map) off by itself */
#define HOS_TELEMETRY_PROFILE_ENV "HL_TELEMETRY_PROFILE"
enum { HOS_TELEMETRY_URL_MAX = 300 };

/* Whether the pipeline state records may go, by HL_TELEMETRY_PIPELINES: on unless it is exactly "0" */
static inline int hos_telemetry_pipelines_on(const char *env_value) {
    return !(env_value && env_value[0] == '0' && env_value[1] == 0);
}

/* Whether the per-zone shader and pipeline usage may go, by HL_TELEMETRY_ZONES: on unless it is exactly "0" */
static inline int hos_telemetry_zones_on(const char *env_value) {
    return !(env_value && env_value[0] == '0' && env_value[1] == 0);
}

/* Whether the detailed performance profile may go, by HL_TELEMETRY_PROFILE: on unless it is exactly "0" */
static inline int hos_telemetry_profile_on(const char *env_value) {
    return !(env_value && env_value[0] == '0' && env_value[1] == 0);
}

/* An address as the game takes it: one line, no spaces or control characters, no backslash, no user:password@ in it, a host
   after the scheme. https:// always; http:// only to this PC itself and only when allow_loopback_http */
static inline int hos_telemetry_url_ok(const char *url, int allow_loopback_http) {
    if (!url) return 0;
    const size_t n = strlen(url);
    if (n < 9 || n > HOS_TELEMETRY_URL_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c == 0x7f || c == '\\' || c >= 0x80) return 0;
    }
    size_t start;
    if (!strncmp(url, "https://", 8)) start = 8;
    else if (allow_loopback_http && !strncmp(url, "http://", 7)) start = 7;
    else return 0;
    size_t end = start;
    while (end < n && url[end] != '/' && url[end] != '?' && url[end] != '#') end++;
    if (end == start) return 0;
    for (size_t i = start; i < end; i++)
        if (url[i] == '@') return 0;   /* (a login in the address) */
    if (start == 7) {   /* (plain http: the host must be this PC, alone or with a port) */
        static const char *const here[] = {"127.0.0.1", "localhost", "[::1]"};
        int ok = 0;
        for (size_t h = 0; h < sizeof here / sizeof here[0]; h++) {
            const size_t hn = strlen(here[h]);
            if (end - start >= hn && !strncmp(url + start, here[h], hn) &&
                (end - start == hn || (url[start + hn] == ':' && end - start > hn + 1))) {
                ok = 1;
                for (size_t i = start + hn + 1; end - start > hn && i < end; i++)
                    if (url[i] < '0' || url[i] > '9') ok = 0;
            }
        }
        if (!ok) return 0;
    }
    return 1;
}

/* The address reports go to: the environment's (HL_TELEMETRY_URL) when it is set and not empty, else the stamp's. Without a
   good one: 0 and out empty (a bad override is not replaced by the stamp's: it was meant, so nothing is sent). Its trailing
   slashes are taken off; at most HOS_TELEMETRY_URL_MAX characters */
static inline int hos_telemetry_base(const char *env_url, const char *env_test_http, const char *stamp_url, char *out, size_t cap) {
    if (cap) out[0] = 0;
    const char *url = env_url && env_url[0] ? env_url : stamp_url;
    const int allow = env_url && env_url[0] && env_test_http && !strcmp(env_test_http, "1");
    if (!url || !url[0] || !cap) return 0;
    char tmp[HOS_TELEMETRY_URL_MAX + 8];
    if (strlen(url) >= sizeof tmp) return 0;
    snprintf(tmp, sizeof tmp, "%s", url);
    size_t n = strlen(tmp);
    while (n > 8 && tmp[n - 1] == '/') tmp[--n] = 0;
    if (!hos_telemetry_url_ok(tmp, allow) || n + 1 > cap) return 0;
    memcpy(out, tmp, n + 1);
    return 1;
}

/* base + path ("/telemetry/crash"): the address a report is posted to. 0 when it does not fit */
static inline int hos_telemetry_join(char *out, size_t cap, const char *base, const char *path) {
    const int n = snprintf(out, cap, "%s%s", base, path);
    return n > 0 && (size_t)n < cap;
}

/* A key's value in hookshot_version.txt's text ("telemetry=https://..."), "" when it has none */
static inline int hos_telemetry_stamp_value(const char *stamp, const char *key, char *out, size_t cap) {
    const size_t kn = strlen(key);
    if (cap) out[0] = 0;
    for (const char *p = stamp; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (strncmp(p, key, kn) || p[kn] != '=') continue;
        const char *v = p + kn + 1;
        size_t n = 0;
        while (v[n] && v[n] != '\n' && v[n] != '\r') n++;
        if (n + 1 > cap) return 0;
        memcpy(out, v, n), out[n] = 0;
        return n > 0;
    }
    return 0;
}

/* The player's two choices, in hookshot_options.ini's text: a line key=1, blanks around it allowed; anything else (no line, 0,
   2, text) is off. Both are OFF by default (the menu's rows start at 0) */
static inline int hos_telemetry_opt_on(const char *ini, const char *key) {
    const size_t kn = strlen(key);
    int on = 0;
    for (const char *p = ini; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, kn)) continue;
        const char *q = p + kn;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        const int one = *q == '1';
        q += one;
        while (*q == ' ' || *q == '\t' || *q == '\r') q++;
        on = one && (!*q || *q == '\n');   /* (the last line of the key wins, as the menu's file is read) */
    }
    return on;
}
#define HOS_TELEMETRY_KEY_CRASH "send_crash_reports"
#define HOS_TELEMETRY_KEY_PERF "send_performance_stats"
