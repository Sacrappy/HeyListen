/* SPDX-License-Identifier: GPL-3.0-or-later */
/* (heylisten) The window after a crash (host/hos_crash_dialog.c): the parts that decide, as plain C with no system calls, for
   tests/test_crash_dialog.py.

   The game starts a small helper of itself early, `HeyListen --crash-watch <pid> <run id> <game folder> <log> <options>`
   (<options> the run's hookshot_options.ini, host/options_path.c; "-" none, and left out the game folder's), which waits for
   the game to end and then reads what it left: a crash record of its run in bug-reports/pending (hos_crash_record.c), an
   unclean-end record (the game ended without its crash handler: killed, out of memory), the quit the player asked for, and
   the run's log. From those it decides whether to say anything (hos_crash_verdict), when it happened (hos_crash_stage) and
   why, where that is known (hos_crash_cause). A start that finds a crash record nobody showed (no helper could run) asks a
   helper to show it: `HeyListen --crash-show <record> <game folder> <log> <options>`. Nothing is sent unless the player asks: Report
   manually copies a redacted text and opens the issue form; Report automatically shows the report it would post to the HeyListen telemetry server,
   and posts it (with the system's curl) only when the player presses Send. */
#pragma once
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOS_CRASH_WATCH_ARG "--crash-watch"
#define HOS_CRASH_SHOW_ARG "--crash-show"
/* (GitHub's new-issue page: the repository has no bug form, so the link carries a title and the report's body, trimmed to fit, hos_crash_issue_url) */
#define HOS_CRASH_ISSUE_FORM "https://github.com/Sacrappy/HeyListen/issues/new"
#define HOS_CRASH_ISSUES "https://github.com/Sacrappy/HeyListen/issues/new"
/* (what the crash window and the menu's row say before a report is sent: it is public, not private to the team) */
#define HOS_CRASH_PUBLIC_TEXT "Your report is published on the project's GitHub (issue #55) and Discord, without your user name or folders."
enum { HOS_CRASH_LINK_MAX = 7500, HOS_CRASH_LOG_LINES = 200 };

/* ---- the helper's command line ---- */
typedef struct HosCrashArgs {
    int mode;              /* 1 watch, 2 show */
    uint64_t pid;          /* (watch) the game's process */
    char run_id[33];       /* (watch) its run's id, 32 hex digits (hos_report_run_id): what its records carry */
    const char *record;    /* (show) a record's file name in bug-reports/pending */
    const char *dir;       /* the game folder */
    const char *log;       /* the run's log, NULL when it logged to no file ("-") */
    const char *options;   /* the run's hookshot_options.ini (host/options_path.c), NULL when not given ("-", or left out): the
                              game folder's */
} HosCrashArgs;

static inline int hos_crash_hex_id(const char *s) {
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) return 0;
    return n == 32;
}
/* A pending record's name, crash-<UTC>.txt or unclean-<UTC>.txt (hos_crash_record.c publish: maybe -<pid>-<n> before .txt):
   1 crash, 2 unclean, 0 any other name. when: its UTC, "YYYYMMDDTHHMMSSZ" */
static inline int hos_crash_record_name(const char *name, char when[17]) {
    const int kind = !strncmp(name, "crash-", 6) ? 1 : !strncmp(name, "unclean-", 8) ? 2 : 0;
    if (!kind) return 0;
    const char *p = name + (kind == 1 ? 6 : 8);
    for (int i = 0; i < 16; i++) {
        const char c = p[i];
        if (i == 8 ? c != 'T' : i == 15 ? c != 'Z' : !(c >= '0' && c <= '9')) return 0;
    }
    const char *s = p + 16;
    if (*s == '-') {   /* (-<pid>-<n>) */
        s++;
        if (!(*s >= '0' && *s <= '9')) return 0;
        while (*s >= '0' && *s <= '9') s++;
        if (*s++ != '-' || !(*s >= '0' && *s <= '9')) return 0;
        while (*s >= '0' && *s <= '9') s++;
    }
    if (strcmp(s, ".txt")) return 0;
    if (when) memcpy(when, p, 16), when[16] = 0;
    return kind;
}
/* 1 watch, 2 show, 0 not a helper's command line (the game itself), -1 a helper's that is not whole: it then does nothing */
static inline int hos_crash_args(int argc, char **argv, HosCrashArgs *a) {
    memset(a, 0, sizeof *a);
    if (argc < 2 || !argv[1]) return 0;
    const int watch = !strcmp(argv[1], HOS_CRASH_WATCH_ARG), show = !strcmp(argv[1], HOS_CRASH_SHOW_ARG);
    if (!watch && !show) return 0;
    const int base = watch ? 6 : 5;   /* (and the options file after the log, which a helper started before it came leaves out) */
    if (argc != base && argc != base + 1) return -1;
    if (watch) {
        const char *p = argv[2];
        uint64_t pid = 0;
        size_t n = 0;
        for (; p[n] >= '0' && p[n] <= '9' && n < 20; n++) pid = pid * 10 + (uint64_t)(p[n] - '0');
        if (!n || p[n] || !pid || !hos_crash_hex_id(argv[3])) return -1;
        a->pid = pid;
        memcpy(a->run_id, argv[3], 33);
    } else {
        if (!hos_crash_record_name(argv[2], NULL)) return -1;   /* (a bare name: never a path) */
        a->record = argv[2];
    }
    const char *dir = argv[watch ? 4 : 3], *log = argv[watch ? 5 : 4];
    if (!dir[0] || !log[0]) return -1;
    a->dir = dir;
    a->log = strcmp(log, "-") ? log : NULL;
    if (argc == base + 1) {
        const char *options = argv[base];
        if (!options[0]) return -1;
        a->options = strcmp(options, "-") ? options : NULL;
    }
    a->mode = watch ? 1 : 2;
    return a->mode;
}
/* Whether the game runs under gamescope (the Steam Deck's Game Mode, or a gamescope session): GAMESCOPE_WAYLAND_DISPLAY is
   what gamescope sets for what it starts (gamescope-0), and the Deck's Game Mode session also sets XDG_CURRENT_DESKTOP=gamescope.
   (Not SteamDeck=1 alone: Steam sets that in the Deck's Desktop Mode too.) Whether gamescope shows a box of a child process
   is not tested yet, and a box nobody sees would keep the helper, and so the game in Steam's eyes, running */
static inline int hos_crash_under_gamescope(const char *gamescope_display, const char *desktop) {
    if (gamescope_display && gamescope_display[0]) return 1;
    if (!desktop) return 0;
    for (const char *p = desktop; *p;) {   /* (a list: KDE:gamescope; any case) */
        size_t n = 0;
        while (p[n] && p[n] != ':') n++;
        if (n == 9) {
            size_t k = 0;
            for (; k < 9 && (p[k] | 0x20) == "gamescope"[k]; k++) {}
            if (k == 9) return 1;
        }
        p += n + (p[n] == ':');
    }
    return 0;
}
/* Whether this start gets a helper: a game run with a window, not the build's --prepare-shaders run, not a helper itself, not
   with HOS_HEADLESS (no window: tests, frame dumps), HOS_CRASH_WATCH=0 or in a profile-guided training run (HOS_PGO: the helper
   is the same program and would run the profile's writer too), not under gamescope (hos_crash_under_gamescope: neither the
   helper nor the next start's box of an earlier crash), and only with a run id its records can be found by */
static inline int hos_crash_watch_wanted(int argc, char **argv, int headless, int switched_off, int pgo, int gamescope,
                                         const char *run_id) {
    if (headless || switched_off || pgo || gamescope || !run_id || !hos_crash_hex_id(run_id)) return 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--prepare-shaders") || !strcmp(argv[i], HOS_CRASH_WATCH_ARG) || !strcmp(argv[i], HOS_CRASH_SHOW_ARG))
            return 0;
    return 1;
}
/* One argument of a Windows command line as CommandLineToArgvW reads it back: quoted, a quote inside escaped, the backslashes
   before a quote (and before the closing one) doubled. The length written, 0 when it does not fit */
static inline size_t hos_crash_quote_arg(char *out, size_t cap, const char *arg) {
    size_t n = 0;
#define HOS_CRASH_PUT(c) do { if (n + 1 >= cap) return 0; out[n++] = (c); } while (0)
    HOS_CRASH_PUT('"');
    for (const char *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == '\\') slashes++, p++;
        if (!*p) {
            for (size_t i = 0; i < slashes * 2; i++) HOS_CRASH_PUT('\\');
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) HOS_CRASH_PUT('\\');
        } else {
            for (size_t i = 0; i < slashes; i++) HOS_CRASH_PUT('\\');
        }
        HOS_CRASH_PUT(*p);
    }
    HOS_CRASH_PUT('"');
#undef HOS_CRASH_PUT
    out[n] = 0;
    return n;
}

/* ---- what the run's log says (each line in turn) ---- */
typedef struct HosCrashLog {
    int map, preparing, prepared;   /* the stage: a map line (host/hos_main.c, a save loaded), the shaders' preparation */
    int files, vulkan, window, reserve, memory, low_memory, device_lost;
    char version[128], gpu[160], driver[160], devices[240], dlss[240], fsr4[240];
    /* (what Report automatically's card says too, from the same lines as build_report (hos_crash_dialog.c): the report: lines, the
       place (the last map, else the shaders' preparation or the title and intro) and the crash in one line) */
    char os[200], cpu[200], renderer[120], place[120], crash[200];
} HosCrashLog;
static inline void hos_crash_take(char *dst, size_t cap, const char *from) {
    size_t n = 0;
    while (from[n] && from[n] != '\n' && from[n] != '\r' && n + 1 < cap) n++;
    memcpy(dst, from, n);
    dst[n] = 0;
}
/* (the names of the time before the first map: the crash report's and the performance summary's) */
#define HOS_CRASH_TITLE_AND_INTRO "Title and intro"
#define HOS_CRASH_PREPARING_SHADERS "Preparing shaders"
/* The crash in one line, as the crash report's summary says it: "0xc0000005 (write) at mkw.exe+0x1a5b9928" from
   "CRASH: exception 0xc0000005 (write 0x10, guest 0x...) at /path/to/mkw.exe+0x1a5b9928" (the module without its folder);
   "" when the line is not one */
