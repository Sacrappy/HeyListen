"""Check a pipeline state list before it is shipped (profile/pipeline_states.bin; engine/nx2/pipeline_states.py makes it, the back end
reads it as hookshot_pipeline_states_shipped.bin beside the game, gpu/vk/vk_draw.cpp StateRec). The list may hold only hashes of the
game's shaders, formats and small draw-state values: no title id, no name of the game or of the console's maker, no local path or
user name, no guest address or host pointer. Exits 1 and says why when the file is not such a list.

    python -I tools/release/check_states.py <list> [--title-id <16 hex digits>]...

The file: "HKSTATE1", u32 record size (528), then the records (engine/nx2/pipeline_states.py has the layout); nothing else, no
other header field. Each field is checked for what it can hold, which leaves no room for an address: the five 64-bit hashes (stage
code keys, variant) are only compared with the title ids; the blend states are 37-bit bit fields (gpu/render/rx_nvn.h
nvn_blend_st) whose factors and equations must be NVN's; every other field is 32 bits or less and bounded by what it is (a count, a flag, a
Vulkan format, a packed vertex attribute or texture answer); unused answers and questions and the record's padding are zero. The
whole file is also searched for the title ids (ASCII hex in both cases, UTF-16, and their 8 bytes in both byte orders), for names
(4 letters and more: shorter ones occur in hashes by chance), and for paths (a drive letter, a separator and three name characters;
'users', 'appdata').
"""
import argparse
import re
import struct
import sys

MAGIC = b"HKSTATE1"
SIZE = 528
# (what no list may hold, in any case: the game's, its series' and places' names, the console's and its maker's, the project's)
NAMES = ("zelda", "ocarina", "hyrule", "navi", "kokiri", "nintendo", "switch", "hookshot", "heylisten", "users", "appdata")
PATH = re.compile(rb"[A-Za-z]:[\\/][A-Za-z0-9 _.-]{3}|\\\\[A-Za-z0-9_.-]{3}")


def vk_format(v):
    """A VkFormat this back end can name: the core ones, or an extension's (1000xxx000 + n)"""
    return v <= 184 or 1000000000 <= v < 1001000000


# NVN's blend factors and equations (gpu/render/nvn_api.h), 0: a target never set
FACTORS = {0} | set(range(1, 0xC)) | set(range(0x10, 0x14)) | set(range(0x61, 0x65))
EQUATIONS = set(range(0, 6))


def blend_ok(v):
    """nvn_blend_st (gpu/render/rx_nvn.h): target 3 bits, four factors of 7, two equations of 3, nothing above bit 37; each field
    one of NVN's values, which an address does not keep to"""
    if v >> 37:
        return False
    factors = [v >> s & 127 for s in (3, 10, 17, 24)]
    return all(f in FACTORS for f in factors) and (v >> 31 & 7) in EQUATIONS and (v >> 34 & 7) in EQUATIONS


# u32 fields: (name, offset, count, the test)
U32 = [("mesh", 40, 1, lambda v: v <= 1), ("topology", 44, 1, lambda v: v <= 10), ("targets", 48, 1, lambda v: v <= 8),
       ("samples", 52, 1, lambda v: v in (1, 2, 4, 8, 16, 32, 64)), ("depth format", 56, 1, vk_format),
       ("has depth", 60, 1, lambda v: v <= 1), ("colour formats", 64, 8, vk_format),
       ("vertex attributes", 168, 16, lambda v: True),   # (nvn_va_st: format 8, offset 19, stream 5 bits: all 32 used)
       ("attribute count", 232, 1, lambda v: v <= 16), ("strides", 236, 16, lambda v: v <= 0xFFFF),
       ("divisors", 300, 16, lambda v: v <= 0xFFFFF), ("blend enables", 364, 1, lambda v: v <= 0xFF),
       ("channel masks", 368, 1, lambda v: True),   # (4 bits a target, 8 targets)
       ("polygon mode", 372, 1, lambda v: v <= 0xFFFF), ("depth clamp", 376, 1, lambda v: v <= 1),
       ("alpha to coverage", 380, 1, lambda v: v <= 1), ("pad", 384, 1, lambda v: v == 0),
       ("answers", 388, 1, lambda v: v <= 24), ("questions", 392, 1, lambda v: v <= 32),
       ("answer", 396, 24, lambda v: v >> 28 <= 5 and (v >> 9) & 7 == 0)]   # (stage 4, descriptor 16, image 1, type 8 bits)


