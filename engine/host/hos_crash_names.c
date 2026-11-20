/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See hos_crash_names.h. The table format is nx2/symtab.py's; this file only reads it. */
#include "hos_crash_names.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

enum { HEADER_BYTES = 32, ENTRY_BYTES = 8, MAX_NAME = 400, MAX_TABLE = 64 << 20 };
static const char kMagic[8] = {'H', 'L', 'S', 'Y', 'M', 0, 0, 1};
/* (tests/test_crash_names.py counts the two calls the loader makes, to prove the lookups make none) */
#ifdef HOS_CRASH_NAMES_MALLOC
void *HOS_CRASH_NAMES_MALLOC(size_t);
void HOS_CRASH_NAMES_FREE(void *);
#else
#define HOS_CRASH_NAMES_MALLOC malloc
#define HOS_CRASH_NAMES_FREE free
#endif

typedef struct Table { unsigned char *data; size_t size; } Table;
static Table tables[HOS_CRASH_NAMES_TABLES];
static const uint32_t *lifted_addrs;
static void *const *lifted_fns;
static uint32_t lifted_count;
static unsigned long reads;

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char *p) {
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}
static uint32_t count_of(const Table *t) { return t->data ? le32(t->data + 8) : 0; }
static uint32_t names_of(const Table *t) { return t->data ? le32(t->data + 12) : 0; }
static const unsigned char *entry_of(const Table *t, uint32_t i) { return t->data + HEADER_BYTES + (size_t)i * ENTRY_BYTES; }
static const char *name_of(const Table *t, uint32_t offset) {
    return offset < names_of(t) ? (const char *)(t->data + HEADER_BYTES + (size_t)count_of(t) * ENTRY_BYTES + offset) : "";
}
static int same(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}
static int contains(const char *text, const char *part) {
    const size_t n = strlen(part);
    for (; *text; text++) if (same(text, part, n)) return 1;
    return 0;
}
/* Which table a module's name asks for; -1 when it is neither the game nor its renderer */
static int which_of(const char *module) {
    if (!module || !module[0]) return -1;
    if (contains(module, "hookshot_vk")) return HOS_CRASH_NAMES_VK;   /* (upstream's name: libhookshot_vk.so) */
    if (contains(module, "heylisten") || contains(module, "hookshot")) return HOS_CRASH_NAMES_EXE;
    return -1;
}
/* The fields a table and the binary it names share: PE TimeDateStamp, SizeOfImage and CheckSum (the same
   formula as nx2/symtab.py's pe_identity); 0 when there is no PE to check (Linux) */
