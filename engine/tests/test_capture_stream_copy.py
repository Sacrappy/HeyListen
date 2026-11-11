"""TICK-037: the capture's streaming copy (vk_memory.cpp stream_memcpy) writes the same bytes as memcpy: aligned runs of
whole pages through the streaming stores, anything else (a destination not 32-byte aligned, a size not a multiple of 128)
through memcpy; the copy_part loop fences the stores and uses it only when RX_VK_CAPTURE_STREAM allows.

python -I -B tests/test_capture_stream_copy.py --cxx <compiler>
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    copy = function(source, "__attribute__((noinline)) void* stream_memcpy(")
    part = function(source, "void copy_part(const CopyPart& w)")
    for need in ("const bool stream = stream_on();", "guarded_copy_stream(", "if (stream) stream_fence();"):
        if need not in part:
            raise SystemExit("copy_part lacks: " + need)
    if 'getenv("RX_VK_CAPTURE_STREAM")' not in source:
        raise SystemExit("no RX_VK_CAPTURE_STREAM switch")
    fixture = r'''
#include <immintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x); std::exit(1); } } while (0)
''' + copy + r'''
int main() {
    std::vector<uint8_t> src(1 << 20), a((1 << 20) + 64), b((1 << 20) + 64);
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 2654435761u >> 13);
    uint8_t* da = (uint8_t*)(((uintptr_t)a.data() + 63) & ~(uintptr_t)63);
    uint8_t* db = (uint8_t*)(((uintptr_t)b.data() + 63) & ~(uintptr_t)63);
    const size_t sizes[] = {0, 1, 31, 127, 128, 129, 4096, 4096 * 3, 4096 * 17 + 128, 65536};
    const size_t src_offs[] = {0, 1, 7, 4096};
    const size_t dst_offs[] = {0, 8, 32, 33};
    for (size_t n : sizes)
        for (size_t so : src_offs)
            for (size_t dof : dst_offs) {
                std::memset(da, 0xAB, 1 << 19);
                std::memset(db, 0xAB, 1 << 19);
                CHECK(stream_memcpy(da + dof, src.data() + so, n) == da + dof);
                _mm_sfence();
                std::memcpy(db + dof, src.data() + so, n);
                CHECK(std::memcmp(da, db, 1 << 19) == 0);
            }
    std::puts("ok");
    return 0;
}
'''
    work = Path(os.environ.get("RX_TEST_WORK", tempfile.gettempdir())) / "test_capture_stream_copy"
    work.mkdir(parents=True, exist_ok=True)
    cpp = work / "t.cpp"
    exe = work / ("t.exe" if os.name == "nt" else "t")
    cpp.write_text(fixture, encoding="utf-8")
    subprocess.run([args.cxx, "-std=c++20", "-O2", "-mavx2", str(cpp), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True)
    if out.returncode != 0 or "ok" not in out.stdout:
        raise SystemExit("stream copy differs: " + out.stdout + out.stderr)
    print("ok: stream_memcpy matches memcpy")


if __name__ == "__main__":
    main()
