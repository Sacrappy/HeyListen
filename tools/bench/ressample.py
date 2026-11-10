"""TICK-038 resource sampler, run beside tools/bench/run_bench.py (never launches or kills the game):
    python tools/bench/ressample.py <out dir>   (started just before run_bench.py; waits 30 s for HeyListen.exe)
Once a second until the game exits (130 s at most): <out>/res.csv (private bytes, working set, threads, CPU seconds, the
process's GPU memory from the GPU Process Memory counters), <out>/threads.csv (CPU seconds by thread and name), <out>/smi.csv
(nvidia-smi: memory used, utilisation) and at 12, 30, 45 and 95 s <out>/regions.txt: committed / resident MB of the guest
window, images, mapped files and private allocations (by size class, the largest listed). Needs psutil and numpy."""
import ctypes as C, ctypes.wintypes as W, csv, subprocess, sys, time
import psutil

out = sys.argv[1]
NAME = "HeyListen.exe"
k32 = C.windll.kernel32
pdh = C.windll.pdh
k32.OpenThread.restype = W.HANDLE
k32.GetThreadDescription.argtypes = [W.HANDLE, C.POINTER(C.c_wchar_p)]

def tname(tid):
    h = k32.OpenThread(0x0800, False, tid)  # THREAD_QUERY_LIMITED_INFORMATION
    if not h:
        return "?"
    p = C.c_wchar_p()
    try:
        if k32.GetThreadDescription(h, C.byref(p)) >= 0 and p.value:
            s = p.value
            k32.LocalFree(p)
            return s
        return ""
    finally:
        k32.CloseHandle(h)

class PDH_FMT_COUNTERVALUE(C.Structure):
    _fields_ = [("CStatus", W.DWORD), ("pad", W.DWORD), ("doubleValue", C.c_double)]

def pdh_counters(pid):
    q = W.HANDLE()
    if pdh.PdhOpenQueryW(None, 0, C.byref(q)):
        return None, []
    cs = []
    for name in ("Dedicated Usage", "Shared Usage", "Total Committed"):
        path = f"\\GPU Process Memory(pid_{pid}_*)\\{name}"
        size = W.DWORD(0)
        pdh.PdhExpandWildCardPathW(None, path, None, C.byref(size), 0)
        buf = C.create_unicode_buffer(size.value + 2)
        if pdh.PdhExpandWildCardPathW(None, path, buf, C.byref(size), 0):
            continue
        paths = [p for p in C.wstring_at(C.addressof(buf), size.value).split("\0") if p]
        hs = []
        for p in paths:
            h = W.HANDLE()
            if not pdh.PdhAddEnglishCounterW(q, p, 0, C.byref(h)):
                hs.append(h)
        cs.append((name, hs))
    pdh.PdhCollectQueryData(q)
    return q, cs

def pdh_read(q, cs):
    res = {}
    if not q:
        return res
    pdh.PdhCollectQueryData(q)
    for name, hs in cs:
        tot = 0.0
        for h in hs:
            v = PDH_FMT_COUNTERVALUE()
            if not pdh.PdhGetFormattedCounterValue(h, 0x200, None, C.byref(v)):  # PDH_FMT_DOUBLE
                tot += v.doubleValue
        res[name] = tot
    return res

class MBI(C.Structure):
    _fields_ = [("BaseAddress", C.c_uint64), ("AllocationBase", C.c_uint64), ("AllocationProtect", W.DWORD),
                ("PartitionId", W.WORD), ("RegionSize", C.c_uint64), ("State", W.DWORD), ("Protect", W.DWORD),
                ("Type", W.DWORD)]
k32.OpenProcess.restype = W.HANDLE
k32.VirtualQueryEx.argtypes = [W.HANDLE, C.c_uint64, C.POINTER(MBI), C.c_size_t]
k32.VirtualQueryEx.restype = C.c_size_t
psapi = C.windll.psapi
psapi.GetMappedFileNameW.argtypes = [W.HANDLE, C.c_uint64, C.c_wchar_p, W.DWORD]
class WSEX(C.Structure):
    _fields_ = [("VirtualAddress", C.c_uint64), ("Attr", C.c_uint64)]
psapi.QueryWorkingSetEx.argtypes = [W.HANDLE, C.c_void_p, W.DWORD]
G0, G1 = 0x100000000000, 0x100000000000 + (1 << 39)