static uint64_t pe_identity(uintptr_t base) {
    if (!base) return 0;
    const unsigned char *image = (const unsigned char *)base;
#ifdef _WIN32
    {   /* (the crash path: no read of a page that is not committed or is a guard) */
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(image, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return 0;
    }
#endif
    if (image[0] != 'M' || image[1] != 'Z') return 0;
    const uint32_t at = le32(image + 0x3C);
    if (at < 0x40 || at > 0x100000) return 0;
#ifdef _WIN32
    {   /* (the NT headers too: e_lfanew is small, but a damaged image is not worth a second fault) */
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(image + at, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return 0;
        const unsigned char *end = (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (image + at + 92 > end) return 0;
    }
#endif
    if (memcmp(image + at, "PE\0\0", 4)) return 0;
    const uint64_t timestamp = le32(image + at + 8);
    const uint64_t size_of_image = le32(image + at + 80);
    const uint64_t checksum = le32(image + at + 88);
    uint64_t value = (timestamp << 32) | size_of_image;
    value ^= (checksum * 0x9E3779B97F4A7C15ULL);
    return value ? value : 1;
}
/* The executable's (or any loaded module's) identity as the tables carry it: the build id the performance profile sends. 0 when unknown */
uint64_t hos_crash_module_identity(uintptr_t base) { return pe_identity(base); }
static int load_table(int which, const char *path) {
    if (which < 0 || which >= HOS_CRASH_NAMES_TABLES || !path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    const long size = ftell(f);
    if (size < HEADER_BYTES || size > MAX_TABLE || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    unsigned char *data = (unsigned char *)HOS_CRASH_NAMES_MALLOC((size_t)size);
    if (!data) { fclose(f); return 0; }
    const size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size || memcmp(data, kMagic, sizeof kMagic)) { HOS_CRASH_NAMES_FREE(data); return 0; }
    const uint32_t count = le32(data + 8), names = le32(data + 12);
    const size_t entries = (size_t)count * ENTRY_BYTES;
    if (entries > (size_t)size - HEADER_BYTES || names > (size_t)size - HEADER_BYTES - entries ||
        HEADER_BYTES + entries + names != (size_t)size) { HOS_CRASH_NAMES_FREE(data); return 0; }
    const unsigned char *entries_at = data + HEADER_BYTES;
    const unsigned char *pool = entries_at + entries;
    uint32_t was = 0;
    for (uint32_t i = 0; i < count; i++) {   /* (ascending, each name inside the pool and terminated) */
        const uint32_t rva = le32(entries_at + (size_t)i * ENTRY_BYTES);
        const uint32_t at = le32(entries_at + (size_t)i * ENTRY_BYTES + 4);
        if (at >= names || !memchr(pool + at, 0, names - at) || (i && rva <= was)) { HOS_CRASH_NAMES_FREE(data); return 0; }
        was = rva;
    }
    HOS_CRASH_NAMES_FREE(tables[which].data);
    tables[which].data = data;
    tables[which].size = (size_t)size;
    __atomic_add_fetch(&reads, 1, __ATOMIC_RELAXED);
    return 1;
}
int hos_crash_names_load(int which, const char *path) { return load_table(which, path); }
int hos_crash_names_loaded(int which) {
    return which >= 0 && which < HOS_CRASH_NAMES_TABLES && tables[which].data != NULL;
}
uint32_t hos_crash_names_count(int which) {
    return hos_crash_names_loaded(which) ? count_of(&tables[which]) : 0;
}
uint64_t hos_crash_names_identity(int which) {
    return hos_crash_names_loaded(which) ? le64(tables[which].data + 16) : 0;
}
unsigned long hos_crash_names_reads(void) { return __atomic_load_n(&reads, __ATOMIC_RELAXED); }

/* The largest entry whose rva is <= target; -1 when none */
static int find(const Table *t, uint64_t target) {
    uint32_t lo = 0, hi = count_of(t);
    if (target > 0xFFFFFFFFULL) return (int)hi - 1;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (le32(entry_of(t, mid)) <= (uint32_t)target) lo = mid + 1;
        else hi = mid;
    }
    return (int)lo - 1;
}
int hos_crash_name(const char *module_name, uint64_t rva, uintptr_t base, char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    const int which = which_of(module_name);
    if (which < 0 || !hos_crash_names_loaded(which)) return 0;
    const Table *t = &tables[which];
    const uint64_t identity = le64(t->data + 16);
    if (base && identity && pe_identity(base) != identity) return 0;   /* (another build's table) */
    const int i = find(t, rva);
    if (i < 0) return 0;
    const uint32_t start = le32(entry_of(t, (uint32_t)i));
    snprintf(out, cap, "%s+0x%llx", name_of(t, le32(entry_of(t, (uint32_t)i) + 4)),
             (unsigned long long)(rva - start));
    return out[0] != 0;
}
void hos_crash_names_lifted(const uint32_t *addrs, void *const *fns, uint32_t count) {
    lifted_addrs = addrs;
    lifted_fns = fns;
    lifted_count = addrs && fns ? count : 0;
}
static void lifted_text(char *out, size_t cap, uint32_t guest, uint64_t offset, int guess) {
    snprintf(out, cap, "%sf_%08x+0x%llx", guess ? "~" : "", guest, (unsigned long long)offset);
}
int hos_crash_guest_name(uint64_t guest, char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!lifted_addrs || !lifted_count || guest >> 32) return 0;
    uint32_t lo = 0, hi = lifted_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (lifted_addrs[mid] <= (uint32_t)guest) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return 0;
    const uint32_t start = lifted_addrs[lo - 1];
    if (lo < lifted_count ? (uint32_t)guest >= lifted_addrs[lo] : guest - start > 0x10000) return 0;
    lifted_text(out, cap, start, guest - start, 0);
    return 1;
}
int hos_crash_lifted_name(uintptr_t start, uintptr_t pc, char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!lifted_fns || !lifted_count) return 0;
    uint32_t best = lifted_count;
    for (uint32_t i = 0; i < lifted_count; i++) {
        const uintptr_t at = (uintptr_t)lifted_fns[i];
        if (start ? at == start : (at <= pc && pc - at < 0x10000 &&
                                   (best == lifted_count || at > (uintptr_t)lifted_fns[best])))
            best = i;
        if (start && best == i) break;
    }
    if (best == lifted_count) return 0;
    lifted_text(out, cap, lifted_addrs[best], pc - (uintptr_t)lifted_fns[best], !start);
    return 1;
}
/* The executable's own folder and name, without the file name */
static int module_dir(char *out, size_t cap) {
#ifdef _WIN32
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
#else
    char path[4096];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0 || (size_t)n >= sizeof path) return 0;
    path[n] = 0;
#endif
    size_t end = strlen(path);
    while (end && path[end - 1] != '\\' && path[end - 1] != '/') end--;
    if (!end || end + 1 > cap) return 0;
    memcpy(out, path, end);
    out[end] = 0;
    return 1;
}
static int join(char *out, size_t cap, const char *dir, const char *leaf) {
    const size_t n = strlen(dir);
    const char *sep = n && (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/";
    const int k = snprintf(out, cap, "%s%s%s", dir, sep, leaf);
    return k > 0 && (size_t)k < cap;
}
int hos_crash_names_start(void) {
    static int started;
    if (started) return hos_crash_names_loaded(HOS_CRASH_NAMES_EXE) || hos_crash_names_loaded(HOS_CRASH_NAMES_VK);
    started = 1;
    char dir[1024], path[1200];
    if (!module_dir(dir, sizeof dir)) return 0;
    static const char *const exe_names[] = {"HeyListen.sym", "hookshot.sym"};
    for (size_t i = 0; i < sizeof exe_names / sizeof exe_names[0]; i++)
        if (join(path, sizeof path, dir, exe_names[i]) && load_table(HOS_CRASH_NAMES_EXE, path)) break;
    if (join(path, sizeof path, dir, "hookshot_vk.sym")) load_table(HOS_CRASH_NAMES_VK, path);
    return hos_crash_names_loaded(HOS_CRASH_NAMES_EXE) || hos_crash_names_loaded(HOS_CRASH_NAMES_VK);
}