static inline void hos_crash_summary(char *out, size_t cap, const char *line) {
    out[0] = 0;
    const char *code = strstr(line, "CRASH: exception 0x");
    if (!code || !cap) return;
    code += 17;
    size_t nc = 2;
    while (isxdigit((unsigned char)code[nc])) nc++;
    const char *how = code + nc;
    if (strncmp(how, " (read ", 7) && strncmp(how, " (write ", 8)) return;
    const char *kind = how[2] == 'r' ? "read" : "write";
    const char *close = strchr(how, ')');
    if (!close || strncmp(close, ") at ", 5)) return;
    const char *place = close + 5;
    size_t np = 0;
    while (place[np] && !isspace((unsigned char)place[np])) np++;
    const char *plus = NULL;
    for (size_t i = 0; i + 2 < np; i++)
        if (place[i] == '+' && place[i + 1] == '0' && place[i + 2] == 'x') plus = place + i;
    const char *name = place;
    for (const char *p = place; plus && p < plus; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    snprintf(out, cap, "%.*s (%s) at %.*s", (int)nc, code, kind, (int)(place + np - name), name);
}
/* The function the game resolved for a frame of the crash, "" when it named none:
   "CRASH: host frame 0: HeyListen.exe+0x12f44593 f_80580b38+0x1c4" -> "f_80580b38+0x1c4";
   "CRASH: guest frame 1: 0x80580b38 f_80580b38+0x1c4" -> "f_80580b38+0x1c4". A system DLL's frame has one token
   (module+offset) and so names none. The signature and the report's frame lists are made from these. */
static inline void hos_crash_frame_name(const char *line, char *out, size_t cap) {
    if (cap) out[0] = 0;
    const char *c = !strncmp(line, "CRASH: ", 7) ? line : strstr(line, "] CRASH: ");
    if (!c || !cap) return;
    const char *at = strstr(c, "host frame ");
    if (!at) at = strstr(c, "guest frame ");
    if (!at) return;
    const char *colon = strchr(at, ':');
    if (!colon) return;
    const char *p = colon + 1;
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;   /* (the address, or module+offset) */
    while (*p == ' ') p++;
    if (!*p) return;
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\r' && p[n] != '\n' && n + 1 < cap) {
        out[n] = p[n];
        n++;
    }
    out[n] = 0;
    for (size_t i = 0; out[i]; i++) if (out[i] == ' ') out[i] = '_';
}
/* The crash's grouping signature from its own lines: the top host frame's resolved function ("f_80580b38+0x1c4",
   "hos_crash_helper_main+0x10"), the same on every PC that built the same sources; else the exception's module+offset
   (what the relay reads today). 0 and "" when neither is there. */
static inline int hos_crash_signature(char *out, size_t cap, const char *const *lines, int n) {
    if (cap) out[0] = 0;
    char frame[200];
    for (int i = 0; i < n; i++) {
        if (!lines[i] || !strstr(lines[i], "host frame 0:")) continue;
        hos_crash_frame_name(lines[i], frame, sizeof frame);
        if (frame[0]) {
            snprintf(out, cap, "%s", frame);
            return 1;
        }
    }
    for (int i = 0; i < n; i++) {   /* (no resolved frame: the place the exception line gives, as before) */
        char summary[240];
        if (!lines[i]) continue;
        hos_crash_summary(summary, sizeof summary, lines[i]);
        if (!summary[0]) continue;
        const char *at = strstr(summary, " at ");
        if (at) {
            snprintf(out, cap, "%s", at + 4);
            return 1;
        }
    }
    return 0;
}
/* The log's first line's version, "v3.0.0 (source 47395392)": the version alone into version, its source fingerprint into source
   ("" for none) */
