"""First person's near plane (OoT 1.1.1): the gameplay camera's near distance, lowered while first person is on.

The camera update (the projection's build) loads the near distance from the camera at +0x134
into s0 and stores it to sead's perspective projection (+0x98). The hook after that load (host/hos_main.c hos_oot_near)
changes s0 only, never the camera's own field, so the next update starts from the game's value again.
"""

MAIN = 0x80020000
SITE = MAIN + 0x4c52d4
# ldr s0, [x19, #0x134]; mov w21, #1; fmov s1, #0.5
WORDS = (0xbd413660, 0x52800035, 0x1e2c1001)
HOOK = ("{ extern void hos_oot_near(Ctx *c, uint64_t camera); hos_oot_near(c, c->x[19]); }"
        "   /* (nx2/oot_near.py: first person's near plane) */")


def install(lifter, img):
    """The site into lifter.after, after what is there; left out when its words differ (another game version)"""
    if any(img.word(SITE + 4 * i) != word for i, word in enumerate(WORDS)):
        print("near plane: unsupported camera code, left out")
        return 0
    lifter.after[SITE] = (lifter.after[SITE] + "\n" if SITE in lifter.after else "") + HOOK
    print("near plane: gameplay camera hooked")
    return 1
