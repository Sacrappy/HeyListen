/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The names behind a crash's host and guest addresses, from the tables the build writes beside the game
   (nx2/symtab.py: HeyListen.sym, hookshot_vk.sym; the map the linker wrote, compacted).

   The game is compiled on each player's PC, so HeyListen.exe+0x12f44593 is another function in every build.
   The tables hold the names the linker map already has: the lifted code's `f_<guest address>` (the same on
   every PC) and the host's C/C++ functions. They are read once at normal start-up, by ordinary file I/O, and
   the crash path only binary-searches them: no allocation, no lock, no file. A missing table (an older game
   folder, or a build made before the tables existed) is no failure: the frames stay module+offset, and the
   lifted ones are still named from a64_fn_addr/a64_fn_ptr, which every build carries. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { HOS_CRASH_NAMES_EXE = 0, HOS_CRASH_NAMES_VK = 1, HOS_CRASH_NAMES_TABLES = 2 };

/* Loads <exe>.sym and hookshot_vk.sym from beside the running executable. Normal start-up only (one allocation
   per table); 1 when at least one loaded, 0 when neither is there. */
int hos_crash_names_start(void);

/* Loads one table. 1 when the file is one this build wrote (magic, sizes and order checked), 0 otherwise. */
int hos_crash_names_load(int which, const char *path);

/* Whether a table is loaded, and how many functions it names */
int hos_crash_names_loaded(int which);
uint32_t hos_crash_names_count(int which);
uint64_t hos_crash_names_identity(int which);

/* "name+0xoff" for rva in the module module_name ("HeyListen.exe", "hookshot", "hookshot_vk.dll", ...).
   base is the module's load base, so a stale table left beside a newer binary is refused; 0 skips the check.
   No allocation, no lock, no I/O. 0 when no table names it. */
int hos_crash_name(const char *module_name, uint64_t rva, uintptr_t base, char *out, size_t cap);

/* The lifted code's table (hl_host.c's a64_fn_addr/a64_fn_ptr), registered once at normal start-up */
void hos_crash_names_lifted(const uint32_t *addrs, void *const *fns, uint32_t count);

/* "f_<guest address>+0xoff" for a guest code address. No allocation, no lock. 0 when no function holds it. */
int hos_crash_guest_name(uint64_t guest, char *out, size_t cap);

/* "f_<guest address>+0xoff" for a host frame in the lifted code. start: the function's host start from the
   unwind data (exact); 0: the nearest start below pc, written with a leading ~ (a guess). No allocation. */
int hos_crash_lifted_name(uintptr_t start, uintptr_t pc, char *out, size_t cap);

/* How many tables have been read: the crash path must not change it (tests/test_crash_names.py) */
unsigned long hos_crash_names_reads(void);
