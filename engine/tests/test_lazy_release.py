"""The release of a mirror holding left-out write-backs writes a left-out page back into guest memory only while its pool is
live and nothing wrote the page since the mirror's last sync (gpu/vk/vk_memory.cpp: lazy_release_verdict, in_live_pool).

python -I -B tests/test_lazy_release.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    enum = source[source.index("enum { kRelCopy,"):]
    enum = enum[:enum.index(";") + 1]
    live = block(source, "bool in_live_pool(")
    verdict = block(source, "int lazy_release_verdict(")
    # the release's page loop asks both before it copies, and drops what it does not copy from the left-out set
    release = source[source.index("if (!lazy_release.empty())"):]
    release = release[:release.index("mirror_free(*p);")]
    for want in ("lazy_release_verdict(", "in_live_pool(guest_pools", "page_gen(q), p->synced_gen", "skipped_pages.erase",
                 "verdict != kRelCopy", "vkCmdCopyBuffer(cmd, p->mirror, st.buf"):
        if want not in release:
            raise SystemExit(f"the lazy release does not use {want!r}")
    if "p->buffer" in release:
        raise SystemExit("the lazy release copies into the pool's import on the GPU again: it is staged")
    # the staged bytes reach guest memory only through the last check and the host's live-pool write
    apply = block(source, "void lazy_apply()")
    for want in ("it->value > done", "lazy_apply_verdict(page_gen(page), it->synced, cpu)", "write_live(pc.guest",
                 "lazy_host_verdict(", "skipped_pages.erase(page)", "destroy_buffer("):
        if want not in apply:
            raise SystemExit(f"lazy_apply does not use {want!r}")
    if "lazy_apply();" not in block(source, "void memory_end_submission("):
        raise SystemExit("memory_end_submission does not apply the staged write-backs")
    host = (ROOT / "host" / "nvdrv.c").read_text(encoding="utf-8")
    live_write = block(host, "__declspec(dllexport) int hos_gpu_write_live(")
    for want in ("LOCK();", "tracked[i].lo", "a64_dirty[", "UNLOCK();"):
        if want not in live_write:
            raise SystemExit(f"hos_gpu_write_live does not use {want!r}")
    apply_verdict = block(source, "int lazy_apply_verdict(")
    host_verdict = block(source, "int lazy_host_verdict(")
    fixture = f'''#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
struct GuestPool {{ uint64_t lo = 0, hi = 0; }};
{enum}
{live}
{verdict}
{apply_verdict}
{host_verdict}
int main() {{
    CHECK(lazy_apply_verdict(7, 7, false) == kRelCopy);
    CHECK(lazy_apply_verdict(8, 7, false) == kRelWritten);   // (stamped since the release: the CPU's bytes are newer)
    CHECK(lazy_apply_verdict(7, 7, true) == kRelCpuPage);
    CHECK(lazy_host_verdict(1) == kRelCopy);
    CHECK(lazy_host_verdict(2) == kRelWritten);   // (written by the CPU since the last collection)
    CHECK(lazy_host_verdict(0) == kRelDead);      // (no live pool: finalized, or no host function)
    const std::vector<GuestPool> pools = {{{{0x1000, 0x3000}}, {{0x8000, 0x9000}}}};
    CHECK(in_live_pool(pools, 0x1000, 0x3000));
    CHECK(in_live_pool(pools, 0x2000, 0x3000));
    CHECK(!in_live_pool(pools, 0x3000, 0x4000));    // (a finalized pool's range: heap again)
    CHECK(!in_live_pool(pools, 0x2000, 0x4000));    // (past a live pool's end)
    CHECK(!in_live_pool(pools, 0x0, 0x1000));
    CHECK(in_live_pool(pools, 0x8000, 0x9000));
    CHECK(lazy_release_verdict(true, 5, 7, false, false) == kRelCopy);
    CHECK(lazy_release_verdict(false, 5, 7, false, false) == kRelDead);
    CHECK(lazy_release_verdict(true, 8, 7, false, false) == kRelWritten);   // (written since the mirror's sync)
    CHECK(lazy_release_verdict(true, 7, 7, true, false) == kRelCpuPage);
    CHECK(lazy_release_verdict(true, 7, 7, false, true) == kRelTexture);
    CHECK(lazy_release_verdict(false, 8, 7, true, true) == kRelDead);
    std::printf("PASS: a left-out page is written back only into a live pool nothing wrote since\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="lazy-release-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
