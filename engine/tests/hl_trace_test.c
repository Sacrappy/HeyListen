/* Game-free test of the crash recorder's core (host/hl_trace_core.c; tests/test_hl_trace.py builds it): the threads' rings
   under eight writers (no torn event, each ring in order), the live and freed blocks' tables (an address inside a freed
   block, a block allocated again over a freed one, removals among colliding entries, eight threads at once), the file,
   and the crash report's lines on a synthetic state. Exits non-zero on a failure. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hl_trace.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static char lines[4096][256];
static int nlines;
static void out(const char *line) {
    if (nlines < 4096) snprintf(lines[nlines++], sizeof lines[0], "%s", line);
}
static _Thread_local char my_name[32];
static const char *name(void) { return my_name; }
static int has_line(const char *needle) {
    for (int k = 0; k < nlines; k++)
        if (strstr(lines[k], needle)) return 1;
    return 0;
}

enum { WRITERS = 8, EVENTS = 1024, PER = 3000 };

static DWORD WINAPI writer(void *p) {
    const uint64_t t = (uint64_t)(uintptr_t)p;
    snprintf(my_name, sizeof my_name, "writer%llu", (unsigned long long)t);
    for (uint64_t i = 0; i < PER; i++) hl_trace_event(HL_EV_JOB, t << 32 | i, ~(t << 32 | i), (uint32_t)i);
    /* blocks of its own: allocated, half freed */
    for (uint64_t i = 0; i < 2000; i++) {
        const uint32_t chain[HL_TRACE_CHAIN] = {(uint32_t)(0x80000000 + t), (uint32_t)i};
        hl_trace_alloc(0x100000000ull * (t + 1) + i * 0x40, 0x40, t, chain);
    }
    for (uint64_t i = 0; i < 2000; i += 2) hl_trace_free(0x100000000ull * (t + 1) + i * 0x40, t, NULL);
    return 0;
}

int main(void) {
    snprintf(my_name, sizeof my_name, "main");
    const HlTraceConfig cfg = {.thread_name = name, .out = out, .ring_events = EVENTS, .live_slots = 1u << 16, .freed_slots = 1u << 14};
    CHECK(hl_trace_start(HL_TR_ALL, &cfg), "started");
    HANDLE th[WRITERS];
    for (int t = 0; t < WRITERS; t++) th[t] = CreateThread(NULL, 0, writer, (void *)(uintptr_t)t, 0, NULL);
    WaitForMultipleObjects(WRITERS, th, TRUE, INFINITE);

    /* the tables after eight threads: every even block freed, every odd one live, with its record */
    for (uint64_t t = 0; t < WRITERS; t++)
        for (uint64_t i = 0; i < 2000; i++) {
            HlTraceRecord live;
            HlTraceFree fr;
            const uint64_t a = 0x100000000ull * (t + 1) + i * 0x40 + 8;
            const int got = hl_trace_lookup(a, &live, &fr);
            if (i % 2) CHECK(got == 1 && live.size == 0x40 && live.heap == t && live.chain[1] == (uint32_t)i, "live %llx", (unsigned long long)a);
            else CHECK(got == 2 && fr.has_alloc && fr.alloc.chain[0] == 0x80000000 + t && fr.freed.size == 0x40, "freed %llx", (unsigned long long)a);
        }

    /* a block freed, then a smaller one allocated over it: both told */
    const uint32_t c1[HL_TRACE_CHAIN] = {0x80001000, 0x80002000}, c2[HL_TRACE_CHAIN] = {0x80003000};
    hl_trace_alloc(0x15000, 0x90, 7, c1);
    hl_trace_free(0x15000, 7, c2);
    HlTraceRecord live;
    HlTraceFree fr;
    CHECK(hl_trace_lookup(0x15028, &live, &fr) == 2 && fr.freed.addr == 0x15000 && fr.alloc.chain[1] == 0x80002000 &&
          fr.freed.chain[0] == 0x80003000, "the freed block and its two records");
    hl_trace_alloc(0x15020, 0x20, 7, c1);
    CHECK(hl_trace_lookup(0x15028, &live, &fr) == 3 && live.addr == 0x15020 && fr.freed.addr == 0x15000, "reused after its free");
    CHECK(hl_trace_lookup(0x15088, &live, &fr) == 2, "the rest of the freed block");
    CHECK(hl_trace_lookup(0x9999999, &live, &fr) == 0, "an address nothing knows");

    /* the report on that state */
    const uint64_t regs[2] = {0x15000, 0x15028};
    const char *const names[2] = {"x0", "x1"};
    hl_trace_report(0x15008, regs, names, 2, 20, 5);
    CHECK(has_line("the faulting address 0x15008 is in a FREED block 0x15000 (+0x8)"), "the fault's block");
    CHECK(has_line("x1 0x15028 is in a FREED block 0x15000 (+0x28), since allocated again"), "a register's block");
    CHECK(has_line("x1 0x15028 is in a live block 0x15020 (+0x8)"), "the block over it");
    CHECK(has_line("by 0x80003000") && has_line("by 0x80001000 0x80002000"), "the callers");
    CHECK(has_line("writer3") && has_line("main") && has_line(" free "), "the events with the threads' names");

    /* the rings, through the file: each writer's last EVENTS events whole and in order */
    char path[MAX_PATH];
    GetTempPathA(sizeof path, path);
    strcat(path, "hl_trace_test.bin");
    CHECK(hl_trace_write(path) == 0, "written");
    FILE *fp = fopen(path, "rb");
    char magic[8];
    double tms = 0;
    uint64_t now = 0;
    uint32_t events = 0, nr = 0;
    if (!fp) { puts("FAIL: no file"); return 1; }
    CHECK(fread(magic, 1, 8, fp) == 8 && !memcmp(magic, "HLTRACE1", 8), "the file's header");
    if (fread(&tms, 8, 1, fp) != 1 || fread(&now, 8, 1, fp) != 1 || fread(&events, 4, 1, fp) != 1 || fread(&nr, 4, 1, fp) != 1)
        CHECK(0, "the file's header");
    CHECK(events == EVENTS && nr == WRITERS + 1, "%u events, %u rings", events, nr);
    static uint64_t ev[EVENTS][4];
    int writers = 0;
    for (uint32_t r = 0; r < nr; r++) {
        char rn[32];
        uint32_t tid;
        uint64_t head;
        if (fread(rn, 1, 32, fp) != 32 || fread(&tid, 4, 1, fp) != 1 || fread(&head, 8, 1, fp) != 1 ||
            fread(ev, 32, EVENTS, fp) != EVENTS)
            break;
        if (strncmp(rn, "writer", 6)) continue;
        writers++;
        uint64_t last = 0, prev_i = 0;
        int jobs = 0;
        for (uint64_t k = head - EVENTS; k < head; k++) {
            const uint64_t *e = ev[k % EVENTS];
            const uint32_t kind = (uint32_t)e[1];
            CHECK(e[0] >= last, "ring %s in order", rn);
            last = e[0];
            if (kind != HL_EV_JOB) continue;
            CHECK(e[3] == ~e[2] && (uint32_t)(e[1] >> 32) == (uint32_t)e[2], "an event whole");
            if (jobs++) CHECK((uint32_t)e[2] == prev_i + 1, "the events one after the other");
            prev_i = (uint32_t)e[2];
        }
    }
    CHECK(writers == WRITERS, "%d writers' rings", writers);
    if (fp) fclose(fp);
    remove(path);
    if (failures) {
        printf("hl_trace_test: %d failures\n", failures);
        return 1;
    }
    puts("hl trace: ok");
    return 0;
}
