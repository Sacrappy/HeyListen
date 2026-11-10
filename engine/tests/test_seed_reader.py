"""The shaders seed reader (hookshot_shaders.bin, gpu/vk/vk_shader.cpp seed_open) stays inside the file.

Builds the real seed_open with stand-ins for the control section parser, the code key and the program cache, then reads:
a valid seed (two stages, two programs); files cut short at every length; stage and program counts larger than the file;
a stage size past the file's end; a program list count past it; a program naming a stage that does not exist. Each bad
file must give no seed (0), with nothing read past the buffer (built with the sanitizers when the compiler has them), and
the valid one its programs. A short random-mutation loop over the valid seed checks the same.

python -I -B tests/test_seed_reader.py [--cxx g++]
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADER = ROOT / "gpu" / "vk" / "vk_shader.cpp"


def block(text, marker):
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", default="g++")
    a = ap.parse_args()
    src = SHADER.read_text(encoding="utf-8")
    seed_open = block(src, "size_t seed_open() {")
    cpp = r"""
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define _fseeki64 fseeko
#define _ftelli64 ftello
namespace Shader { struct ProgramHeader { uint8_t b[0x50]; }; }
constexpr size_t kControlSize = 8;
static bool control_end(const uint8_t* c, uint32_t* end) { uint32_t v; std::memcpy(&v, c, 4); *end = v; return true; }
static uint64_t code_key(const uint8_t* c, const uint8_t* code, uint32_t n) { (void)c; (void)code; return n; }
static void disk_init() {}
static bool disk_listing = false;
static std::unordered_set<uint64_t> seed_keys;
static std::string the_path;
static std::string seed_path() { return the_path; }
struct SeedStage {
    uint64_t key = 0;
    uint32_t stage = 0, end = 0;
    size_t control = 0, code = 0;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
};
struct Seed {
    std::vector<uint8_t> blob;
    std::vector<SeedStage> stages;
    std::vector<std::vector<uint32_t>> programs;
    std::unordered_map<uint64_t, uint32_t> index;
} seed;
""" + seed_open + r"""
static void put32(std::vector<uint8_t>& b, uint32_t v) { const uint8_t* p = (const uint8_t*)&v; b.insert(b.end(), p, p + 4); }
static std::vector<uint8_t> valid() {
    std::vector<uint8_t> b(8);
    std::memcpy(b.data(), "HKSEED01", 8);
    put32(b, 2), put32(b, 2);              // programs, stages
    for (int s = 0; s < 2; s++) {          // {u32 a, u32 b, stage, control size, code size} control, code
        put32(b, 0), put32(b, 0), put32(b, (uint32_t)s), put32(b, 8), put32(b, 16);
        put32(b, 16), put32(b, 0);          // control: its code end
        b.insert(b.end(), 16, (uint8_t)(0x40 + s));
    }
    put32(b, 2), put32(b, 0), put32(b, 1);  // program 0: stages 0 and 1
    put32(b, 1), put32(b, 1);               // program 1: stage 1
    return b;
}
static size_t read(const std::vector<uint8_t>& b) {
    FILE* f = fopen(the_path.c_str(), "wb");
    if (!f || fwrite(b.data(), 1, b.size(), f) != b.size()) { fprintf(stderr, "write failed\n"); exit(2); }
    fclose(f);
    seed = Seed{};
    return seed_open();
}
static void require(bool ok, const char* m) { if (!ok) { fprintf(stderr, "FAIL: %s\n", m); exit(1); } }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    the_path = argv[1];
    const std::vector<uint8_t> good = valid();
    require(read(good) == 2 && seed.stages.size() == 2 && seed.programs[0].size() == 2 && seed.programs[1][0] == 1,
            "the valid seed");
    for (size_t n = 16; n < good.size(); n++) {
        std::vector<uint8_t> cut(good.begin(), good.begin() + (ptrdiff_t)n);
        require(read(cut) == 0, "a seed cut short");
    }
    auto with = [&](size_t at, uint32_t v) { std::vector<uint8_t> b = good; std::memcpy(b.data() + at, &v, 4); return b; };
    require(read(with(12, 0x10000000u)) == 0, "a stage count past the file");
    require(read(with(8, 0x7fffffffu)) == 0, "a program count past the file");
    require(read(with(16 + 16, 0xfffffff0u)) == 0, "a stage's code size past the file");
    const size_t lists = good.size() - 20;
    require(read(with(lists, 0x40000000u)) == 0, "a program list count past the file");
    require(read(with(lists + 4, 1000)) == 0, "a program naming a stage that does not exist");
    std::mt19937 rng(12345);
    for (int i = 0; i < 20000; i++) {
        std::vector<uint8_t> b = good;
        const int edits = 1 + (int)(rng() % 4);
        for (int k = 0; k < edits; k++) b[16 + rng() % (b.size() - 16)] = (uint8_t)rng();
        if (rng() % 4 == 0) b.resize(16 + rng() % (b.size() - 16));
        const size_t n = read(b);
        for (const auto& list : seed.programs)
            for (const uint32_t k : list) require(k < seed.stages.size(), "a program names a stage that does not exist");
        (void)n;
    }
    puts("PASS: the seed reader keeps to the file: cut, oversized counts and sizes, bad stage indices; 20000 mutations");
    return 0;
}
"""
    cxx = shutil.which(a.cxx)
    if not cxx:
        raise SystemExit("test_seed_reader: no C++ compiler %r (pass --cxx)" % a.cxx)
    with tempfile.TemporaryDirectory(prefix="seed-reader-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(cpp, encoding="utf-8")
        exe = tmp / "t.exe"
        base = [cxx, "-std=c++20", "-O1", "-g", str(tmp / "t.cpp"), "-o", str(exe)]
        sanitized = subprocess.run(base[:2] + ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] + base[2:],
                                   capture_output=True, text=True)
        if sanitized.returncode:   # (no sanitizer runtime with this compiler: the checks alone)
            subprocess.run(base + ["-D_GLIBCXX_ASSERTIONS"], check=True)
        r = subprocess.run([str(exe), str(tmp / "hookshot_shaders.bin")], capture_output=True, text=True, timeout=300)
        print(r.stdout, end="")
        if r.returncode:
            print(r.stderr[-4000:], end="")
            raise SystemExit("test_seed_reader: failed (exit %d)" % r.returncode)


if __name__ == "__main__":
    main()
