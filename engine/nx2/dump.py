"""What a dump is: program id, Switch 1 or Switch 2, modules, build ids, SDK, graphics API, romfs size.

Reads the user's decrypted dump in place (exefs/ + romfs/); writes nothing into the repository. With --set it
records the dump's folder in work/dump.txt (ignored by git), where nx2/kit.py finds it.

Switch 1 or Switch 2 is decided from several independent signals, all of which must agree:
  - the program id's prefix (Switch 1 titles are 01xx..., Switch 2 ones 04xx...);
  - main.npdm's ACID layout (the Switch 1 ACID has its magic at +0x200; the Switch 2 one does not);
  - the services the npdm allows (Switch 1 games open nvdrv and vi:u for NVN; Switch 2 ones go through nx2 gs/gpu);
  - DT_RELR packed relocations (the Switch 2 toolchain emits them; the Switch 1 one emits RELA only);
  - ARMv8.1+ instructions in main (Cortex-A78C: PAC returns, LSE atomics, LDAPR; the A57 has none of them);
  - the NVN names in main (NVN2 adds DLSS and mesh-task procs) and the shader container names in romfs
    (Nin_NX_NVN for Maxwell, Nin_Ounce_NVN2 for Ampere).

Usage:  python -I nx2/dump.py [--dump <folder with exefs/ and romfs/>] [--set] [--no-romfs]
"""
import argparse
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit
import nso2  # noqa: F401  (DT_RELR)
import nso
import numpy as np


