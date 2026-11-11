"""Compile and run the mirror syncs' chunk diff (gpu/vk/msync_diff.h; no game/GPU), with and without AVX2, and check that
vk_memory.cpp still voids the hashes wherever the mirror may be written another way (TICK-048)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    src = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    for needle in ('#include "msync_diff.h"',
                   # (the GPU's writes: storage stamps, bare entries' guesses, wide or bare written ranges, written bindings)
                   "    diff_gpu_wrote(guest, size);\n    memory_stamp(guest, size);",
                   "diff_gpu_wrote(guest, size);   // (TICK-048: a bare entry's guessed range)",
                   "diff_gpu_wrote(guest, size);   // (TICK-048)\n    std::lock_guard lk(pools_mu);\n    rb_hazard_locked",
                   "writebacks.push_back({p, off, std::min(off + size, p->hi - p->lo)});\n        diff_gpu_wrote(",
                   # (the readback refresh writes the mirror from guest memory)
                   "diff_gpu_wrote(pp->lo + r.dstOffset, r.size)",
                   # (a new mirror: other hashes)
                   "p->mirror_id = ++diff_mirror_ids;",
                   # (hashes held only while recorded after the GPU's last write, for this mirror)
                   "d->rec && d->mid == p.mirror_id && d->rec > d->gw && d->rec > diff_gw_all",
                   # (not recorded by a parallel segment's sync)
                   "const bool d_ok = dmode && p.mirror_id && !par_active.load(std::memory_order_relaxed);"):
        if needle not in src:
            raise SystemExit("vk_memory.cpp no longer has: " + needle)
    sync = src[src.index("int64_t mirror_sync(Pool& p) {"):]
    sync = sync[:sync.index("\n}\n")]
    # (pages copied from guest memory or kept for the GPU void their hashes)
    if sync.count("d_void(page);") < 3:
        raise SystemExit("mirror_sync no longer voids the hashes of pages it copies another way")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="msync-diff-") as tmp:
        for flags in ([], ["-mavx2"]):
            exe = Path(tmp) / ("msync-diff" + ("-avx2" if flags else "") + ".exe")
            subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", *flags,
                            str(ROOT / "tests/test_msync_diff.cpp"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