static inline void hos_crash_version_split(const char *line, char *version, size_t vcap, char *source, size_t scap) {
    const char *at = strstr(line, " (source ");
    const size_t n = at ? (size_t)(at - line) : strlen(line);
    snprintf(version, vcap, "%.*s", (int)n, line);
    source[0] = 0;
    if (at) {
        const char *s = at + 9;
        size_t k = 0;
        while (isxdigit((unsigned char)s[k])) k++;
        if (s[k] == ')') snprintf(source, scap, "%.*s", (int)k, s);
    }
}
static inline int hos_crash_digits(const char **p) {
    const char *s = *p;
    while (**p >= '0' && **p <= '9') (*p)++;
    return *p > s;
}
/* A release's version (the crash report's release flag): v1.2.3, v1.2.3-rc.4, nightly-<count>-<8 hex> */
static inline int hos_crash_release_version(const char *v) {
    const char *p = v;
    if (!strncmp(p, "nightly-", 8)) {
        p += 8;
        if (!hos_crash_digits(&p) || *p++ != '-') return 0;
        for (int i = 0; i < 8; i++, p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
        return !*p;
    }
    if (*p == 'v') p++;
    for (int part = 0; part < 3; part++) {
        if (!hos_crash_digits(&p)) return 0;
        if (part < 2 && *p++ != '.') return 0;
    }
    if (!strncmp(p, "-rc.", 4)) {
        p += 4;
        if (!hos_crash_digits(&p)) return 0;
    }
    return !*p;
}
/* Release build, as Report automatically's card says it: a release's version, built from the release's own sources (the log's
   source fingerprint is the one hookshot_version.txt's source= names) */
static inline int hos_crash_release(const char *version, const char *source, const char *stamp_source) {
    return hos_crash_release_version(version) && source[0] && !strcmp(source, stamp_source);
}
/* A setting the reports hold (Report automatically's options and the manual report's): key=value, the value a number, a size (1920x1080)
   or a pair (16,9), never the bindings, the microphone's name or the hardware hashes. 1 with its key and value */
static inline int hos_crash_setting(const char *line, char *key, size_t kcap, char *value, size_t vcap) {
    while (*line == ' ' || *line == '\t') line++;
    const char *eq = strchr(line, '=');
    if (!eq || eq == line) return 0;
    for (const char *p = line; p < eq; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    const size_t kn = (size_t)(eq - line);
    size_t vn = strcspn(eq + 1, "\r\n");
    while (vn && (eq[vn] == ' ' || eq[vn] == '\t')) vn--;
    const char *v = eq + 1, *p = v;
    if (*p == '-') p++;
    int ok = hos_crash_digits(&p);
    if (ok && p == v + vn) ok = 1;
    else if (ok && *v != '-' && (*p == 'x' || *p == ',')) p++, ok = hos_crash_digits(&p) && p == v + vn;
    else ok = 0;
    if (!ok) return 0;
    static const char *const left_out[] = {"microphone_device", "auto_for", "auto_tuned"};
    if (!strncmp(line, "key_", 4) || !strncmp(line, "pad_", 4)) return 0;
    for (size_t i = 0; i < sizeof left_out / sizeof left_out[0]; i++)
        if (kn == strlen(left_out[i]) && !strncmp(line, left_out[i], kn)) return 0;
    if (kn + 1 > kcap || vn + 1 > vcap) return 0;
    memcpy(key, line, kn), key[kn] = 0;
    memcpy(value, v, vn), value[vn] = 0;
    return 1;
}
/* A line of the log (without its home folder: hos_crash_redact_line first, for what is kept of it) */
static inline void hos_crash_log_line(HosCrashLog *f, const char *line) {
    const char *at;
    /* (the stage) */
    if (strstr(line, "] map: ")) f->map = 1;
    if (strstr(line, "] vk: start-up: the shaders took")) f->prepared = 1;
    else if (strstr(line, "] vk: start-up: ") || strstr(line, "] vk: precompile") || strstr(line, "] vk: preparation: ") ||
             strstr(line, "] vk: prepared "))
        f->preparing = 1;
    /* (a start that cannot go on: host/hos_main.c's own lines, written straight to the log) */
    if (!strncmp(line, "cannot open ", 12) || !strncmp(line, "usage: ", 7) || strstr(line, ": lift and image do not match") ||
        strstr(line, "] rx: mkwvk_init version mismatch") || strstr(line, " has no mkwvk_init"))
        f->files = 1;
    if (!strncmp(line, "cannot reserve the guest window", 31)) f->reserve = 1;
    /* (the Vulkan back end did not start: host/rx_backend.c, gpu/vk/vk_main.cpp, host/posix/display_sdl.c) */
    if (strstr(line, "] rx: RX_BACKEND=vulkan, but ") || strstr(line, "] vk: load_global failed") ||
        strstr(line, "] vk: vkCreateInstance failed") || strstr(line, "] vk: no Vulkan 1.3 GPU") ||
        strstr(line, "] vk: create_device failed") || strstr(line, "] rx: the Vulkan back end could not start") ||
        strstr(line, "] display: the renderer did not start"))
        f->vulkan = 1;
    if (strstr(line, "] display: no SDL") || strstr(line, "] display: SDL_Init: ") || strstr(line, "] display: SDL_CreateWindow: ") ||
        ((at = strstr(line, "] display: ")) && strstr(at, " failed (error ")))
        f->window = 1;
    if (strstr(line, "] mem: Windows is out of memory") || ((at = strstr(line, "] mem: commit of ")) && strstr(at, " failed (error ")))
        f->memory = 1;
    if (strstr(line, "] boot: LOW MEMORY")) f->low_memory = 1;
    if (strstr(line, "device lost") || strstr(line, "VK_ERROR_DEVICE_LOST")) f->device_lost = 1;
    /* (what the report names) */
    if (!f->version[0] && (at = strstr(line, "] hookshot "))) hos_crash_take(f->version, sizeof f->version, at + 11);
    if ((at = strstr(line, "] vk: driver: "))) hos_crash_take(f->driver, sizeof f->driver, at + 14);
    if ((at = strstr(line, "] vk: ")) && strstr(at, ", Vulkan ") && strstr(at, "; mesh shaders ")) {   /* (the device chosen) */
        const char *name = at + 6, *end = strstr(name, ", Vulkan ");
        const size_t n = (size_t)(end - name) < sizeof f->gpu ? (size_t)(end - name) : sizeof f->gpu - 1;
        memcpy(f->gpu, name, n), f->gpu[n] = 0;
    }
    if ((at = strstr(line, "] vk: device ")) && strstr(at, ", Vulkan ")) {   /* (every device listed) */
        const char *name = strchr(at + 13, ':');
        const char *end = name ? strstr(name, ", Vulkan ") : NULL;
        const size_t used = strlen(f->devices);
        if (name && end && end > name + 2 && used + (size_t)(end - name) + 2 < sizeof f->devices) {
            if (used) memcpy(f->devices + used, "; ", 3);
            strncat(f->devices, name + 2, (size_t)(end - name - 2));
        }
    }
    if ((at = strstr(line, "] report: os "))) hos_crash_take(f->os, sizeof f->os, at + 13);
    if ((at = strstr(line, "] report: cpu "))) hos_crash_take(f->cpu, sizeof f->cpu, at + 14);
    if ((at = strstr(line, "] report: renderer "))) hos_crash_take(f->renderer, sizeof f->renderer, at + 19);
    if (!f->crash[0] && (at = strstr(line, "CRASH: exception 0x"))) hos_crash_summary(f->crash, sizeof f->crash, at);
    if ((at = strstr(line, "] map: "))) hos_crash_take(f->place, sizeof f->place, at + 7);
    else if (strstr(line, "] vk: start-up: the shaders took ") || strstr(line, "] vk: preparation: done") ||
             strstr(line, "] vk: shader preparation stopped "))
        snprintf(f->place, sizeof f->place, "%s", HOS_CRASH_TITLE_AND_INTRO);
    else if ((strstr(line, "] vk: start-up: ") || strstr(line, "] vk: preparation: ") || strstr(line, "] vk: preparing program ") ||
              strstr(line, "] vk: shader preparation ")) &&
             (!f->place[0] || !strcmp(f->place, HOS_CRASH_TITLE_AND_INTRO)))
        snprintf(f->place, sizeof f->place, "%s", HOS_CRASH_PREPARING_SHADERS);
    if (!f->dlss[0] && (at = strstr(line, "] vk: NVIDIA DLSS: "))) hos_crash_take(f->dlss, sizeof f->dlss, at + 6);
    if (!f->fsr4[0] && (at = strstr(line, "] vk: AMD FSR 4: "))) hos_crash_take(f->fsr4, sizeof f->fsr4, at + 6);
}
/* (HOS_STAGE_UNKNOWN: the run's log was not found (it logged to a terminal or a program's pipe, or has been rotated away since):
   the window then says nothing about when, rather than "during the start" for a crash in play) */
enum { HOS_STAGE_START, HOS_STAGE_PREPARING, HOS_STAGE_PLAY, HOS_STAGE_UNKNOWN };
static inline int hos_crash_stage(const HosCrashLog *f) {
    return f->map ? HOS_STAGE_PLAY : f->preparing && !f->prepared ? HOS_STAGE_PREPARING : HOS_STAGE_START;
}
/* (a start that stopped before play for a reason the log names) */
static inline int hos_crash_start_failed(const HosCrashLog *f) {
    return !f->map && (f->files || f->vulkan || f->window || f->reserve);
}

/* ---- a pending record (hos_crash_record.c format_record): its "key: value" lines ---- */
typedef struct HosCrashRecord {
    int kind;                 /* 0 none, 1 crash, 2 unclean */
    int quit;                 /* (unclean) the player had asked to quit: "quit_asked: yes" */
    uint32_t code;            /* exception: 0x... */
    int signal_number;
    int oom;                  /* (unclean) the out-of-memory killer took the game: "oom_kill: yes" (the helper adds it) */
    char reason[40], module[160], run_id[33], when[17], uptime[24];
} HosCrashRecord;
static inline int hos_crash_record_value(const char *text, const char *key, char *out, size_t cap) {
    const size_t k = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (!strncmp(p, key, k) && p[k] == ':' && p[k + 1] == ' ') {
            hos_crash_take(out, cap, p + k + 2);
            return 1;
        }
    }
    if (cap) out[0] = 0;
    return 0;
}
static inline void hos_crash_record_parse(const char *text, HosCrashRecord *r) {
    memset(r, 0, sizeof *r);
    if (!strncmp(text, "HeyListen crash record 1\n", 24)) r->kind = 1;
    else if (!strncmp(text, "HeyListen unclean record 1\n", 26)) r->kind = 2;
    else return;
    char v[40];
    hos_crash_record_value(text, "reason", r->reason, sizeof r->reason);
    hos_crash_record_value(text, "module_offset", r->module, sizeof r->module);
    hos_crash_record_value(text, "run_id", r->run_id, sizeof r->run_id);
    hos_crash_record_value(text, "when", r->when, sizeof r->when);
    hos_crash_record_value(text, "uptime_s", r->uptime, sizeof r->uptime);
    if (hos_crash_record_value(text, "exception", v, sizeof v)) r->code = (uint32_t)strtoul(v, NULL, 16);
    if (hos_crash_record_value(text, "signal", v, sizeof v)) r->signal_number = atoi(v);
    r->quit = hos_crash_record_value(text, "quit_asked", v, sizeof v) && !strcmp(v, "yes");
    r->oom = hos_crash_record_value(text, "oom_kill", v, sizeof v) && !strcmp(v, "yes");
}
/* (Linux) Out of memory. The kernel's OOM killer ends the game with SIGKILL: no handler runs, so the end is unclean, and
   systemd then stops the game's whole scope (DefaultOOMPolicy=stop), the helper with it unless it left that scope. The
   helper reads the game's cgroup's memory.events (its oom_kill: the processes any OOM killer took there) when the game
   starts and when it ends, and writes "oom_kill: yes" in the unclean record when it grew */
/* The memory.events file of the cgroup in /proc/<pid>/cgroup's text (cgroup v2: "0::<path>"): 0 when it has none */
static inline int hos_crash_cgroup_events(const char *cgroup, char *out, size_t cap) {
    for (const char *p = cgroup; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (strncmp(p, "0::/", 4)) continue;
        size_t n = 0;
        while (p[3 + n] && p[3 + n] != '\n' && p[3 + n] != '\r') n++;
        const int k = snprintf(out, cap, "/sys/fs/cgroup%.*s%smemory.events", (int)n, p + 3, n > 1 ? "/" : "");
        return k > 0 && (size_t)k < cap;
    }
    return 0;
}
/* memory.events' oom_kill count; -1 when the text has none */
static inline long hos_crash_oom_kills(const char *events) {
    for (const char *p = events; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (!strncmp(p, "oom_kill ", 9)) return strtol(p + 9, NULL, 10);
    return -1;
}
/* A crash in the graphics driver's own code: the record's module (a file name, never a path) */
static inline int hos_crash_driver_module(const char *module) {
    static const char *const drivers[] = {"nvoglv64", "nvgpucomp", "nvvk", "amdvlk", "amdxc", "igvk", "igc", "igd", "libnvidia-",
                                          "libvulkan_", "libGLX_nvidia", "libamdvlk"};
    for (size_t i = 0; i < sizeof drivers / sizeof drivers[0]; i++) {
        const size_t n = strlen(drivers[i]);
        size_t k = 0;
        for (; k < n && module[k]; k++) {
            char c = module[k];
            if (c >= 'A' && c <= 'Z' && drivers[i][k] >= 'a') c = (char)(c + 32);
            if (c != drivers[i][k]) break;
        }
        if (k == n) return 1;
    }
    return 0;
}

/* ---- was it a crash ---- */
enum { HOS_CRASH_QUIET, HOS_CRASH_SHOW_CRASH, HOS_CRASH_SHOW_UNCLEAN, HOS_CRASH_SHOW_START };
/* What the helper says once the game has ended. A crash record of the run: it crashed, whatever came before it, a quit
   included (as the renderer's crash markers count a crash after a quit: PR 498). A start the log says could not go on (its
   files, Vulkan, the window, memory) before a save was loaded: that, a quit included (the black window closed). An unclean
   end, the run's marker left with no crash record (killed, out of memory, a handler that could not run): only when the
   player had not asked to quit (a quit never counts, however the process ends after it: a second close of a window that
   stopped answering while it closes, PR 498) and, on Windows, the exit code is not 0. Anything else (the marker removed:
   a quit, a return, a restart) says nothing; nor does a run another took the place of (Linux's restart: the same process). */
static inline int hos_crash_verdict(const HosCrashRecord *r, const HosCrashLog *f, int have_exit, unsigned long exit_code,
                                    int replaced) {
    if (replaced) return HOS_CRASH_QUIET;
    if (r->kind == 1) return HOS_CRASH_SHOW_CRASH;
    if (hos_crash_start_failed(f)) return HOS_CRASH_SHOW_START;
    if (r->kind == 2 && !r->quit && !(have_exit && exit_code == 0)) return HOS_CRASH_SHOW_UNCLEAN;
    return HOS_CRASH_QUIET;
}
enum { HOS_CAUSE_UNKNOWN, HOS_CAUSE_FILES, HOS_CAUSE_VULKAN, HOS_CAUSE_WINDOW, HOS_CAUSE_MEMORY, HOS_CAUSE_GPU_DRIVER,
       HOS_CAUSE_ENDED };
/* Why, where the record or the log says: the first that holds of the game's files, Vulkan, the window (before play), memory,
   the graphics driver; an unclean end with nothing else known was ended from outside (or out of memory, when the start
   warned of low memory); else not known */
static inline int hos_crash_cause(int verdict, const HosCrashRecord *r, const HosCrashLog *f, int have_exit, unsigned long exit_code) {
    if (f->files && !f->map) return HOS_CAUSE_FILES;
    if (f->vulkan && !f->map) return HOS_CAUSE_VULKAN;
    if (f->window && !f->map) return HOS_CAUSE_WINDOW;
    if (f->reserve || f->memory || r->oom || r->code == 0xC0000017u || (have_exit && exit_code == 0xC0000017ul)) return HOS_CAUSE_MEMORY;
    if (!strcmp(r->reason, "device lost") || !strcmp(r->reason, "Vulkan fatal") || f->device_lost ||
        (r->kind == 1 && hos_crash_driver_module(r->module)))
        return HOS_CAUSE_GPU_DRIVER;
    if (verdict == HOS_CRASH_SHOW_UNCLEAN) return f->low_memory ? HOS_CAUSE_MEMORY : HOS_CAUSE_ENDED;
    return HOS_CAUSE_UNKNOWN;
}
static inline const char *hos_crash_title(int verdict) {
    return verdict == HOS_CRASH_SHOW_START ? "HeyListen couldn't start" : "HeyListen closed unexpectedly";
}
static inline const char *hos_crash_stage_text(int stage) {
    return stage == HOS_STAGE_PLAY ? "while you were playing" : stage == HOS_STAGE_PREPARING ? "while it prepared the shaders"
         : stage == HOS_STAGE_UNKNOWN ? "" : "during the start";
}
/* What happened and when, as a sentence without its full stop: "HeyListen closed unexpectedly while you were playing" */
static inline void hos_crash_headline(char *out, size_t cap, const char *what, int stage) {
    const char *when = hos_crash_stage_text(stage);
    snprintf(out, cap, "%s%s%s", what, when[0] ? " " : "", when);
}
static inline const char *hos_crash_cause_text(int cause) {
    switch (cause) {
    case HOS_CAUSE_FILES:
        return "HeyListen's game files are missing or don't match each other. Build the game again with the HeyListen Builder.";
    case HOS_CAUSE_VULKAN:
        return "Vulkan didn't start on your graphics card. Update your graphics driver: HeyListen needs a graphics card and "
               "driver with Vulkan 1.3.";
    case HOS_CAUSE_WINDOW: return "HeyListen couldn't open its window. Restart your PC, and update your graphics driver.";
    case HOS_CAUSE_MEMORY:
        return "Your PC ran out of memory. Close other programs and try again; HeyListen needs 16 GB of RAM, and on Windows a "
               "page file Windows manages.";
    case HOS_CAUSE_GPU_DRIVER:
        return "The graphics driver failed. Update your graphics driver, and turn off overlays and tools hooked into the game.";
    case HOS_CAUSE_ENDED:
        return "HeyListen ended without a crash report: it was closed from outside (Task Manager, a kill), or the system "
               "stopped it, often for running out of memory.";
    default: return "An unexpected error.";
    }
}
/* The window's text: what happened and when, why, and what Report manually (and Report automatically, when send) does */
static inline size_t hos_crash_message(char *out, size_t cap, int verdict, int stage, int cause, int send) {
    char headline[160];
    hos_crash_headline(headline, sizeof headline, verdict == HOS_CRASH_SHOW_START ? "HeyListen stopped" : "HeyListen closed unexpectedly",
                       stage);
    const int n = snprintf(out, cap,
                           "%s.\n\nWhat went wrong: %s\n\n%s"
                           "Report manually copies a report to your clipboard (the error, your PC's hardware and the end of the log, "
                           "with your home folder and user name taken out) and opens HeyListen's issue form in your browser, "
                           "for you to paste it into. %s",
                           headline, hos_crash_cause_text(cause),
                           send ? "Report automatically shows you what would go to the HeyListen team, and sends it only when you "
                                  "press Send.\n\n" : "",
                           send ? "Report manually itself sends nothing." : "HeyListen itself sends nothing.");
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}

/* ---- privacy: the home folder, the user's name and the devices' names are taken out of everything a report holds ---- */
static inline int hos_crash_sep(char c) { return c == '/' || c == '\\'; }
static inline int hos_crash_name_char(char c) {
    const unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || c == '.' || c == '_' || c == '-' || u >= 0x80;
}
static inline int hos_crash_matches_at(const char *s, size_t n, size_t at, const char *what, size_t wn, int windows) {
    if (!wn || at + wn > n) return 0;
    for (size_t i = 0; i < wn; i++) {
        char a = s[at + i], b = what[i];
        if (hos_crash_sep(a) && hos_crash_sep(b)) {
            if (!windows && a != b) return 0;
            continue;
        }
        if (windows) {
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        }
        if (a != b) return 0;
    }
    return 1;
}
/* Whether a path may start at s[at]: not after a letter, a digit, '.', ':', '/', '~' or '<' (the bytes of a UTF-8 letter
   count as a letter too), so a URL's "//" and a ratio's "60/120" are not taken for paths */
static inline int hos_crash_path_start(const char *s, size_t at) {
    if (!at) return 1;
    const unsigned char c = (unsigned char)s[at - 1];
    return !(isalnum(c) || c >= 0x80 || c == '.' || c == ':' || c == '/' || c == '~' || c == '<');
}
/* A character a path's folder may hold: anything but ':' and the bytes a path never has (spaces stay, for "Program Files" and
   "C:\Users\Private User\...") */
static inline int hos_crash_path_char(unsigned char c) {
    return c && c != ':' && c != '"' && c != '<' && c != '>' && c != '|' && c != '?' && c != '*';
}
/* The length of the absolute folder path at s[at], which a report writes <path> and its separator (kept in *sep) in place of,
   keeping the file name after it: Windows "X:\any\folders\" (a drive, then one or more folders) and POSIX "/a/b/..." (two or
   more folders), each folder followed by a separator. 0 when there is no such path. hos_crash_path_start says where one may
   start */
static inline size_t hos_crash_path_prefix(const char *s, size_t n, size_t at, char *sep) {
    if (at >= n || !hos_crash_path_start(s, at)) return 0;
    size_t p = at, boundary = 0, folders = 0;
    int drive = 0;
    if (isalpha((unsigned char)s[p]) && p + 2 < n && s[p + 1] == ':' && hos_crash_sep(s[p + 2])) drive = 1, p += 3;
    else if (s[p] == '/') p += 1;
    else return 0;
    while (p < n) {
        size_t q = p;
        while (q < n && !hos_crash_sep(s[q]) && hos_crash_path_char((unsigned char)s[q]) &&
               s[q] != '\n' && s[q] != '\r' && s[q] != '\t')
            q++;
        /* (a folder: a whole component, then a separator; a file name has none after it) */
        if (q >= n || !hos_crash_sep(s[q]) || q == p || isspace((unsigned char)s[p]) || isspace((unsigned char)s[q - 1]))
            break;
        folders++;
        boundary = q + 1;
        p = q + 1;
    }
    if (!boundary || folders < (drive ? 1u : 2u)) return 0;
    *sep = s[boundary - 1];
    return boundary - at;
}
/* A log line with every absolute folder path <path>, the home folder written ~ (%USERPROFILE% on Windows) and the user's name
   as a folder <user>. The paths first (so a path's file name is all that is kept of it, even in the home folder); the length
   written (cut to cap) */
static inline size_t hos_crash_redact_line(char *out, size_t cap, const char *line, size_t n, const char *home, const char *user,
                                           int windows) {
    size_t hn = home ? strlen(home) : 0, un = user ? strlen(user) : 0, o = 0;
    while (hn > 1 && hos_crash_sep(home[hn - 1])) hn--;
    const int with_home = hn > 1;
    if (!cap) return 0;
#define HOS_CRASH_OUT(s, k) do { for (size_t j_ = 0; j_ < (k) && o + 1 < cap; j_++) out[o++] = (s)[j_]; } while (0)
    for (size_t i = 0; i < n;) {
        char sep = 0;
        const size_t path = hos_crash_path_prefix(line, n, i, &sep);
        if (path) {
            HOS_CRASH_OUT("<path>", 6);
            HOS_CRASH_OUT(&sep, 1);
            i += path;
            continue;
        }
        if (with_home && (i == 0 || (!hos_crash_name_char(line[i - 1]) && !hos_crash_sep(line[i - 1]))) &&
            hos_crash_matches_at(line, n, i, home, hn, windows) && (i + hn >= n || !hos_crash_name_char(line[i + hn]))) {
            const char *w = windows ? "%USERPROFILE%" : "~";
            HOS_CRASH_OUT(w, strlen(w));
            i += hn;
            continue;
        }
        if (un && i > 0 && hos_crash_sep(line[i - 1]) && hos_crash_matches_at(line, n, i, user, un, windows) &&
            (i + un >= n || !hos_crash_name_char(line[i + un]))) {
            HOS_CRASH_OUT("<user>", 6);
            i += un;
            continue;
        }
        HOS_CRASH_OUT(line + i, 1);
        i++;
    }
#undef HOS_CRASH_OUT
    out[o] = 0;
    return o;
}
/* Whether a setting's value goes into the report: a number of at most 12 characters (60, -1, 1.5), a size (1920x1080), or
   on/off, true/false, yes/no, or empty; and, for a switch (name not NULL), never for a name that may hold a secret, a place or
   a person (TOKEN, KEY, ROOM, SECRET, PASS, SERVER or USER in it). The menu's value_shown; every other value is left out */
static inline int hos_crash_value_shown(const char *name, const char *value) {
    if (name) {
        static const char *const words[] = {"TOKEN", "KEY", "ROOM", "SECRET", "PASS", "SERVER", "USER"};
        char upper[128];
        size_t n = 0;
        for (; name[n] && n + 1 < sizeof upper; n++) upper[n] = (name[n] >= 'a' && name[n] <= 'z') ? (char)(name[n] - 32) : name[n];
        upper[n] = 0;
        for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
            if (strstr(upper, words[i])) return 0;
    }
    static const char *const plain[] = {"", "on", "off", "true", "false", "yes", "no"};
    const size_t vn = strlen(value);
    if (vn <= 5) {
        char lower[8];
        for (size_t i = 0; i <= vn; i++) lower[i] = (value[i] >= 'A' && value[i] <= 'Z') ? (char)(value[i] + 32) : value[i];
        for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++)
            if (!strcmp(lower, plain[i])) return 1;
    }
    if (vn > 12) return 0;
    size_t at = value[0] == '-' ? 1 : 0, from = at;
    while (value[at] >= '0' && value[at] <= '9') at++;
    if (at == from) return 0;
    if (!value[at]) return 1;
    const char mid = value[at++];
    if (mid != '.' && !(mid == 'x' && value[0] != '-')) return 0;
    from = at;
    while (value[at] >= '0' && value[at] <= '9') at++;
    return at > from && !value[at];
}

/* ---- the issue form's link ---- */
static inline size_t hos_crash_url_part(char *out, size_t cap, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    if (!cap) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const int plain = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-' ||
                          *p == '_' || *p == '.' || *p == '~';
        if (n + (plain ? 1 : 3) >= cap) break;
        if (plain) out[n++] = (char)*p;
        else out[n++] = '%', out[n++] = hex[*p >> 4], out[n++] = hex[*p & 15];
    }
    out[n] = 0;
    return n;
}
/* ---- the report in the link's body. Issues #62 and #63 came with an empty code block: the link asked the player to paste the
   report from the clipboard and nobody did. The body is now built from the report text itself, the parts that matter first,
   encoded and cut at a whole line to fit HOS_CRASH_LINK_MAX; the whole report is still on the clipboard and saved ---- */
