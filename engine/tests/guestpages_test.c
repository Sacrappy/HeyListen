/* Game-free test of host/hos_guestpages.c (tests/test_guestpages.py builds it with that file; Windows only): a small window
   reserved the way the guest window is, granules dropped and restored while threads touch them. Exits non-zero on a
   failure. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hos_guestpages.h"

#define GRAN 0x10000ULL
#define WINDOW (256ULL << 20)

void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("  log: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static uint8_t *win;
static uint64_t expected(uint8_t *p) { const uint64_t a = (uint64_t)(p - win); return a * 0x9E3779B97F4A7C15ULL ^ 0x5bd1e995; }
static void pattern(uint8_t *p, size_t n) {
    for (size_t k = 0; k < n; k += 8) *(uint64_t *)(p + k) = expected(p + k);
}

/* the owner: restores from the pattern (what the bytes were), slowly when asked */
static volatile LONG owner_calls, fill_calls, slow_ms;
static int fill_pattern(void *user, void *buf, void *p, size_t size) {
    (void)user;
    InterlockedIncrement(&fill_calls);
    if (slow_ms) Sleep((DWORD)slow_ms);
    for (size_t k = 0; k < size; k += 8) *(uint64_t *)((uint8_t *)buf + k) = expected((uint8_t *)p + k);
    return 0;
}
static int fill_fails(void *user, void *buf, void *p, size_t size) { (void)user, (void)buf, (void)p, (void)size; return 7; }
static volatile LONG forget_calls;
static void *forget_at;
static size_t forget_size;
static void on_forget(void *p, size_t size) { InterlockedIncrement(&forget_calls); forget_at = p, forget_size = size; }

static int owner(void *p, size_t size, int write) {
    (void)write;
    InterlockedIncrement(&owner_calls);
    return hos_gp_restore(p, size, fill_pattern, NULL) == 0;
}

/* the handler, as the OS layer's commit_on_touch: a dropped granule to the substrate, a reserved page committed (the 1 MB
   around it, else its 64 KB) */
