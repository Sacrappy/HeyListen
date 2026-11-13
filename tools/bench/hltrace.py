"""Reads hl_trace.bin, the crash recorder's file (engine/host/hl_trace.h; HL_TRACE=1, written at a crash or with
HL_TRACE_DUMP=<s>).

  hltrace.py <file> info                          the threads and their events, the tables' counts
  hltrace.py <file> window <from_ms> <to_ms>      every thread's events in that window (ms before the file was written)
             [--thread <name part>] [--kind <name>]
  hltrace.py <file> addr <hex> [--size <hex>]     the history of an address: the live block and the freed blocks it is
                                                  in (allocated and freed by whom, when), and every event naming it
  hltrace.py <file> freed [n]                     the last n frees, with their allocations (a check that they read)
"""
import argparse
import struct
import sys

KINDS = {0x201: "alloc", 0x202: "free", 0x203: "heap", 0x204: "unmap", 0x205: "nvn", 0x206: "gpu", 0x801: "cold", 0x802: "drop",
         0x803: "zero", 0x401: "stage", 0x402: "group", 0x403: "job", 0x1001: "file"}
REC = struct.Struct("<QQQQI6I4x")      # HlTraceRecord: addr, size, heap, stamp, thread, chain[6]
FREE = struct.Struct("<" + REC.format[1:] * 2 + "i4x")
EV = struct.Struct("<QIIQQ")           # stamp, kind, c, a, b
GPU = struct.Struct("<QQQQQQII")      # the back end's records: guest, size, pool, value, done, stamp, what, thread
GPU_WHAT = {1: "write-back", 2: "lazy release write-back", 3: "mirror made", 4: "mirror given back", 5: "import made",
            6: "import given back", 7: "readback page"}


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"HLTRACE1":
        raise SystemExit("%s: not an hl_trace file" % path)
    tick_ms, now, events, nrings = struct.unpack_from("<dQII", data, 8)
    at = 32
    rings = []
    for _ in range(nrings):
        name = data[at:at + 32].split(b"\0")[0].decode("utf-8", "replace")
        tid, head = struct.unpack_from("<IQ", data, at + 32)
        at += 44
        evs = []
        if head:
            first = max(0, head - events)
            for i in range(first, head):
                stamp, kind, c, a, b = EV.unpack_from(data, at + (i % events) * 32)
                evs.append(((now - stamp) / tick_ms, kind, a, b, c))
        rings.append((tid, name, head, evs))
        at += events * 32
    nfreed, freed_head = struct.unpack_from("<IQ", data, at)
    at += 12
    frees = []
    for i in range(max(0, freed_head - nfreed), freed_head):
        v = FREE.unpack_from(data, at + (i % nfreed) * FREE.size)
        frees.append((record(v[:11], now, tick_ms), record(v[11:22], now, tick_ms) if v[22] else None))
    at += nfreed * FREE.size
    (nlive,) = struct.unpack_from("<I", data, at)
    at += 4
    live = []
    for i in range(nlive):
        v = REC.unpack_from(data, at + i * REC.size)
        if v[0]:
            live.append(record(v, now, tick_ms))
    at += nlive * REC.size
    gpu = []
    if at + 12 <= len(data):
        ngpu, gpu_head = struct.unpack_from("<IQ", data, at)
        at += 12
        for i in range(max(0, gpu_head - ngpu), gpu_head):
            g, size, pool, value, done, stamp, what, tid = GPU.unpack_from(data, at + (i % ngpu) * GPU.size)
            gpu.append({"guest": g, "size": size, "pool": pool, "value": value, "done": done,
                        "ago": (now - stamp) / tick_ms, "what": what, "thread": tid})
    return {"rings": rings, "frees": frees, "live": live, "gpu": gpu, "names": {r[0]: r[1] for r in rings}}


def record(v, now, tick_ms):
    return {"addr": v[0], "size": v[1], "heap": v[2], "ago": (now - v[3]) / tick_ms, "thread": v[4],
            "chain": [x for x in v[5:11] if x]}


def show_record(t, what, r):
    print("  %-9s %10.3f ms ago on %-22s block %#x size %#x heap %#x by %s" % (
        what, r["ago"], t["names"].get(r["thread"], "?"), r["addr"], r["size"], r["heap"],
        " ".join("%#x" % x for x in r["chain"])))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("file")
    p.add_argument("cmd", choices=["info", "window", "addr", "freed"])
    p.add_argument("args", nargs="*")
    p.add_argument("--thread", default="")
    p.add_argument("--kind", default="")
    p.add_argument("--size", default="1")
    a = p.parse_args()
    t = load(a.file)
    if a.cmd == "info":
        for tid, name, head, evs in t["rings"]:
            print("%4d %-28s %9d events written, %d kept" % (tid, name, head, len(evs)))
        print("%d frees kept, %d live blocks listed" % (len(t["frees"]), len(t["live"])))
    elif a.cmd == "window":
        lo, hi = float(a.args[0]), float(a.args[1])
        rows = [(ago, name, ev) for tid, name, _, evs in t["rings"] if a.thread in name
                for ago, *ev in evs if lo <= ago <= hi and (not a.kind or KINDS.get(ev[0]) == a.kind)]
        for ago, name, (kind, x, y, c) in sorted(rows, key=lambda r: -r[0]):
            print("%10.3f ms ago %-24s %-6s %#x %#x %#x" % (ago, name, KINDS.get(kind, hex(kind)), x, y, c))
    elif a.cmd == "addr":
        addr, size = int(a.args[0], 16), int(a.size, 16)
        hit = lambda r: r["addr"] < addr + size and addr < r["addr"] + max(r["size"], 1)
        holders = sorted((r for r in t["live"] if hit(r)), key=lambda r: r["size"])
        for r in holders[:1]:   # (the smallest: a heap's arena is a block of its parent heap too)
            print("live block (%d larger ones hold it: heaps' arenas):" % (len(holders) - 1))
            show_record(t, "allocated", r)
        for fr, al in reversed(t["frees"]):
            if hit(fr):
                print("freed block:")
                show_record(t, "freed", fr)
                if al:
                    show_record(t, "allocated", al)
        print("the back end's records over it (newest first):")
        for g in reversed(t["gpu"]):
            if g["guest"] < addr + size and addr < g["guest"] + max(g["size"], 1):
                print("  %10.3f ms ago %-24s %-24s [%#x, %#x) pool %#x submission %d (GPU done %d then)" % (
                    g["ago"], t["names"].get(g["thread"], "?"), GPU_WHAT.get(g["what"], "?"), g["guest"],
                    g["guest"] + g["size"], g["pool"], g["value"], g["done"]))
        rows = [(ago, name, ev) for tid, name, _, evs in t["rings"] for ago, *ev in evs
                if ev[1] < addr + size and addr < ev[1] + max(ev[2] if ev[0] in (0x201, 0x202) else 1, 1)]
        print("events naming it:")
        for ago, name, (kind, x, y, c) in sorted(rows, key=lambda r: -r[0]):
            print("%10.3f ms ago %-24s %-6s %#x %#x %#x" % (ago, name, KINDS.get(kind, hex(kind)), x, y, c))
    elif a.cmd == "freed":
        n = int(a.args[0]) if a.args else 10
        for fr, al in t["frees"][-n:]:
            print("block %#x:" % fr["addr"])
            show_record(t, "freed", fr)
            if al:
                show_record(t, "allocated", al)
    return 0


if __name__ == "__main__":
    sys.exit(main())
