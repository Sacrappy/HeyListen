"""The pipeline state list a release ships (profile/pipeline_states.bin) passes tools/release/check_states.py, and the checker refuses
lists that hold a title id, a name, a path or an address (synthetic records; no game data). The list is shipped by
engine/scripts/release_parts.py's PROFILE_FILES and checked by tools/release/make_release.py before the package is made.
python -I -B tests/test_check_states.py
"""
import importlib.util
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def record(code=0x1234567890ABCDEF, blend=(2 << 3 | 1 << 10 | 2 << 17 | 1 << 24 | 1 << 31 | 1 << 34), fmt=37):
    r = bytearray(528)
    struct.pack_into("<5Q", r, 0, code, 0x2222, 0, 0, 0x3333)
    struct.pack_into("<6I", r, 40, 0, 3, 1, 1, 126, 1)
    struct.pack_into("<I", r, 64, fmt)
    r[96] = 1
    struct.pack_into("<Q", r, 104, blend)
    struct.pack_into("<I", r, 232, 2)
    struct.pack_into("<I", r, 236, 16)
    struct.pack_into("<2I", r, 388, 1, 1)
    struct.pack_into("<I", r, 396, 1 << 28 | 5 << 12 | 1 << 8 | 2)
    r[492] = 2
    return bytes(r)


def main():
    chk = load("check_states", ROOT / "tools" / "release" / "check_states.py")
    head = b"HKSTATE1" + struct.pack("<I", 528)
    title = 0x0400123456789000   # (a made-up title id)
    good = head + record() + record(code=0x99)
    assert not chk.check(good, [title]), chk.check(good, [title])
    cases = {
        "the title id as bytes": head + record() + struct.pack("<Q", title) + bytes(520),
        "the title id as a hash": head + record(code=title),
        "the title id as text": head + record() + ("%016x" % title).encode() + bytes(512),
        "an address in a blend state": head + record(blend=0x10_2345_6000),
        "a path": head + record() + b"C:\\Users\\someone" + bytes(512),
        "a name": head + record() + b"Nintendo" + bytes(520),
        "a format no Vulkan has": head + record(fmt=0x7F00_0000),
        "a record twice": head + record() + record(),
        "another magic": b"HKSTATE2" + good[8:],
        "a cut record": good[:-4],
    }
    for what, data in cases.items():
        if not chk.check(data, [title]):
            raise SystemExit("the checker let a list with %s through" % what)
    # the shipped list itself, and its wiring into the release
    shipped = ROOT / "profile" / "pipeline_states.bin"
    if shipped.is_file():
        bad = chk.check(shipped.read_bytes(), [])
        if bad:
            raise SystemExit("profile/pipeline_states.bin: " + "; ".join(bad[:5]))
    parts = (ROOT / "engine" / "scripts" / "release_parts.py").read_text(encoding="utf-8")
    if '"pipeline_states.bin"' not in parts:
        raise SystemExit("release_parts.py does not ship profile/pipeline_states.bin")
    release = (ROOT / "tools" / "release" / "make_release.py").read_text(encoding="utf-8")
    if "check_states.py" not in release:
        raise SystemExit("make_release.py does not check the list before shipping it")
    game = (ROOT / "engine" / "nx2" / "make_game.py").read_text(encoding="utf-8")
    if '"work", "pipeline_states.bin"' not in game:
        raise SystemExit("make_game.py no longer reads work/pipeline_states.bin, where the package puts the list")
    print("PASS: the shipped pipeline state list holds hashes and draw state only, and lists that do not are refused")


if __name__ == "__main__":
    sys.exit(main())
