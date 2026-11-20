/* SPDX-License-Identifier: GPL-3.0-or-later */
/* (heylisten) The window right after a crash, at start or in play: "HeyListen closed unexpectedly", when, why where it is known,
   and Report automatically, Report manually, Open the log folder, Close. Report automatically shows the report it would post
   to the HeyListen telemetry server's /telemetry/crash and posts it with the system's curl only when the player presses Send. Report manually
   puts a redacted plain-text report on the clipboard, writes it next to the crash records
   (bug-reports/pending/report-<UTC>.txt) and opens GitHub's new-issue page in the browser with the report's main parts in its
   body (hos_crash_issue_url: url-encoded, within HOS_CRASH_LINK_MAX, the newest log lines last), so nothing depends on a
   paste. Nothing is sent without Send. What decides is host/hos_crash_dialog.h.

   It is shown by a helper the game starts early (hos_crash_watch_start, after the crash record is set up and before the
   guest, the renderer, the sound and the input): the same executable, `HeyListen --crash-watch <pid> <run id> <game folder>
   <log>`, which main() hands here first thing (hos_crash_helper_main), so the helper never starts any of the game. It waits
   for the game's process to end (Windows: its handle, with its exit code; Linux: a pidfd, else its parent changing), reads
   the records and the log, and shows the window at once, or ends quietly. A start that finds a crash record of an earlier run
   that no helper showed (bug-reports/shown.txt lists those shown) shows it first, through a helper it waits for
   (`--crash-show`). Not for --prepare-shaders, HOS_HEADLESS, HOS_PGO, HOS_CRASH_WATCH=0 or under gamescope
   (hos_crash_watch_wanted).

   The window is the crash window drawn as the Insert menu is (gpu/vk/vk_crash_window.cpp: Dear ImGui through SDL's software
   renderer, SDL3 or SDL2, loaded here: the helper loads nothing else of the game's and never Vulkan). Without it, SDL's
   message box, and Windows' MessageBoxW without SDL; the clipboard and the browser are Windows' own there; on Linux the clipboard is wl-copy, xclip or
   xsel's (SDL's X11 one the last resort) and the browser SDL's or xdg-open.
   HOS_CRASH_TEST=start (just after the crash handlers are set up) or =<seconds> (that long after) crashes the game on
   purpose, from the environment only (not hookshot_env.txt), to see the window. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "hos_crash_dialog.h"
#include "hos_crash_record.h"
#include "hos_telemetry.h"
#include "hos_perf_send.h"
#include "../gpu/vk/crash_window_api.h"
#include "../gpu/vk/compat/hookshot_env.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <objbase.h>
#include <shellapi.h>
#else
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

enum { PATH_MAX_UTF8 = 4096 };
#ifdef HOS_CRASH_DIALOG_TEST   /* (tests/test_crash_dialog.py: the window, the clipboard and the browser are the test's) */
int hos_crash_test_box(const char *title, const char *text, const char *const *buttons, int n);
int hos_crash_test_clipboard(const char *text);
int hos_crash_test_open(const char *what);
#endif

/* ---- files, by UTF-8 paths ---- */
#ifdef _WIN32
static wchar_t *wide_of(const char *s) {   /* (malloc'd; NULL when not UTF-8) */
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof *w) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) free(w), w = NULL;
    return w;
}
static char *utf8_of(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = n > 0 ? (char *)malloc((size_t)n) : NULL;
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) != n) free(s), s = NULL;
    return s;
}
#endif
static FILE *open_utf8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *w = wide_of(path), m[8];
    size_t i = 0;
    for (; mode[i] && i + 1 < 8; i++) m[i] = (wchar_t)mode[i];
    m[i] = 0;
    FILE *f = w ? _wfopen(w, m) : NULL;
    free(w);
    return f;
#else
    return fopen(path, mode);
