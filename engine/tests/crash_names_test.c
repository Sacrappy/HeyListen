/* The crash names as a game-free fixture (tests/test_crash_names.py): a crafted nx2/symtab.py table, looked up as
   the crash handler does, then the signature the report groups on. The counting allocator proves the lookup path
   makes none (the loader's two are counted separately). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hos_crash_names.h"
#include "hos_crash_dialog.h"

static int allocations;
void *counting_malloc(size_t n) { allocations++; return malloc(n); }
void counting_free(void *p) { allocations--; free(p); }

static int checks;
static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
    checks++;
}
static int names(uintptr_t base, uint64_t rva, const char *want) {
    char got[400];
    const int named = hos_crash_name("HeyListen.exe", rva, base, got, sizeof got);
    if (want) {
        if (!named || strcmp(got, want)) fprintf(stderr, "  got %s, wanted %s\n", named ? got : "(none)", want);
        return named && !strcmp(got, want);
    }
    return !named;
}
/* A PE this test shapes, so the table's identity check has the same fields nx2/symtab.py read from the crafted file */
static void fake_pe(unsigned char *image, uint32_t timestamp, uint32_t size_of_image, uint32_t checksum) {
    memset(image, 0, 4096);
    image[0] = 'M';
    image[1] = 'Z';
    const uint32_t at = 0x80;
    memcpy(image + 0x3C, &at, 4);
    memcpy(image + at, "PE\0\0", 4);
    memcpy(image + at + 8, &timestamp, 4);
    memcpy(image + at + 80, &size_of_image, 4);
    memcpy(image + at + 88, &checksum, 4);
}
static void table(const char *path) {
    const unsigned long reads = hos_crash_names_reads();
    check(hos_crash_names_load(HOS_CRASH_NAMES_EXE, path), "the crafted table loads");
    check(hos_crash_names_reads() == reads + 1, "a load is one read");
    check(hos_crash_names_count(HOS_CRASH_NAMES_EXE) == 3, "the created table's three functions");
    check(hos_crash_names_load(HOS_CRASH_NAMES_EXE, path), "a table loads a second time");
    check(hos_crash_names_reads() == reads + 2, "the second load is counted");
    static unsigned char image[4096];
    fake_pe(image, 0x11223344u, 0x1000u, 0xabcdefu);
    const unsigned long before = hos_crash_names_reads();
    const int allocations_before = allocations;
    check(names((uintptr_t)image, 0x1000, "f_80580b38+0x0"), "the first function, at its start");
    check(names((uintptr_t)image, 0x1010, "crash_now+0x0"), "a host function");
    check(names((uintptr_t)image, 0x1028, "another_one+0x8"), "the offset inside a function");
    check(names((uintptr_t)image, 0x0fff, NULL), "an address before the first function names none");
    check(names((uintptr_t)image, 0x1020, "another_one+0x0"), "a dropped local label leaves its function");
    check(names(0, 0x1000, "f_80580b38+0x0"), "base 0 skips the identity check (the tests' path)");
    fake_pe(image, 0x11223344u, 0x2000u, 0xabcdefu);   /* (another build's image) */
    check(names((uintptr_t)image, 0x1000, NULL), "a stale table from another build is refused");
    fake_pe(image, 0x11223344u, 0x1000u, 0xabcdefu);
    check(hos_crash_names_reads() == before, "the lookups read no file");
    check(allocations == allocations_before, "the lookups allocate nothing");
    /* the guest names (hl_host.c's table, registered at start-up) */
    static const uint32_t addrs[] = {0x80580b00u, 0x80580c00u, 0x80cb38d0u};
    static void *const fns[] = {(void *)(uintptr_t)0x1000, (void *)(uintptr_t)0x2000, (void *)(uintptr_t)0x3000};
    hos_crash_names_lifted(addrs, fns, 3);
    char got[100];
    check(hos_crash_guest_name(0x80580b38, got, sizeof got) && !strcmp(got, "f_80580b00+0x38"), "a guest return address");
    check(hos_crash_guest_name(0x80580c00, got, sizeof got) && !strcmp(got, "f_80580c00+0x0"), "a guest function's start");
    check(!hos_crash_guest_name(0x90000000, got, sizeof got), "a guest address past the table names none");
    check(hos_crash_lifted_name(0x2000, 0x2010, got, sizeof got) && !strcmp(got, "f_80580c00+0x10"), "an exact host frame");
    check(hos_crash_lifted_name(0, 0x2ff0, got, sizeof got) && !strcmp(got, "~f_80580c00+0xff0"), "a guessed host frame");
    check(allocations == allocations_before, "the guest lookups allocate nothing");
    check(hos_crash_names_reads() == before, "and read no file");
    printf("%d checks: a crafted table, exact and stale identities, guest names, no allocations\n", checks);
}
static void signature(int argc, char **argv) {
    char got[200];
    const char *const *lines = (const char *const *)(argv + 2);
    const int found = hos_crash_signature(got, sizeof got, lines, argc - 2);
    printf("%d|%s\n", found, got);
}
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "table")) table(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "signature")) signature(argc, argv);
    else {
        fprintf(stderr, "usage: %s table <sym> | signature <line>...\n", argv[0]);
        return 2;
    }
    return 0;
}