static LONG CALLBACK handler(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *p = (uint8_t *)rec->ExceptionInformation[1];
    if (p < win || p >= win + WINDOW) return EXCEPTION_CONTINUE_SEARCH;
    const int r = hos_gp_fault(p, rec->ExceptionInformation[0] == 1);
    if (r > 0) return EXCEPTION_CONTINUE_EXECUTION;
    if (r < 0) return EXCEPTION_CONTINUE_SEARCH;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m)) return EXCEPTION_CONTINUE_SEARCH;
    if (m.State == MEM_COMMIT) return EXCEPTION_CONTINUE_EXECUTION;   /* (another thread was first) */
    if (VirtualAlloc((void *)((uintptr_t)p & ~0xFFFFFULL), 0x100000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_EXECUTION;
    if (VirtualAlloc((void *)((uintptr_t)p & ~0xFFFFULL), 0x10000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

static int all_expected(uint8_t *p, size_t n) {
    for (size_t k = 0; k < n; k += 8)
        if (*(volatile uint64_t *)(p + k) != expected(p + k)) return 0;
    return 1;
}
static int all_zero(uint8_t *p, size_t n) {
    for (size_t k = 0; k < n; k += 8)
        if (*(volatile uint64_t *)(p + k)) return 0;
    return 1;
}

/* 8 threads reading a dropped range at once, word by word, from different starting points */
typedef struct { uint8_t *p; size_t n; int id; volatile LONG *go; LONG bad, words; } Reader;
static DWORD WINAPI reader(void *arg) {
    Reader *r = (Reader *)arg;
    while (!*r->go) YieldProcessor();
    for (int round = 0; round < 4; round++)
        for (size_t k = 0; k < r->n; k += 8) {
            uint8_t *q = r->p + ((k + (size_t)r->id * 0x2008) % r->n);
            q = (uint8_t *)((uintptr_t)q & ~7ULL);
            if (*(volatile uint64_t *)q != expected(q)) r->bad++;
            r->words++;
        }
    return 0;
}
static DWORD WINAPI restorer(void *arg) {
    Reader *r = (Reader *)arg;
    while (!*r->go) YieldProcessor();
    r->bad = hos_gp_restore(r->p, r->n, fill_pattern, NULL);
    return 0;
}

/* page by page commits of a reserved range, held as the OS layer holds them (hos_gp_hold) */
typedef struct { uint8_t *p; size_t n; volatile LONG *go; LONG failed; } Committer;
static DWORD WINAPI committer(void *arg) {
    Committer *c = (Committer *)arg;
    while (!*c->go) YieldProcessor();
    for (size_t k = 0; k < c->n; k += 0x1000) {
        hos_gp_hold(1);
        if (!VirtualAlloc(c->p + k, 0x1000, MEM_COMMIT, PAGE_READWRITE)) c->failed++;
        hos_gp_hold(0);
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* a free place for the window */
    void *probe = VirtualAlloc(NULL, WINDOW, MEM_RESERVE, PAGE_NOACCESS);
    VirtualFree(probe, 0, MEM_RELEASE);
    CHECK(hos_gp_granule() == 0 && !hos_gp_ready(), "ready before the reservation");
    if (!hos_gp_reserve(probe, WINDOW)) { printf("FAIL: the window could not be reserved\n"); return 1; }
    win = (uint8_t *)probe;
    CHECK(hos_gp_ready() && hos_gp_granule() == GRAN, "not ready after the reservation");
    AddVectoredExceptionHandler(1, handler);
    hos_gp_set_owner(owner);
    hos_gp_set_on_forget(on_forget);

    /* 32 MB committed in one call (one reservation), the pattern written */
    CHECK(VirtualAlloc(win, 32 << 20, MEM_COMMIT, PAGE_READWRITE) == win, "commit of 32 MB");
    pattern(win, 32 << 20);

    printf("1. drop, then 8 threads read the range while another restores it with a slow fill\n");
    uint8_t *r1 = win + 0x100000;   /* 8 granules */
    CHECK(hos_gp_drop(r1, 8 * GRAN) == 0, "drop");
    for (int i = 0; i < 8; i++) CHECK(hos_gp_is_dropped(r1 + i * GRAN), "granule %d not dropped", i);
    CHECK(!hos_gp_is_dropped(r1 - 1) && !hos_gp_is_dropped(r1 + 8 * GRAN), "a neighbour dropped");
    CHECK(all_expected(r1 - GRAN, GRAN) && all_expected(r1 + 8 * GRAN, GRAN), "the neighbours' bytes changed");
    {
        MEMORY_BASIC_INFORMATION m;
        VirtualQuery(r1, &m, sizeof m);
        CHECK(m.State == MEM_RESERVE, "a dropped granule is not a placeholder (state %#lx)", m.State);
    }
    slow_ms = 200;
    volatile LONG go = 0;
    Reader rd[8], rs = {r1, 8 * GRAN, 0, &go, 0, 0};
    HANDLE th[9];
    th[8] = CreateThread(NULL, 0, restorer, &rs, 0, NULL);
    for (int i = 0; i < 8; i++) {
        rd[i] = (Reader){r1, 8 * GRAN, i, &go, 0, 0};
        th[i] = CreateThread(NULL, 0, reader, &rd[i], 0, NULL);
    }
    InterlockedExchange(&go, 1);
    WaitForMultipleObjects(9, th, TRUE, INFINITE);
    LONG bad = 0, words = 0;
    for (int i = 0; i < 8; i++) bad += rd[i].bad, words += rd[i].words, CloseHandle(th[i]);
    CloseHandle(th[8]);
    CHECK(bad == 0, "%ld words of %ld read wrong while restoring", bad, words);
    CHECK(rs.bad == 0, "the restore returned %ld", rs.bad);
    for (int i = 0; i < 8; i++) CHECK(!hos_gp_is_dropped(r1 + i * GRAN), "granule %d still dropped", i);
    CHECK(all_expected(r1, 8 * GRAN), "restored bytes wrong");
    printf("   %ld words read by 8 threads, %ld wrong; owner calls %ld, fills %ld\n", words, bad, owner_calls, fill_calls);
    slow_ms = 0;

    printf("2. touches from 8 threads alone (the handler and the owner restore)\n");
    CHECK(hos_gp_drop(r1, 8 * GRAN) == 0, "drop of a restored range");
    slow_ms = 50;
    go = 0;
    for (int i = 0; i < 8; i++) {
        rd[i] = (Reader){r1, 8 * GRAN, i, &go, 0, 0};
        th[i] = CreateThread(NULL, 0, reader, &rd[i], 0, NULL);
    }
    InterlockedExchange(&go, 1);
    WaitForMultipleObjects(8, th, TRUE, INFINITE);
    bad = 0;
    for (int i = 0; i < 8; i++) bad += rd[i].bad, CloseHandle(th[i]);
    CHECK(bad == 0, "%ld words read wrong after touch restores", bad);
    CHECK(all_expected(r1, 8 * GRAN), "bytes wrong after touch restores");
    slow_ms = 0;

    printf("3. a restore next to memory committed on first touch\n");
    uint8_t *r3 = win + (64 << 20);   /* reserved only: never committed */
    CHECK(hos_gp_drop(r3 + GRAN, GRAN) == 0, "drop of a reserved granule");
    CHECK(hos_gp_restore(r3 + GRAN, GRAN, fill_pattern, NULL) == 0, "restore");
    CHECK(all_zero(r3, GRAN), "the granule before is not zero (committed on touch)");
    CHECK(all_expected(r3 + GRAN, GRAN), "the restored granule is wrong");
    CHECK(all_zero(r3 + 2 * GRAN, 4 * GRAN), "the granules after are not zero (committed on touch)");

    printf("4. a write to a dropped page: restored, then written\n");
    uint8_t *r4 = win + 0x300000;
    CHECK(hos_gp_drop(r4, 2 * GRAN) == 0, "drop");
    const LONG before = owner_calls;
    *(volatile uint64_t *)(r4 + GRAN + 0x1238) = 0x1122334455667788ULL;
    CHECK(owner_calls == before + 1, "the owner was asked %ld times", owner_calls - before);
    CHECK(*(volatile uint64_t *)(r4 + GRAN + 0x1238) == 0x1122334455667788ULL, "the write was lost");
    *(volatile uint64_t *)(r4 + GRAN + 0x1238) = expected(r4 + GRAN + 0x1238);
    CHECK(all_expected(r4, 2 * GRAN), "the rest of the written granule is wrong");

    printf("5. a fill that fails leaves the range dropped; an imported range is not dropped\n");
    uint8_t *r5 = win + 0x400000;
    CHECK(hos_gp_drop(r5, GRAN) == 0, "drop");
    CHECK(hos_gp_restore(r5, GRAN, fill_fails, NULL) == 7, "the fill's result is not returned");
    CHECK(hos_gp_is_dropped(r5), "dropped no more after a failed fill");
    CHECK(all_expected(r5, GRAN), "touch after a failed fill");
    hos_gp_imported(win + 0x500000, 2 * GRAN);
    CHECK(hos_gp_drop(win + 0x500000, GRAN) == -2, "an imported granule was dropped, or refused for another reason");
    CHECK(!hos_gp_is_dropped(win + 0x500000), "an imported granule is marked dropped");
    CHECK(hos_gp_drop(win + 0x600000 + 0x1000, GRAN) != 0, "an unaligned drop was taken");
    CHECK(hos_gp_restore(win + 0x700000, GRAN, fill_pattern, NULL) == 0 && all_expected(win + 0x700000, GRAN),
          "restore of a range never dropped");
    {   /* (an import over dropped granules restores them first) */
        CHECK(hos_gp_drop(win + 0x800000, 2 * GRAN) == 0, "drop");
        hos_gp_imported(win + 0x800000 - 0x1000, 2 * GRAN);
        CHECK(!hos_gp_is_dropped(win + 0x800000) && !hos_gp_is_dropped(win + 0x810000), "an import left a granule dropped");
    }

    printf("6. commit, ReadFile and decommit over ranges with dropped and restored granules\n");
    uint8_t *r6 = win + 0x900000;   /* 1 MB: granules 2, 3 dropped, 5 restored (a view), the rest private */
    CHECK(hos_gp_drop(r6 + 2 * GRAN, 2 * GRAN) == 0 && hos_gp_drop(r6 + 5 * GRAN, GRAN) == 0, "drop");
    CHECK(hos_gp_restore(r6 + 5 * GRAN, GRAN, fill_pattern, NULL) == 0, "restore");
    CHECK(hos_gp_touches(r6, 0x100000) && !hos_gp_touches(win + 0x1000000, 0x100000), "touches");
    LONG calls = owner_calls;
    CHECK(hos_gp_commit(r6, 0x100000), "commit over the pieces");
    CHECK(owner_calls == calls + 1 && !hos_gp_is_dropped(r6 + 2 * GRAN) && !hos_gp_is_dropped(r6 + 3 * GRAN),
          "the commit did not have the owner restore the dropped granules");
    CHECK(all_expected(r6, 0x100000), "bytes after the commit");
    {   /* the kernel writes across a private piece, two views and private memory again */
        char path[MAX_PATH];
        GetTempPathA(MAX_PATH, path);
        strcat(path, "guestpages_test.bin");
        HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        static uint8_t src[4 * GRAN];
        memset(src, 0xA5, sizeof src);
        DWORD got = 0;
        WriteFile(f, src, sizeof src, &got, NULL);
        SetFilePointer(f, 0, NULL, FILE_BEGIN);
        CHECK(ReadFile(f, r6 + GRAN + 0x800, 4 * GRAN, &got, NULL) && got == 4 * GRAN, "ReadFile across pieces (error %lu)", GetLastError());
        CHECK(r6[GRAN + 0x800] == 0xA5 && r6[5 * GRAN + 0x7FF] == 0xA5, "ReadFile's bytes");
        CloseHandle(f);
        pattern(r6, 0x100000);
    }
    /* decommit [r6 + 0x8000, r6 + 0xF8000): granules 1..14 wholly inside, 0 and 15 cut. Drop 0, 1, 7 and 15 first: 1 and 7 are
       forgotten (the owner is not asked), 0 and 15 restored by the owner first and then cut */
    CHECK(hos_gp_drop(r6, 2 * GRAN) == 0 && hos_gp_drop(r6 + 7 * GRAN, GRAN) == 0 && hos_gp_drop(r6 + 15 * GRAN, GRAN) == 0, "drop");
    calls = owner_calls;
    CHECK(hos_gp_decommit(r6 + 0x8000, 0x100000 - 0x10000), "decommit over the pieces");
    CHECK(owner_calls == calls + 2, "the owner was asked %ld times for the cut granules (2 expected)", owner_calls - calls);
    for (int i = 0; i < 16; i++) CHECK(!hos_gp_is_dropped(r6 + i * GRAN), "granule %d still dropped after the decommit", i);
    CHECK(all_expected(r6, 0x8000) && all_expected(r6 + 0xF8000, 0x8000), "the cut ends lost their bytes");
    calls = owner_calls;
    CHECK(all_zero(r6 + 0x8000, 0x100000 - 0x10000), "the decommitted range does not read as zero");
    CHECK(owner_calls == calls, "the owner was asked for forgotten granules");
    CHECK(hos_gp_commit(r6, 0x100000) && all_zero(r6 + 0x8000, 0x10000), "commit after the decommit");
    CHECK(hos_gp_reset(r6, 0x100000), "reset over the pieces");

    printf("7. a region holding dropped granules given up whole, then used again\n");
    uint8_t *r7 = win + 0xC00000;   /* 4 MB */
    for (int i = 0; i < 64; i += 3) CHECK(hos_gp_drop(r7 + i * GRAN, GRAN) == 0, "drop %d", i);
    CHECK(hos_gp_restore(r7 + 9 * GRAN, GRAN, fill_pattern, NULL) == 0, "restore");
    calls = owner_calls;
    CHECK(hos_gp_decommit(r7, 4 << 20), "decommit of the region");
    CHECK(owner_calls == calls, "the owner was asked during the decommit of whole granules");
    CHECK(forget_calls == 2 && forget_at == r7 && forget_size == (4 << 20), "the forget hook was not told of the region (%ld calls)",
          forget_calls);
    CHECK(all_zero(r7, 4 << 20), "the region does not read as zero");
    CHECK(hos_gp_commit(r7, 4 << 20), "commit of the region");
    pattern(r7, 4 << 20);
    CHECK(hos_gp_drop(r7, 4 << 20) == 0, "drop of the whole region");
    CHECK(hos_gp_restore(r7, 4 << 20, fill_pattern, NULL) == 0 && all_expected(r7, 4 << 20), "restore of the whole region");

    printf("8. many drops and restores in a row, each granule from 4 threads\n");
    for (int round = 0; round < 20; round++) {
        uint8_t *r = win + (16 << 20) + (size_t)(round % 4) * 0x200000;
        CHECK(hos_gp_drop(r, 0x200000) == 0, "drop round %d", round);
        go = 0;
        for (int i = 0; i < 4; i++) {
            rd[i] = (Reader){r, 0x200000, i * 7, &go, 0, 0};
            th[i] = CreateThread(NULL, 0, reader, &rd[i], 0, NULL);
        }
        InterlockedExchange(&go, 1);
        WaitForMultipleObjects(4, th, TRUE, INFINITE);
        for (int i = 0; i < 4; i++) {
            CHECK(rd[i].bad == 0, "round %d: %ld words wrong", round, rd[i].bad);
            CloseHandle(th[i]);
        }
    }
    printf("9. commits held around drops that split the reservation (unheld, Windows fails some of them meanwhile)\n");
    {
        uint8_t *d = win + (128 << 20);
        CHECK(VirtualAlloc(d, 32 << 20, MEM_COMMIT, PAGE_READWRITE) == d, "commit");
        go = 0;
        Committer cm[2];
        for (int i = 0; i < 2; i++) {
            cm[i] = (Committer){win + (192 << 20) + (size_t)i * (32 << 20), 32 << 20, &go, 0};
            th[i] = CreateThread(NULL, 0, committer, &cm[i], 0, NULL);
        }
        InterlockedExchange(&go, 1);
        for (int i = 0; i < 256; i++) CHECK(hos_gp_drop(d + (size_t)i * 2 * GRAN, GRAN) == 0, "drop %d", i);
        WaitForMultipleObjects(2, th, TRUE, INFINITE);
        for (int i = 0; i < 2; i++) {
            CHECK(cm[i].failed == 0, "%ld held commits failed", cm[i].failed);
            CloseHandle(th[i]);
        }
    }
    char line[512];
    hos_gp_report(line, sizeof line);
    printf("   %s\n", line);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("guest pages: all checks passed\n");
    return 0;
}