#endif
}
static int join(char *out, size_t cap, const char *a, const char *b) {
    const int n = snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (size_t)n < cap;
}
/* A file's first max bytes, ended by a NUL (malloc'd), NULL when it cannot be read */
static char *read_text(const char *path, size_t max, size_t *len) {
    FILE *f = open_utf8(path, "rb");
    if (!f) return NULL;
    char *b = (char *)malloc(max + 1);
    size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if (!b) return NULL;
    b[n] = 0;
    if (len) *len = n;
    return b;
}
static int write_text(const char *path, const char *text, size_t n) {
    FILE *f = open_utf8(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static void make_dir(const char *path) {   /* (one level: its parent is there) */
#ifdef _WIN32
    wchar_t *w = wide_of(path);
    if (w) CreateDirectoryW(w, NULL);
    free(w);
#else
    mkdir(path, 0755);
#endif
}
static int file_exists(const char *path) {
#ifdef _WIN32
    wchar_t *w = wide_of(path);
    const DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}
static int remove_utf8(const char *path) {
#ifdef _WIN32
    wchar_t *w = wide_of(path);
    const int ok = w && DeleteFileW(w);
    free(w);
    return ok;
#else
    return unlink(path) == 0;
#endif
}
/* A file replaced whole: written beside it under a name of this process's, then renamed over it, so a reader meanwhile has
   the old text or the new, never an empty or a half-written file */
static int replace_text(const char *path, const char *text, size_t n) {
    char temp[PATH_MAX_UTF8 + 32];
#ifdef _WIN32
    snprintf(temp, sizeof temp, "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
#else
    snprintf(temp, sizeof temp, "%s.%ld.tmp", path, (long)getpid());
#endif
    if (!write_text(temp, text, n)) return remove_utf8(temp), 0;
#ifdef _WIN32
    wchar_t *from = wide_of(temp), *to = wide_of(path);
    const int ok = from && to && MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    free(from), free(to);
#else
    const int ok = rename(temp, path) == 0;
#endif
    if (!ok) remove_utf8(temp);
    return ok;
}
static void utc_now(char out[17]) {
    const time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(out, 17, "%Y%m%dT%H%M%SZ", &tm);
}
static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
#endif
}
/* Each name in a folder (UTF-8), in turn */
static void each_name(const char *dir, void (*take)(const char *name, void *user), void *user) {
#ifdef _WIN32
    char pattern[PATH_MAX_UTF8];
    if (!join(pattern, sizeof pattern, dir, "*")) return;
    wchar_t *w = wide_of(pattern);
    WIN32_FIND_DATAW d;
    HANDLE h = w ? FindFirstFileW(w, &d) : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *name = utf8_of(d.cFileName);
        if (name && !(d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) take(name, user);
        free(name);
    } while (FindNextFileW(h, &d));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') take(e->d_name, user);
    closedir(d);
#endif
}

/* ---- a growing text ---- */
typedef struct Buf { char *s; size_t n, cap; } Buf;
static void put_n(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (b->n + n + 1 > cap) cap *= 2;
        char *more = (char *)realloc(b->s, cap);
        if (!more) return;
        b->s = more, b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void put(Buf *b, const char *s) { put_n(b, s, strlen(s)); }
static void putf(Buf *b, const char *fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) put_n(b, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

/* ---- who is to be left out: the home folder and the user's name (the Insert menu's Bug report's fill_here) ---- */
typedef struct Who { char home[PATH_MAX_UTF8], user[256]; int windows; } Who;
static void who_here(Who *w) {
    memset(w, 0, sizeof *w);
#ifdef _WIN32
    w->windows = 1;
    const wchar_t *h = _wgetenv(L"USERPROFILE"), *u = _wgetenv(L"USERNAME");
    char *hs = h ? utf8_of(h) : NULL, *us = u ? utf8_of(u) : NULL;
    if (hs) snprintf(w->home, sizeof w->home, "%s", hs);
    if (us) snprintf(w->user, sizeof w->user, "%s", us);
    free(hs), free(us);
#else
    const struct passwd *pw = getpwuid(getuid());
    const char *h = getenv("HOME"), *u = getenv("USER");
    snprintf(w->home, sizeof w->home, "%s", h && *h ? h : pw && pw->pw_dir ? pw->pw_dir : "");
    snprintf(w->user, sizeof w->user, "%s", u && *u ? u : pw && pw->pw_name ? pw->pw_name : "");
#endif
}
static char *redacted(const Who *w, const char *s, size_t n) {   /* (malloc'd) */
    const size_t cap = n * 3 + 64;   /* (each byte at most "<user>" for a name of 2 or more: never longer than 3x and a home) */
    char *out = (char *)malloc(cap);
    if (out) hos_crash_redact_line(out, cap, s, n, w->home, w->user, w->windows);
    return out;
}

/* ---- the run's log ---- */
typedef struct RunLog {
    HosCrashLog facts;
    char *lines[HOS_CRASH_LOG_LINES];   /* (the last ones, redacted, a ring) */
    int count, next, found;
    char path[PATH_MAX_UTF8];
} RunLog;
static int log_is_run(const char *path, const char *run_id) {
    char *head = read_text(path, 8192, NULL);
    char want[48];
    snprintf(want, sizeof want, "run_id: %s", run_id);
    const int yes = head && strstr(head, want) != NULL;
    free(head);
    return yes;
}
/* This run's log: the path it named, else its first rotated name (hookshot_log.1.txt: a start since moved it), when its run
   id is the one wanted */
static int find_run_log(RunLog *r, const char *log, const char *run_id) {
    if (!log || !run_id || !run_id[0]) return 0;
    if (log_is_run(log, run_id)) return snprintf(r->path, sizeof r->path, "%s", log), 1;
    const char *dot = strrchr(log, '.'), *sep = strrchr(log, '/');
    const char *bsep = strrchr(log, '\\');
    if (bsep > sep) sep = bsep;
    if (dot && (!sep || dot > sep)) {
        snprintf(r->path, sizeof r->path, "%.*s.1%s", (int)(dot - log), log, dot);
        if (log_is_run(r->path, run_id)) return 1;
    }
    r->path[0] = 0;
    return 0;
}
static void read_log(RunLog *r, const Who *who) {
    FILE *f = r->path[0] ? open_utf8(r->path, "rb") : NULL;
    if (!f) return;
    r->found = 1;
    char line[4096];
    /* (the whole log for what it says, up to 256 MB; its last lines kept) */
    for (uint64_t taken = 0; taken < (256ull << 20) && fgets(line, sizeof line, f);) {
        const size_t n = strlen(line);
        taken += n;
        char *clean = redacted(who, line, n);
        if (!clean) continue;
        hos_crash_log_line(&r->facts, clean);
        free(r->lines[r->next]);
        r->lines[r->next] = clean;
        r->next = (r->next + 1) % HOS_CRASH_LOG_LINES;
        if (r->count < HOS_CRASH_LOG_LINES) r->count++;
    }
    fclose(f);
}

/* ---- the records ---- */
typedef struct Found { const char *pending, *run_id; char name[128]; char *text; int kind; } Found;
static void take_record(const char *name, void *user) {
    Found *f = (Found *)user;
    const int kind = hos_crash_record_name(name, NULL);
    if (!kind || (f->kind == 1 && kind == 2)) return;   /* (a crash record wins over an unclean end) */
    char path[PATH_MAX_UTF8], want[48];
    if (!join(path, sizeof path, f->pending, name)) return;
    char *text = read_text(path, 64 << 10, NULL);
    snprintf(want, sizeof want, "\nrun_id: %s\n", f->run_id);
    if (text && strstr(text, want)) {
        free(f->text);
        f->text = text, f->kind = kind;
        snprintf(f->name, sizeof f->name, "%s", name);
        return;
    }
    free(text);
}
static int pending_of(const char *dir, char *out, size_t cap) {
    char root[PATH_MAX_UTF8];
    return join(root, sizeof root, dir, "bug-reports") && join(out, cap, root, "pending");
}
/* This run's record, crash before unclean: its text (malloc'd), NULL when it has none */
static char *run_record(const char *dir, const char *run_id, char *name, size_t cap) {
    char pending[PATH_MAX_UTF8];
    Found f = {pending, run_id, "", NULL, 0};
    if (!pending_of(dir, pending, sizeof pending)) return NULL;
    each_name(pending, take_record, &f);
    snprintf(name, cap, "%s", f.name);
    return f.text;
}

/* ---- the ledger of records shown: bug-reports/shown.txt ---- */
static int ledger_path(const char *dir, char *out, size_t cap) {
    char root[PATH_MAX_UTF8];
    return join(root, sizeof root, dir, "bug-reports") && join(out, cap, root, "shown.txt");
}
typedef struct Keep { Buf *out; const char *old; } Keep;
static void keep_listed(const char *name, void *user) {
    Keep *k = (Keep *)user;
    if (hos_crash_record_name(name, NULL) && (!k->old || hos_crash_listed(k->old, name))) put(k->out, name), put(k->out, "\n");
}
/* Adds names to the ledger (a NULL name: every record now pending), keeping only the names still pending */
static int ledger_add(const char *dir, const char *name, int all) {
    char path[PATH_MAX_UTF8], pending[PATH_MAX_UTF8];
    if (!ledger_path(dir, path, sizeof path) || !pending_of(dir, pending, sizeof pending)) return 0;
    char *old = read_text(path, 64 << 10, NULL);
    Buf out = {0};
    put(&out, "");
    Keep k = {&out, all || !old ? NULL : old};   /* (no ledger yet: what was pending is not news) */
    each_name(pending, keep_listed, &k);
    if (name && !hos_crash_listed(out.s, name)) put(&out, name), put(&out, "\n");
    const int ok = replace_text(path, out.s ? out.s : "", out.n);
    free(old), free(out.s);
    return ok;
}
typedef struct Newest { const char *shown, *pending; int64_t now; char name[128]; } Newest;
static void newest_unshown(const char *name, void *user) {
    Newest *n = (Newest *)user;
    int oom = 0;
    char path[PATH_MAX_UTF8];
    if (hos_crash_record_name(name, NULL) == 2 && join(path, sizeof path, n->pending, name)) {   /* (unclean: out of memory?) */
        char *text = read_text(path, 64 << 10, NULL);
        HosCrashRecord r;
        hos_crash_record_parse(text ? text : "", &r);
        oom = r.oom;
        free(text);
    }
    if (hos_crash_show_at_start(name, n->shown, n->now, oom) && (!n->name[0] || strcmp(name, n->name) > 0))
        snprintf(n->name, sizeof n->name, "%s", name);
}

/* ---- this PC, as the issue form asks: "CPU (threads), RAM, system" (the menu's system_text) ---- */
static void system_text(char *out, size_t cap) {
    char cpu[64] = "";
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
    unsigned r[12] = {0};
#ifdef _WIN32
    int regs[4];
    __cpuid(regs, (int)0x80000000);
    if ((unsigned)regs[0] >= 0x80000004u)
        for (int k = 0; k < 3; k++) {
            __cpuid(regs, (int)(0x80000002u + (unsigned)k));
            for (int j = 0; j < 4; j++) r[k * 4 + j] = (unsigned)regs[j];
        }
#else
    unsigned top, ebx, ecx, edx;
    if (__get_cpuid(0x80000000u, &top, &ebx, &ecx, &edx) && top >= 0x80000004u)
        for (unsigned k = 0; k < 3; k++) __get_cpuid(0x80000002u + k, &r[k * 4], &r[k * 4 + 1], &r[k * 4 + 2], &r[k * 4 + 3]);
#endif
    memcpy(cpu, r, 48);
    cpu[48] = 0;
#endif
    char *cpu_name = cpu;   /* (the brand string, its spaces around it taken off) */
    while (*cpu_name == ' ') cpu_name++;
    for (size_t n = strlen(cpu_name); n && cpu_name[n - 1] == ' ';) cpu_name[--n] = 0;
    unsigned long long ram = 0;
    long threads = 0;
    char os[256] = "";
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) ram = m.ullTotalPhys;
    threads = (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    typedef LONG(WINAPI * GetVersionFn)(OSVERSIONINFOW *);
    typedef const char *(__cdecl * WineFn)(void);
    const HMODULE nt = GetModuleHandleA("ntdll.dll");
    const GetVersionFn get = nt ? (GetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (get && get(&v) == 0)
        snprintf(os, sizeof os, "%s (build %lu)",
                 v.dwMajorVersion == 10 ? (v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10") : "Windows", v.dwBuildNumber);
    else snprintf(os, sizeof os, "Windows");
    const WineFn wine = nt ? (WineFn)(void *)GetProcAddress(nt, "wine_get_version") : NULL;
    if (wine) snprintf(os + strlen(os), sizeof os - strlen(os), ", Wine %s", wine());
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0) ram = (unsigned long long)pages * (unsigned long long)page;
    threads = sysconf(_SC_NPROCESSORS_ONLN);
    for (int k = 0; k < 2 && !os[0]; k++) {
        FILE *f = fopen(k ? "/usr/lib/os-release" : "/etc/os-release", "r");
        char line[256];
        while (f && fgets(line, sizeof line, f))
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *v = line + 12;
                v[strcspn(v, "\r\n")] = 0;
                size_t n = strlen(v);
                if (n >= 2 && (v[0] == '"' || v[0] == '\'')) v[n - 1] = 0, v++;
                snprintf(os, sizeof os, "%s", v);
            }
        if (f) fclose(f);
    }
    if (!os[0]) snprintf(os, sizeof os, "Linux");
    struct utsname u;
    if (!uname(&u)) snprintf(os + strlen(os), sizeof os - strlen(os), " (%s %s)", u.sysname, u.release);
    const char *desktop = getenv("XDG_CURRENT_DESKTOP"), *session = getenv("XDG_SESSION_TYPE");
    if (desktop && *desktop)
        snprintf(os + strlen(os), sizeof os - strlen(os), ", %s%s%s%s", desktop, session && *session ? " (" : "",
                 session && *session ? session : "", session && *session ? ")" : "");
#endif
    char gb[32] = "";
    if (ram) snprintf(gb, sizeof gb, "%llu GB, ", (ram + (1ull << 29)) >> 30);
    snprintf(out, cap, "%s (%ld threads), %s%s", *cpu_name ? cpu_name : "an unknown CPU", threads, gb, os);
}
/* What the game was built for: this PC's own CPU (nx2/build.py, -march=native and HOOKSHOT_NATIVE_CPU), or a CPU level */
#define HOS_CRASH_TEXT_(...) #__VA_ARGS__
#define HOS_CRASH_TEXT(...) HOS_CRASH_TEXT_(__VA_ARGS__)
static const char *built_for(void) {
#ifdef HOOKSHOT_NATIVE_CPU
    return "this PC's CPU (" HOS_CRASH_TEXT(HOOKSHOT_NATIVE_CPU) ")";
#elif defined(__AVX2__)
    return "x86-64-v3";
#else
    return "an older CPU level (x86-64 or x86-64-v2: no AVX2)";
#endif
}
static const char *channel_of(const char *version) {
    if (strstr(version, "nightly")) return "Nightly";
    if (strstr(version, "-rc.")) return "Release candidate, or built from source";
    const char *p = version;
    if (*p++ != 'v') return "Built from source, or not known";
    for (int part = 0; part < 3; part++) {
        if (!(*p >= '0' && *p <= '9')) return "Built from source, or not known";
        while (*p >= '0' && *p <= '9') p++;
        if (part < 2 && *p++ != '.') return "Built from source, or not known";
    }
    return !*p || *p == ' ' ? "Release" : "Built from source, or not known";
}

/* ---- the report ---- */
typedef struct Report {
    int verdict, stage, cause;
    const char *record, *record_name, *dir, *options;   /* (options: NULL, the game folder's) */
    const RunLog *log;
    const Who *who;
    char system[400], gpu[400], version[160];
    /* (Report automatically's card: the version without its source, whether it is a release build, the settings' count and the
       changed ones as key=value lines) */
    char bare_version[128], changed_list[1200];
    int release, settings, changed;
} Report;
static int cmp_text(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* The game's switches and what loads code into it, NAME=VALUE, a value only as hos_crash_value_shown allows (the menu's
   switches()) */
static void put_switches(Buf *b) {
    static const char *const prefixes[] = {"RX_", "HOS_", "HL_", "HOOKSHOT_", "EDEN_", "NVN_", "SDL_"};
    static const char *const hooks[] = {"VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_DRIVER_FILES", "VK_ICD_FILENAMES",
                                        "ENABLE_VKBASALT", "MANGOHUD", "LD_PRELOAD"};
    char *lines[512];
    int n = 0;
#ifdef _WIN32
    wchar_t *block = GetEnvironmentStringsW();
    for (const wchar_t *e = block; e && *e && n < 512; e += wcslen(e) + 1) {
        char *kv = utf8_of(e);
#else
    for (char **e = environ; e && *e && n < 512; e++) {
        char *kv = strdup(*e);
#endif
        char *eq = kv ? strchr(kv, '=') : NULL;
        int take = 0;
        if (eq && eq != kv) {
            *eq = 0;
            for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) take |= !strncmp(kv, prefixes[i], strlen(prefixes[i]));
            for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) take |= !strcmp(kv, hooks[i]);
        }
        if (take) {
            const size_t cap = strlen(kv) + strlen(eq + 1) + 32;
            lines[n] = (char *)malloc(cap);
            if (lines[n]) snprintf(lines[n], cap, "%s=%s", kv, hos_crash_value_shown(kv, eq + 1) ? eq + 1 : "(value left out)"), n++;
        }
        free(kv);
    }
#ifdef _WIN32
    if (block) FreeEnvironmentStringsW(block);
#endif
    qsort(lines, (size_t)n, sizeof lines[0], cmp_text);
    if (!n) put(b, "Switches set: none.\n");
    else put(b, "Switches set (the environment and hookshot_env.txt; a value only when it is a number or on/off, and never for a "
                "name with TOKEN, KEY, ROOM, SECRET, PASS, SERVER or USER in it):\n");
    for (int i = 0; i < n; i++) put(b, lines[i]), put(b, "\n"), free(lines[i]);
}
/* hookshot_options.ini's settings, key=value, a value only when it is a number, a size or on/off (a device's name is left
   out); not the controls' bindings, nor the file itself */
static void put_options(Buf *b, const char *options, const char *dir) {
    char path[PATH_MAX_UTF8];   /* (the run's, in the user folder: host/options_path.c; not given, the game folder's) */
    char *text = options ? read_text(options, 256 << 10, NULL)
                         : join(path, sizeof path, dir, "hookshot_options.ini") ? read_text(path, 256 << 10, NULL) : NULL;
    if (!text) {
        put(b, "hookshot_options.ini: none (every Insert menu option at its default).\n");
        return;
    }
    put(b, "hookshot_options.ini (a value only when it is a number or on/off; the controls' bindings left out):\n");
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        int name_ok = 1;
        for (const char *p = line; *p; p++) name_ok &= (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (!name_ok || !strncmp(line, "pad_", 4) || !strncmp(line, "key_", 4) || !strncmp(line, "toggle_", 7)) continue;
        putf(b, "%s=%s\n", line, hos_crash_value_shown(NULL, eq + 1) ? eq + 1 : "(value left out)");
    }
    free(text);
}
/* The settings as Report automatically counts them (put_settings): hookshot_options.ini's, or the defaults
   when there is none yet; changed when hookshot_defaults.ini has the key with another value. The changed ones go to list as
   key=value lines, as many as fit */
static void count_settings(Report *in) {
    enum { MAX = 512 };
    static char keys[MAX][64], values[MAX][32];
    char path[PATH_MAX_UTF8], key[64], value[32];
    int nd = 0;
    char *defaults = join(path, sizeof path, in->dir, "hookshot_defaults.ini") ? read_text(path, 256 << 10, NULL) : NULL;
    for (char *line = defaults; line && *line && nd < MAX;) {
        if (hos_crash_setting(line, keys[nd], sizeof keys[nd], values[nd], sizeof values[nd])) nd++;
        line = strchr(line, '\n');
        if (line) line++;
    }
    /* (the run's file, as put_options reads it: in the user folder since host/options_path.c; not given, the game folder's) */
    char *current = in->options ? read_text(in->options, 256 << 10, NULL)
                    : join(path, sizeof path, in->dir, "hookshot_options.ini") ? read_text(path, 256 << 10, NULL) : NULL;
    if (!current) in->settings = nd;
    size_t used = 0;
    for (char *line = current; line && *line;) {
        if (hos_crash_setting(line, key, sizeof key, value, sizeof value)) {
            in->settings++;
            for (int i = 0; i < nd; i++)
                if (!strcmp(keys[i], key)) {
                    if (strcmp(values[i], value)) {
                        in->changed++;
                        const int n = snprintf(in->changed_list + used, sizeof in->changed_list - used, "%s=%s\n", key, value);
                        if (n > 0 && used + (size_t)n < sizeof in->changed_list) used += (size_t)n;
                        else in->changed_list[used] = 0;
                    }
                    break;
                }
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    free(defaults), free(current);
}
typedef struct Recent { int64_t now; int count; } Recent;
static void count_recent(const char *name, void *user) {
    Recent *r = (Recent *)user;
    char when[17];
    if (hos_crash_record_name(name, when) && r->now - hos_crash_epoch(when) <= 86400) r->count++;
}
static char *report_text(const Report *in, size_t *len) {
    Buf b = {0};
    char now[17];
    utc_now(now);
    putf(&b, "HeyListen crash report, made by the window HeyListen shows after a crash (%s UTC). HeyListen sent nothing: paste "
             "this into the issue form, and attach the game folder's logs/hookshot_log.txt if you can.\n\n", now);
    put(&b, "### What happened\n\n");
    char message[1024];
    hos_crash_message(message, sizeof message, in->verdict, in->stage, in->cause, 0);
    message[strcspn(message, "\n")] = 0;
    putf(&b, "%s\nWhat went wrong: %s\n", message, hos_crash_cause_text(in->cause));
    HosCrashRecord r;
    hos_crash_record_parse(in->record ? in->record : "", &r);
    if (r.uptime[0] && strcmp(r.uptime, "unknown")) putf(&b, "It had run for %s s.\n", r.uptime);
    char pending[PATH_MAX_UTF8];
    Recent recent = {(int64_t)time(NULL), 0};
    if (pending_of(in->dir, pending, sizeof pending)) each_name(pending, count_recent, &recent);
    putf(&b, "Crash and unclean-end records in the last day: %d.\n\n", recent.count);
    /* (the same fields as Report automatically's card, worded as it words them) */
    const HosCrashLog *f = &in->log->facts;
    put(&b, "### Summary (what Report automatically sends, too)\n\n");
    putf(&b, "Version: %s\n", in->bare_version[0] ? in->bare_version : "not known");
    putf(&b, "Release build: %s\n", in->release ? "Yes" : "No (a local or modified build)");
    putf(&b, "Operating system: %s\n", f->os[0] ? f->os : "not known");
    putf(&b, "Renderer: %s\n", f->renderer[0] ? f->renderer : "not known");
    putf(&b, "Graphics card: %s\n", in->gpu);
    putf(&b, "CPU and RAM: %s\n", f->cpu[0] ? f->cpu : "not known");
    putf(&b, "Map: %s\n", f->place[0] ? f->place : in->log->found ? HOS_CRASH_TITLE_AND_INTRO : "not known");
    putf(&b, "Settings: %d, %d changed from the defaults\n", in->settings, in->changed);
    putf(&b, "Crash: %s\n", f->crash[0] ? f->crash : in->record ? "unknown" : "none (no crash record: an unclean end)");
    putf(&b, "Log: %s\n\n", in->log->found ? "its end, below" : "not found");
    putf(&b, "### HeyListen version\n\n%s\nChannel (from the version): %s\n\n", in->version, channel_of(in->version));
    putf(&b, "### Graphics card and driver version\n\n%s\n", in->gpu);
    if (in->log->facts.devices[0]) putf(&b, "Devices the log lists: %s\n", in->log->facts.devices);
    putf(&b, "\n### CPU, RAM and operating system\n\n%s\nBuilt for: %s\n\n", in->system, built_for());
    put(&b, "### Upscalers (the log's first line of each)\n\n");
    putf(&b, "%s\n%s\n\n", in->log->facts.dlss[0] ? in->log->facts.dlss : "NVIDIA DLSS: not named in the log",
         in->log->facts.fsr4[0] ? in->log->facts.fsr4 : "AMD FSR 4: not named in the log");
    put(&b, "### Changes from the defaults\n\n");
    put_options(&b, in->options, in->dir);
    put_switches(&b);
    putf(&b, "\n### Crash record%s%s\n\n", in->record_name && in->record_name[0] ? ": " : "",
         in->record_name ? in->record_name : "");
    if (in->record) {   /* (already private-free: hos_crash_record.c writes fixed labels, module names and registers only) */
        put(&b, "```\n");
        put(&b, in->record);
        if (b.n && b.s[b.n - 1] != '\n') put(&b, "\n");
        put(&b, "```\n\n");
    } else {
        put(&b, "None: the game's crash handler wrote no record (it stopped before the game could run, or ended normally).\n\n");
    }
    if (in->log->found) {
        putf(&b, "### The end of hookshot_log.txt\n\nIts last %d lines (the home folder written %s, the user's name <user>):\n\n```\n",
             in->log->count, in->who->windows ? "%USERPROFILE%" : "~");
        for (int i = 0; i < in->log->count; i++) {
            const char *line = in->log->lines[(in->log->next - in->log->count + i + HOS_CRASH_LOG_LINES) % HOS_CRASH_LOG_LINES];
            put(&b, line);
            if (!*line || line[strlen(line) - 1] != '\n') put(&b, "\n");
        }
        put(&b, "```\n\n");
    } else {
        put(&b, "### hookshot_log.txt\n\nNot found: the game logged to no file (started from a terminal or by a program that "
                "reads its output), or its log was moved since.\n\n");
    }
    put(&b, "### Not in this report\n\nSaves, game files, the game's executable and caches, memory dumps, keys, the options file "
            "itself and the path of your home folder.\n");
    if (len) *len = b.n;
    return b.s;
}

/* ---- SDL's message box, clipboard and links (loaded here, not through host/hos_sdl.c: the helper starts nothing else) ---- */
static struct {
    int version;   /* 3, 2 or 0 */
    void *lib;     /* (the library: the crash window takes its functions from it, gpu/vk/vk_crash_window.cpp) */
    void *ShowMessageBox, *SetClipboardText, *OpenURL, *InitSubSystem, *PumpEvents;
} sdl;
static void *sdl_lib(const char *name) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4], wname[64];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 4);
    while (n && exe[n - 1] != L'\\' && exe[n - 1] != L'/') n--;
    size_t k = 0;
    for (; name[k] && k + 1 < 64; k++) wname[k] = (wchar_t)name[k];
    wname[k] = 0;
    HMODULE m = NULL;
    if (n && n + k + 1 < MAX_PATH * 4) {
        memcpy(exe + n, wname, (k + 1) * sizeof *exe);
        m = LoadLibraryW(exe);
    }
    return m ? (void *)m : (void *)LoadLibraryExW(wname, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    char exe[PATH_MAX_UTF8];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 64);
    void *m = NULL;
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) strcpy(slash + 1, name), m = dlopen(exe, RTLD_NOW | RTLD_LOCAL);
    }
    return m ? m : dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void *sdl_fn(void *lib, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}
static int sdl_load(void) {
    static int tried;
    if (tried) return sdl.version;
    tried = 1;
    const char *want = getenv("HOS_SDL_VERSION");
#ifdef _WIN32
    const char *names[2] = {"SDL3.dll", "SDL2.dll"};
#else
    const char *names[2] = {"libSDL3.so.0", "libSDL2-2.0.so.0"};
#endif
    for (int k = want && atoi(want) == 2 ? 1 : 0; k < 2 && !sdl.version; k++) {
        void *lib = sdl_lib(names[k]);
        if (!lib) continue;
        sdl.ShowMessageBox = sdl_fn(lib, "SDL_ShowMessageBox");
        sdl.SetClipboardText = sdl_fn(lib, "SDL_SetClipboardText");
        sdl.OpenURL = sdl_fn(lib, "SDL_OpenURL");   /* (SDL 2.0.14 on) */
        sdl.InitSubSystem = sdl_fn(lib, "SDL_InitSubSystem");
        sdl.PumpEvents = sdl_fn(lib, "SDL_PumpEvents");
        if (sdl.ShowMessageBox && sdl.InitSubSystem) sdl.version = k ? 2 : 3, sdl.lib = lib;
    }
    return sdl.version;
}
typedef struct { uint32_t flags; int buttonid; const char *text; } SdlButton;   /* (SDL3's and SDL2's alike) */
typedef struct {
    uint32_t flags;
    void *window;
    const char *title, *message;
    int numbuttons;
    const SdlButton *buttons;
    const void *colors;
} SdlBox;
/* The button clicked, by its index (the first is the Enter key's, the last Escape's), -1 when the box was closed otherwise, -2
   when it could not be shown */
static int sdl_box(const char *title, const char *text, const char *const *buttons, int n) {
    SdlButton b[4];
    for (int i = 0; i < n && i < 4; i++) b[i].flags = (i == 0 ? 1u : 0u) | (i == n - 1 ? 2u : 0u), b[i].buttonid = i, b[i].text = buttons[i];
    const SdlBox box = {0x10 | 0x80 /* ERROR, buttons left to right */, NULL, title, text, n, b, NULL};
    int hit = -1;
    if (sdl.version == 3) return ((_Bool (*)(const SdlBox *, int *))sdl.ShowMessageBox)(&box, &hit) ? hit : -2;
    return ((int (*)(const SdlBox *, int *))sdl.ShowMessageBox)(&box, &hit) == 0 ? hit : -2;
}
/* ---- the crash window: the pages drawn as the Insert menu is (gpu/vk/vk_crash_window.cpp, in hookshot_vk beside this
   executable), through SDL's renderer; SDL's message box when that module or a window can't be had ---- */
static struct { int tried; mkwvk_crash_page_fn page; mkwvk_crash_close_fn close; int open; } window;
static void *sdl_sym(const char *name) { return sdl_fn(sdl.lib, name); }
static int window_load(void) {   /* (1 when the module has the window: whether a window opens is the first page's to find) */
#ifdef HOS_CRASH_DIALOG_TEST
    return 0;
#endif
    if (!window.tried) {
        window.tried = 1;
#ifdef _WIN32
        void *m = sdl_load() ? sdl_lib("hookshot_vk.dll") : NULL;
#else
        void *m = sdl_load() ? sdl_lib("hookshot_vk.so") : NULL;
#endif
        window.page = m ? (mkwvk_crash_page_fn)sdl_fn(m, "mkwvk_crash_page") : NULL;
        window.close = m ? (mkwvk_crash_close_fn)sdl_fn(m, "mkwvk_crash_close") : NULL;
    }
    return window.page != NULL;
}
static int window_page(const char *title, const char *text, const char *block, const char *const *buttons, int n) {
    if (!window_load()) return -2;
    /* (the banner's heading: the title without "HeyListen: ", its first letter a capital) */
    char heading[160];
    snprintf(heading, sizeof heading, "%s", !strncmp(title, "HeyListen: ", 10) ? title + 10 : title);
    if (heading[0] >= 'a' && heading[0] <= 'z') heading[0] = (char)(heading[0] - 'a' + 'A');
    const MkwCrashSdl with = {sdl.version, sdl_sym};
    const MkwCrashPage page = {title, heading, text, block, buttons, n};
    const int hit = window.page(&with, &page);
    if (hit != -2) window.open = 1;
    else window.page = NULL;   /* (no window: the message boxes from now on) */
    return hit;
}
static void window_close(void) {
    if (window.open && window.close) window.close();
    window.open = 0;
}
/* A page: the button pressed, by its index (the first is the Enter key's, the last Escape's), -1 when the window was closed
   otherwise, -2 when nothing could be shown. block: text shown as it is below the page's (in a box of its own in the window;
   after the text in a message box) */
static int ui_page(const char *title, const char *text, const char *block, const char *const *buttons, int n) {
#ifdef HOS_CRASH_DIALOG_TEST
    if (block) {
        Buf t = {0};
        put(&t, text), put(&t, "\n\n"), put(&t, block);
        const int hit = hos_crash_test_box(title, t.s ? t.s : "", buttons, n);
        free(t.s);
        return hit;
    }
    return hos_crash_test_box(title, text, buttons, n);
#endif
    const int hit = window_page(title, text, block, buttons, n);
    if (hit != -2) return hit;
    Buf t = {0};
    if (block) put(&t, text), put(&t, "\n\n"), put(&t, block), text = t.s ? t.s : text;
    int r = -2;
    if (sdl_load()) r = sdl_box(title, text, buttons, n);
#ifdef _WIN32
    if (r == -2) {   /* (no SDL: Windows' own box, its Yes/No/Cancel standing for the buttons, said in its text) */
        Buf w = {0};
        put(&w, text);
        put(&w, "\n\n");
        static const char *const keys[] = {"Yes", "No", "Cancel"};
        for (int i = 0; i < n && i < 3; i++) putf(&w, "%s%s: %s", i ? ", " : "", n == 1 ? "OK" : keys[i], buttons[i]);
        wchar_t *wt = w.s ? wide_of(w.s) : NULL, *wc = wide_of(title);
        const UINT kind = n >= 3 ? MB_YESNOCANCEL : n == 2 ? MB_YESNO : MB_OK;
        const int k = wt && wc ? MessageBoxW(NULL, wt, wc, kind | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST) : 0;
        free(w.s), free(wt), free(wc);
        r = k == IDYES || k == IDOK ? 0 : k == IDNO ? 1 : k == IDCANCEL ? 2 : k ? -1 : -2;
    }
#endif
    free(t.s);
    return r;   /* (no SDL, no window, not Windows: -2, the game itself would have had none) */
}
static int ui_box(const char *title, const char *text, const char *const *buttons, int n) { return ui_page(title, text, NULL, buttons, n); }

#ifdef _WIN32
/* The report onto the clipboard: 0 not, 1 there for good (the system's or a tool's), 2 only while this helper runs */
static int clipboard(const char *text) {   /* (Windows' own: the text stays on the clipboard after the helper ends) */
#ifdef HOS_CRASH_DIALOG_TEST
    return hos_crash_test_clipboard(text);
#endif
    Buf crlf = {0};
    put(&crlf, "");
    for (const char *p = text; *p; p++) {   /* (Notepad and the like want \r\n) */
        if (*p == '\n' && (p == text || p[-1] != '\r')) put_n(&crlf, "\r", 1);
        put_n(&crlf, p, 1);
    }
    wchar_t *w = crlf.s ? wide_of(crlf.s) : NULL;
    free(crlf.s);
    if (!w) return 0;
    const size_t bytes = (wcslen(w) + 1) * sizeof *w;
    HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    int ok = 0;
    for (int tries = 0; tries < 20 && !ok; tries++) {   /* (another program can hold the clipboard for a moment) */
        if (!OpenClipboard(owner)) {
            sleep_ms(50);
            continue;
        }
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
        void *p = g ? GlobalLock(g) : NULL;
        if (p) {
            memcpy(p, w, bytes);
            GlobalUnlock(g);
            ok = SetClipboardData(CF_UNICODETEXT, g) != NULL;
        }
        if (g && !ok) GlobalFree(g);
        CloseClipboard();
        break;
    }
    if (owner) DestroyWindow(owner);
    free(w);
    return ok;
}
static int open_thing(const char *what) {   /* (a link, a folder or a file: what the system opens it with) */
#ifdef HOS_CRASH_DIALOG_TEST
    return hos_crash_test_open(what);
#endif
    wchar_t *w = wide_of(what);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const int ok = w && (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) > 32;
    free(w);
    return ok;
}
#else
static int video_on;
/* A clipboard tool (hos_crash_clip_tool) given the text on its input: 1 when it took it (it exited with 0, as wl-copy, xclip and
   xsel do once a background copy of theirs serves the selection; or is still running a moment later, serving it itself) */
static int clip_tool(const char *const *argv, const char *text) {
    int in[2];
    if (pipe2(in, O_CLOEXEC)) return 0;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    const int started = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    if (!started) return close(in[1]), 0;
    signal(SIGPIPE, SIG_IGN);   /* (a tool that ends without reading: a failed write, not this helper's end) */
    int wrote = 1;
    for (size_t n = strlen(text), at = 0; at < n;) {
        const ssize_t k = write(in[1], text + at, n - at);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { wrote = 0; break; }
        at += (size_t)k;
    }
    close(in[1]);
    for (int waited = 0; waited < 3000; waited += 20) {   /* (reaped here: no zombie; its background copy is init's) */
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return wrote && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (r < 0 && errno != EINTR) return 0;
        sleep_ms(20);
    }
    return wrote;   /* (still running after 3 s with all of the text: it serves it in the foreground) */
}
/* Wayland's or X11's clipboard, through a tool that keeps it after the helper ends; else X11's through SDL, given while the next
   box is open (hence its pumping then). Never SDL on Wayland: it takes the text only from a focused window, which the helper
   has none of once its box is gone, and says it did anyway */
static int clipboard(const char *text) {
#ifdef HOS_CRASH_DIALOG_TEST
    return hos_crash_test_clipboard(text);
#endif
    const char *wl = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
    const int wayland = wl && *wl, x11 = x && *x;
    for (int i = 0; hos_crash_clip_tool(wayland, x11, i); i++)
        if (clip_tool(hos_crash_clip_tool(wayland, x11, i), text)) return 1;
    if (!x11 || !sdl_load() || !sdl.SetClipboardText || !sdl.PumpEvents) return 0;
    if (!video_on) {
        setenv("SDL_VIDEODRIVER", "x11", 1), setenv("SDL_VIDEO_DRIVER", "x11", 1);
        video_on = sdl.version == 3 ? ((_Bool (*)(uint32_t))sdl.InitSubSystem)(0x20) : ((int (*)(uint32_t))sdl.InitSubSystem)(0x20) == 0;
        if (!video_on) return 0;
    }
    const int ok = sdl.version == 3 ? ((_Bool (*)(const char *))sdl.SetClipboardText)(text)
                                    : ((int (*)(const char *))sdl.SetClipboardText)(text) == 0;
    return ok ? 2 : 0;
}
/* A path as a file:// link (each byte that is not plain escaped, its slashes kept) */
static void file_url(char *out, size_t cap, const char *path) {
    size_t n = (size_t)snprintf(out, cap, "file://");
    for (const char *p = path; *p && n + 4 < cap; p++) {
        if (*p == '/') out[n++] = '/';
        else {
            const char one[2] = {*p, 0};
            n += hos_crash_url_part(out + n, cap - n, one);
        }
    }
    out[n < cap ? n : cap - 1] = 0;
}
static int open_thing(const char *what) {
#ifdef HOS_CRASH_DIALOG_TEST
    return hos_crash_test_open(what);
#endif
    char url[PATH_MAX_UTF8 * 3 + 16];
    if (what[0] == '/') file_url(url, sizeof url, what);
    else snprintf(url, sizeof url, "%s", what);
    if (sdl_load() && sdl.OpenURL)
        return sdl.version == 3 ? ((_Bool (*)(const char *))sdl.OpenURL)(url) : ((int (*)(const char *))sdl.OpenURL)(url) == 0;
    /* (an SDL before 2.0.14: xdg-open itself) */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = {(char *)"xdg-open", url, NULL};
    pid_t pid;
    const int ok = posix_spawnp(&pid, "xdg-open", &fa, NULL, argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    int status = 0;
    if (ok) waitpid(pid, &status, 0);
    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
typedef struct BoxJob { const char *title, *text; const char *const *buttons; int n, hit; volatile int done; } BoxJob;
static void *box_thread(void *p) {
    BoxJob *j = (BoxJob *)p;
    j->hit = ui_box(j->title, j->text, j->buttons, j->n);
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    return NULL;
}
#endif
/* A box while the clipboard is given (Linux: the box on a thread of its own, SDL's events pumped here meanwhile, on the thread
   that started SDL's video, so a paste in the browser gets the text) */
static int page_serving(const char *title, const char *text, const char *block, const char *const *buttons, int n) {
#ifndef _WIN32
    /* (the crash window's own loop takes SDL's events, so the clipboard is served while it shows a page) */
    if (video_on && sdl.PumpEvents && !window_load()) {
        BoxJob j = {title, text, buttons, n, -1, 0};
        pthread_t t;
        if (!pthread_create(&t, NULL, box_thread, &j)) {
            while (!__atomic_load_n(&j.done, __ATOMIC_ACQUIRE)) ((void (*)(void))sdl.PumpEvents)(), sleep_ms(20);
            pthread_join(t, NULL);
            return j.hit;
        }
    }
#endif
    return ui_page(title, text, block, buttons, n);
}
static int box_serving(const char *title, const char *text, const char *const *buttons, int n) {
    return page_serving(title, text, NULL, buttons, n);
}

/* ---- the window, and Report ---- */
typedef struct Shown {
    int verdict, stage, cause;
    const char *dir, *record, *record_name, *options;
    RunLog *log;
    Who who;
    /* (where Report automatically posts to: <hookshot_version.txt's telemetry=, or HL_TELEMETRY_URL>/telemetry/crash, and the
       optional crash_issue=; reports "" when there is no address or no curl: the window then offers Report manually only) */
    char reports[512], issue[512];
} Shown;
static void log_folder(const Shown *s, char *out, size_t cap) {
    snprintf(out, cap, "%s", s->log->path[0] ? s->log->path : "");
    char *a = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > a) a = b;
    if (a) *a = 0;
    else if (!join(out, cap, s->dir, "logs")) snprintf(out, cap, "%s", s->dir);
#ifdef _WIN32
    for (char *p = out; *p; p++)
        if (*p == '/') *p = '\\';
    if (!strncmp(out, "\\\\?\\", 4) && strncmp(out, "\\\\?\\UNC\\", 8)) memmove(out, out + 4, strlen(out + 4) + 1);
#endif
}
/* The reports Report saved (bug-reports/pending/report-<UTC>.txt): the newest REPORTS_KEPT kept, the older removed (the
   records' own pruning, hos_crash_record.c prune_pending, takes only crash- and unclean- names) */
enum { REPORTS_KEPT = 10 };
typedef struct Reports { char names[64][32]; int n; } Reports;
static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void take_report(const char *name, void *user) {
    Reports *r = (Reports *)user;
    const size_t n = strlen(name);
    if (r->n < 64 && n == 27 && !strncmp(name, "report-", 7) && !strcmp(name + 23, ".txt"))
        memcpy(r->names[r->n++], name, n + 1);
}
static void prune_reports(const char *pending) {
    Reports r;
    r.n = 0;
    each_name(pending, take_report, &r);
    qsort(r.names, (size_t)r.n, sizeof r.names[0], cmp_name);   /* (oldest first: their UTC) */
    for (int i = 0; i + REPORTS_KEPT < r.n; i++) {
        char path[PATH_MAX_UTF8];
        if (join(path, sizeof path, pending, r.names[i])) remove_utf8(path);
    }
}
/* Report's report: its text, saved beside the crash records (bug-reports/pending/report-<UTC>.txt; local is that path as the
   system writes paths, "" when it could not be saved), and the issue form filled in for it. 0 when there is no text */
typedef struct Made {
    char *text;
    size_t n;
    char local[PATH_MAX_UTF8];
    char url[HOS_CRASH_LINK_MAX + 1];
} Made;
static void report_facts(const Shown *s, Report *out) {
    Report in;
    memset(&in, 0, sizeof in);
    in.verdict = s->verdict, in.stage = s->stage, in.cause = s->cause;
    in.record = s->record, in.record_name = s->record_name, in.dir = s->dir, in.options = s->options, in.log = s->log, in.who = &s->who;
    system_text(in.system, sizeof in.system);
    const HosCrashLog *f = &s->log->facts;
    if (f->gpu[0]) snprintf(in.gpu, sizeof in.gpu, "%s, %s", f->gpu, f->driver[0] ? f->driver : "driver not named in the log");
    else snprintf(in.gpu, sizeof in.gpu, "Not known: the log names no Vulkan device in use");
    char record_version[160] = "";
    if (s->record) hos_crash_record_value(s->record, "version", record_version, sizeof record_version);
    snprintf(in.version, sizeof in.version, "%s", f->version[0] ? f->version : record_version[0] ? record_version : "not known");
    char source[64], stamp_source[64] = "";
    hos_crash_version_split(f->version[0] ? f->version : record_version, in.bare_version, sizeof in.bare_version, source, sizeof source);
    {
        char path[PATH_MAX_UTF8];
        char *stamp = join(path, sizeof path, s->dir, "hookshot_version.txt") ? read_text(path, 16 << 10, NULL) : NULL;
        const char *at = stamp ? strstr(stamp, "source=") : NULL;
        if (at && (at == stamp || at[-1] == '\n')) hos_crash_take(stamp_source, sizeof stamp_source, at + 7);
        stamp_source[strcspn(stamp_source, " \t")] = 0;
        free(stamp);
    }
    in.release = hos_crash_release(in.bare_version, source, stamp_source);
    count_settings(&in);
    *out = in;
}
static int make_report(const Shown *s, Made *out) {
    memset(out, 0, sizeof *out);
    static Report in;
    report_facts(s, &in);
    const HosCrashLog *f = &s->log->facts;
    out->text = report_text(&in, &out->n);
    if (!out->text) return 0;
    char pending[PATH_MAX_UTF8], path[PATH_MAX_UTF8], leaf[64], stamp[17];
    utc_now(stamp);
    snprintf(leaf, sizeof leaf, "report-%s.txt", stamp);
    const int saved = pending_of(s->dir, pending, sizeof pending) && join(path, sizeof path, pending, leaf) &&
                      write_text(path, out->text, out->n);
    if (saved) prune_reports(pending);
    char headline[160], title[200], what[400];
    hos_crash_headline(headline, sizeof headline, hos_crash_title(s->verdict), s->stage);
    snprintf(title, sizeof title, "Game: %s", headline);
    snprintf(what, sizeof what, "%s. %s", headline, hos_crash_cause_text(s->cause));
    /* (the system as the card says it: the CPU and RAM, then the operating system, from the log's report: lines, else the
       helper's own reading) */
    char system[420];
    if (f->cpu[0] && f->os[0]) snprintf(system, sizeof system, "%s; %s", f->cpu, f->os);
    else snprintf(system, sizeof system, "%s", in.system);
    /* (the form's Channel: a release build's from its version, anything else built from source) */
    const char *track = !in.release ? "Built from source" : !strncmp(in.bare_version, "nightly-", 8) ? "Nightly"
                      : strstr(in.bare_version, "-rc.") ? "Release candidate" : "Release";
    char where[200];
    snprintf(where, sizeof where, "%s", f->place[0] ? f->place : s->log->found ? HOS_CRASH_TITLE_AND_INTRO : "");
    {   /* (one separator between names: the game folder ends with one; a share's leading two kept) */
        const char *from = saved ? path : "";
        size_t o = 0;
        for (size_t i = 0; from[i] && o + 1 < sizeof out->local; i++) {
            const int sep = from[i] == '/' || from[i] == '\\';
            if (sep && i > 1 && o && (out->local[o - 1] == '/' || out->local[o - 1] == '\\')) continue;
#ifdef _WIN32
            out->local[o++] = sep ? '\\' : from[i];
#else
            out->local[o++] = from[i];
#endif
        }
        out->local[o] = 0;
    }
    {   /* (the saved report's path in the link without the home folder and the user's name, as the report's own lines have it) */
        char *shown = out->local[0] ? redacted(&s->who, out->local, strlen(out->local)) : NULL;
        hos_crash_issue_url(out->url, sizeof out->url, title, in.version, f->gpu[0] ? in.gpu : "", system, what, track, where,
                            in.changed_list, out->text, shown ? shown : "");
        free(shown);
    }
    return 1;
}
static void report(const Shown *s) {
    Made *made = (Made *)malloc(sizeof *made);
    if (!made || !make_report(s, made)) {
        free(made);
        return;
    }
    const int copied = clipboard(made->text), opened = open_thing(made->url), saved = made->local[0] != 0;
    char *m = (char *)malloc(PATH_MAX_UTF8 + 1024);
    if (m) hos_crash_ready_text(m, PATH_MAX_UTF8 + 1024, copied, opened, saved ? made->local : NULL, HOS_CRASH_ISSUES);
    const char *const buttons[] = {"Open the report", "Done"};
    for (int shown = 0;; shown++) {
        const int hit = box_serving("HeyListen: report ready", m ? m : "", saved ? buttons : buttons + 1, saved ? 2 : 1);
        /* (no box could be shown at all: the saved report opened instead, so the player has it in front of them) */
        if (hit == -2 && !shown && saved) open_thing(made->local);
        if (!saved || hit != 0) break;
        open_thing(made->local);
    }
    free(m), free(made->text), free(made);
}
/* ---- Report automatically: the report as the HeyListen telemetry server's /telemetry/crash takes it, built here from the run's log, shown in
   full, and posted with the system's curl only when the player presses Send (docs/crash-report-window.md) ---- */
static int spawn_exe(const char *exe_path, const char *const *args, int n, int wait_ms, int *code);
/* (bug-reports/pending/sending-<record><ext>: the report, curl's answer and its headers, removed once the window is done) */
static int sending_path(const Shown *s, const char *ext, char *out, size_t cap) {
    char pending[PATH_MAX_UTF8], leaf[192];
    snprintf(leaf, sizeof leaf, "sending-%s%s", s->record_name, ext);
    return pending_of(s->dir, pending, sizeof pending) && join(out, cap, pending, leaf);
}
/* (bug-reports/sent/<record>: the answer to a report sent, so Send is not offered for it again) */
static int sent_path(const Shown *s, char *out, size_t cap, int make) {
    char root[PATH_MAX_UTF8], sent[PATH_MAX_UTF8];
    if (!join(root, sizeof root, s->dir, "bug-reports") || !join(sent, sizeof sent, root, "sent")) return 0;
    if (make) make_dir(sent);
    return join(out, cap, sent, s->record_name);
}
static int sent_already(const Shown *s) {
    char path[PATH_MAX_UTF8];
    char *t = sent_path(s, path, sizeof path, 0) ? read_text(path, 4096, NULL) : NULL;
    free(t);
    return t != NULL;
}
static int find_curl(char *out, size_t cap);
static void find_addresses(Shown *s) {
    char path[PATH_MAX_UTF8];
    char *stamp = join(path, sizeof path, s->dir, "hookshot_version.txt") ? read_text(path, 16 << 10, NULL) : NULL;
    char base[HOS_TELEMETRY_URL_MAX + 8], from_stamp[HOS_TELEMETRY_URL_MAX + 8] = "";
    s->reports[0] = s->issue[0] = 0;
    if (stamp) hos_telemetry_stamp_value(stamp, "telemetry", from_stamp, sizeof from_stamp);
    /* (the build's address, or HL_TELEMETRY_URL's for a test; none: nothing can be sent and the window does not offer it) */
    if (hos_telemetry_base(getenv(HOS_TELEMETRY_ENV), getenv(HOS_TELEMETRY_TEST_HTTP_ENV), from_stamp, base, sizeof base) &&
        !hos_telemetry_join(s->reports, sizeof s->reports, base, HOS_TELEMETRY_CRASH_PATH))
        s->reports[0] = 0;
    if (stamp && s->reports[0]) hos_crash_stamp_address(stamp, "crash_issue", s->issue, sizeof s->issue);   /* (optional) */
    free(stamp);
    {   /* (no system curl, no Report automatically: Windows 10 and later ship curl.exe in System32; the manual way is still there) */
        char curl[PATH_MAX_UTF8];
        if (s->reports[0] && !find_curl(curl, sizeof curl)) s->reports[0] = s->issue[0] = 0;
    }
}
/* A line of the report, cleaned (hos_crash_clean), as a JSON string after sep */
static void put_field(Buf *b, const char *sep, const char *key, const char *value, size_t limit, const Shown *s,
                      const char *const *devices, int ndevices) {
    const size_t cap = strlen(value) * 3 + 64;
    char *c = (char *)malloc(cap), *j = (char *)malloc(cap * 2 + 8);
    if (c && j) {
        snprintf(c, cap, "%s", value);
        hos_crash_clean(c, cap, limit, s->who.home, s->who.user, s->who.windows, devices, ndevices);
        hos_crash_json(j, cap * 2 + 8, c);
        if (key) putf(b, "%s\"%s\": %s", sep, key, j);
        else putf(b, "%s%s", sep, j);
    }
    free(c), free(j);
}
typedef struct Setting { char key[64], value[32]; int changed; } Setting;
static int cmp_setting(const void *a, const void *b) {
    const Setting *x = (const Setting *)a, *y = (const Setting *)b;
    return x->changed != y->changed ? y->changed - x->changed : strcmp(x->key, y->key);
}
/* Every setting, key=value lines, the changed ones first (the defaults when the run had no file yet), as many as fit 4000
   characters; *devices: the microphone's name in it, to leave out of every line */
static void put_settings(Buf *b, const Shown *s, char *mic, size_t mic_cap) {
    enum { MAX = 512 };
    static Setting rows[MAX], defaults[MAX];
    char path[PATH_MAX_UTF8];
    int nd = 0, n = 0;
    mic[0] = 0;
    char *dtext = join(path, sizeof path, s->dir, "hookshot_defaults.ini") ? read_text(path, 256 << 10, NULL) : NULL;
    for (char *line = dtext; line && *line && nd < MAX; line = strchr(line, '\n'), line = line ? line + 1 : NULL)
        if (hos_crash_setting(line, defaults[nd].key, sizeof defaults[nd].key, defaults[nd].value, sizeof defaults[nd].value)) nd++;
    char *ctext = s->options ? read_text(s->options, 256 << 10, NULL)
                  : join(path, sizeof path, s->dir, "hookshot_options.ini") ? read_text(path, 256 << 10, NULL) : NULL;
    for (char *line = ctext; line && *line && n < MAX; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        if (!strncmp(line, "microphone_device=", 18)) hos_crash_take(mic, mic_cap, line + 18), mic[strcspn(mic, "\r\n")] = 0;
        if (!hos_crash_setting(line, rows[n].key, sizeof rows[n].key, rows[n].value, sizeof rows[n].value)) continue;
        rows[n].changed = 0;
        for (int i = 0; i < nd; i++)
            if (!strcmp(defaults[i].key, rows[n].key)) rows[n].changed = strcmp(defaults[i].value, rows[n].value) != 0;
        n++;
    }
    if (!ctext) memcpy(rows, defaults, sizeof rows[0] * (size_t)nd), n = nd;
    qsort(rows, (size_t)n, sizeof rows[0], cmp_setting);
    Buf o = {0};
    put(&o, "");
    for (int i = 0; i < n && o.n + strlen(rows[i].key) + strlen(rows[i].value) + 2 <= HOS_CRASH_MAX_OPTIONS; i++)
        putf(&o, "%s%s=%s", i ? "\n" : "", rows[i].key, rows[i].value);
    char *j = (char *)malloc(o.n * 2 + 8);
    if (j) hos_crash_json(j, o.n * 2 + 8, o.s ? o.s : ""), putf(b, ",\n  \"options\": %s", j);
    free(j), free(o.s), free(dtext), free(ctext);
}
/* The run's log again, from its file, for the crash's lines as they are (the ring of read_log is cut to fit the menu's
   report): the 150 lines before the crash's first and its own (at most 80). 0 when the log has no crash.
   lifted: the build's lifted code key, from the log's "object keys: lifted code <key>" line ("" when it is not there) */
typedef struct Lines { char **at; int n, cap; } Lines;
static void lines_add(Lines *l, const char *line) {
    if (l->n == l->cap) {
        char **more = (char **)realloc(l->at, sizeof *more * (size_t)(l->cap ? l->cap * 2 : 256));
        if (!more) return;
        l->at = more, l->cap = l->cap ? l->cap * 2 : 256;
    }
    l->at[l->n] = strdup(line);
    if (l->at[l->n]) l->n++;
}
static void lines_free(Lines *l) {
    for (int i = 0; i < l->n; i++) free(l->at[i]);
    free(l->at);
    memset(l, 0, sizeof *l);
}
static int crash_lines(const Shown *s, Lines *tail, Lines *crash, char *lifted, size_t lifted_cap) {
    FILE *f = s->log->path[0] ? open_utf8(s->log->path, "rb") : NULL;
    if (!f) return 0;
    char line[4096], one[4096];
    Lines ring = {0};
    int found = 0;
    if (lifted_cap) lifted[0] = 0;
    for (uint64_t taken = 0; taken < (256ull << 20) && fgets(line, sizeof line, f);) {
        taken += strlen(line);
        line[strcspn(line, "\r\n")] = 0;
        if (lifted_cap && !lifted[0]) {
            const char *key = strstr(line, "object keys: lifted code ");
            if (key) {
                snprintf(lifted, lifted_cap, "%s", key + 25);
                lifted[strcspn(lifted, " \t")] = 0;
            }
        }
        if (!found && hos_crash_starts(line)) found = 1;
        if (!found) {   /* (the lines before it: the last 150, a ring) */
            if (ring.n < HOS_CRASH_TAIL_LINES) lines_add(&ring, line);
            else {
                free(ring.at[0]);
                memmove(ring.at, ring.at + 1, sizeof *ring.at * (HOS_CRASH_TAIL_LINES - 1));
                ring.at[HOS_CRASH_TAIL_LINES - 1] = strdup(line);
                if (!ring.at[HOS_CRASH_TAIL_LINES - 1]) ring.n--;
            }
        } else if (crash->n < HOS_CRASH_CRASH_LINES && hos_crash_report_line(one, sizeof one, line)) {
            lines_add(crash, one);
        }
    }
    fclose(f);
    *tail = ring;
    return found;
}
/* The crash's resolved frames, as JSON string array members (frame lines only, in the crash's order): host frames
   or guest frames. 0 when the game named none (an older log, or a crash in a system DLL) */
static int put_frame_array(Buf *json, const Lines *crash, const char *kind, const Shown *s,
                           const char *const *devices, int nd) {
    char want[24];
    snprintf(want, sizeof want, "%s frame ", kind);
    int taken = 0;
    for (int i = 0; i < crash->n; i++) {
        char function[200];
        if (!strstr(crash->at[i], want)) continue;
        hos_crash_frame_name(crash->at[i], function, sizeof function);
        if (!function[0]) continue;
        put_field(json, taken ? ",\n    " : "\n    ", NULL, function, 160, s, devices, nd);
        taken++;
    }
    return taken;
}
/* The report: its JSON (the payload) and what the page shows of it; 0 with *why when there is none to send */
static int build_report(const Shown *s, Buf *json, Buf *shown, const char **why) {
    Lines tail = {0}, crash = {0};
    char lifted[64] = "";
    if (!crash_lines(s, &tail, &crash, lifted, sizeof lifted) || !crash.n) {
        *why = s->log->path[0] ? "The log of the run that crashed has no crash in it, so there is no report to send."
                               : "The log of the run that crashed is gone (four runs since), so there is no report to send.";
        lines_free(&tail), lines_free(&crash);
        return 0;
    }
    /* (the signature the server groups on: the resolved top frame, the same in every build of these sources) */
    char signature[200] = "";
    const char *const *crash_at = (const char *const *)crash.at;
    hos_crash_signature(signature, sizeof signature, crash_at, crash.n);
    static Report in;
    report_facts(s, &in);
    const HosCrashLog *f = &s->log->facts;
    char mic[128], version[64];
    Buf settings = {0};
    put(&settings, "");
    put_settings(&settings, s, mic, sizeof mic);
    const char *devices[1] = {mic};
    const int nd = mic[0] ? 1 : 0;
    hos_crash_report_version(version, sizeof version, in.bare_version);
    /* (a crash before the renderer wrote its "report:" lines: the CPU and the OS from the helper's own reading, split, and the
       adapters the log listed so far for the GPU; what is not known at all is left out, never sent empty) */
    char os[sizeof in.system], cpu_own[sizeof in.system];
    hos_crash_system_split(in.system, cpu_own, sizeof cpu_own, os, sizeof os);
    if (f->os[0]) snprintf(os, sizeof os, "%s", f->os);
    const char *cpu_text = f->cpu[0] ? f->cpu : cpu_own;
    const char *gpu_text = f->gpu[0] ? f->gpu : f->devices;
    const char *place = f->place[0] ? f->place : HOS_CRASH_TITLE_AND_INTRO;
    putf(json, "{\n  \"version\": \"%s\",\n  \"release\": %s", version, in.release ? "true" : "false");
    if (os[0]) put_field(json, ",\n  ", "os", os, HOS_CRASH_MAX_OS, s, devices, nd);
    if (f->renderer[0]) put_field(json, ",\n  ", "renderer", f->renderer, HOS_CRASH_MAX_RENDERER, s, devices, nd);
    if (gpu_text[0]) put_field(json, ",\n  ", "gpu", gpu_text, HOS_CRASH_MAX_GPU, s, devices, nd);
    if (cpu_text[0]) put_field(json, ",\n  ", "cpu", cpu_text, HOS_CRASH_MAX_CPU, s, devices, nd);
    put_field(json, ",\n  ", "map", place, HOS_CRASH_MAX_MAP, s, devices, nd);
    put(json, ",\n  \"crash\": [");
    for (int i = 0; i < crash.n; i++) put_field(json, i ? ",\n    " : "\n    ", NULL, crash.at[i], HOS_CRASH_LINE_LIMIT, s, devices, nd);
    put(json, crash.n ? "\n  ]" : "]");
    if (signature[0]) {
        char *j = (char *)malloc(strlen(signature) * 2 + 8);
        if (j) hos_crash_json(j, strlen(signature) * 2 + 8, signature), putf(json, ",\n  \"signature\": %s", j);
        free(j);
    }
    if (lifted[0]) put_field(json, ",\n  ", "lifted_code", lifted, 40, s, devices, nd);
    {   /* (the resolved frames, when the game named them: absent from an older log or a system DLL's crash) */
        const size_t mark = json->n;
        put(json, ",\n  \"frames\": [");
        const int host_frames = put_frame_array(json, &crash, "host", s, devices, nd);
        if (host_frames) put(json, "\n  ]");
        else json->n = mark, json->s[mark] = 0;
        const size_t mark2 = json->n;
        put(json, ",\n  \"guest_frames\": [");
        const int guest_frames = put_frame_array(json, &crash, "guest", s, devices, nd);
        if (guest_frames) put(json, "\n  ]");
        else json->n = mark2, json->s[mark2] = 0;
    }
    put(json, ",\n  \"log_tail\": [");
    for (int i = 0; i < tail.n; i++) put_field(json, i ? ",\n    " : "\n    ", NULL, tail.at[i], HOS_CRASH_LINE_LIMIT, s, devices, nd);
    put(json, tail.n ? "\n  ]" : "]");
    put(json, settings.s);
    put(json, "\n}\n");
    /* (the page: the card's rows, the crash's first lines, what else goes; the whole of it is the file Show all of it opens.
       The first line says where it goes: players are not told anywhere else on this page that it becomes public) */
    putf(shown, "%s\n\nVersion: %s\nRelease build: %s\nOperating system: %s\nRenderer: %s\nGraphics card: %s\nCPU and RAM: %s\nMap: %s\n",
         HOS_CRASH_PUBLIC_TEXT,
         version, in.release ? "Yes" : "No (a local or modified build)", os, f->renderer[0] ? f->renderer : "not known",
         gpu_text[0] ? gpu_text : "not known", cpu_text[0] ? cpu_text : "not known", place);
    if (lifted[0]) putf(shown, "Lifted code: %s\n", lifted);
    if (signature[0]) putf(shown, "Signature: %s\n", signature);
    putf(shown, "Settings: %d (%d changed from the defaults)\n\nThe crash:\n", in.settings, in.changed);
    for (int i = 0; i < crash.n && i < 12; i++) {
        char line[HOS_CRASH_LINE_LIMIT * 4 + 8];
        snprintf(line, sizeof line, "%s", crash.at[i]);
        hos_crash_clean(line, sizeof line, HOS_CRASH_LINE_LIMIT, s->who.home, s->who.user, s->who.windows, devices, nd);
        putf(shown, "%s\n", line);
    }
    if (crash.n > 12) putf(shown, "(and %d more lines of it)\n", crash.n - 12);
    putf(shown, "\nand the %d log lines before it, and every setting (Show all of it).", tail.n);
    free(settings.s), lines_free(&tail), lines_free(&crash);
    return 1;
}
/* The system's curl (Windows 10 and 11 have one in System32): its path, 0 without one */
static int find_curl(char *out, size_t cap) {
#ifdef HOS_CRASH_DIALOG_TEST
    const char *stand_in = getenv("CRASH_TEST_CURL");   /* (tests/test_crash_dialog.py's: the server's answers, no network) */
    return stand_in && snprintf(out, cap, "%s", stand_in) > 0;
#endif
#ifdef _WIN32
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    char *d = n && n < MAX_PATH ? utf8_of(dir) : NULL;
    const int ok = d && join(out, cap, d, "curl.exe");
    free(d);
    if (ok) for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
    return ok && file_exists(out);
#else
    static const char *const places[] = {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", "/run/host/usr/bin/curl"};
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++)
        if (file_exists(places[i]) && access(places[i], X_OK) == 0) return snprintf(out, cap, "%s", places[i]) > 0;
    return 0;
#endif
}
/* Posts the payload file: the HTTP status (0 when there was no answer) and the answer's body in *body (malloc'd) */
static int post_report(const Shown *s, const char *payload, char **body) {
    char curl[PATH_MAX_UTF8], headers[PATH_MAX_UTF8], answer[PATH_MAX_UTF8], data[PATH_MAX_UTF8 + 1];
    *body = NULL;
    if (!find_curl(curl, sizeof curl) || !sending_path(s, ".headers.txt", headers, sizeof headers) ||
        !sending_path(s, ".answer.txt", answer, sizeof answer))
        return 0;
    remove_utf8(headers), remove_utf8(answer);
    snprintf(data, sizeof data, "@%s", payload);
    /* (no redirect followed: the report goes to the address in the stamp and nowhere else; at most 30 s. --disable first, so
       no .curlrc / _curlrc of the player's adds a redirect, a proxy, --insecure or an --output of its own) */
    /* (https only; the one exception is HL_TELEMETRY_TEST_HTTP=1 with a loopback address, which hos_telemetry_base let through) */
    const char *const proto = strncmp(s->reports, "https://", 8) ? "=http" : "=https";
    const char *const args[] = {"--disable", "--silent", "--show-error", "--max-time", "25", "--connect-timeout", "10", "--proto", proto,
                                "--header", "Content-Type: application/json", "--user-agent", "heylisten-crash-report",
                                "--data-binary", data, "--dump-header", headers, "--output", answer, s->reports};
    const int ran = spawn_exe(curl, args, (int)(sizeof args / sizeof args[0]), 30000, NULL);
    char *head = ran == 1 ? read_text(headers, 16 << 10, NULL) : NULL;
    const int status = head ? hos_crash_http_status(head) : 0;
    *body = ran == 1 ? read_text(answer, 16 << 10, NULL) : NULL;
    free(head), remove_utf8(headers), remove_utf8(answer);
    return status;
}
/* (bug-reports/sent/<record>.json, <record> without its .txt: a copy of the report sent on its own, as it went) */
static int sent_copy_path(const Shown *s, char *out, size_t cap) {
    char answer[PATH_MAX_UTF8];
    if (!sent_path(s, answer, sizeof answer, 1)) return 0;
    const size_t n = strlen(answer);
    if (n > 4 && !strcmp(answer + n - 4, ".txt")) answer[n - 4] = 0;
    const int k = snprintf(out, cap, "%s.json", answer);
    return k > 0 && (size_t)k < cap;
}
/* (a line at the end of the run's log, the helper's own: the game has ended, so the log is no longer written by it. Nothing
   when the run's log was not found) */
static void note_in_log(const Shown *s, const char *line) {
    FILE *f = s->log->path[0] ? open_utf8(s->log->path, "ab") : NULL;
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}
/* (a report sent: its answer kept in bug-reports/sent/<record>, so it is not offered or sent again) */
static void keep_answer(const Shown *s, int status, const char *link) {
    char path[PATH_MAX_UTF8], kept[HOS_CRASH_LINK_MAX + 64];
    const int n = snprintf(kept, sizeof kept, "status: %d\nlink: %s\n", status, link);
    if (sent_path(s, path, sizeof path, 1) && n > 0) write_text(path, kept, (size_t)n);
}
/* Report automatically with no window, as the player chose in the menu (hos_crash_auto_send): the same report, posted
   the same way. 1 when it was sent; 0 when it could not be made or sent, and the window shows it as it would have */
static int send_quietly(const Shown *s) {
    char ini[PATH_MAX_UTF8], payload[PATH_MAX_UTF8];
    char *choice = s->options ? read_text(s->options, 256 << 10, NULL)
                   : join(ini, sizeof ini, s->dir, "hookshot_options.ini") ? read_text(ini, 256 << 10, NULL) : NULL;
    const int wanted = choice && hos_crash_auto_send(choice);
    free(choice);
    if (!wanted) return 0;
    Buf json = {0}, shown = {0};
    const char *why = NULL;
    int sent = 0;
    if (sending_path(s, ".json", payload, sizeof payload) && build_report(s, &json, &shown, &why) &&
        write_text(payload, json.s, json.n)) {
        char *body = NULL, result[700], link[HOS_CRASH_LINK_MAX + 1];
        int again = 0;
        const int status = post_report(s, payload, &body);
        sent = hos_crash_answer(result, sizeof result, status, body, s->issue, link, sizeof link, &again);
        free(body);
        /* (what went, kept beside its answer, and the answer in the run's log: with no window, the player can still see both) */
        char copy[PATH_MAX_UTF8], line[sizeof result + sizeof link + 64];
        if (sent) {
            keep_answer(s, status, link);
            if (sent_copy_path(s, copy, sizeof copy)) write_text(copy, json.s, json.n);
        }
        snprintf(line, sizeof line, "[hos crash] sent on its own (send_crash_reports=1): %s", sent ? link : result);
        for (char *p = line; *p; p++)
            if (*p == '\n' || *p == '\r') *p = ' ';
        note_in_log(s, line);
        remove_utf8(payload);
    }
    free(json.s), free(shown.s);
    return sent;
}
/* 1 when it was sent (Send is not offered again), 0 otherwise */
static int send_report(const Shown *s) {
    char payload[PATH_MAX_UTF8];
    const char *const close[] = {"Close"};
    Buf json = {0}, shown = {0};
    const char *why = NULL;
    if (!sending_path(s, ".json", payload, sizeof payload) || !build_report(s, &json, &shown, &why) ||
        !write_text(payload, json.s, json.n)) {
        char m[600];
        snprintf(m, sizeof m, "The report could not be made, so nothing was sent.\n\n%s",
                 why ? why : "It could not be saved in bug-reports.");
        box_serving("HeyListen: send to the team", m, close, 1);
        free(json.s), free(shown.s);
        return 0;
    }
    const char *const ask[] = {"Send", "Show all of it", "Cancel"};
    int sent = 0;
    for (;;) {
        const int hit = page_serving("HeyListen: send to the team",
                                     "This is what is sent, and nothing else. Show all of it opens the report as it is sent, "
                                     "every line of it.",
                                     shown.s ? shown.s : "", ask, 3);
        if (hit == 1) {
            open_thing(payload);
            continue;
        }
        if (hit != 0) break;
        char *body = NULL, result[700], link[HOS_CRASH_LINK_MAX + 1];
        int again = 0;
        const int status = post_report(s, payload, &body);
        sent = hos_crash_answer(result, sizeof result, status, body, s->issue, link, sizeof link, &again);
        free(body);
        if (sent) {
            keep_answer(s, status, link);
            const char *const done[] = {"Open it", "Done"};
            if (box_serving("HeyListen: sent", result, link[0] ? done : done + 1, link[0] ? 2 : 1) == 0 && link[0])
                open_thing(link);
            break;
        }
        const char *const retry[] = {"Try again", "Close"};
        if (box_serving("HeyListen: not sent", result, again ? retry : retry + 1, again ? 2 : 1) != 0 || !again) break;
    }
    free(json.s), free(shown.s), remove_utf8(payload);
    return sent;
}
static void show(Shown *s) {
    char message[1400];
    find_addresses(s);
    /* (the player's choice in the menu: sent on its own, no window; one that can't be sent is shown as always) */
    if (hos_crash_send_offered(s->record_name, s->reports[0] != 0, s->record_name && sent_already(s)) && send_quietly(s)) return;
    for (;;) {
        /* (Report automatically first when it can be: a crash record, the addresses in the stamp, not sent already) */
        const int offered = hos_crash_send_offered(s->record_name, s->reports[0] != 0, s->record_name && sent_already(s));
        hos_crash_message(message, sizeof message, s->verdict, s->stage, s->cause, offered);
        const char *const with_send[] = {"Report automatically", "Report manually", "Open the log folder", "Close"};
        const char *const *buttons = offered ? with_send : with_send + 1;
        const int hit = ui_box(hos_crash_title(s->verdict), message, buttons, offered ? 4 : 3) - offered;
        if (hit == -1) {
            send_report(s);
            continue;   /* (the window again: Report and the log folder are still to be had) */
        }
        if (hit == 1) {
            char folder[PATH_MAX_UTF8];
            log_folder(s, folder, sizeof folder);
            open_thing(folder);
            continue;   /* (the window again: Report is still to be had) */
        }
        if (hit == 0) report(s);
        return;
    }
}

/* ---- the helper ---- */
#ifdef _WIN32
/* (the arguments as Windows has them, as UTF-8: the CRT's argv is the code page's, UTF-8 only with the game's manifest) */
static char **utf8_argv(int *argc) {
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), argc);
    char **a = w ? (char **)calloc((size_t)*argc + 1, sizeof *a) : NULL;
    for (int i = 0; a && i < *argc; i++) a[i] = utf8_of(w[i]);
    if (w) LocalFree(w);
    return a;
}
#endif
/* Waits for the game to end: 1, with its exit code where the system gives one; 0 when another run took its place (Linux's
   restart: the same process, a new run) */
static int wait_game(const HosCrashArgs *a, int *have_exit, unsigned long *code) {
    *have_exit = 0;
#ifdef _WIN32
    HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)a->pid);
    if (!p) return 1;   /* (gone already) */
    WaitForSingleObject(p, INFINITE);
    DWORD c = 0;
    if (GetExitCodeProcess(p, &c) && c != STILL_ACTIVE) *have_exit = 1, *code = c;
    CloseHandle(p);
    return 1;
