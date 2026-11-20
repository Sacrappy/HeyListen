# SPDX-License-Identifier: MIT
# The reference below is a line-by-line port of RenoDX (github.com/clshortfuse/renodx); Copyright (c) 2025 Carlos Lopez Jr.,
# MIT: gpu/vk/eden/shader_recompiler/ir_opt/LICENSE-renodx.md
"""PsychoV35 as the scene tone map pass emits it (gpu/vk/eden/shader_recompiler/ir_opt/psychov35.h) gives what RenoDX's
zelda-oot-oz mod gives at its defaults.

The header's math is written over its operations; here they are floats. The reference is RenoDX's path, as written in
scene_tonemap.slangi and psychov/test35.hlsl (github.com/clshortfuse/renodx, MIT), ported line by line: the log-domain
response, the residual's normalization before its length, the source and target matrices from their XYZ definitions. The
header has the reduced form (one anchor power, the logistic) and its matrices baked; both are checked here.

python -I -B tests/test_psychov35.py [--cxx c++]
"""
import argparse
import math
import random
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "gpu" / "vk" / "eden" / "shader_recompiler" / "ir_opt" / "psychov35.h"


def mul(m, v):
    return [sum(m[i][j] * v[j] for j in range(3)) for i in range(3)]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def norm(v):
    return math.sqrt(dot(v, v))


BT709_TO_XYZ = [[.4123907993, .3575843394, .1804807884], [.2126390059, .7151686788, .0721923154],
                [.0193308187, .1191947798, .9505321522]]
XYZ_TO_BT709 = [[3.2409699419, -1.5373831776, -.4986107603], [-.9692436363, 1.8759675015, .0415550574],
                [.0556300797, -.2039769589, 1.0569715142]]
XYZ_TO_BT2020 = [[1.7166511880, -.3556707838, -.2533662814], [-.6666843518, 1.6164812366, .0157685458],
                 [.0176398574, -.0427706133, .9421031212]]
XYZ_TO_LMS = [[.2670502842655792, .8471990148492798, -.03470416612462053],
              [-.38706882411220156, 1.165429935890458, .10302286696614202],
              [.026727793989083093, -.02729131667566509, .5333267257603284]]
_det = dot(XYZ_TO_LMS[0], cross(XYZ_TO_LMS[1], XYZ_TO_LMS[2]))
_cols = [cross(XYZ_TO_LMS[1], XYZ_TO_LMS[2]), cross(XYZ_TO_LMS[2], XYZ_TO_LMS[0]), cross(XYZ_TO_LMS[0], XYZ_TO_LMS[1])]
LMS_TO_XYZ = [[_cols[j][i] / _det for j in range(3)] for i in range(3)]   # (transpose(float3x3(rows)) / det)
BT709_TO_LMS = matmul(XYZ_TO_LMS, BT709_TO_XYZ)
LMS_TO_BT709 = matmul(XYZ_TO_BT709, LMS_TO_XYZ)
LMS_TO_BT2020 = matmul(XYZ_TO_BT2020, LMS_TO_XYZ)
WHITE = mul(BT709_TO_LMS, [1, 1, 1])


def mean(v):
    return v[0] + ((v[1] - v[0]) + (v[2] - v[0])) / 3


def target_residual(n, target):
    return mul(LMS_TO_BT709 if target == 0 else LMS_TO_BT2020, [n[i] * WHITE[i] for i in range(3)])


def response_logs(log_q, k, n, h):
    latent = math.log(k) - math.log1p(-k) + n * log_q
    correction = math.log1p(math.exp(-abs(latent)))
    log_u = (min(latent, 0) - correction) / h
    log_tail = -max(latent, 0) - correction
    if latent > 20:
        return log_u, log_tail - math.log(h)
    d = -log_u * (1 + log_u * (.5 + log_u * (1 / 6 + log_u / 24))) if abs(log_u) < .01 else 1 - math.exp(log_u)
    return log_u, math.log(d)


YF_WEIGHTS = [.68990272, .34832189, 0.0]
PURITY_BOUND = False   # (the last call's purity gain was cut where a cone reaches zero)


