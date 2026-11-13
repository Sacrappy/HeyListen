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
                 "verdict != kRelCopy"):
        if want not in release:
            raise SystemExit(f"the lazy release does not use {want!r}")
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
int main() {{
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