#else
    (void)code;
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
    const int fd = (int)syscall(SYS_pidfd_open, (pid_t)a->pid, 0);
    for (;;) {
        if (fd >= 0) {
            struct pollfd p = {fd, POLLIN, 0};
            if (poll(&p, 1, 500) > 0) break;
        } else {
            /* (an older kernel: the game is this helper's parent, so its end makes another process the parent) */
            if (getppid() != (pid_t)a->pid || (kill((pid_t)a->pid, 0) && errno == ESRCH)) break;
            sleep_ms(250);
        }
        if (hos_crash_record_marker(a->dir, a->pid, a->run_id) == 3) {   /* (restarted: execve, a new run's marker) */
            if (fd >= 0) close(fd);
            return 0;
        }
    }
    if (fd >= 0) close(fd);
    return 1;
#endif
}
#ifndef _WIN32
/* (Linux) The game's cgroup's oom_kill count (hos_crash_oom_kills), from the memory.events path; -1 when it can't be read */
static long oom_kills(const char *events) {
    char *text = events[0] ? read_text(events, 4096, NULL) : NULL;
    const long n = text ? hos_crash_oom_kills(text) : -1;
    free(text);
    return n;
}
#ifndef _WIN32
/* (Linux) The environment for a program of the system's (busctl, curl): the helper's, without the game's library paths and
   preloads (LD_LIBRARY_PATH, LD_PRELOAD: the game's own folders, an overlay's), so it loads its own libraries. Malloc'd
   (the strings are environ's), NULL when there is no memory */