/* The length of the encoded form of the n bytes of s (as hos_crash_url_part writes them) */
static inline size_t hos_crash_url_len(const char *s, size_t n) {
    size_t e = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        const int plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
                          c == '.' || c == '~';
        e += plain ? 1 : 3;
    }
    return e;
}
/* The body being written: s (raw, used of cap bytes), its encoded length, and the encoded length it may not pass */
typedef struct HosCrashBody { char *s; size_t used, cap, enc, budget; } HosCrashBody;
static inline int hos_crash_body_add(HosCrashBody *b, const char *s, size_t n) {
    const size_t e = hos_crash_url_len(s, n);
    if (b->used + n + 1 > b->cap || b->enc + e > b->budget) return 0;
    memcpy(b->s + b->used, s, n);
    b->used += n, b->enc += e;
    b->s[b->used] = 0;
    return 1;
}
/* A section of the report ("### <prefix>..."): its text after the heading line, up to the next "### " line. 0 when there is none */
static inline int hos_crash_section(const char *report, const char *prefix, const char **body, size_t *n) {
    const size_t k = strlen(prefix);
    for (const char *p = report; p && *p;) {
        if (!strncmp(p, "### ", 4) && !strncmp(p + 4, prefix, k)) {
            const char *start = strchr(p, '\n');
            if (!start) return 0;
            start++;
            const char *end = start;
            while (*end && strncmp(end, "### ", 4)) {
                const char *nl = strchr(end, '\n');
                end = nl ? nl + 1 : end + strlen(end);
            }
            *body = start, *n = (size_t)(end - start);
            return 1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}
enum { HOS_CRASH_BODY_LINE = 300, HOS_CRASH_BODY_LINES = 256 };
/* The lines of a section as the link keeps them: the report's own code fences and empty lines left out, a line cut at
   HOS_CRASH_BODY_LINE bytes (not inside a UTF-8 letter), those starting with one of skip (NULL-ended) dropped */
static inline int hos_crash_body_lines(const char *body, size_t n, const char *const *skip, const char **at, size_t *len) {
    int count = 0;
    for (const char *p = body, *end = body + n; p < end && count < HOS_CRASH_BODY_LINES;) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *next = nl ? nl + 1 : end;
        while (l && (p[l - 1] == '\r' || p[l - 1] == ' ')) l--;
        int keep = l && strncmp(p, "```", 3);
        for (int i = 0; keep && skip && skip[i]; i++)
            if (!strncmp(p, skip[i], strlen(skip[i]))) keep = 0;
        if (keep) {
            if (l > HOS_CRASH_BODY_LINE) {
                l = HOS_CRASH_BODY_LINE;
                while (l > 1 && ((unsigned char)p[l] & 0xC0) == 0x80) l--;
            }
            at[count] = p, len[count++] = l;
        }
        p = next;
    }
    return count;
}
static inline int hos_crash_has_crash(const char *s, size_t n) {
    for (size_t i = 0; i + 5 <= n; i++)
        if (!memcmp(s + i, "CRASH", 5)) return 1;
    return 0;
}
/* One titled part in a code block: its lines from the first, while the part stays within part_max encoded characters and the
   body within its budget; nothing is written for a part of which no line fits. newest: the part is the log's end: the CRASH
   lines first, then the newest lines that fit, written in the log's order, after a note that older ones were left out */
static inline void hos_crash_body_part(HosCrashBody *b, const char *title, const char *report, const char *prefix, size_t part_max,
                                       const char *const *skip, int newest) {
    static const char closer[] = "```\n\n", dropped[] = "(older lines left out; the CRASH lines are kept)\n";
    const char *body, *at[HOS_CRASH_BODY_LINES];
    size_t n, len[HOS_CRASH_BODY_LINES];
    char picked[HOS_CRASH_BODY_LINES] = {0}, head[160];
    if (!hos_crash_section(report, prefix, &body, &n)) return;
    const int count = hos_crash_body_lines(body, n, skip, at, len);
    const int hn = snprintf(head, sizeof head, "**%s**\n```\n", title);
    if (!count || hn <= 0 || (size_t)hn >= sizeof head) return;
    const size_t fixed = hos_crash_url_len(head, (size_t)hn) + hos_crash_url_len(closer, sizeof closer - 1);
    size_t room = b->budget > b->enc + fixed ? b->budget - b->enc - fixed : 0;
    if (room > part_max) room = part_max > fixed ? part_max - fixed : 0;
    size_t used = 0;
    int took = 0, left_out = 0;
    if (!newest) {
        for (int i = 0; i < count; i++) {
            const size_t e = hos_crash_url_len(at[i], len[i]) + 3;   /* (+3: the line's end) */
            if (used + e > room) break;
            used += e, picked[i] = 1, took++;
        }
    } else {
        const size_t note = hos_crash_url_len(dropped, sizeof dropped - 1);
        int crash = 0;
        for (int i = count - 1; i >= 0 && crash < 8; i--) {
            const size_t e = hos_crash_url_len(at[i], len[i]) + 3;
            if (!hos_crash_has_crash(at[i], len[i]) || used + e + note > room) continue;
            used += e, picked[i] = 1, took++, crash++;
        }
        for (int i = count - 1; i >= 0; i--) {
            const size_t e = hos_crash_url_len(at[i], len[i]) + 3;
            if (picked[i]) continue;
            if (used + e + note > room) break;
            used += e, picked[i] = 1, took++;
        }
        for (int i = 0; i < count; i++) left_out |= !picked[i];
    }
    if (!took) return;
    hos_crash_body_add(b, head, (size_t)hn);
    if (left_out) hos_crash_body_add(b, dropped, sizeof dropped - 1);
    for (int i = 0; i < count; i++) {
        if (!picked[i]) continue;
        hos_crash_body_add(b, at[i], len[i]);
        hos_crash_body_add(b, "\n", 1);
    }
    hos_crash_body_add(b, closer, sizeof closer - 1);
}
/* GitHub's new-issue page with the title and the body filled in (GitHub reads title= and body= from the link; the repository
   has no bug form, only the plain issue page), within HOS_CRASH_LINK_MAX characters. The body: the version, the GPU, the system,
   where in the game and what happened (each line only while the link stays within its size), a line saying the whole report
   is also saved at saved (a path with the home folder already left out; "" when it could not be saved: then it says the
   report is on the clipboard), then the report's parts, the most useful first, each only as far as it fits: the summary's
   renderer, settings and crash lines, the crash record, the settings changed from the defaults, then the log's end (its CRASH
   lines, then the newest lines). report is the text report_text writes (already without the user's name, paths and values that
   are private); NULL or "": the lines above only. Nothing is asked of the player to paste */
static inline size_t hos_crash_issue_url(char *out, size_t cap, const char *title, const char *version, const char *gpu,
                                         const char *system, const char *what, const char *track, const char *where,
                                         const char *settings, const char *report, const char *saved) {
    (void)track, (void)settings;
    const size_t max = cap < HOS_CRASH_LINK_MAX + 1 ? cap : HOS_CRASH_LINK_MAX + 1;
    size_t n = strlen(HOS_CRASH_ISSUE_FORM);
    if (n + 1 > max) return 0;
    memcpy(out, HOS_CRASH_ISSUE_FORM, n + 1);
    char piece[1024];
    if (title && title[0]) {
        memcpy(piece, "?title=", 7);
        const size_t v = hos_crash_url_part(piece + 7, sizeof piece - 7, title);
        if (v && n + 7 + v + 1 <= max) memcpy(out + n, piece, 7 + v + 1), n += 7 + v;
    }
    const char *key = n == strlen(HOS_CRASH_ISSUE_FORM) ? "?body=" : "&body=";
    const size_t kn = strlen(key);
    if (n + kn + 1 > max) return n;
    char raw[HOS_CRASH_LINK_MAX + 1];   /* (the body before it is encoded: never longer than its encoded form) */
    HosCrashBody b = {raw, 0, sizeof raw, 0, max - n - kn - 1};
    raw[0] = 0;
    const char *const labels[] = {"**Version:** ", "**GPU:** ", "**System:** ", "**Where in the game:** ", "**What happened:** "};
    const char *const values[] = {version, gpu, system, where, what};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        char line[512];
        if (!values[i] || !values[i][0]) continue;
        const int w = snprintf(line, sizeof line, "%s%.300s\n\n", labels[i], values[i]);
        if (w > 0 && (size_t)w < sizeof line) hos_crash_body_add(&b, line, (size_t)w);   /* (this line does not fit: the next may) */
    }
    {   /* (where the whole report is: it is saved, and on the clipboard) */
        char line[700];
        const char *tail = saved ? saved : "";
        const size_t tl = strlen(tail);
        if (tl > 260) tail += tl - 260;
        const int w = tail[0] ? snprintf(line, sizeof line, "The report from HeyListen's crash window, trimmed to fit this link, is below. "
                                         "The whole report is also saved on my PC at `%s%s`, I can attach it if it helps.\n\n",
                                         tail == saved ? "" : "...", tail)
                              : snprintf(line, sizeof line, "The report from HeyListen's crash window, trimmed to fit this link, is below. "
                                         "The whole report could not be saved to a file; it is on my clipboard.\n\n");
        if (w > 0 && (size_t)w < sizeof line) hos_crash_body_add(&b, line, (size_t)w);
    }
    if (report && report[0]) {
        static const char *const summary_skip[] = {"Version:", "Operating system:", "Graphics card:", "CPU and RAM:", "Map:", "Log:", NULL};
        hos_crash_body_part(&b, "Summary", report, "Summary", 700, summary_skip, 0);
        hos_crash_body_part(&b, "Crash record", report, "Crash record", 1900, NULL, 0);
        hos_crash_body_part(&b, "Settings changed from the defaults", report, "Changes from the defaults", 1100, NULL, 0);
        hos_crash_body_part(&b, "The end of hookshot_log.txt", report, "The end of hookshot_log.txt", HOS_CRASH_LINK_MAX, NULL, 1);
    }
    if (b.used) {
        memcpy(out + n, key, kn);
        n += kn;
        n += hos_crash_url_part(out + n, max - n, raw);
    }
    return n;
}

