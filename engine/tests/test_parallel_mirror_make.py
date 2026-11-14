"""No pool's mirror is made inside a parallel replay's segment, except a never-imported pool's, into the earliest segment's init.

gpu_buffer_bind makes a pool's mirror at its first binding: the page table points at the mirror at once, and its fill
is recorded into the uploads of the segment whose worker bound the pool first. A parallel replay (RX_VK_PARALLEL)
records its segments out of order, so an earlier segment, submitted first, read the mirror unfilled (houses, rocks
and bushes missing, geometry torn; #994). In a segment the mirror is wanted instead, made at the next submission's
start. The one exception (RX_VK_POOL_MIRROR): a pool never imported has nothing else the GPU can read, so its mirror is
made in the segment, with every command recorded into the earliest open segment's init command buffer, which is
submitted before any segment's uploads. This checks the first-binding condition in gpu/vk/vk_memory.cpp.
python -I -B tests/test_parallel_mirror_make.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    start = source.index("BufferRef gpu_buffer_bind(")
    body = source[start:source.index("\n}\n", start)]
    if not re.search(r"const bool par = par_active\.load\(", body):
        raise SystemExit("FAIL: gpu_buffer_bind no longer reads par_active into par")
    made = [i for i, line in enumerate(body.splitlines()) if "mirror_make(*p)" in line]
    if len(made) != 1:
        raise SystemExit(f"FAIL: expected one mirror_make at the first binding, found {len(made)}")
    lines = body.splitlines()
    window = "\n".join(lines[max(0, made[0] - 4):made[0] + 2])
    if not re.search(r"\(!par \|\| pm_par\)", window):
        raise SystemExit("FAIL: the first binding's mirror is not limited to outside a segment or a never-imported pool:\n" + window)
    if not re.search(r"const bool pm_par = par && p->never_imported && !p->memory;", body):
        raise SystemExit("FAIL: the segment's exception is not limited to a never-imported pool")
    if not ("upload_to_earliest_begin()" in window and "upload_to_earliest_end(" in window):
        raise SystemExit("FAIL: a never-imported pool's mirror made in a segment is not recorded into the earliest segment's init")
    print("PASS: a pool's mirror is made inside a parallel segment only when never imported, into the earliest segment's init")


if __name__ == "__main__":
    main()