static char **system_env(void) {
    size_t n = 0, k = 0;
    while (environ && environ[n]) n++;
    char **env = (char **)malloc((n + 1) * sizeof *env);
    if (!env) return NULL;
    for (size_t i = 0; i < n; i++)
        if (strncmp(environ[i], "LD_LIBRARY_PATH=", 16) && strncmp(environ[i], "LD_PRELOAD=", 11)) env[k++] = environ[i];
    env[k] = NULL;
    return env;
}
#endif
/* (Linux) This helper out of the game's systemd scope, into a scope of its own (heylisten-crash-<pid>.scope, through the
   user manager's StartTransientUnit, as busctl asks it): the OOM killer ends the game, then systemd stops the scope the
   game was in (DefaultOOMPolicy=stop) and everything in it, which must not be this helper. Where it can't (no systemd, or
   in a Flatpak, which keeps the sandbox's processes in its scope), it stays; an out-of-memory end is then shown at the
   next start (hos_crash_show_at_start). At most 2 s, its output nowhere, without the game's library paths */
static void own_scope(void) {
    char unit[64], pid[24];
    snprintf(pid, sizeof pid, "%ld", (long)getpid());
    snprintf(unit, sizeof unit, "heylisten-crash-%s.scope", pid);
    char *const argv[] = {"busctl", "--user", "--quiet", "call", "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                          "org.freedesktop.systemd1.Manager", "StartTransientUnit", "ssa(sv)a(sa(sv))", unit, "fail", "2",
                          "PIDs", "au", "1", pid, "CollectMode", "s", "inactive-or-failed", "0", NULL};
    char **env = system_env();
    if (!env) return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t child;
    const int ok = posix_spawnp(&child, "busctl", &fa, NULL, argv, env) == 0;
    posix_spawn_file_actions_destroy(&fa);
    free(env);
    if (!ok) return;
    for (int waited = 0;; waited += 20) {
        int status;
        const pid_t got = waitpid(child, &status, WNOHANG);
        if (got == child || (got < 0 && errno != EINTR)) return;
        if (waited >= 2000) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            return;
        }
        sleep_ms(20);
    }
}
#endif
/* An unclean end the OOM killer made: "oom_kill: yes" added to its record (pending/name), and to *text */
static void mark_oom(const char *dir, const char *name, char **text) {
    char pending[PATH_MAX_UTF8], path[PATH_MAX_UTF8];
    const size_t n = *text ? strlen(*text) : 0;
    char *more = (char *)malloc(n + 32);
    if (!more) return;
    snprintf(more, n + 32, "%s%soom_kill: yes\n", *text ? *text : "", n && (*text)[n - 1] != '\n' ? "\n" : "");
    if (pending_of(dir, pending, sizeof pending) && join(path, sizeof path, pending, name)) write_text(path, more, strlen(more));
    free(*text);
    *text = more;
}
static int helper(const HosCrashArgs *a) {
    Shown s;
    memset(&s, 0, sizeof s);
    s.dir = a->dir, s.options = a->options;
    who_here(&s.who);
    RunLog *log = (RunLog *)calloc(1, sizeof *log);
    if (!log) return 0;
    s.log = log;
    char name[128] = "", run_id[33] = "";
    char *record = NULL;
    int have_exit = 0;
    unsigned long code = 0;
#ifndef _WIN32
    long oom_before = -1;
    char events[PATH_MAX_UTF8] = "";
    /* (SIGTERM held back until the run's record is written: systemd stops the game's scope at once after an OOM kill, this
       helper with it where it could not leave it, and the record must say out of memory first; let through after it) */
    sigset_t term;
    sigemptyset(&term);
    sigaddset(&term, SIGTERM);
    if (a->mode == 1) sigprocmask(SIG_BLOCK, &term, NULL);
    if (a->mode == 1) {   /* (the game's cgroup and its OOM count first, then out of the game's scope) */
        char cgroup_path[64];
        snprintf(cgroup_path, sizeof cgroup_path, "/proc/%llu/cgroup", (unsigned long long)a->pid);
        char *cg = read_text(cgroup_path, 16 << 10, NULL);
        if (!cg || !hos_crash_cgroup_events(cg, events, sizeof events)) events[0] = 0;
        free(cg);
        oom_before = oom_kills(events);
        own_scope();
    }
#endif
    int oom = 0;
    if (a->mode == 1) {
        if (!wait_game(a, &have_exit, &code)) return 0;
#ifndef _WIN32
        /* (at once: systemd stops the game's scope a moment after the kill, and its cgroup goes with it) */
        const long oom_after = oom_kills(events);
        oom = oom_before >= 0 && oom_after > oom_before;
#endif
        /* (its marker becomes an unclean record once the process is gone; Windows lets go of the marker's lock a moment
           after the process ends, so a while) */
        for (int i = 0; i < 30; i++) {
            const int m = hos_crash_record_marker(a->dir, a->pid, a->run_id);
            if (m == 0 || m == 3) break;
            if (m == 1) hos_crash_record_recover(a->dir);
            if (hos_crash_record_marker(a->dir, a->pid, a->run_id) == 0) break;
            sleep_ms(100);
        }
        snprintf(run_id, sizeof run_id, "%s", a->run_id);
        record = run_record(a->dir, run_id, name, sizeof name);
        if (oom && record && hos_crash_record_name(name, NULL) == 2) mark_oom(a->dir, name, &record);
#ifndef _WIN32
        sigprocmask(SIG_UNBLOCK, &term, NULL);
#endif
    } else {
        char pending[PATH_MAX_UTF8], path[PATH_MAX_UTF8];
        if (pending_of(a->dir, pending, sizeof pending) && join(path, sizeof path, pending, a->record))
            record = read_text(path, 64 << 10, NULL);
        if (!record) return 0;
        snprintf(name, sizeof name, "%s", a->record);
        hos_crash_record_value(record, "run_id", run_id, sizeof run_id);
    }
    HosCrashRecord r;
    hos_crash_record_parse(record ? record : "", &r);
    if (find_run_log(log, a->log, run_id)) read_log(log, &s.who);
    s.verdict = hos_crash_verdict(&r, &log->facts, have_exit, code, 0);
    if (a->mode == 2 && r.kind != 1 && !r.oom) s.verdict = HOS_CRASH_QUIET;
#ifdef _WIN32
    /* (a logoff or a shutdown ended the game: no window, which would hold the shutdown up; nor listed as shown, so a crash
       record of the run is still shown at the next start) */
#ifndef SM_SHUTTINGDOWN
#define SM_SHUTTINGDOWN 0x2000
#endif
    if (s.verdict != HOS_CRASH_QUIET && GetSystemMetrics(SM_SHUTTINGDOWN)) {
        free(record);
        for (int i = 0; i < HOS_CRASH_LOG_LINES; i++) free(log->lines[i]);
        free(log);
        return 0;
    }
#endif
    if (s.verdict != HOS_CRASH_QUIET) {
        s.stage = log->found ? hos_crash_stage(&log->facts) : HOS_STAGE_UNKNOWN;
        s.cause = hos_crash_cause(s.verdict, &r, &log->facts, have_exit, code);
        s.record = record, s.record_name = name[0] ? name : NULL;
        /* (before the window: a start meanwhile does not show it again. Out of memory: after it, so a helper that systemd
           stopped with the game's scope (in a Flatpak) leaves it to the next start) */
        if (name[0] && !r.oom) ledger_add(a->dir, name, 0);
        show(&s);
        window_close();
        if (name[0] && r.oom) ledger_add(a->dir, name, 0);
    } else if (name[0]) {
        ledger_add(a->dir, name, 0);   /* (an end that says nothing: never shown at a start either) */
    }
    free(record);
    for (int i = 0; i < HOS_CRASH_LOG_LINES; i++) free(log->lines[i]);
    free(log);
    return 0;
}
#ifndef _WIN32
/* (Linux: what the game had open without close-on-exec (what a launcher or a shell left it) is let go first, so the helper,
   which can outlive the game while its window is open, holds no pipe or file of the game's) */