/* ---- Linux's clipboard: a tool that keeps serving the text once the helper has ended (a Wayland selection is only taken from
   a client with the keyboard's focus and dies with its process; X11's, with its owner), in turn: wl-copy on Wayland, then
   xclip and xsel on X11 (XWayland too). The text goes on the tool's input. NULL past the last */
static inline const char *const *hos_crash_clip_tool(int wayland, int x11, int i) {
    static const char *const wl[] = {"wl-copy", NULL};
    static const char *const xclip[] = {"xclip", "-selection", "clipboard", "-in", NULL};
    static const char *const xsel[] = {"xsel", "--clipboard", "--input", NULL};
    const char *const *all[3];
    int n = 0;
    if (wayland) all[n++] = wl;
    if (x11) all[n++] = xclip, all[n++] = xsel;
    return i >= 0 && i < n ? all[i] : NULL;
}
/* The box after Report: whether the report is on the clipboard (copied: 0 not, 1 kept there by the system or a tool, 2 by this
   helper only while it runs: X11 through SDL), whether the issue form opened, and the saved report's path (NULL when it could
   not be saved). Never says it was copied when it was not; with nothing copied, the saved file is the way to it */
static inline size_t hos_crash_ready_text(char *out, size_t cap, int copied, int opened, const char *saved, const char *issues) {
    const int n = snprintf(out, cap, "%s%s%s%s%s%s",
        copied ? "The report is on your clipboard. "
        : saved ? "The report could not be put on the clipboard. Click Open the report, select all of it and copy it. "
                : "The report could not be put on the clipboard, nor saved. ",
        opened ? "The new issue that opened in your browser already has the main parts of it (the end of the log is cut to fit "
                 "the link): say what you were doing and submit it. Paste or attach the whole report there too if you can."
               : "Open ",
        opened ? "" : issues,
        opened ? "" : " and paste it into a new issue.",
        saved ? "\n\nIt is saved as " : "", saved ? saved : "");
    size_t used = n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
    const char *tail = saved ? "." : "";
    const char *keep = copied == 2 ? "\n\nKeep this window open until you have pasted it." : "";
    if (used < cap) {
        const int m = snprintf(out + used, cap - used, "%s%s", tail, keep);
        if (m > 0) used += (size_t)m < cap - used ? (size_t)m : cap - used - 1;
    }
    return used;
}

