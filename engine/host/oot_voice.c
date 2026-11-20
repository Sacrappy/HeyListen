/* (longshot) The game's Voice Language option, kept after a restart (issue 430).

   The game's voice resource (its update, f_80a9ca30) picks the voice files' language in two places only:
   - at its first update, the default of the text language (f_809d48c0): applied at once, and written to the option
     OptionData.VoiceLanguage through GameDataMgr's queue of writes;
   - after that, whenever GameDataMgr lists the option among the flags written since its last pass (f_80a9d1c0(mgr, hash),
     the list at mgr+0x288): the option's value is read and applied (f_80a9d330).
   option.sav is read about half a second later, and its values replace the options' without being listed. So a Voice Language
   saved in an earlier session stood in the option (the game's Options screen showed it) while the voices stayed the text
   language's default: English under English text with Japanese chosen. Changing the option in a session wrote it, and
   was applied; the next start lost it again. Seen in a run (v3.0.0-rc.4, en-US, option.sav holding Japanese): the option
   English at frame 4 with USen applied, the saved value from frame 70 on, the voice files opened still USen_*.

   nx2/oot_patches.py makes the update's one call of f_80a9d1c0 come here, with the game's function. Its answer stands, and
   it is also yes when the option's value is not the one it had at the update before: then the game's own branch reads the
   option and applies it, as for a write. The first update only notes the value (the default was applied just before).

   An enum of GameDataMgr, as the update's own branch reads it (0x80a9cd30): +0x260 the table of {u32 hash, u32 entry} found
   by hash % its size (+0x26c) and walked on, a hash of 0 ending it; +0x258 the entries of 0x38 bytes (+0x250 how many, an
   entry past them: the first), the value's hash at +0x1c.

   HOS_VOICE_OPTION_FOLLOW=0: the game's answer alone, as shipped. */
#include "hos.h"
#include <stdlib.h>

#define VOICE_OPTION 0xa2dcdc9dU   /* (MurmurHash3 of "OptionData.VoiceLanguage") */
enum { ENUM_COUNT = 0x250, ENUM_ENTRIES = 0x258, ENUM_TABLE = 0x260, ENUM_TABLE_SIZE = 0x26c, ENUM_ENTRY = 0x38, ENUM_VALUE = 0x1c };

static int follow_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("HOS_VOICE_OPTION_FOLLOW") || atoi(getenv("HOS_VOICE_OPTION_FOLLOW")) != 0;
    return on;
}

/* The option's value (the hash of its choice's name) in *value: 1 when GameDataMgr has the option */
static int voice_option(Ctx *c, uint64_t mgr, uint32_t *value) {
    const uint64_t table = mgr ? recomp_load64(c, mgr + ENUM_TABLE) : 0;
    const uint32_t size = table ? (uint32_t)recomp_load32(c, mgr + ENUM_TABLE_SIZE) : 0;
    if (!size) return 0;
    uint32_t at = VOICE_OPTION % size;
    for (uint32_t walked = 0; walked < size; walked++, at = at + 1 < size ? at + 1 : 0) {
        const uint32_t hash = (uint32_t)recomp_load32(c, table + 8ULL * at);
        if (!hash) return 0;
        if (hash != VOICE_OPTION) continue;
        uint32_t entry = (uint32_t)recomp_load32(c, table + 8ULL * at + 4);
        if (entry >= (uint32_t)recomp_load32(c, mgr + ENUM_COUNT)) entry = 0;
        *value = (uint32_t)recomp_load32(c, recomp_load64(c, mgr + ENUM_ENTRIES) + (uint64_t)ENUM_ENTRY * entry + ENUM_VALUE);
        return 1;
    }
    return 0;
}

/* The voice resource's question "was the Voice Language option written?" (x0 GameDataMgr, x1 the option's hash; the answer in
   x0 bit 0), asked once an update on the game's thread. game: the game's f_80a9d1c0 */
void hos_oot_voice_option(Ctx *c, A64Fn game) {
    const uint64_t mgr = c->x[0];
    const int ours = (uint32_t)c->x[1] == VOICE_OPTION;
    game(c);
    if (!ours || !follow_on()) return;
    static uint32_t seen;   /* (the option's value at the update before; the updates run one at a time, on the game's module workers) */
    static int known;
    uint32_t value;
    if (!voice_option(c, mgr, &value)) return;
    if (known && value != seen && !(c->x[0] & 1)) {
        static int logged;
        if (logged < 8 && ++logged)
            hos_log("voice: the Voice Language option changed without a write (%#x, was %#x: option.sav was read): applied", value, seen);
        c->x[0] |= 1;
    }
    seen = value, known = 1;
}