static void close_inherited(void) {
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
    int fds[256], n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return;
    const int own = dirfd(d);
    for (struct dirent *e; (e = readdir(d)) && n < 256;) {
        const int fd = atoi(e->d_name);
        if (fd > 2 && fd != own) fds[n++] = fd;
    }
    closedir(d);
    for (int i = 0; i < n; i++) close(fds[i]);
}
#endif
/* main()'s first call: the helper's work and its exit code, or -1 for the game itself */
int hos_crash_helper_main(int argc, char **argv) {
    HosCrashArgs a;
#ifdef _WIN32
    int wargc = 0;
    char **wargv = argc >= 2 && argv[1] && (!strcmp(argv[1], HOS_CRASH_WATCH_ARG) || !strcmp(argv[1], HOS_CRASH_SHOW_ARG))
                       ? utf8_argv(&wargc) : NULL;
    if (wargv) argc = wargc, argv = wargv;
    if (wargv) {   /* (sharp text in the window on a scaled screen, as the game's own: per-monitor aware) */
        typedef BOOL(WINAPI * SetAware)(void *);
        SetAware set = (SetAware)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        if (set) set((void *)(intptr_t)-4);
    }
#endif
    const int mode = hos_crash_args(argc, argv, &a);
    if (!mode) return -1;
#ifndef _WIN32
    close_inherited();
#endif
    return mode < 0 ? 0 : helper(&a);
}