/* ---- which records were shown (bug-reports/shown.txt: a name a line) ---- */
static inline int hos_crash_listed(const char *list, const char *name) {
    const size_t k = strlen(name);
    for (const char *p = list; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (!strncmp(p, name, k) && (p[k] == '\n' || p[k] == '\r' || !p[k])) return 1;
    return 0;
}
/* Seconds since 1970 of a record's UTC ("YYYYMMDDTHHMMSSZ"): days from the civil date (H. Hinnant's algorithm) */
static inline int64_t hos_crash_epoch(const char when[17]) {
    int v[6], at[6] = {0, 4, 6, 9, 11, 13}, len[6] = {4, 2, 2, 2, 2, 2};
    for (int i = 0; i < 6; i++) {
        v[i] = 0;
        for (int k = 0; k < len[i]; k++) v[i] = v[i] * 10 + (when[at[i] + k] - '0');
    }
    int y = v[0] - (v[1] <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (v[1] + (v[1] > 2 ? -3 : 9)) + 2) / 5 + v[2] - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + v[3] * 3600 + v[4] * 60 + v[5];
}
/* Whether a start shows a record no helper showed (its helper could not run, or was ended with the game): a crash record,
   or an unclean end the out-of-memory killer made (oom: its "oom_kill: yes"; systemd stops the game's scope after it, the
   helper with it when it could not leave it, in a Flatpak); not another unclean end (a game a launcher or a logout stopped
   leaves one of those, and is no crash). A day old at most, and not listed as shown */
static inline int hos_crash_show_at_start(const char *name, const char *shown, int64_t now, int oom) {
    char when[17];
    const int kind = hos_crash_record_name(name, when);
    if (!(kind == 1 || (kind == 2 && oom)) || hos_crash_listed(shown, name)) return 0;
    const int64_t t = hos_crash_epoch(when);
    return t <= now + 300 && now - t <= 86400;
}

/* ---- Report automatically: the report built and sent here (docs/crash-report-window.md) ---- */
/* Where it goes: telemetry= (the HeyListen telemetry server's address; the report is POSTed to <it>/telemetry/crash) and
   crash_issue= (optional: the issue the reports are collected on, the only place the Sent page's link may go) in the folder's
   hookshot_version.txt, which nx2/make_game.py writes from nx2/addresses.ini. A key's value, "" when the stamp has none; 0
   then, or when it is not an https:// address on one line */
static inline int hos_crash_stamp_address(const char *stamp, const char *key, char *out, size_t cap) {
    const size_t kn = strlen(key);
    if (cap) out[0] = 0;
    for (const char *p = stamp; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (strncmp(p, key, kn) || p[kn] != '=') continue;
        const char *v = p + kn + 1;
        size_t n = 0;
        while (v[n] && v[n] != '\n' && v[n] != '\r') n++;
        if (n < 9 || strncmp(v, "https://", 8) || n + 1 > cap || memchr(v, ' ', n)) return 0;
        memcpy(out, v, n), out[n] = 0;
        return 1;
    }
    return 0;
}
/* Whether the window offers Report automatically: a crash record (an unclean end has no crash to report), an address to send it
   to, and its report not sent already (its answer in bug-reports/sent/) */
static inline int hos_crash_send_offered(const char *record_name, int have_address, int sent_already) {
    return record_name && hos_crash_record_name(record_name, NULL) == 1 && have_address && !sent_already;
}
/* Whether the player chose to send crash reports without the window: the in-game menu's "Send crash reports automatically", a
   line send_crash_reports=1 in hookshot_options.ini (blanks around it allowed). Any other value, or no file: the window asks.
   A report that can't be sent then shows the window after all */
static inline int hos_crash_auto_send(const char *ini) {
    for (const char *p = ini; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "send_crash_reports=1", 20)) continue;
        const char *q = p + 20;
        while (*q == ' ' || *q == '\t' || *q == '\r') q++;
        if (!*q || *q == '\n') return 1;
    }
    return 0;
}

/* The report, as the relay (the HeyListen telemetry server's telemetry/crash/report.go) takes it: {version, release, os, renderer, gpu, cpu, map,
   crash, log_tail, options}. The crash lines are the run's log's CRASH: lines from its exception on, without the rows of the
   game's memory, each module named without its folder: the relay's signature reads them, so they must stay as they are */
