"""Retired and idle imports given back on AMD (gpu/vk/vk_memory.cpp retired_imports_give_back, imports_give_back; after
Hookshot c4ec14b263).

On Windows AMD kept its retired imports for the session and never gave idle ones back; a few area changes later the driver
held ~7 GB pinned and refused every new import (VK_ERROR_INVALID_EXTERNAL_HANDLE): the terrain's index buffers could not be
bound and Hyrule Field's ground was left out. This checks, from the source as it is:
- Windows: retired imports kept by default on Intel alone (not NVIDIA, not AMD); RX_VK_KEEP_RETIRED still chooses;
- Windows: idle imports given back on AMD past 70 % of the system heap's budget down to 60 %, still off elsewhere by
  default; RX_VK_IMPORT_HIGH / RX_VK_IMPORT_LOW still choose;
- Linux unchanged: retired imports given back unless RX_VK_KEEP_RETIRED=1, idle ones past 85 % down to 75 %, only where the
  heap's budget is under 12 GB;
- the refused-import diagnostics (RX_VK_IMPORT_DIAG) only with their switch;
- issue 61 (texel views over a freed import): every import buffer these paths free goes through destroy_import_buffer,
  which releases the texel views made on it first; no bare vkDestroyBuffer in them.

python -I -B tests/test_import_give_back_amd.py
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def split_win(body):
    """the #ifdef _WIN32 part and the #else part of the first such block in body"""
    a = body.index("#ifdef _WIN32")
    b = body.index("#else", a)
    c = body.index("#endif", b)
    return body[a:b], body[b:c]


def main():
    text = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    retired = block(text, "void retired_imports_give_back() {")
    win, linux = split_win(retired)
    code = lambda s: "\n".join(l.split("//")[0] for l in s.splitlines())
    w = re.sub(r"\s+", " ", code(win))
    if 'getenv("RX_VK_KEEP_RETIRED") ? getenv("RX_VK_KEEP_RETIRED")[0] != \'0\' : dev.props.vendorID != 0x10DE && ' \
       '!(dev.props.vendorID == 0x1002 && amd_give_back_on());' not in w:
        raise SystemExit("FAIL: on Windows, retired imports are not given back by default on NVIDIA alone (AMD: "
                         "RX_VK_AMD_GIVE_BACK=1, off by default since an RX 6750 XT lost the device with it)")
    if 'static const bool keep = getenv("RX_VK_KEEP_RETIRED") && getenv("RX_VK_KEEP_RETIRED")[0] != \'0\';' not in linux:
        raise SystemExit("FAIL: on Linux, retired imports are no longer given back by default")

    idle = block(text, "void imports_give_back() {")
    win, linux = split_win(idle)
    w, l = re.sub(r"\s+", " ", code(win)), re.sub(r"\s+", " ", code(linux))
    for want in ("static const bool amd = dev.props.vendorID == 0x1002 && amd_give_back_on();",
                 "static const int high = env_high ? atoi(env_high) : amd ? 70 : 0;",
                 'std::clamp(atoi(getenv("RX_VK_IMPORT_LOW")), 10, 95) : amd ? 60 : 75;'):
        if want not in w:
            raise SystemExit("FAIL: Windows idle imports on AMD: missing " + want)
    for want in ("static const int high = env_high ? atoi(env_high) : 85;",
                 'std::clamp(atoi(getenv("RX_VK_IMPORT_LOW")), 10, 95) : 75;'):
        if want not in l:
            raise SystemExit("FAIL: Linux idle imports changed: missing " + want)
    if "small = env_high || budget < (12ull << 30);" not in idle:
        raise SystemExit("FAIL: the small-budget rule for idle imports is gone")

    imp = block(text, "bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {")
    if 'if (allocated != VK_SUCCESS && getenv("RX_VK_IMPORT_DIAG")) {' not in imp or imp.count("import diag:") != 1:
        raise SystemExit("FAIL: the refused-import diagnostics are not behind RX_VK_IMPORT_DIAG")
    if "size_t retired_pools_count() { return retired_pools.size() + retired_done.size(); }" not in text:
        raise SystemExit("FAIL: retired_pools_count is missing")

    reap = block(text, "void reap_mirrors() {")
    for name, body in (("reap_mirrors", reap), ("imports_give_back", idle)):
        if "vkDestroyBuffer(" in body:
            raise SystemExit(f"FAIL: {name} destroys an import buffer without releasing its texel views (issue 61)")
        if "destroy_import_buffer(" not in body:
            raise SystemExit(f"FAIL: {name} no longer frees imports through destroy_import_buffer")
    dib = block(text, "void destroy_import_buffer(VkBuffer buffer) {")
    if dib.index("release_texel_buffer_views(buffer)") > dib.index("vkDestroyBuffer"):
        raise SystemExit("FAIL: destroy_import_buffer does not release the texel views before the buffer goes")
    print("PASS: retired and idle imports given back on AMD (Windows), Linux as before, the diagnostics behind their switch, "
          "freed imports' texel views released first")


if __name__ == "__main__":
    main()
