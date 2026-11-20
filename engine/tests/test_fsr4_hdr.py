"""AMD FSR 4 with the game's native HDR (HDR on Native): the game's scene colour goes past 1 there, and an LDR context given the
colour clamped to 1 cut every highlight at the paper white. While the game presents PQ (present_native_pq) FSR 4's context is
made HDR, linear, with AMD's own exposure, and again when that changes; the chain then gives it linear light, unclamped, and
takes the linear picture back (dlss_prep.comp's kSquare and kColorMax, dlss_out.comp's kSquare, vk_temporal.cpp's second pair of
pipelines). Read from the sources, without a GPU.

    python -I -B tests/test_fsr4_hdr.py
"""
import re
import sys
from pathlib import Path

VK = Path(__file__).resolve().parents[1] / "gpu" / "vk"


def function(source, start):
    """The text of the function whose first line begins with start, to its closing brace in column 0"""
    i = source.index(start)
    j = source.index("\n}\n", i)
    return source[i:j + 3]


def read(name):
    return (VK / name).read_text(encoding="utf-8")


def main():
    fsr4 = read("vk_fsr4.cpp")
    prepare = function(fsr4, "int prepare(")
    if "const bool hdr = hdr_allowed && present_native_pq.load(std::memory_order_relaxed);" not in prepare or \
            'env_switch("RX_VK_FSR4_HDR")' not in prepare or "opts.hdr" in prepare or \
            "s.context->hdr != hdr" not in prepare or "make_context(ow, oh, hdr)" not in prepare:
        sys.exit("FAIL: FSR 4's context is not made for HDR colour while the game presents PQ (and again when that changes)")
    make = function(fsr4, "std::shared_ptr<Context> make_context(")
    if "FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE" not in make or \
            "FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE" not in make:
        sys.exit("FAIL: FSR 4's HDR context is not linear HDR with AMD's exposure (or its LDR one is not non-linear)")
    evaluate = function(fsr4, "bool evaluate(")
    if "d.flags = s.context->hdr ? 0u : (uint32_t)FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;" not in evaluate:
        sys.exit("FAIL: FSR 4's dispatch calls HDR colour sRGB (it is linear light there)")
    if "start, prepare, evaluate, release, memory, hdr}" not in fsr4:
        sys.exit("FAIL: vk_fsr4.cpp's engine does not tell the chain it takes HDR colour")
    if "extern std::atomic<bool> present_native_pq;" not in fsr4:
        sys.exit("FAIL: vk_fsr4.cpp does not read what the game presents (present_native_pq)")
    replay = read("vk_replay.cpp")
    declared = re.search(r"^std::atomic<bool> present_native_pq\{false\};", replay, re.M)
    first_anon = replay.index("\nnamespace {\n", replay.index("bool write_png("))
    if not declared or declared.start() > first_anon:
        sys.exit("FAIL: present_native_pq is in an anonymous namespace: vk_fsr4.cpp cannot link to it")
    # the chain: the HDR pair of passes, chosen by what the engine says
    temporal = read("vk_temporal.cpp")
    for need in ("const bool hdr = engine.e->hdr && engine.e->hdr();", "s.prep_hdr", "s.out_hdr", "16384.0f"):
        if need not in temporal:
            sys.exit("FAIL: vk_temporal.cpp does not run the HDR pair of passes for an engine that takes HDR colour (%s)" % need)
    if "bool (*hdr)() = nullptr;" not in read("vk_temporal.h"):
        sys.exit("FAIL: TemporalEngine has no hdr()")
    prep, out = read("shaders/dlss_prep.comp"), read("shaders/dlss_out.comp")
    if "constant_id = 0) const float kColorMax = 1.0;" not in prep or "constant_id = 1) const bool kSquare = false;" not in prep:
        sys.exit("FAIL: dlss_prep.comp's HDR constants (kColorMax, kSquare) are gone")
    if "constant_id = 0) const bool kSquare = true;" not in out or "if (kSquare) c *= c;" not in out:
        sys.exit("FAIL: dlss_out.comp's kSquare is gone")
    # DLSS (NVIDIA) takes HDR colour too since its HDR feature was ported (Longshot a0dfd28b, #2243's part): its engine says hdr
    if "memory, hdr}" not in read("vk_dlss.cpp"):
        sys.exit("FAIL: DLSS does not say it takes HDR colour with the game's native HDR")
    print("test_fsr4_hdr: ok")


if __name__ == "__main__":
    main()