def psychotm_test35(x, peak, cone, a_in, a_out, highlights=1.0, shadows=1.0, contrast=1.0, purity=1.0):
    """test35.hlsl's psychotm_test35 as zelda-oot-oz calls it: exposure 1, its highlights, shadows, contrast and purity
    (saturation), BT.2020 target, gamut compression 1, compression 1, no conditioning, exact fit"""
    lms = mul(BT709_TO_LMS, x)
    anchor_in = mul(BT709_TO_LMS, [a_in] * 3)
    anchor_out = mul(BT709_TO_LMS, [a_out] * 3)
    if highlights != 1 or shadows != 1:
        input_yf, anchor_yf = dot(YF_WEIGHTS, lms), dot(YF_WEIGHTS, anchor_in)
        graded = input_yf

        def smooth(t):
            return t * t * t * (t * (t * 6 - 15) + 10)

        if highlights != 1:
            ratio = graded / anchor_yf
            blend = min(1.0, max(0.0, math.log2(ratio) / math.log2(1 / anchor_yf))) if graded > anchor_yf else 0.0
            blend = smooth(blend)
            shaped = ratio ** abs(highlights - 1) if graded > anchor_yf else 1.0
            factor = 1 + (shaped - 1) * blend
            graded = graded * factor if highlights > 1 else graded / factor
        if shadows != 1:
            ratio = graded / anchor_yf
            blend = min(1.0, max(0.0, math.log2(ratio) / -4)) if graded > anchor_yf * 2 ** -4 else 1.0
            blend = smooth(blend)
            a = anchor_yf
            if shadows > 1:
                target = graded * (1 - a * a) + a * a * a * max(ratio, 1e-30) ** (2 - shadows)
            else:
                target = graded * (1 + a * a) - a * a * a * max(ratio, 1e-30) ** shadows
            graded = graded + (target - graded) * blend
        if input_yf != 0:
            lms = [v * graded / input_yf for v in lms]
    sr = [lms[i] / anchor_in[i] for i in range(3)]
    global PURITY_BOUND
    PURITY_BOUND = False
    if purity != contrast:
        neutral = dot(YF_WEIGHTS, lms) / dot(YF_WEIGHTS, anchor_in)
        direction = [v - neutral for v in sr]
        amount = purity / contrast
        if all(v >= 0 for v in sr) and neutral > 0 and amount > 1:
            for c in range(3):
                if direction[c] < 0 and neutral / -direction[c] < amount:
                    amount = neutral / -direction[c]
                    PURITY_BOUND = True
            sr = [max(neutral + amount * d, 0.0) for d in direction]
        else:
            sr = [neutral + amount * d for d in direction]
    # (a cone below zero, as Shadows under 1 gives the darkest pixels: the HLSL takes its log, NaN; the header's clamp at 0
    # makes it black, which is what is compared)
    sr = [max(v, 0.0) for v in sr]
    k = [anchor_out[i] / (WHITE[i] * peak) for i in range(3)]
    n = [contrast * cone / (1 - k[i]) for i in range(3)]
    if all(v == 0 for v in sr):
        return [0.0] * 3
    drive = max(abs(v) for v in sr)
    ref = [v / drive for v in sr]
    stops = math.log2(drive)
    resp, defi = [0.0] * 3, [0.0] * 3
    for i in range(3):
        if ref[i] == 0:
            resp[i], defi[i] = 0.0, 1.0
        else:
            lu, ld = response_logs(math.log(ref[i]) + stops * math.log(2), k[i], n[i], 1.0)
            resp[i], defi[i] = math.exp(lu), math.exp(ld)
    rm = mean(resp)
    rres = [mean(defi) - d for d in defi] if rm > .5 else [r - rm for r in resp]
    scale = max(abs(v) for v in rres)
    sres = [v / scale for v in rres] if scale > 0 else [0.0] * 3
    srad = norm(sres)
    radius = scale * srad
    direction = [v / srad for v in sres] if srad > 0 else [0.0] * 3
    source = [v - mean(ref) for v in ref]
    source_radius = norm(source)
    if source_radius > 0 and radius > 0:
        middle = [source[i] / source_radius + direction[i] for i in range(3)]
        if norm(middle) > 0:
            direction = [v / norm(middle) for v in middle]
    level, headroom = peak * rm, peak * mean(defi)
    residual = [peak * radius * d for d in direction]
    if all(v == 0 for v in residual):
        return [level] * 3
    td = target_residual(residual, 1)
    s = 1.0
    for c in range(3):
        if td[c] > 0:
            s = min(s, headroom / td[c])
        elif td[c] < 0:
            s = min(s, level / -td[c])
    out = target_residual([v * s for v in residual], 0)
    return [level + v for v in out]


