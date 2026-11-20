"""Game-free: the crash names a player's build can give its own frames, and the signature they group by.

Compiles the real host/hos_crash_names.c (with a counting allocator) and tests/crash_names_test.c, writes a crafted
linker map through nx2/symtab.py, and checks: the table's names and offsets, a stale table refused, the guest
f_<address> names, the lookup path allocating nothing, and hos_crash_signature giving one key for the same crash on
two builds whose raw HeyListen.exe offsets differ.

    python -I -B tests/test_crash_names.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

MAP = """\
 HeyListen

 Preferred load address is 0000000140000000

 Start         Length     Name                   Class
 0001:00000000 00000030H .text                   CODE

 0001:00000000       f_80580b38           0000000140001000     code_001.o
 0001:00000000       .weak._ZN3foo3barEv  0000000140001000     weak.o
 0001:00000010       crash_now            0000000140001010     host.o
 0001:00000020       another_one          0000000140001020     host.o
 0001:00000020       _ZN6helper8functionEv 0000000140001020    host.o
 0001:00000028       .L.str               0000000140001028     host.o
 0001:00000030       absolute_thing       0000000140001030     <absolute>
"""


def fake_pe(path, timestamp=0x11223344, size_of_image=0x1000, checksum=0xABCDEF):
    """The fields nx2/symtab.py and host/hos_crash_names.c both read (and tests/crash_names_test.c rebuilds)"""
    image = bytearray(4096)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<I", image, 0x88, timestamp)
    struct.pack_into("<I", image, 0x80 + 80, size_of_image)
    struct.pack_into("<I", image, 0x80 + 88, checksum)
    Path(path).write_bytes(image)


def read_table(path):
    data = Path(path).read_bytes()
    magic, count, names, identity, preferred = struct.unpack_from("<8sIIQQ", data, 0)
    assert magic == b"HLSYM\x00\x00\x01", magic
    entries = [struct.unpack_from("<II", data, 32 + 8 * i) for i in range(count)]
    pool = data[32 + 8 * count:]
    out = []
    for rva, at in entries:
        out.append((rva, pool[at:pool.index(b"\0", at)].decode("ascii")))
    assert entries == sorted(entries), "ascending by RVA"
    assert all(a[0] < b[0] for a, b in zip(entries, entries[1:])), "one name per address"
    return out, identity, preferred, len(data)


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300, **kw)


def source_contract():
    glue = (ROOT / "host/mkw_glue.c").read_text(encoding="utf-8")
    assert '#include "hos_crash_names.h"' in glue
    assert "hos_crash_name(name, offset, base, function, sizeof function)" in glue
    assert "hos_crash_lifted_name(start, (uintptr_t)ctx.Rip, function, sizeof function)" in glue
    assert "hos_crash_guest_name(lr, function, sizeof function)" in glue
    assert "hos_crash_names_lifted(addrs, fns, count)" in glue
    assert 'hos_log("CRASH: host frame %d: %s+%#llx%s%s"' in glue
    assert 'hos_log("CRASH: guest frame %d: %#llx %s"' in glue
    assert 'hos_log("CRASH: build %s, lifted code %s"' in glue
    report = glue[glue.index("static LONG WINAPI report_crash("):]
    report = report[:report.index("\n}\n") + 1]
    for forbidden in ("malloc(", "calloc(", "realloc(", "fopen(", "CreateFileA(", "CreateFileW("):
        assert forbidden not in report, "the crash path does not allocate or open a file: " + forbidden
    record = (ROOT / "host/hos_crash_record.c").read_text(encoding="utf-8")
    start = record[record.index("void hos_crash_record_start("):]
    assert start.index("hos_crash_names_start();") < start.index("recover();"), "names before the handlers"
    assert 'label_field(b, "lifted_code", hos_lift_key(), 40);' in record
    host_stack = record[record.index("static void host_stack("):]
    assert host_stack.index("frame_field(") < host_stack.index("RtlVirtualUnwind("), "each frame is named as it is walked"
    main = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    assert '#define HOOKSHOT_LIFT_KEY "unknown"' in main and "const char *hos_lift_key(void)" in main
    assert 'hos_log("object keys: lifted code %s", HOOKSHOT_LIFT_KEY);' in main
    build = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
    assert "write_symtab(exe" in build and "write_symtab(vdll" in build and "lift_def" in build
    game = (ROOT / "nx2/make_game.py").read_text(encoding="utf-8")
    assert '"HeyListen.sym", "hookshot_vk.sym"' in game or '"hookshot.sym", "hookshot_vk.sym"' in game
    dialog = (ROOT / "host/hos_crash_dialog.c").read_text(encoding="utf-8")
    for field in ('\\"signature\\"', '\\"frames\\"', '\\"guest_frames\\"', '"lifted_code"'):
        assert field in dialog, field
    assert "hos_crash_signature(signature, sizeof signature, crash_at, crash.n)" in dialog
    header = (ROOT / "host/hos_crash_dialog.h").read_text(encoding="utf-8")
    assert "static inline void hos_crash_frame_name(" in header and "static inline int hos_crash_signature(" in header
    print("PASS: the crash path names frames from the loaded tables only; the build writes and ships them")


def table_and_fixture(cc, temp):
    fake_pe(temp / "fake.exe")
    (temp / "fixture.map").write_text(MAP, encoding="utf-8")
    run([sys.executable, "-I", "-B", str(ROOT / "nx2/symtab.py"), str(temp / "fixture.map"),
         str(temp / "fake.exe"), str(temp / "fixture.sym"), "--no-demangle"])
    entries, identity, preferred, size = read_table(temp / "fixture.sym")
    assert (entries, preferred) == ([(0x1000, "f_80580b38"), (0x1010, "crash_now"), (0x1020, "another_one")],
                                    0x140000000), entries
    assert identity and size > 32
    print("PASS: a crafted map -> three functions (labels and <absolute> dropped, one name per address)")
    exe = temp / ("fixture.exe" if os.name == "nt" else "fixture")
    cmd = [cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"),
           "-DHOS_CRASH_NAMES_MALLOC=counting_malloc", "-DHOS_CRASH_NAMES_FREE=counting_free",
           str(ROOT / "tests/crash_names_test.c"), str(ROOT / "host/hos_crash_names.c"), "-o", str(exe)]
    r = run(cmd)
    assert r.returncode == 0, r.stdout
    r = run([str(exe), "table", str(temp / "fixture.sym")])
    assert r.returncode == 0 and "checks:" in r.stdout, r.stdout
    print(r.stdout.strip())
    return exe


def signatures(exe):
    def sign(*lines):
        r = run([str(exe), "signature", *lines])
        assert r.returncode == 0, r.stdout
        return r.stdout.strip("\r\n")

    a = "CRASH: exception 0xc0000005 (write 0x10, guest 0xfff) at HeyListen.exe+0x12f44593"
    b = "CRASH: exception 0xc0000005 (write 0x10, guest 0xfff) at HeyListen.exe+0x453d405"
    top = "CRASH: host frame 0: %s f_80580b38+0x1c4"
    first = sign(a, top % "HeyListen.exe+0x12f44593", "CRASH: host frame 1: HeyListen.exe+0x32acd6c hos_some_frame+0x40")
    second = sign(b, top % "HeyListen.exe+0x453d405", "CRASH: host frame 1: HeyListen.exe+0x2b275b5 recomp_fpe_sub64+0x2d7")
    assert first == second == "1|f_80580b38+0x1c4", (first, second)
    assert sign(a, "CRASH: host frame 0: HeyListen.exe+0x12f44593") == "1|HeyListen.exe+0x12f44593", \
        "no resolved name: the exception's place, as before"
    assert sign("CRASH: exception 0x80000003 (write 0x0) at SDL3.dll+0x11c011",
                "CRASH: host frame 0: SDL3.dll+0x11c011") == "1|SDL3.dll+0x11c011"
    assert sign("CRASH: host frame 1: ntdll.dll+0x3") == "0|", "nothing to group on"
    assert sign("CRASH: host frame 0: HeyListen.exe+0x1 crash_now+0x10") == "1|crash_now+0x10"
    print("PASS: the signature is the resolved top frame: one key for two builds' different raw offsets")
    assert sign("CRASH: guest frame 0: 0x80580b38 f_80580b38+0x0") == "0|", "a guest frame is not the host signature"
    assert sign("CRASH: host frame 0: HeyListen.exe+0x1 operator_new+0x4") == "1|operator_new+0x4"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=shutil.which("gcc") or shutil.which("clang") or "cc")
    a = p.parse_args()
    source_contract()
    with tempfile.TemporaryDirectory(prefix="heylisten-crash-names-") as tmp:
        temp = Path(tmp)
        exe = table_and_fixture(a.cc, temp)
        signatures(exe)
    print("ok: crash names")


if __name__ == "__main__":
    main()