enum { HOS_CRASH_LINE_LIMIT = 300, HOS_CRASH_CRASH_LINES = 80, HOS_CRASH_TAIL_LINES = 150 };
enum { HOS_CRASH_MAX_VERSION = 40, HOS_CRASH_MAX_OS = 200, HOS_CRASH_MAX_RENDERER = 120, HOS_CRASH_MAX_GPU = 200,
       HOS_CRASH_MAX_CPU = 200, HOS_CRASH_MAX_MAP = 120, HOS_CRASH_MAX_OPTIONS = 4000 };
/* A crash's first line: "CRASH: exception 0x<hex> " anywhere in it */
static inline int hos_crash_starts(const char *line) {
    for (const char *at = line; (at = strstr(at, "CRASH: exception 0x")); at++) {
        const char *p = at + 19;
        if (!isxdigit((unsigned char)*p)) continue;
        while (isxdigit((unsigned char)*p)) p++;
        if (*p == ' ') return 1;
    }
    return 0;
}
/* (a row of the game's memory: the registers, "CRASH:   x0 ...", and what they point to, "CRASH: [x19+0x10] ...") */
static inline int hos_crash_memory_row(const char *crash) {
    for (const char *at = crash; (at = strstr(at, "CRASH:")); at++) {
        const char *p = at + 6;
        if (*p == ' ' && p[1] == '[' && p[2] == 'x' && isdigit((unsigned char)p[3])) {
            p += 3;
            while (isdigit((unsigned char)*p)) p++;
            if (*p == '+') return 1;
        }
        if (*p != ' ') continue;
        while (*p == ' ') p++;
        if (*p == 'x' && isdigit((unsigned char)p[1])) return 1;
    }
    return 0;
}
/* A log line's crash part, "CRASH: ..." at its start or after the log's "] ", as the report holds it: modules without their
   folder ("at /usr/lib/libc.so.6+0x10" is "at libc.so.6+0x10") and, on the crash's first line, the module of the place in the
   relay's characters ("at ?+0x10" is "at unknown+0x10"). 0 for a line with none, or a row of the game's memory */
static inline int hos_crash_report_line(char *out, size_t cap, const char *line) {
    const char *c = !strncmp(line, "CRASH: ", 7) ? line : strstr(line, "] CRASH: ");
    if (!c || !cap) return 0;
    if (c != line) c += 2;
    if (hos_crash_memory_row(c)) return 0;
    size_t o = 0;
    const int first = hos_crash_starts(c);
    int module_done = 0;
#define HOS_CRASH_PUT(ch) (o + 1 < cap ? (void)(out[o++] = (ch)) : (void)0)
    for (const char *p = c; *p && *p != '\n' && *p != '\r';) {
        if (p >= c + 4 && !strncmp(p - 4, " at ", 4)) {
            /* (p is a place: its token up to a space; the folders off it when a module+0x follows the last separator) */
            const char *end = p;
            while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') end++;
            const char *name = p;
            for (const char *q = p; q < end; q++)
                if (*q == '/' || *q == '\\') name = q + 1;
            if (name != p) {
                const char *plus = name + 1;
                while (plus + 2 < end && strncmp(plus, "+0x", 3)) plus++;
                if (!(name < end && plus + 2 < end && !strncmp(plus, "+0x", 3))) name = p;
            }
            if (first && !module_done) {
                /* (the place's module: [^\s+]* before +0x<hex>, then a space or the end) */
                const char *plus = name;
                while (plus < end && *plus != '+') plus++;
                const char *hex = plus + 3;
                int ok = plus + 3 <= end && !strncmp(plus, "+0x", 3) && hex < end && isxdigit((unsigned char)*hex);
                while (ok && hex < end && isxdigit((unsigned char)*hex)) hex++;
                if (ok && hex == end) {
                    module_done = 1;
                    size_t kept = 0;
                    for (const char *q = name; q < plus && kept < 64; q++)
                        if (isalnum((unsigned char)*q) || *q == '_' || *q == '.' || *q == '-') HOS_CRASH_PUT(*q), kept++;
                    if (!kept)
                        for (const char *u = "unknown"; *u; u++) HOS_CRASH_PUT(*u);
                    name = plus;
                }
            }
            for (const char *q = name; q < end; q++) HOS_CRASH_PUT(*q);
            p = end;
            continue;
        }
        HOS_CRASH_PUT(*p);
        p++;
    }
#undef HOS_CRASH_PUT
    out[o] = 0;
    return 1;
}
/* s[at, at + n) as with, in place in s's cap bytes: what no longer fits is cut off the end (never a UTF-8 character in two),
   and with itself is left out when it does not fit. Where the text goes on after with (its end, or at when it was left out) */
static inline size_t hos_crash_splice(char *s, size_t cap, size_t at, size_t n, const char *with) {
    const size_t len = strlen(s), wn = strlen(with);
    if (!cap || at > len) return len;
    if (n > len - at) n = len - at;
    if (at + wn + 1 > cap) {
        s[at] = 0;
        return at;
    }
    size_t tail = len - at - n;
    const size_t room = cap - 1 - at - wn;
    if (tail > room) {
        tail = room;
        while (tail && ((unsigned char)s[at + n + tail] & 0xC0) == 0x80) tail--;
    }
    memmove(s + at + wn, s + at + n, tail);
    s[at + wn + tail] = 0;
    memcpy(s + at, with, wn);
    return at + wn;
}
/* (whether s[at] is inside a <device> already written there) */
static inline int hos_crash_in_device(const char *s, size_t at) {
    for (size_t back = 0; back < 8 && back <= at; back++)
        if (!strncmp(s + at - back, "<device>", 8)) return 1;
    return 0;
}
/* (the names a player gives their devices, as the log says them: host/nn_audio.c's output, host/nn_audio_in.c's microphone;
   s has cap bytes) */
static inline void hos_crash_devices_out(char *s, size_t cap) {
    static const char *const heads[] = {"audio: output on ", "audio in: "};
    for (int k = 0; k < 2; k++) {
        char *at = strstr(s, heads[k]);
        if (!at) continue;
        char *name = at + strlen(heads[k]), *end = NULL;
        if (k == 0) {   /* ("audio: output on <name> (3 devices)") */
            for (char *q = strstr(name, " ("); q; q = strstr(q + 1, " (")) {
                const char *d = q + 2;
                if (!isdigit((unsigned char)*d)) continue;
                while (isdigit((unsigned char)*d)) d++;
                if (!strncmp(d, " device", 7)) { end = q; break; }
            }
        } else {   /* ("audio in: <name> open (48000 Hz", "<name> is back", "<name>: <why>, tried again from time to time") */
            char *open = strstr(name, " open ("), *back = strstr(name, " is back"), *again = strstr(name, ", tried again from time to time");
            if (open) end = open;
            else if (back && !back[8]) end = back;
            else if (again) {
                end = strstr(name, ": ");
                if (end && end > again) end = NULL;
            }
        }
        if (!end || end == name) continue;
        hos_crash_splice(s, cap, (size_t)(name - s), (size_t)(end - name), "<device>");
    }
}
static inline int hos_crash_word_char(unsigned char c) { return isalnum(c); }
static inline int hos_crash_ncase(const char *a, const char *b, size_t n) {   /* (strncasecmp, which not every C library has) */
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 1;
    return 0;
}
/* Every place a home folder is by its shape, after the user's own (hos_crash_redact_line): /home/<name>, /var/home/<name> and
   /Users/<name> as ~, C:\Users\<name> as %USERPROFILE%, and a drive's \home\<name> (Wine and Proton's Z:\home\<name>) as ~;
   and the user's name, 3 letters or more, as a word of its own anywhere ("Alice's PC"), in any case unless it is one of the
   words the report's own lines use (a Steam Deck's deck). In place */
static inline void hos_crash_homes_out(char *s, size_t cap, const char *user) {
    static const char *const common[] = {"deck", "steam", "arch", "linux", "ubuntu", "fedora", "debian", "mint", "manjaro",
                                         "bazzite", "nobara", "cachyos", "windows", "wine", "vulkan", "intel", "amd", "nvidia",
                                         "radeon", "game", "games", "gamer", "user", "admin", "player"};
    char *out = (char *)malloc(cap);
    if (!out || !cap) { free(out); return; }
    const size_t un = user ? strlen(user) : 0;
    int any_case = un >= 3;
    for (size_t i = 0; any_case && i < sizeof common / sizeof common[0]; i++) {
        if (strlen(common[i]) != un) continue;
        size_t j = 0;
        while (j < un && tolower((unsigned char)user[j]) == common[i][j]) j++;
        if (j == un) any_case = 0;
    }
    size_t o = 0, n = strlen(s);
#define HOS_CRASH_PUTS(t) do { for (const char *t_ = (t); *t_ && o + 1 < cap; t_++) out[o++] = *t_; } while (0)
    for (size_t i = 0; i < n;) {
        const int before = i == 0 || !hos_crash_word_char((unsigned char)s[i - 1]);
        /* (a drive: a letter on its own, its colon, separators) */
        if (before && i + 2 < n && isalpha((unsigned char)s[i]) && s[i + 1] == ':' && hos_crash_sep(s[i + 2])) {
            size_t j = i + 2;
            while (j < n && hos_crash_sep(s[j])) j++;
            static const char *const tops[] = {"var", "home", "users"};
            size_t k = j;
            int home = 0, users = 0;
            if (k + 3 < n && !hos_crash_ncase(s + k, tops[0], 3) && hos_crash_sep(s[k + 3])) {
                k += 3;
                while (k < n && hos_crash_sep(s[k])) k++;
            }
            if (k + 4 < n && !hos_crash_ncase(s + k, tops[1], 4) && hos_crash_sep(s[k + 4])) home = 1, k += 4;
            else if (k == j && k + 5 < n && !hos_crash_ncase(s + k, tops[2], 5) && hos_crash_sep(s[k + 5])) users = 1, k += 5;
            if (home || users) {
                while (k < n && hos_crash_sep(s[k])) k++;
                size_t e = k;
                while (e < n && !hos_crash_sep(s[e]) && !isspace((unsigned char)s[e])) e++;
                if (e > k) {
                    HOS_CRASH_PUTS(home ? "~" : "%USERPROFILE%");
                    i = e;
                    continue;
                }
            }
        }
        static const char *const unix_homes[] = {"/var/home/", "/home/", "/Users/"};
        int taken = 0;
        for (int h = 0; h < 3 && !taken; h++) {
            const size_t hn = strlen(unix_homes[h]);
            if (i + hn < n && !strncmp(s + i, unix_homes[h], hn) && !hos_crash_sep(s[i + hn]) && !isspace((unsigned char)s[i + hn])) {
                size_t e = i + hn;
                while (e < n && s[e] != '/' && !isspace((unsigned char)s[e])) e++;
                HOS_CRASH_PUTS("~");
                i = e, taken = 1;
            }
        }
        if (taken) continue;
        if (un >= 3 && before && i + un <= n && (any_case ? !hos_crash_ncase(s + i, user, un) : !strncmp(s + i, user, un)) &&
            (i + un == n || !hos_crash_word_char((unsigned char)s[i + un]))) {
            HOS_CRASH_PUTS("<user>");
            i += un;
            continue;
        }
        if (o + 1 < cap) out[o++] = s[i];
        i++;
    }
#undef HOS_CRASH_PUTS
    out[o] = 0;
    memcpy(s, out, o + 1);
    free(out);
}
/* A field or line as the report holds it, in place (s has cap bytes; text that grows past them is cut off the end): no
   control characters, the device names out, the home folder and user out (the menu's rules, then by shape), the spaces
   around it off, at most limit characters (UTF-8's: a character is never cut in two) */
