"""(fork) The gameplay view's hook for the free camera and first person (host/hos_main.c hos_oot_free_view), at lift time.

nx2/oot_patches.py's patch_view looks for the loop head of f_804e48c0 (main+0x4c48c0, the view-matrix routine) as a bare
label in the lifted C, and finds it 0 times on this build ("the free camera was found 0 times" in every build log, stock
9f725aa1 included), so the free camera's view was never rebuilt. This puts the same call after the instruction at that
loop head instead (main+0x4c5068, ldr x8, [x19, #0x20]): the view's 3x4 matrix at view+8 is complete there (its last row
stored just before, at main+0x4c5064), and x20 is the view. patch_view counts a function that already has the call as
found and adds no second one.
"""

MAIN = 0x80020000
SITE = MAIN + 0x4c5068
# ldr x8, [x19, #0x20]; cbz x8, ...; ldr x21, [x8, #0x150]
WORDS = (0xf9401268, 0xb4005868, 0xf940a915)
HOOK = ("{ extern void hos_oot_free_view(Ctx *c, uint64_t view); hos_oot_free_view(c, c->x[20]); }"
        "   /* (nx2/oot_view.py: the free camera and first person) */")


def install(lifter, img):
    """The site into lifter.after, after what is there; left out when its words differ (another game version)"""
    if any(img.word(SITE + 4 * i) != word for i, word in enumerate(WORDS)):
        print("gameplay view: unsupported view code, left out")
        return 0
    lifter.after[SITE] = (lifter.after[SITE] + "\n" if SITE in lifter.after else "") + HOOK
    print("gameplay view: free camera and first person hooked")
    return 1