def check(data, title_ids):
    """The problems found (empty: a list that may be shipped)"""
    bad = []
    if data[:8] != MAGIC:
        return ["the magic is %r, not %r" % (data[:8], MAGIC)]
    if len(data) < 12 or struct.unpack_from("<I", data, 8)[0] != SIZE:
        return ["the record size is not %d" % SIZE]
    body = len(data) - 12
    if body % SIZE:
        bad.append("%d bytes after the header: not a whole number of %d-byte records" % (body, SIZE))
    count = body // SIZE
    if not count:
        bad.append("no records")
    seen = set()
    for i in range(count):
        r = data[12 + i * SIZE:12 + (i + 1) * SIZE]
        where = "record %d" % i
        if r in seen:
            bad.append("%s: twice in the list" % where)
        seen.add(r)
        hashes = struct.unpack_from("<5Q", r, 0)
        if not hashes[0]:
            bad.append("%s: no vertex or mesh stage" % where)
        for t in title_ids:
            if t in hashes:
                bad.append("%s: a hash equals the title id %016x" % (where, t))
        for k, t in enumerate(r[96:104]):
            if t > 15:
                bad.append("%s: colour type %d is %d" % (where, k, t))
        for k, v in enumerate(struct.unpack_from("<8Q", r, 104)):
            if not blend_ok(v):
                bad.append("%s: blend %d is %#x: not a blend state" % (where, k, v))
        for name, off, n, ok in U32:
            for k, v in enumerate(struct.unpack_from("<%dI" % n, r, off)):
                if not ok(v):
                    bad.append("%s: %s%s is %#x" % (where, name, "[%d]" % k if n > 1 else "", v))
        nans, nq = struct.unpack_from("<2I", r, 388)
        if nans <= 24 and nq <= 32 and (any(struct.unpack_from("<24I", r, 396)[nans:]) or any(r[492 + nq:524])):
            bad.append("%s: bytes past its %d answers and %d questions" % (where, nans, nq))
        if any(r[524:528]):
            bad.append("%s: its padding is not zero" % where)
        if len(bad) > 40:
            bad.append("...")
            break
    low = data.lower()
    for t in title_ids:
        for form in ("%016x" % t, "%x" % t):
            for s in (form, form.upper()):
                if s.encode() in data or s.encode("utf-16-le") in data:
                    bad.append("the title id %s is in the file as text" % s)
        for b in (struct.pack("<Q", t), struct.pack(">Q", t)):
            if b in data:
                bad.append("the title id %016x is in the file as 8 bytes at %d" % (t, data.find(b)))
    for word in NAMES:
        for enc in (word.encode(), word.encode("utf-16-le")):
            if enc in low:
                bad.append("%r is in the file at %d" % (word, low.find(enc)))
    for m in PATH.finditer(data):
        bad.append("a path is in the file at %d: %r" % (m.start(), m.group()))
        break
    return bad


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("list")
    p.add_argument("--title-id", action="append", default=[], help="16 hex digits; may be given more than once")
    a = p.parse_args(argv)
    with open(a.list, "rb") as f:
        data = f.read()
    bad = check(data, [int(t, 16) for t in a.title_id])
    if bad:
        print("%s: not a list that may be shipped:" % a.list)
        for b in bad:
            print("  " + b)
        return 1
    print("%s: %d pipeline states, nothing but hashes, formats and draw state" % (a.list, (len(data) - 12) // SIZE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
