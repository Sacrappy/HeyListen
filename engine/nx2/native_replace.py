"""Native replacements of hot guest code (TICK-026): gates in the lifted C that hand a guest block to a host function
(host/hos_native.c). Applied by oot_patches.apply after the lift (and by hand: python -I nx2/native_replace.py <gen>).

A replacement is a block of one lifted function: an entry (the text just before the block's first label, where the gate
goes), the exit label(s) where the block always goes on (the differential check goes at their head), and the regions of the
function whose text the native code was written against, with their SHA-1. The gate is only put in when every region
hashes as recorded: another game version or a lifter change that alters the block leaves the guest code alone and prints
a warning with the new hash (check the native code against the new text before updating it).

    gate:  if (hos_nr_mode[ID]) { if (hos_nr_<name>(c)) goto <exit>; }
    exit:  if (hos_nr_mode[ID] == 2) hos_nr_diff_exit(c, ID);

ID is the index in host/hos_native.c's enum (keep both lists in the same order); the switch is HOS_NR_<NAME> (0 / 1 / diff).
To add one: write the native function and its gate in hos_native.c (enum, nr_info, the function, hos_nr_<name>), add an
entry here, lift (dev-build.ps1 -Lift), run in differential mode over a full bench run (0 differences), then measure.
"""
import glob
import hashlib
import os
import sys

REPLACEMENTS = [
    {
        # f_805aeea0's binary search over a sorted name table (guest 0x805afbc0-0x805afda4): its inlined strcmp, 8 bytes a
        # group, guest 0x805afc70-0x805afda0. Every way out of it goes to the search loop's head, 0x805afbc0.
        "id": 0,
        "name": "strcmp_805afc70",
        "head": "void f_805aeea0(Ctx *__restrict A64_NOESC c) {\n",
        "entry": "    c->x[11] = 0x400000ULL;\n    c->x[11] &= 0xFFFFFFFFULL;\nL_805afc70:; A64_POLL(c);\n",
        "gate_before": "L_805afc70:;",
        "exits": ["L_805afbc0:; A64_POLL(c);\n"],
        "goto": "L_805afbc0",
        "regions": [
            ("L_805afc70:;", "L_805afda4:;", "3dc3022f619d8950"),
            ("L_805afe28:;", "L_805afe30:;", "bc3d5102404211fe"),
            ("L_805afea8:;", "L_805afee8:;", "35e72f49a9051a11"),
        ],
    },
]

DECL = "extern volatile int hos_nr_mode[];\nextern void hos_nr_diff_exit(Ctx *c, int id);\n"


def _body(s, head):
    start = s.find(head)
    if start < 0:
        return None, None
    end = s.find("\n}\n", start)
    return start, (end if end >= 0 else len(s))


def region_hashes(body, r):
    out = []
    for a, b, _ in r["regions"]:
        i = body.find(a)
        j = body.find(b, i + 1) if i >= 0 else -1
        out.append(hashlib.sha1(body[i:j].encode()).hexdigest()[:16] if i >= 0 and j > i else None)
    return out


def apply_one(s, r):
    """(new text, status) for one file's text"""
    start, end = _body(s, r["head"])
    if start is None:
        return s, None
    body = s[start:end]
    gate = "if (hos_nr_mode[%d]) { if (hos_nr_%s(c)) goto %s; }   /* (nx2/native_replace.py) */\n" % (
        r["id"], r["name"], r["goto"])
    if gate in body:
        return s, "applied already"
    got = region_hashes(body, r)
    want = [h for _, _, h in r["regions"]]
    if got != want:
        return s, "NOT applied: the block's text differs (hashes %s, recorded %s)" % (got, want)
    if body.count(r["entry"]) != 1 or any(body.count(x) != 1 for x in r["exits"]):
        return s, "NOT applied: entry or exit not found once"
    at = body.index(r["entry"]) + r["entry"].index(r["gate_before"])
    body = body[:at] + gate + body[at:]
    for x in r["exits"]:
        body = body.replace(x, x + "    if (hos_nr_mode[%d] == 2) hos_nr_diff_exit(c, %d);\n" % (r["id"], r["id"]), 1)
    s = s[:start] + body + s[end:]
    decl = "extern int hos_nr_%s(Ctx *c);\n" % r["name"]
    s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL + decl, 1)
    return s, "applied"


def apply(gen):
    found = {r["name"]: 0 for r in REPLACEMENTS}
    for path in sorted(glob.glob(os.path.join(gen, "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        out = s
        for r in REPLACEMENTS:
            out, st = apply_one(out, r)
            if st is not None:
                found[r["name"]] += 1
                print("native_replace: %s in %s: %s" % (r["name"], os.path.basename(path), st))
        if out != s:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(out)
    for n, k in found.items():
        if k != 1:
            print("native_replace: WARNING: %s's function was found %d times" % (n, k))


if __name__ == "__main__":
    apply(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "sa", "gen"))