import numpy as np
def resident_many(h, regions):
    """resident bytes of each (base, size) committed region (one QueryWorkingSetEx)"""
    sizes = np.array([sz // 4096 for _, sz in regions], dtype=np.int64)
    n = int(sizes.sum())
    if not n:
        return [0] * len(regions)
    arr = np.zeros((n, 2), dtype=np.uint64)
    i = 0
    for (b, sz), k in zip(regions, sizes):
        arr[i:i + k, 0] = b + np.arange(k, dtype=np.uint64) * 4096
        i += k
    if not psapi.QueryWorkingSetEx(h, arr.ctypes.data, arr.nbytes):
        return [-1] * len(regions)
    valid = (arr[:, 1] & 1).astype(np.int64)
    cs = np.concatenate([[0], np.cumsum(valid)])
    ends = np.cumsum(sizes)
    starts = ends - sizes
    return list((cs[ends] - cs[starts]) * 4096)

def region_scan(pid, path, t):
    h = k32.OpenProcess(0x0410, False, pid)  # QUERY_INFORMATION | VM_READ
    if not h:
        return
    m = MBI()
    a = 0x10000
    rows = {}
    guest_regs = []
    allregs, allkeys = [], []
    nm = C.create_unicode_buffer(520)
    while a < 0x7FFFFFFF0000 and k32.VirtualQueryEx(h, a, C.byref(m), C.sizeof(m)):
        end = m.BaseAddress + m.RegionSize
        if m.State == 0x1000:  # MEM_COMMIT
            if G0 <= m.BaseAddress < G1:
                key = ("guest", "")
                guest_regs.append((m.BaseAddress, m.RegionSize))
            elif m.Type == 0x1000000:  # MEM_IMAGE
                psapi.GetMappedFileNameW(h, m.BaseAddress, nm, 520)
                key = ("image", nm.value.split("\\")[-1])
            elif m.Type == 0x40000:  # MEM_MAPPED
                psapi.GetMappedFileNameW(h, m.BaseAddress, nm, 520)
                key = ("mapped", nm.value.split("\\")[-1] or "(pagefile section)")
            else:
                key = ("private", "%x" % m.AllocationBase)
            r = rows.setdefault(key, [0, 0, 0])
            r[0] += m.RegionSize
            r[1] += 1
            allregs.append((m.BaseAddress, m.RegionSize))
            allkeys.append(key)
        a = end if end > a else a + 0x1000
    try:
        res = resident_many(h, allregs)
    except Exception as e:
        res = [-1] * len(allregs)
    for k, rb in zip(allkeys, res):
        rows[k][2] += int(rb)
    gres = rows.get(("guest", ""), [0, 0, 0])[2]
    k32.CloseHandle(h)
    with open(path, "a") as f:
        tot, totr = {}, {}
        for (kind, name), (b, n, rb) in rows.items():
            tot[kind] = tot.get(kind, 0) + b
            totr[kind] = totr.get(kind, 0) + rb
        f.write(f"# t={t:.0f} committed/resident MB: " + " ".join(f"{k}={tot[k] >> 20}/{totr[k] >> 20}" for k in sorted(tot)) + "\n")
        cls = {}
        for (kind, name), (b, n, rb) in rows.items():
            if kind != "private":
                continue
            c = "a<1MB" if b < 1 << 20 else "b<16MB" if b < 16 << 20 else "c16-31MB" if b < 32 << 20 else "d32-63MB" if b < 64 << 20 else "e64-511MB" if b < 512 << 20 else "f>=512MB"
            e = cls.setdefault(c, [0, 0, 0])
            e[0] += 1; e[1] += b; e[2] += rb
        f.write("# private by size: " + "; ".join(f"{c}: {v[0]} allocs {v[1] >> 20}/{v[2] >> 20} MB" for c, v in sorted(cls.items())) + "\n")
        priv = sorted(((v[0], k) for k, v in rows.items() if k[0] != "guest"), reverse=True)
        for b, (kind, name) in priv[:60]:
            f.write(f"{t:.0f},{kind},{name},{b >> 20},{rows[(kind, name)][2] >> 20},{rows[(kind, name)][1]}\n")

t0 = time.monotonic()
proc = None
while time.monotonic() - t0 < 30 and proc is None:
    for p in psutil.process_iter(["name"]):
        if p.info["name"] == NAME:
            proc = p
            break
    time.sleep(0.2)
if proc is None:
    sys.exit("no game")
start = proc.create_time()
smi = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,memory.used,utilization.gpu", "--format=csv,noheader,nounits",
                        "-lms", "1000"], stdout=open(out + "/smi.csv", "w"), stderr=subprocess.DEVNULL)
q, cs = None, []
rf = open(out + "/res.csv", "w", newline="")
tf = open(out + "/threads.csv", "w", newline="")
rw, tw = csv.writer(rf), csv.writer(tf)
rw.writerow(["t", "private_mb", "commit_mb", "wset_mb", "peak_wset_mb", "nthreads", "cpu_s", "gpu_ded_mb", "gpu_shared_mb", "gpu_commit_mb", "handles"])
tw.writerow(["t", "tid", "name", "user_s", "sys_s"])
names = {}
scanned = set()
try:
    while time.monotonic() - t0 < 130:
        try:
            if not proc.is_running():
                break
            t = time.time() - start
            if q is None and t > 3:
                q, cs = pdh_counters(proc.pid)
            m = proc.memory_info()
            ct = proc.cpu_times()
            g = pdh_read(q, cs)
            ths = proc.threads()
            rw.writerow([f"{t:.1f}", m.private >> 20, m.pagefile >> 20, m.wset >> 20, m.peak_wset >> 20, len(ths),
                         f"{ct.user + ct.system:.2f}", int(g.get("Dedicated Usage", 0)) >> 20,
                         int(g.get("Shared Usage", 0)) >> 20, int(g.get("Total Committed", 0)) >> 20, proc.num_handles()])
            for th in ths:
                if th.id not in names or not names[th.id]:
                    names[th.id] = tname(th.id)
                tw.writerow([f"{t:.1f}", th.id, names[th.id], f"{th.user_time:.3f}", f"{th.system_time:.3f}"])
            rf.flush(); tf.flush()
            for s_at in (12, 30, 45, 95):
                if t >= s_at and s_at not in scanned:
                    scanned.add(s_at)
                    region_scan(proc.pid, out + "/regions.txt", t)
        except psutil.NoSuchProcess:
            break
        time.sleep(1.0)
finally:
    smi.kill()
    rf.close(); tf.close()
