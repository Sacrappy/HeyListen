"""Source-shape check of three Hookshot ports (pixl27, 8cb5a334 / f0fab2a1 / 20352f9c): the memory blocks VMA empties are given back on a
thread of their own (RX_VK_FREE_LATER=0: as before), the sync-object table keeps a live byte a slot (HOS_SYNC_REHASH_OLD=1: liveness read
from each object), and the retired pools nothing needs are set aside (RX_VK_RETIRED_COMPACT=0: as before)."""
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1]
vma = (root / "gpu/vk/vk_vma.cpp").read_text(encoding="utf-8")
mem = (root / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
syn = (root / "host/nn_os_sync.c").read_text(encoding="utf-8")


def need(text, what, name):
    if what not in text:
        raise SystemExit(f"FAIL: {name}: missing {what!r}")


need(vma, "f.vkFreeMemory = free_memory_later;", "VMA's vkFreeMemory is ours")
need(vma, 'getenv("RX_VK_FREE_LATER")', "free-later off switch")
need(vma, "if (!free_later_on() || on_free_thread)", "the free thread frees on its own thread")
need(syn, 'getenv("HOS_SYNC_REHASH_OLD")', "rehash off switch")
need(syn, "static uint8_t *table_alive_of(const Table *t)", "alive bytes after the slots and keys")
need(syn, "sizeof(Obj *) + sizeof(uint64_t) + 1", "the table allocates a byte a slot")
need(syn, "alive_set(at, 0, __ATOMIC_RELEASE)", "kind changes set the byte")
if "->kind = " in syn.replace("__atomic_store_n", ""):
    raise SystemExit("FAIL: a kind written outside alive_set / the atomic stores")
need(mem, 'getenv("RX_VK_RETIRED_COMPACT")', "retired compaction off switch")
need(mem, "retired_compact();", "retired_compact is called from the submission walk")
need(mem, "size_t retired_pools_count() { return retired_pools.size() + retired_done.size(); }", "the count includes the set-aside ones")
need(mem, "vk: retired pools: %zu walked, %zu set aside", "the counters line")
print("PASS: free-later, live-byte sync table and retired pool compaction are in place")