def npdm(path):
    b = open(path, "rb").read()
    out = {"size": len(b)}
    if b[:4] != b"META":
        out["error"] = "no META magic"
        return out
    flags = b[0xC]
    out["is64"] = bool(flags & 1)
    out["address_space"] = {0: "32-bit", 1: "36-bit", 2: "32-bit (no reserved)", 3: "39-bit"}.get((flags >> 1) & 7,
                                                                                                    "type %d" % ((flags >> 1) & 7))
    out["main_thread_priority"] = b[0xE]
    out["main_thread_core"] = b[0xF]
    out["system_resource_size"] = struct.unpack_from("<I", b, 0x14)[0]
    out["version"] = struct.unpack_from("<I", b, 0x18)[0]
    out["main_thread_stack"] = struct.unpack_from("<I", b, 0x1C)[0]
    out["name"] = b[0x20:0x30].split(b"\0")[0].decode("ascii", "replace")
    out["product_code"] = b[0x30:0x40].split(b"\0")[0].decode("ascii", "replace")
    aoff, asz, doff, dsz = struct.unpack_from("<IIII", b, 0x70)
    aci = b[aoff:aoff + asz]
    if aci[:4] == b"ACI0":
        out["program_id"] = struct.unpack_from("<Q", aci, 0x10)[0]
        sacoff, sacsz = struct.unpack_from("<II", aci, 0x28)
        sac, i, names = aci[sacoff:sacoff + sacsz], 0, []
        while i < len(sac):
            c = sac[i]
            n = (c & 7) + 1
            names.append(sac[i + 1:i + 1 + n].decode("ascii", "replace") + (" (host)" if c & 0x80 else ""))
            i += 1 + n
        out["services"] = names
        kacoff, kacsz = struct.unpack_from("<II", aci, 0x30)
        kac = struct.unpack_from("<%dI" % (kacsz // 4), aci, kacoff)
        svcs = set()
        for d in kac:
            low = (~d & (d + 1)) & 0xFFFFFFFF   # the descriptor type is the count of trailing ones
            if low == 1 << 4:                   # 0b01111: enabled SVCs, 24 per descriptor
                idx = (d >> 29) & 7
                mask = (d >> 5) & 0xFFFFFF
                svcs |= {idx * 24 + j for j in range(24) if mask >> j & 1}
        out["svcs"] = sorted(svcs)
    acid = b[doff:doff + dsz]
    out["acid_switch1_layout"] = acid[0x200:0x204] == b"ACID"
    return out


def arm_v81_census(m):
    """occurrences in .text of instruction classes the Cortex-A57 (Switch 1) does not have"""
    lo, hi = m.code_range()
    w = np.frombuffer(bytes(m.image[lo:hi]), dtype=np.uint32)
    c = collections.OrderedDict()
    c["RETAA/RETAB"] = int(np.count_nonzero((w == 0xD65F0BFF) | (w == 0xD65F0FFF)))
    c["PACIBSP/PACIASP"] = int(np.count_nonzero((w == 0xD503237F) | (w == 0xD503233F)))
    c["LSE (LDADD..SWP)"] = int(np.count_nonzero((w & 0x3F200C00) == 0x38200000))
    c["CAS/CASP"] = int(np.count_nonzero(((w & 0x3FA07C00) == 0x08A07C00) | ((w & 0xBFA07C00) == 0x08207C00)))
    c["LDAPR"] = int(np.count_nonzero((w & 0x3FFFFC00) == 0x38BFC000))
    c["words"] = int(len(w))
    return c


def strings_with(m, pat, limit=12):
    data = bytes(m.image)
    out = []
    for hit in re.finditer(pat, data):
        s = data[hit.start():data.find(b"\0", hit.start())]
        if s not in out:
            out.append(s)
        if len(out) >= limit:
            break
    return [s.decode("ascii", "replace") for s in out]


def romfs_summary(root, cap=400000):
    files = size = 0
    shaders = collections.Counter()
    top = collections.Counter()
    for d, dirs, fs in os.walk(root):
        rel = os.path.relpath(d, root)
        head = rel.split(os.sep)[0] if rel != "." else "."
        for f in fs:
            files += 1
            try:
                s = os.path.getsize(os.path.join(d, f))
            except OSError:
                s = 0
            size += s
            top[head] += s
            for tag in ("Nin_NX_NVN", "Nin_Ounce_NVN2", "NVN2", ".bfsha", ".bnsh", ".bntx", ".sarc", ".zs", ".szs"):
                if tag in f:
                    shaders[tag] += 1
        if files >= cap:
            break
    return files, size, top, shaders


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dump", default=kit.DUMP, help="the folder with exefs/ and romfs/")
    p.add_argument("--set", action="store_true", help="record --dump in work/dump.txt")
    p.add_argument("--no-romfs", action="store_true")
    a = p.parse_args()
    if not a.dump:
        raise SystemExit("give --dump <folder with exefs/ and romfs/>")
    exefs = os.path.join(a.dump, "exefs")
    romfs = os.path.join(a.dump, "romfs")
    if not os.path.isdir(exefs):
        raise SystemExit("%s: no exefs/" % a.dump)
    if a.set:
        os.makedirs(kit.WORK, exist_ok=True)
        with open(os.path.join(kit.WORK, "dump.txt"), "w", encoding="utf-8") as f:
            f.write("# the user's decrypted dump (exefs/ + romfs/), read in place; never copied into the repo\n%s\n"
                    % os.path.abspath(a.dump))
        print("recorded in %s" % os.path.join(kit.WORK, "dump.txt"))

    print("== dump: %s" % os.path.abspath(a.dump))
    votes = {}
    meta = None
    if os.path.exists(os.path.join(exefs, "main.npdm")):
        meta = npdm(os.path.join(exefs, "main.npdm"))
        pid = meta.get("program_id")
        print("npdm: name %r, product %r, %s, %s address space, main thread prio %d core %d stack %#x, "
              "system resource %#x, version %d" % (
                  meta.get("name"), meta.get("product_code"), "64-bit" if meta.get("is64") else "32-bit",
                  meta.get("address_space"), meta.get("main_thread_priority", 0), meta.get("main_thread_core", 0),
                  meta.get("main_thread_stack", 0), meta.get("system_resource_size", 0), meta.get("version", 0)))
        if pid is not None:
            print("program id: %016X" % pid)
            votes["program id prefix"] = {0x01: 1, 0x04: 2}.get(pid >> 56)
        votes["ACID layout"] = 1 if meta.get("acid_switch1_layout") else 2
        svc = meta.get("services", [])
        print("services (%d): %s" % (len(svc), " ".join(svc)))
        print("SVCs allowed (%d): %s" % (len(meta.get("svcs", [])), " ".join("%#x" % s for s in meta.get("svcs", []))))
        votes["nvdrv in services"] = 1 if "nvdrv" in svc else 2
    mods = nso.load_exefs(exefs)
    print("modules (rtld's load order):")
    print("  %-8s %-40s %12s %12s %12s %10s %9s %8s" % ("name", "build id", ".text", ".rodata", ".data", ".bss",
                                                       "eh starts", "RELR"))
    relr = 0
    for m in mods:
        segs = {n: s for _, s, n in m.segments}
        print("  %-8s %-40s %12s %12s %12s %10s %9s %8s" % (
            m.name, m.build_id[:40], "{:,}".format(segs[".text"]), "{:,}".format(segs[".rodata"]),
            "{:,}".format(segs[".data"]), "{:,}".format(m.bss_size), "{:,}".format(len(m.eh_funcs)),
            "{:,}".format(getattr(m, "relr_count", 0))))
        relr += getattr(m, "relr_count", 0)
    votes["DT_RELR"] = 2 if relr else 1
    main_m = next((m for m in mods if m.name == "main"), None)
    sdk_m = next((m for m in mods if m.name == "sdk"), None)
    if main_m:
        c = arm_v81_census(main_m)
        print("main .text ARMv8.1+ (Switch 2 CPU) forms: " + ", ".join("%s %s" % (k, "{:,}".format(v)) for k, v in c.items()))
        v81 = c["RETAA/RETAB"] + c["PACIBSP/PACIASP"] + c["LSE (LDADD..SWP)"] + c["CAS/CASP"] + c["LDAPR"]
        votes["ARMv8.1+ in main"] = 2 if v81 > c["words"] // 2000 else 1
        nvn = sorted(set(strings_with(main_m, rb"nvn[A-Z][A-Za-z0-9]{3,80}\0", limit=100000)))
        dlss = [s for s in nvn if "Dlss" in s or "MeshTask" in s]
        print("main names %d nvn* procs (%d DLSS / mesh-task)" % (len(nvn), len(dlss)))
        if nvn:
            votes["NVN names"] = 2 if dlss else 1
    for m in (sdk_m, main_m):
        if m:
            sdkv = strings_with(m, rb"SDK MW\+[^\0]{4,120}\0", limit=20)
            if sdkv:
                print("%s middleware strings: %s" % (m.name, "; ".join(sdkv[:12])))
    if not a.no_romfs and os.path.isdir(romfs):
        files, size, top, shaders = romfs_summary(romfs)
        print("romfs: %s files, %.2f GiB; biggest folders: %s" % ("{:,}".format(files), size / 2 ** 30, ", ".join(
            "%s %.2f GiB" % (k, v / 2 ** 30) for k, v in top.most_common(8))))
        print("romfs names: " + ", ".join("%s %d" % kv for kv in shaders.most_common()))
        if shaders["Nin_NX_NVN"] or shaders["Nin_Ounce_NVN2"] or shaders["NVN2"]:
            votes["romfs shader names"] = 2 if (shaders["Nin_Ounce_NVN2"] or shaders["NVN2"]) else 1
    print("platform signals: " + ", ".join("%s: Switch %s" % (k, v if v else "?") for k, v in votes.items()))
    known = {v for v in votes.values() if v}
    print("PLATFORM: %s" % ("Switch %d" % known.pop() if len(known) == 1 else "signals disagree: %s" % votes))


if __name__ == "__main__":
    main()
