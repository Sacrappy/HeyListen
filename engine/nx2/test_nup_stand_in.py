"""Damaged .zs files in an unpacked romfs and their stand-ins: python -I -B nx2/test_nup_stand_in.py

No game bytes: small made-up files in a temporary folder. Standard library only.
"""
import importlib.util
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("tested_nup", os.path.join(HERE, "nup.py"))
nup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nup)

ZSTD = b"\x28\xb5\x2f\xfd"
failures = []


def check(what, got, want):
    if got != want:
        failures.append("%s: %.200r, %.200r expected" % (what, got, want))


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


with tempfile.TemporaryDirectory() as romfs:
    tex = os.path.join(romfs, "Tex")
    # (as in the 23.3 GB dump: the start of the file is zeros, the rest is there)
    write(os.path.join(tex, "Rock_Temple_Gate_M_01_Nrm.bntx.zs"), bytes(400) + b"rest" * 30)
    write(os.path.join(tex, "Rock_Temple_Gate_P_01_Nrm.bntx.zs"), ZSTD + b"P" * 500)    # the nearest name: chosen
    write(os.path.join(tex, "Rock_Temple_Gate_M_01_Alb.bntx.zs"), ZSTD + b"A" * 4000)   # another kind of texture
    write(os.path.join(tex, "Rock_Temple_Gate_A_Nrm.bntx.zs"), ZSTD + b"A" * 4000)      # as close a name, a size further off
    write(os.path.join(tex, "Rock_Temple_Gate_M_02_Nrm.bntx.zs"), bytes(64))            # damaged too: never a stand-in
    write(os.path.join(tex, "Bake_Stage_G_2_1_d0_t0.bntx.zs"), bytes(64))
    write(os.path.join(tex, "Bake_Stage_G_2_2_d0_t0.bntx.zs"), ZSTD + b"t" * 60)
    write(os.path.join(tex, "Bake_Stage_G_2_2_d1_un3.bntx.zs"), ZSTD + b"u" * 60)
    write(os.path.join(romfs, "Lonely", "Only_One_Nrm.bntx.zs"), bytes(16))             # nothing of its kind beside it
    write(os.path.join(romfs, "Other", "plain.txt"), bytes(16))                         # not a .zs: not looked at

    damaged = sorted(os.path.relpath(p, romfs) for p in nup.damaged_game_files(romfs))
    check("damaged", damaged, sorted(os.path.join(*p) for p in (
        ("Lonely", "Only_One_Nrm.bntx.zs"), ("Tex", "Bake_Stage_G_2_1_d0_t0.bntx.zs"),
        ("Tex", "Rock_Temple_Gate_M_01_Nrm.bntx.zs"), ("Tex", "Rock_Temple_Gate_M_02_Nrm.bntx.zs"))))

    done = {os.path.relpath(p, romfs): o and os.path.basename(o) for p, o in nup.stand_in_damaged(romfs)}
    check("normal map", done.get(os.path.join("Tex", "Rock_Temple_Gate_M_01_Nrm.bntx.zs")), "Rock_Temple_Gate_P_01_Nrm.bntx.zs")
    check("second normal map", done.get(os.path.join("Tex", "Rock_Temple_Gate_M_02_Nrm.bntx.zs")), "Rock_Temple_Gate_P_01_Nrm.bntx.zs")
    check("bake", done.get(os.path.join("Tex", "Bake_Stage_G_2_1_d0_t0.bntx.zs")), "Bake_Stage_G_2_2_d0_t0.bntx.zs")
    check("alone", done.get(os.path.join("Lonely", "Only_One_Nrm.bntx.zs"), "missing"), None)
    with open(os.path.join(tex, "Rock_Temple_Gate_M_01_Nrm.bntx.zs"), "rb") as f:
        check("replaced bytes", f.read(), ZSTD + b"P" * 500)
    check("no .part left", [n for _, _, fs in os.walk(romfs) for n in fs if n.endswith(".part")], [])
    check("a second pass", [os.path.relpath(p, romfs) for p, _ in nup.stand_in_damaged(romfs)],
          [os.path.join("Lonely", "Only_One_Nrm.bntx.zs")])

if failures:
    print("\n".join(failures))
    sys.exit(1)
print("stand-ins: ok")