def aces(m):
    return min(1.0, max(0.0, m * (2.51 * m + .03) / (m * (2.43 * m + .59) + .14)))


DEFAULTS = {"hue_clip": .5, "highlights": 1.0, "shadows": 1.0, "contrast": 1.0, "saturation": 1.0, "tone_slope": 1.0,
            "exposure": 1.0, "vanilla_midgray": True, "vanilla_slope": True}


def bank(st):
    """The renderer's settings bank for these settings, in psychov35.h's kSetting order (vk_menu.cpp psychov35_bank)"""
    return [st["hue_clip"], st["highlights"], st["shadows"], st["contrast"], st["saturation"],
            (1.3355037565264822 if st["vanilla_slope"] else 1.0) * st["tone_slope"],
            st["exposure"] * (aces(.18) / .18 if st["vanilla_midgray"] else 1.0)]


def scene_tonemap(scene, gain, peak, st=DEFAULTS):
    """scene_tonemap.slangi's ApplySceneToneMap with the mod's settings st, white and graphics white the same; the scene
    pass's clamp at 0 after it"""
    if st["exposure"] == 0:
        return [0.0] * 3
    u = list(scene)
    m = [abs(v) for v in u]
    r = [aces(v) for v in m]
    lo, rng = min(r), max(r) - min(r)
    neutral = rng == 0
    t = [min(m) + (r[i] - lo) / (1 if neutral else rng) * (max(m) - min(m)) for i in range(3)]
    t = [math.copysign(t[i], u[i]) for i in range(3)]
    if neutral:
        t = u
    if st["hue_clip"] > 0:
        bent = [u[i] + (t[i] - u[i]) * st["hue_clip"] for i in range(3)]
        w = [.2126390059, .7151686788, .0721923154]
        y, yb = dot(w, u), dot(w, bent)
        if y > 0 and yb > 0:
            u = [v * (y / yb) for v in bent]
    reference = aces(.18) / .18 if st["vanilla_midgray"] else 1.0
    a_out = .18 * gain * reference * st["exposure"]
    cone = (1.3355037565264822 if st["vanilla_slope"] else 1.0) * st["tone_slope"]
    out = psychotm_test35(u, peak, cone, .18, a_out, st["highlights"], st["shadows"], st["contrast"], st["saturation"])
    return [max(0.0, v) for v in out]


def header_checks():
    text = HEADER.read_text(encoding="utf-8")
    to_ratio = [[BT709_TO_LMS[i][j] / (.18 * WHITE[i]) for j in range(3)] for i in range(3)]
    to_2020 = [[LMS_TO_BT2020[i][j] * WHITE[j] for j in range(3)] for i in range(3)]
    to_709 = [[LMS_TO_BT709[i][j] * WHITE[j] for j in range(3)] for i in range(3)]
    for name, want in (("kToRatio", to_ratio), ("kToBt2020", to_2020), ("kToBt709", to_709)):
        start = text.index("inline constexpr float %s[3][3]" % name)
        got = [float(v.rstrip("f")) for v in
               __import__("re").findall(r"-?\d+\.\d+f", text[start:text.index(";", start)])]
        flat = [v for row in want for v in row]
        if len(got) != 9 or any(abs(a - b) > 1e-7 * max(1, abs(b)) for a, b in zip(got, flat)):
            raise SystemExit("FAIL: %s is not the matrix test35.hlsl's definitions give: %s" % (name, flat))
    start = text.index("inline constexpr float kLuminance[3]")
    got = [float(v.rstrip("f")) for v in __import__("re").findall(r"-?\d+\.\d+f", text[start:text.index(";", start)])]
    if got != BT709_TO_XYZ[1]:
        raise SystemExit("FAIL: kLuminance is not BT.709's Y row %s" % BT709_TO_XYZ[1])


FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS   /* (Windows: scanf is not deprecated here) */
#include <cmath>
#include <cstdio>
#include "psychov35.h"
#ifdef DOUBLE   // (the same header in double: its logic against the reference, without float rounding)
using Real = double;
#define READ "%lf"
#else
using Real = float;
#define READ "%f"
#endif
struct Ops {
    using F = Real;
    using B = bool;
    F Imm(float v) { return v; }
    F Add(F a, F b) { return a + b; }
    F Sub(F a, F b) { return a - b; }
    F Mul(F a, F b) { return a * b; }
    F Fma(F a, F b, F c) { return std::fma(a, b, c); }
    F Div(F a, F b) { return a * (F(1) / b); }   // (as the pass: a reciprocal)
    F Neg(F a) { return -a; }
    F Abs(F a) { return std::fabs(a); }
    F Min(F a, F b) { return std::fmin(a, b); }
    F Max(F a, F b) { return std::fmax(a, b); }
    F Sqrt(F a) { return std::sqrt(a); }
    F Log2(F a) { return std::log2(a); }
    F Exp2(F a) { return std::exp2(a); }
    F Select(B c, F a, F b) { return c ? a : b; }
    B Gt(F a, F b) { return a > b; }
    B Lt(F a, F b) { return a < b; }
    B Eq(F a, F b) { return a == b; }
    B And(B a, B b) { return a && b; }
};
int main() {
    Real r, g, b, gain, peak, v[7];
    Ops o;
    while (std::scanf(READ READ READ READ READ READ READ READ READ READ READ READ, &r, &g, &b, &gain, &peak, &v[0], &v[1], &v[2],
                      &v[3], &v[4], &v[5], &v[6]) == 12) {
        const Shader::Optimization::PsychoV35::Settings<Real> settings{v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
        const auto out = Shader::Optimization::PsychoV35::SceneToneMap(o, {r, g, b}, gain, peak, settings);
        std::printf("%.17g %.17g %.17g\n", (double)out[0], (double)out[1], (double)out[2]);
    }
    // (the bank's defaults are the header's own)
    const auto d = Shader::Optimization::PsychoV35::Defaults(o);
    std::printf("defaults %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", (double)d.hue_clip, (double)d.highlights, (double)d.shadows,
                (double)d.contrast, (double)d.purity, (double)d.cone_exponent, (double)d.anchor_scale);
}
'''


def fixture_checks(cxx):
    rng = random.Random(35)
    cases = [(.18, .18, .18, 1, 1000 / 203), (1, 1, 1, 1, 1000 / 203), (10, 10, 10, 1, 1000 / 203), (5, 1, .2, 1, 4),
             (0, 0, 0, 1, 5), (0, 2, 0, 1.5, 5), (100, 0, 0, .7, 10), (1e-6, 2e-6, 0, 1, 5), (2e3, 1e3, 5e3, 1, 20)]
    for _ in range(3000):
        scale = 10 ** rng.uniform(-3, 2.5)
        c = [rng.random() ** 3 * scale for _ in range(3)]
        if rng.random() < .1:
            c[rng.randrange(3)] = 0.0
        cases.append((*c, 10 ** rng.uniform(-.5, .8), rng.uniform(2, 50)))
    cases = [(c, DEFAULTS) for c in cases]
    # (the grading: each setting alone at its ends and between, then all of them at random. An output anchor past the
    # header's kMaxAnchor (0.95 of the peak) is left out: the reference has no such limit)
    singles = [("hue_clip", v) for v in (0.0, .25, 1.0)] + \
              [(k, v) for k in ("highlights", "shadows", "contrast", "saturation", "tone_slope") for v in (0.0, .5, 1.5, 2.0)] + \
              [("exposure", v) for v in (0.0, .5, 2.0)] + [("vanilla_midgray", False), ("vanilla_slope", False)]
    for _ in range(3000):
        st = dict(DEFAULTS)
        if rng.random() < .6:
            k, v = rng.choice(singles)
            st[k] = v
        else:
            for k in ("highlights", "shadows", "contrast", "saturation", "tone_slope"):
                st[k] = rng.uniform(0, 2)
            st["hue_clip"], st["exposure"] = rng.random(), rng.uniform(0, 2)
            st["vanilla_midgray"], st["vanilla_slope"] = rng.random() < .5, rng.random() < .5
        if st["contrast"] == 0 or st["tone_slope"] == 0:
            continue   # (no response at all: the mod divides by them)
        if not .25 <= st["saturation"] / st["contrast"] <= 4 and st["saturation"] != st["contrast"]:
            # (a purity gain past 4, as a contrast near 0 gives: the bound's near-cancelling cones carry float rounding
            # past the tolerance, on the GPU as in the mod's own HLSL)
            continue
        scale = 10 ** rng.uniform(-3, 2.5)
        c = (*[rng.random() ** 3 * scale for _ in range(3)], 10 ** rng.uniform(-.5, .8), rng.uniform(2, 50))
        if c[3] * .18 * bank(st)[6] / c[4] > .95:
            continue
        cases.append((c, st))
    results = {}
    with tempfile.TemporaryDirectory(prefix="psychov35-") as tmp:
        src = Path(tmp) / "fixture.cpp"
        src.write_text(FIXTURE, encoding="utf-8")
        inp = "".join("%.17g %.17g %.17g %.17g %.17g " % c + "%.17g %.17g %.17g %.17g %.17g %.17g %.17g\n" % tuple(bank(st))
                      for c, st in cases)
        for real, flags in (("float", []), ("double", ["-DDOUBLE"])):
            exe = Path(tmp) / ("fixture_" + real)
            subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", *flags, "-I", str(HEADER.parent), str(src),
                            "-o", str(exe)], check=True)
            results[real] = subprocess.run([str(exe)], input=inp, capture_output=True, text=True,
                                           check=True).stdout.splitlines()
    for real, lines in results.items():
        defaults = [float(v) for v in lines.pop().split()[1:]]
        if any(abs(a - b) > 1e-6 for a, b in zip(defaults, bank(DEFAULTS))):
            raise SystemExit("FAIL: the header's Defaults %s are not the mod's %s" % (defaults, bank(DEFAULTS)))
        if len(lines) != len(cases):
            raise SystemExit("FAIL: %d results for %d cases (%s)" % (len(lines), len(cases), real))
    # (in float, as the GPU runs it, at the mod's defaults; in double, every grading: the header's logic is the mod's. A
    # strong grading (the purity bound, a low contrast) carries float rounding past the tolerance, as the HLSL's own does)
    # A purity gain cut where a cone reaches zero sits on that cone's near-cancelling difference: the constants' float
    # rounding moves it, so such a colour is only checked for NaN.
    worst, compared = 0.0, 0
    for i, (case, st) in enumerate(cases):
        want = scene_tonemap(case[:3], case[3], case[4], st)
        bound = PURITY_BOUND
        for real, tolerance in (("float", 2e-4), ("double", 1e-5)):
            got = [float(v) for v in results[real][i].split()]
            if any(math.isnan(v) for v in got):
                raise SystemExit("FAIL: NaN for %s %s (%s)" % (case, st, real))
            if (real == "float" and st is not DEFAULTS) or bound:
                continue
            compared += 1
            err = max(abs(a - b) for a, b in zip(got, want)) / max(1e-3, max(abs(v) for v in want))
            worst = max(worst, err)
            if err > tolerance:
                raise SystemExit("FAIL: %s %s gives %s in %s, RenoDX's path %s" % (case, st, got, real, want))
    midgray = [float(v) for v in results["float"][0].split()]
    if any(abs(v - aces(.18)) > 1e-5 for v in midgray):
        raise SystemExit("FAIL: midgray is not the game's (%s, not %s)" % (midgray, aces(.18)))
    print("PASS: PsychoV35 as RenoDX's zelda-oot-oz has it (%d colours and gradings, %d comparisons, worst relative "
          "difference %.2g)" % (len(cases), compared, worst))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="c++")
    a = p.parse_args()
    header_checks()
    fixture_checks(a.cxx)


if __name__ == "__main__":
    main()
