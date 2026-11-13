// HeyListen native engine: the cold-page policy, run with no game (tests/test_native_engine.py).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hl/mem/ColdPages.h"

using hl::mem::ColdPages;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

ColdPages::Config small() {
    ColdPages::Config c;
    c.units = 1u << 20;
    c.unit_pages = 16;
    c.backoff_passes = 4;
    c.give_up = 3;
    c.budget_pages = 40;
    return c;
}

// A unit taken out is not taken out again while out; its come-back is counted and holds it in, longer each time, and
// after give_up come-backs for good.
void backoff() {
    ColdPages cold(small());
    CHECK(cold.ok());
    cold.begin_pass();
    CHECK(cold.look(7, 16));
    cold.taken_out(7, 16, 0);
    CHECK(cold.stats().out_pages == 16);
    cold.begin_pass();
    CHECK(!cold.look(7, 0));                     // (still out)
    CHECK(!cold.look(7, 3));                     // (came back: 3 pages)
    CHECK(cold.stats().back_pages == 3 && cold.stats().back_units == 1 && cold.stats().out_pages == 0);
    int waited = 0;                              // (held backoff_passes passes)
    for (;; waited++) {
        cold.begin_pass();
        if (cold.look(7, 16)) break;
        CHECK(waited < 100);
    }
    CHECK(waited == 3);                          // (taken out again at pass + 4)
    cold.taken_out(7, 16, 0);
    cold.begin_pass();
    CHECK(!cold.look(7, 16));                    // (second come-back: held 8 passes)
    waited = 0;
    for (;; waited++) {
        cold.begin_pass();
        if (cold.look(7, 16)) break;
        CHECK(waited < 100);
    }
    CHECK(waited == 7);
    cold.taken_out(7, 16, 0);
    cold.begin_pass();
    CHECK(!cold.look(7, 16));                    // (third: left in for good)
    CHECK(cold.stats().given_up_units == 1);
    for (int i = 0; i < 1000; i++) {
        cold.begin_pass();
        CHECK(!cold.look(7, 16));
    }
    CHECK(cold.stats().back_units == 3);
}

// The budget bounds what one pass takes out; a unit that nothing left is held like a come-back but not counted as one.
void budget_and_stayed() {
    ColdPages cold(small());
    cold.begin_pass();
    CHECK(cold.look(1, 16));
    cold.taken_out(1, 16, 0);
    CHECK(cold.look(2, 16));
    cold.taken_out(2, 16, 0);
    CHECK(!cold.look(3, 16));                    // (40 - 32 = 8 left: not enough for 16)
    CHECK(cold.look(4, 8));
    cold.taken_out(4, 0, 8);                     // (pinned: nothing left)
    CHECK(cold.stats().stayed_pages == 8 && cold.stats().back_units == 0);
    cold.begin_pass();
    CHECK(cold.look(3, 16));
    CHECK(!cold.look(4, 8));                     // (held after a take-out that did nothing)
    CHECK(!cold.look(5, 0));                     // (nothing resident: nothing to do)
}

// Imported units are never taken out; forget() clears a unit's history and its pages out.
void imported_and_forget() {
    ColdPages cold(small());
    cold.mark_imported(10, 12);
    cold.mark_imported(11, 13);
    CHECK(cold.stats().imported_units == 3);
    cold.begin_pass();
    CHECK(!cold.look(10, 16) && !cold.look(12, 16));
    CHECK(cold.look(13, 16));
    cold.taken_out(13, 16, 0);
    CHECK(cold.stats().out_pages == 16);
    cold.forget(13, 14);
    CHECK(cold.stats().out_pages == 0);
    CHECK(cold.look(13, 16));                    // (a new pool there: a fresh unit)
    CHECK(!cold.look(1u << 20, 16));             // (outside the space)
    char line[512];
    CHECK(cold.report(line, sizeof line) > 0 && std::strstr(line, "out of the working set"));
}

// With settle_passes (the heaps): a unit goes only at a look settle_passes or more after an earlier one, and only if it
// has not gained resident pages since; a unit that gained pages waits again.
void settle() {
    ColdPages::Config c = small();
    c.settle_passes = 3;
    c.budget_pages = 1000;
    ColdPages cold(c);
    cold.begin_pass();
    CHECK(!cold.look(5, 8));                     // (first look: noted)
    cold.begin_pass();
    CHECK(!cold.look(5, 8));                     // (one pass later: too soon)
    cold.begin_pass();
    cold.begin_pass();
    CHECK(cold.look(5, 8));                      // (three passes after the first look, no growth)
    cold.taken_out(5, 8, 0);
    CHECK(cold.stats().out_pages == 8);

    CHECK(!cold.look(6, 4));                     // (a unit being filled: 4, then 9 pages)
    for (int i = 0; i < 3; i++) cold.begin_pass();
    CHECK(!cold.look(6, 9));                     // (grew: waits again)
    for (int i = 0; i < 2; i++) cold.begin_pass();
    CHECK(!cold.look(6, 9));                     // (two passes after the growth: too soon)
    cold.begin_pass();
    CHECK(cold.look(6, 7));                      // (fewer pages than at the last look: still)
    cold.taken_out(6, 7, 0);

    CHECK(!cold.look(7, 0));                     // (nothing resident: nothing noted)
    for (int i = 0; i < 5; i++) cold.begin_pass();
    CHECK(!cold.look(7, 16));                    // (its first look with pages)

    cold.begin_pass();                           // (a come-back is held as without settle, then settles again)
    CHECK(!cold.look(5, 8));
    CHECK(cold.stats().back_units == 1);
    for (int i = 0; i < 4; i++) cold.begin_pass();
    CHECK(!cold.look(5, 8));                     // (held 4 passes, then seen with more pages than noted (0): settles)
    for (int i = 0; i < 3; i++) cold.begin_pass();
    CHECK(cold.look(5, 8));
    cold.forget(5, 6);
    CHECK(!cold.look(5, 8));                     // (new data: a fresh unit, noted first)
}

// The worst come-backs between two reports are kept.
void report_worst() {
    ColdPages cold(small());
    cold.begin_pass();
    CHECK(cold.look(1, 16));
    cold.taken_out(1, 16, 0);
    CHECK(!cold.look(1, 16));                    // (16 pages back)
    char line[512];
    CHECK(cold.report(line, sizeof line) > 0 && std::strstr(line, "0.1 MB at most between two lines"));
    CHECK(cold.report(line, sizeof line) > 0 && std::strstr(line, "0.0 MB in 0 units since the last line, 0.1 MB at most"));
}

}   // namespace

int main() {
    backoff();
    budget_and_stayed();
    imported_and_forget();
    settle();
    report_worst();
    std::printf("mem_test: ok\n");
    return 0;
}