/* ---- the game's side ---- */
/* Starts a program (exe NULL: this executable) with args, and waits for it to end when wait_ms is not 0 (-1: as long as it
   takes). 0 when it could not be started, 1 when it ended (*code its exit code, when code is given), 2 when it still ran
   after wait_ms (it is left to finish) */
static int spawn_exe(const char *exe_path, const char *const *args, int n, int wait_ms, int *code) {
#ifdef _WIN32
    wchar_t self[MAX_PATH * 4];
    wchar_t *exe = exe_path ? wide_of(exe_path) : NULL;
    if (!exe_path) {
        const DWORD len = GetModuleFileNameW(NULL, self, MAX_PATH * 4);
        if (!len || len >= MAX_PATH * 4) return 0;
    }
    char *exe8 = exe_path ? (exe ? utf8_of(exe) : NULL) : utf8_of(self);
    Buf line = {0};
    char quoted[PATH_MAX_UTF8 * 2 + 8];
    if (!exe8 || !hos_crash_quote_arg(quoted, sizeof quoted, exe8)) return free(exe8), free(exe), 0;
    put(&line, quoted);
    for (int i = 0; i < n; i++) {
        if (!hos_crash_quote_arg(quoted, sizeof quoted, args[i])) return free(exe8), free(exe), free(line.s), 0;
        put(&line, " "), put(&line, quoted);
    }
    wchar_t *cmd = line.s ? wide_of(line.s) : NULL;
    free(exe8), free(line.s);
    if (!cmd) return free(exe), 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    /* (no console (a console build's), no handle of the game's (its log stays the game's to move), and out of the launcher's
       job where it allows that, so the window outlives a job closed with the game) */
    const DWORD flags = CREATE_NO_WINDOW;
    const wchar_t *program = exe ? exe : self;
    int ok = CreateProcessW(program, cmd, NULL, NULL, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi) ||
             CreateProcessW(program, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi);
    free(cmd), free(exe);
    if (!ok) return 0;
    AllowSetForegroundWindow(pi.dwProcessId);   /* (its window may come to the front once the game is gone) */
    int r = 1;
    if (wait_ms) {
        if (WaitForSingleObject(pi.hProcess, wait_ms < 0 ? INFINITE : (DWORD)wait_ms) == WAIT_TIMEOUT) r = 2;
        DWORD c = 0;
        if (r == 1 && code && GetExitCodeProcess(pi.hProcess, &c)) *code = (int)c;
    }
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return r;
#else
    char *argv[24] = {(char *)(exe_path ? exe_path : "HeyListen")};
    for (int i = 0; i < n && i < 22; i++) argv[i + 1] = (char *)args[i];
    argv[n + 1 < 24 ? n + 1 : 23] = NULL;
    /* (its output to nowhere: not the game's log, and not a launcher's pipe, which it would keep open after the game) */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    /* (a program of the system's, curl, without the game's library paths: system_env; this executable with them) */
    char **env = exe_path ? system_env() : NULL;
    if (exe_path && !env) return posix_spawn_file_actions_destroy(&fa), 0;
    pid_t pid;
    const int ok = posix_spawn(&pid, exe_path ? exe_path : "/proc/self/exe", &fa, NULL, argv, env ? env : environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    free(env);
    if (!ok) return 0;
    for (int waited = 0; wait_ms; waited += 50) {
        int status;
        const pid_t got = waitpid(pid, &status, wait_ms < 0 ? 0 : WNOHANG);
        if (got < 0 && errno == EINTR) continue;
        if (got == pid) {
            if (code) *code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            return 1;
        }
        if (got < 0) return 1;
        if (waited >= wait_ms) return 2;
        sleep_ms(50);
    }
    return 1;
#endif
}
static int spawn(const char *const *args, int n, int wait) { return spawn_exe(NULL, args, n, wait ? -1 : 0, NULL) != 0; }
/* At the start, once the crash record is set up, before anything heavy: a crash record of an earlier run nobody showed is
   shown first (the game waits for that window), then the helper for this run starts */
void hos_crash_watch_start(int argc, char **argv) {
    void hos_log(const char *fmt, ...);
    const char *run = hos_crash_record_run(), *dir = hos_crash_record_dir(), *log = hos_crash_record_log();
    const char *off = getenv("HOS_CRASH_WATCH"), *pgo = getenv("HOS_PGO");
    const int gamescope = hos_crash_under_gamescope(getenv("GAMESCOPE_WAYLAND_DISPLAY"), getenv("XDG_CURRENT_DESKTOP"));
    if (!hos_crash_watch_wanted(argc, argv, env_on("HOS_HEADLESS"), off && env_value_off(off), pgo && *pgo, gamescope, run) || !dir) {
        if (gamescope && run) hos_log("crash window: off under gamescope (the Steam Deck's Game Mode): a crash is in bug-reports");
        return;
    }
    const char *log_arg = log && log[0] ? log : "-";
    const char *options_arg = "-";   /* (the settings file is the game folder's hookshot_options.ini) */
    char ledger[PATH_MAX_UTF8], pending[PATH_MAX_UTF8];
    if (ledger_path(dir, ledger, sizeof ledger) && pending_of(dir, pending, sizeof pending)) {
        char *shown = read_text(ledger, 64 << 10, NULL);
        if (!shown) {
            ledger_add(dir, NULL, 1);   /* (the first start with this window: what was pending before is not news) */
        } else {
            Newest pick = {shown, pending, (int64_t)time(NULL), ""};
            each_name(pending, newest_unshown, &pick);
            if (pick.name[0] && ledger_add(dir, pick.name, 0)) {
                hos_log("crash window: %s was not shown after its run: shown now", pick.name);
                const char *args[] = {HOS_CRASH_SHOW_ARG, pick.name, dir, log_arg, options_arg};
                spawn(args, 5, 1);
            }
            free(shown);
        }
    }
    char pid[24];
#ifdef _WIN32
    snprintf(pid, sizeof pid, "%lu", (unsigned long)GetCurrentProcessId());
#else
    snprintf(pid, sizeof pid, "%ld", (long)getpid());
#endif
    const char *args[] = {HOS_CRASH_WATCH_ARG, pid, run, dir, log_arg, options_arg};
    hos_log(spawn(args, 6, 0) ? "crash window: watching this run (HOS_CRASH_WATCH=0: off)"
                              : "crash window: its helper could not be started: a crash is shown at the next start");
}

/* ---- HOS_CRASH_TEST: a crash on purpose, to see the window ---- */
/* (noinline: the deliberate crash keeps a frame of its own, so the report names it, as any named function is) */
static __attribute__((noinline)) void crash_now(void) {
    void hos_log(const char *fmt, ...);
    void hos_log_flush(void);
    hos_log("crash test: crashing now (HOS_CRASH_TEST)");
    hos_log_flush();
    volatile uintptr_t where = 16;
    *(volatile int *)where = 1;
}
#ifdef _WIN32
static DWORD WINAPI crash_later(void *p) {
    Sleep((DWORD)(uintptr_t)p * 1000);
    crash_now();
    return 0;
}
#else
static void *crash_later(void *p) {
    sleep_ms((int)(uintptr_t)p * 1000);
    crash_now();
    return NULL;
}
#endif
/* After the crash handlers are set up (hos_main.c reserve_guest): start, at once; a number, that many seconds later */
void hos_crash_test_arm(void) {
    const char *e = getenv("HOS_CRASH_TEST");
    if (!e || !*e) return;
    if (!strcmp(e, "start")) crash_now();
    const int seconds = atoi(e);
    if (seconds <= 0) return;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, crash_later, (void *)(uintptr_t)seconds, 0, NULL);
    if (t) CloseHandle(t);
#else
    pthread_t t;
    if (!pthread_create(&t, NULL, crash_later, (void *)(uintptr_t)seconds)) pthread_detach(t);
#endif
}