static inline void hos_crash_clean(char *s, size_t cap, size_t limit, const char *home, const char *user, int windows,
                                   const char *const *devices, int ndevices) {
    if (!cap) return;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!(*p < 0x20 && *p != '\t' && *p != '\n') && *p != 0x7f) s[o++] = (char)*p;
    s[o] = 0;
    hos_crash_devices_out(s, cap);
    /* (each name found from after the last one written: a name inside <device> itself, "dev" or "vice", is never found again) */
    for (int d = 0; d < ndevices; d++) {
        const size_t dn = strlen(devices[d]);
        if (dn < 3) continue;
        for (size_t from = 0; from < strlen(s);) {
            const char *at = strstr(s + from, devices[d]);
            if (!at) break;
            const size_t i = (size_t)(at - s);
            from = hos_crash_in_device(s, i) ? i + 1 : hos_crash_splice(s, cap, i, dn, "<device>");
        }
    }
    char *r = (char *)malloc(cap);
    if (r) {
        hos_crash_redact_line(r, cap, s, strlen(s), home, user, windows);
        memcpy(s, r, strlen(r) + 1);
        free(r);
    }
    hos_crash_homes_out(s, cap, user);
    size_t start = 0, n = strlen(s);
    while (start < n && isspace((unsigned char)s[start])) start++;
    while (n > start && isspace((unsigned char)s[n - 1])) n--;
    memmove(s, s + start, n - start);
    n -= start;
    s[n] = 0;
    size_t chars = 0, i = 0;
    for (; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80 && chars++ == limit) break;
    s[i] = 0;
}
/* s as a JSON string, quotes included; its length (cut to cap, and then not valid: the caller makes room) */
static inline size_t hos_crash_json(char *out, size_t cap, const char *s) {
    size_t o = 0;
    if (!cap) return 0;
#define HOS_CRASH_JPUT(ch) (o + 1 < cap ? (void)(out[o++] = (ch)) : (void)0)
    HOS_CRASH_JPUT('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') HOS_CRASH_JPUT('\\'), HOS_CRASH_JPUT((char)*p);
        else if (*p == '\n') HOS_CRASH_JPUT('\\'), HOS_CRASH_JPUT('n');
        else if (*p == '\t') HOS_CRASH_JPUT('\\'), HOS_CRASH_JPUT('t');
        else if (*p < 0x20 || *p == 0x7f) {
            char u[8];
            snprintf(u, sizeof u, "\\u%04x", *p);
            for (const char *q = u; *q; q++) HOS_CRASH_JPUT(*q);
        } else HOS_CRASH_JPUT((char)*p);
    }
    HOS_CRASH_JPUT('"');
#undef HOS_CRASH_JPUT
    out[o] = 0;
    return o;
}
/* The version as the relay takes it: what is not [0-9A-Za-z.+_-] as _, at most 40; "unknown" for none */
static inline void hos_crash_report_version(char *out, size_t cap, const char *version) {
    size_t o = 0;
    for (const char *p = version; *p && o + 1 < cap && o < HOS_CRASH_MAX_VERSION; p++)
        out[o++] = isalnum((unsigned char)*p) || *p == '.' || *p == '+' || *p == '_' || *p == '-' ? *p : '_';
    out[o] = 0;
    if (!o) snprintf(out, cap, "unknown");
}

/* The helper's own system line, "<cpu> (<n> threads), <ram> GB, <os>" (hos_crash_dialog.c system_text), split into the CPU part
   ("<cpu> (<n> threads), <ram> GB") and the operating system's, for a crash before the game wrote its "report:" lines (the
   renderer writes them once it starts: a crash at the start has none). The split is after the RAM ("... GB, "), else after the
   threads' parenthesis; a line with neither is all CPU. Both parts are "" when the line is empty */
static inline void hos_crash_system_split(const char *system, char *cpu, size_t ccap, char *os, size_t ocap) {
    if (ccap) cpu[0] = 0;
    if (ocap) os[0] = 0;
    if (!system || !system[0]) return;
    const char *end = NULL, *rest = NULL;   /* (where the CPU part ends, where the OS part starts) */
    for (const char *p = strstr(system, " GB, "); p && !end; p = strstr(p + 1, " GB, ")) {
        const char *q = p;
        while (q > system && isdigit((unsigned char)q[-1])) q--;
        if (q < p && q > system && q[-1] == ' ') end = p + 3, rest = p + 5;   /* (a number before " GB, ": the RAM) */
    }
    if (!end) {
        const char *th = strstr(system, " threads), ");
        if (th) end = th + 9, rest = th + 11;
    }
    if (ccap) snprintf(cpu, ccap, "%.*s", (int)(end ? (size_t)(end - system) : strlen(system)), system);
    if (rest && ocap) snprintf(os, ocap, "%s", rest);
}

/* The answer to the send, from the HTTP status (0: no answer: curl could not reach the server, or did not run) and the relay's
   JSON ({"comment": link} or {"error": why}), as the Sent page or the Not sent page says it: 1 sent (*link: the report's own
   comment on the crash issue when the answer names one there, else the issue itself), 0 not (*again: whether trying again
   could help: the server unreachable or busy, not a refusal) */
static inline int hos_crash_json_field(const char *json, const char *key, char *out, size_t cap) {
    char want[64];
    snprintf(want, sizeof want, "\"%s\"", key);
    const char *at = json ? strstr(json, want) : NULL;
    if (cap) out[0] = 0;
    if (!at) return 0;
    at += strlen(want);
    while (*at == ' ' || *at == ':') at++;
    if (*at++ != '"') return 0;
    size_t o = 0;
    for (; *at && *at != '"' && o + 1 < cap; at++) {
        if (*at == '\\' && at[1]) at++;
        out[o++] = *at;
    }
    out[o] = 0;
    return *at == '"';
}
static inline int hos_crash_answer(char *out, size_t cap, int status, const char *body, const char *issue, char *link,
                                   size_t link_cap, int *again) {
    char said[300], comment[300];
    *again = 0;
    if (link_cap) link[0] = 0;
    if (status >= 200 && status < 300) {
        hos_crash_json_field(body, "comment", comment, sizeof comment);
        const size_t in = issue ? strlen(issue) : 0;
        int own = in && !strncmp(comment, issue, in) && !strncmp(comment + in, "#issuecomment-", 14) && comment[in + 14];
        for (const char *d = comment + in + 14; own && *d; d++) own = isdigit((unsigned char)*d);
        snprintf(link, link_cap, "%s", own ? comment : issue ? issue : "");
        snprintf(out, cap, "Sent. Thank you: the team sees it%s.%s", link[0] ? " on the crash reports' issue" : "",
                 link[0] ? " Open it to follow the crash there." : "");
        return 1;
    }
    if (status == 429) {
        *again = 1;
        snprintf(out, cap, "Not sent: the crash server is busy. Nothing was sent: try again in a minute.");
    } else if (status >= 400 && status < 500) {
        hos_crash_json_field(body, "error", said, sizeof said);
        snprintf(out, cap, "Not sent: the crash server refused the report (%d%s%s).", status, said[0] ? ": " : "", said);
    } else if (status) {
        *again = 1;
        snprintf(out, cap, "Not sent: the crash server answered %d. Nothing was sent: try again later.", status);
    } else {
        *again = 1;
        snprintf(out, cap, "Not sent: couldn't reach the crash server (no internet, or curl is missing). Nothing was sent.");
    }
    return 0;
}
/* The status of the last answer in curl's --dump-header file (a redirect's answers come first): 0 without one */
static inline int hos_crash_http_status(const char *headers) {
    int status = 0;
    for (const char *p = headers; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (!strncmp(p, "HTTP/", 5)) {
            const char *sp = strchr(p, ' ');
            if (sp && isdigit((unsigned char)sp[1])) status = atoi(sp + 1);
        }
    return status;
}