/* ---- the performance summaries' sender (hos_perf_send.h) ----
   The renderer leaves a summary in the game folder's perf_pending/ every few minutes of play and at exit while "Send
   performance statistics" is on (gpu/vk/perf_report.cpp); here, at the next start, they are posted one by one with the system's
   curl to <the build's address>/telemetry/perf: no account, no id, no redirect followed, https only. */
typedef struct PerfNames { char names[HOS_PERF_MAX_PENDING * 4][40]; int n; } PerfNames;
static void perf_take(const char *name, void *user) {
    PerfNames *p = (PerfNames *)user;
    if (hos_perf_name(name, NULL, NULL) && p->n < HOS_PERF_MAX_PENDING * 4 && strlen(name) < 40) snprintf(p->names[p->n++], 40, "%s", name);
}
static int perf_cmp(const void *a, const void *b) {
    return hos_perf_older((const char *)a, (const char *)b) ? -1 : hos_perf_older((const char *)b, (const char *)a) ? 1 : 0;
}
/* What one kind of report is: where its files wait and are kept once sent, where it goes, what it says it is, and its limits. The
   performance summaries and the pipeline state records (gpu/vk/state_report.h) are sent by the same code, each from its own folders */
typedef struct PerfFeed {
    const char *pending, *sent, *path, *agent;
    long max_body;
    int max_pending, max_sends, keep_sent;
} PerfFeed;
static const PerfFeed perf_summaries = {HOS_PERF_PENDING, HOS_PERF_SENT, HOS_TELEMETRY_PERF_PATH, "heylisten-perf-report",
                                        HOS_PERF_MAX_BODY, HOS_PERF_MAX_PENDING, HOS_PERF_MAX_SENDS, HOS_PERF_MAX_PENDING};
static const PerfFeed pipeline_states = {HOS_PIPES_PENDING, HOS_PIPES_SENT, HOS_TELEMETRY_PIPELINES_PATH, "heylisten-pipeline-states",
                                         HOS_PIPES_MAX_BODY, HOS_PIPES_MAX_PENDING, HOS_PIPES_MAX_SENDS, HOS_PIPES_KEEP_SENT};
static const PerfFeed zone_usage = {HOS_ZONES_PENDING, HOS_ZONES_SENT, HOS_TELEMETRY_ZONES_PATH, "heylisten-zone-usage",
                                    HOS_ZONES_MAX_BODY, HOS_ZONES_MAX_PENDING, HOS_ZONES_MAX_SENDS, HOS_ZONES_KEEP_SENT};
static const PerfFeed profile_usage = {HOS_PROFILE_PENDING, HOS_PROFILE_SENT, HOS_TELEMETRY_PROFILE_PATH, "heylisten-profile",
                                       HOS_PROFILE_MAX_BODY, HOS_PROFILE_MAX_PENDING, HOS_PROFILE_MAX_SENDS, HOS_PROFILE_KEEP_SENT};
/* (the sent summaries kept as they went, perf_sent/: what the team got, for the player to read; the newest 48) */
static void perf_keep_sent(const char *dir, const PerfFeed *feed, const char *name, const char *body, size_t n) {
    char sent[PATH_MAX_UTF8], path[PATH_MAX_UTF8];
    if (!join(sent, sizeof sent, dir, feed->sent)) return;
    make_dir(sent);
    if (join(path, sizeof path, sent, name)) replace_text(path, body, n);
    static PerfNames kept;
    kept.n = 0;
    each_name(sent, perf_take, &kept);
    qsort(kept.names, (size_t)kept.n, sizeof kept.names[0], perf_cmp);
    for (int i = 0; i + feed->keep_sent < kept.n; i++)
        if (join(path, sizeof path, sent, kept.names[i])) remove_utf8(path);
}
/* One summary posted: the server's HTTP status (0: no answer), its Retry-After in *retry (-1 none) */
static int perf_post(const char *dir, const PerfFeed *feed, const char *url, const char *file, long *retry) {
    char curl[PATH_MAX_UTF8], pending[PATH_MAX_UTF8], headers[PATH_MAX_UTF8], answer[PATH_MAX_UTF8], data[PATH_MAX_UTF8 + 1];
    *retry = -1;
    if (!find_curl(curl, sizeof curl) || !join(pending, sizeof pending, dir, feed->pending) ||
        !join(headers, sizeof headers, pending, ".sending.headers") || !join(answer, sizeof answer, pending, ".sending.answer"))
        return 0;
    remove_utf8(headers), remove_utf8(answer);
    snprintf(data, sizeof data, "@%s", file);
    const long limit = hos_perf_env_number(getenv("HL_PERF_TIMEOUT"), HOS_PERF_TIMEOUT, 2, 60);
    char secs[16];
    snprintf(secs, sizeof secs, "%ld", limit);
    const char *const proto = strncmp(url, "https://", 8) ? "=http" : "=https";   /* (http: only HL_TELEMETRY_TEST_HTTP's loopback) */
    const char *const args[] = {"--disable", "--silent", "--show-error", "--max-time", secs, "--connect-timeout", secs, "--proto", proto,
                                "--header", "Content-Type: application/json", "--user-agent", feed->agent,
                                "--data-binary", data, "--dump-header", headers, "--output", answer, url};
    const int ran = spawn_exe(curl, args, (int)(sizeof args / sizeof args[0]), (int)limit * 1000 + 5000, NULL);
    char *head = ran == 1 ? read_text(headers, 16 << 10, NULL) : NULL;
    const int status = head ? hos_crash_http_status(head) : 0;
    if (head) *retry = hos_perf_header_retry(head);
    free(head), remove_utf8(headers), remove_utf8(answer);
    return status;
}
/* Sends what waits in dir's perf_pending/ (dir: the game folder), when the player chose to and the build has an address.
   Returns the summaries the server took or refused; -1 when nothing was tried: not chosen, no address, no curl, or the server
   asked to be left alone (the summaries stay for the next start) */
static int perf_feed_run(const char *dir, const PerfFeed *feed) {
    char path[PATH_MAX_UTF8], pending[PATH_MAX_UTF8], url[HOS_TELEMETRY_URL_MAX + 40], base[HOS_TELEMETRY_URL_MAX + 8];
    char stamped[HOS_TELEMETRY_URL_MAX + 8] = "";
    char *ini = join(path, sizeof path, dir, "hookshot_options.ini") ? read_text(path, 256 << 10, NULL) : NULL;
    const int chosen = ini && hos_telemetry_opt_on(ini, HOS_TELEMETRY_KEY_PERF);
    free(ini);
    if (!chosen || !join(pending, sizeof pending, dir, feed->pending)) return -1;
    char *stamp = join(path, sizeof path, dir, "hookshot_version.txt") ? read_text(path, 16 << 10, NULL) : NULL;
    if (stamp) hos_telemetry_stamp_value(stamp, "telemetry", stamped, sizeof stamped);
    free(stamp);
    if (!hos_telemetry_base(getenv(HOS_TELEMETRY_ENV), getenv(HOS_TELEMETRY_TEST_HTTP_ENV), stamped, base, sizeof base) ||
        !hos_telemetry_join(url, sizeof url, base, feed->path))
        return -1;
    char curl[PATH_MAX_UTF8];
    if (!find_curl(curl, sizeof curl)) return -1;
    static PerfNames waiting;
    waiting.n = 0;
    each_name(pending, perf_take, &waiting);
    qsort(waiting.names, (size_t)waiting.n, sizeof waiting.names[0], perf_cmp);
    int skip = 0;
    for (; skip + feed->max_pending < waiting.n; skip++)   /* (too many: the oldest go unsent) */
        if (join(path, sizeof path, pending, waiting.names[skip])) remove_utf8(path);
    const int backoff = hos_perf_env_number(getenv("HL_PERF_BACKOFF"), 1, 0, 1) != 0;
    char next[PATH_MAX_UTF8];
    long before = 0;
    if (!join(next, sizeof next, pending, HOS_PERF_NEXT)) return -1;
    {
        char *text = read_text(next, 256, NULL);
        int64_t at = 0;
        long wait = 0;
        if (text && hos_perf_next_parse(text, &at, &wait)) {
            before = wait;
            if (backoff && at > (int64_t)time(NULL)) return free(text), -1;
        }
        free(text);
    }
    const long most = hos_perf_env_number(getenv("HL_PERF_MAX_SENDS"), feed->max_sends, 1, feed->max_pending);
    int done = 0, all = 1;
    for (int i = skip; i < waiting.n && done < most; i++) {
        if (!join(path, sizeof path, pending, waiting.names[i])) continue;
        size_t n = 0;
        char *body = read_text(path, (size_t)feed->max_body + 1, &n);
        int result;
        if (!body || n == 0 || n > (size_t)feed->max_body) {
            result = HOS_PERF_REFUSED;   /* (unreadable or too big: dropped) */
        } else {
            long retry = -1;
            const int status = perf_post(dir, feed, url, path, &retry);
            if (!status) {   /* (not reached: kept for the next start) */
                free(body), all = 0;
                break;
            }
            result = hos_perf_answer(status);
            if (result == HOS_PERF_BUSY) {
                if (backoff) {
                    char note[64];
                    const long wait = hos_perf_busy_wait(retry, before);
                    const int k = snprintf(note, sizeof note, "%lld %ld\n", (long long)time(NULL) + wait, wait);
                    if (k > 0) replace_text(next, note, (size_t)k);
                }
                free(body), all = 0;
                break;
            }
        }
        remove_utf8(path);   /* (before the copy: nothing between the server's yes and the summary leaving perf_pending/) */
        if (result == HOS_PERF_SENT_OK && body) perf_keep_sent(dir, feed, waiting.names[i], body, n);
        free(body);
        done++;
    }
    if (all && done == waiting.n - skip) remove_utf8(next);
    return done;
}
int hos_perf_send_run(const char *dir) { return perf_feed_run(dir, &perf_summaries); }
/* The pipeline state records the renderer queued in pipelines_pending/ (gpu/vk/state_report.h), by the same rules, to
   <the build's address>/telemetry/pipelines. Nothing with HL_TELEMETRY_PIPELINES=0, nor without Send performance statistics. The
   return is hos_perf_send_run's */
int hos_pipes_send_run(const char *dir) {
    if (!hos_telemetry_pipelines_on(getenv(HOS_TELEMETRY_PIPELINES_ENV))) return -1;
    return perf_feed_run(dir, &pipeline_states);
}
/* The per-zone shader and pipeline usage the renderer queued in zones_pending/ (gpu/vk/zone_report.h), by the same rules,
   to <the build's address>/telemetry/zones. Nothing with HL_TELEMETRY_ZONES=0, nor without Send performance statistics. The
   return is hos_perf_send_run's */
int hos_zones_send_run(const char *dir) {
    if (!hos_telemetry_zones_on(getenv(HOS_TELEMETRY_ZONES_ENV))) return -1;
    return perf_feed_run(dir, &zone_usage);
}
/* The detailed performance profile the renderer queued in profile_pending/ (gpu/vk/profile_report.h), by the same rules, to
   <the build's address>/telemetry/profile. Nothing with HL_TELEMETRY_PROFILE=0, nor without Send performance statistics. The
   return is hos_perf_send_run's */
int hos_profile_send_run(const char *dir) {
    if (!hos_telemetry_profile_on(getenv(HOS_TELEMETRY_PROFILE_ENV))) return -1;
    return perf_feed_run(dir, &profile_usage);
}
#ifdef _WIN32
static DWORD WINAPI perf_send_thread(void *p) {
#else
static void *perf_send_thread(void *p) {
#endif
    char *dir = (char *)p;
    void hos_log(const char *fmt, ...);
    const int n = hos_perf_send_run(dir);
    if (n >= 0) hos_log("perf: %d performance summaries sent or refused (Send performance statistics is on)", n);
    const int m = hos_pipes_send_run(dir);
    if (m > 0) hos_log("perf: %d pipeline state batches sent or refused (Send performance statistics is on, HL_TELEMETRY_PIPELINES is not 0)", m);
    else if (n >= 0 && !hos_telemetry_pipelines_on(getenv(HOS_TELEMETRY_PIPELINES_ENV))) hos_log("perf: pipeline state records not sent: HL_TELEMETRY_PIPELINES=0");
    const int k = hos_zones_send_run(dir);
    if (k > 0) hos_log("perf: %d zone usage batches sent or refused (Send performance statistics is on, HL_TELEMETRY_ZONES is not 0)", k);
    else if (n >= 0 && !hos_telemetry_zones_on(getenv(HOS_TELEMETRY_ZONES_ENV))) hos_log("perf: per-zone shader and pipeline usage not sent: HL_TELEMETRY_ZONES=0");
    const int q = hos_profile_send_run(dir);
    if (q > 0) hos_log("perf: %d performance profile batches sent or refused (Send performance statistics is on, HL_TELEMETRY_PROFILE is not 0)", q);
    else if (n >= 0 && !hos_telemetry_profile_on(getenv(HOS_TELEMETRY_PROFILE_ENV))) hos_log("perf: performance profile not sent: HL_TELEMETRY_PROFILE=0");
    free(dir);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
/* At the game's start (hos_main.c), on a thread of its own: the summaries the last runs left. Nothing unless the player chose
   Send performance statistics and the build has an address (hos_perf_send_run) */
void hos_perf_send_start(void) {
    const char *dir = hos_crash_record_dir();
    if (!dir || !dir[0] || env_on("HOS_HEADLESS") || env_on("HOS_PGO")) return;
    char *copy = strdup(dir);
    if (!copy) return;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, perf_send_thread, copy, 0, NULL);
    if (t) CloseHandle(t);
    else free(copy);
#else
    pthread_t t;
    if (!pthread_create(&t, NULL, perf_send_thread, copy)) pthread_detach(t);
    else free(copy);
#endif
}
