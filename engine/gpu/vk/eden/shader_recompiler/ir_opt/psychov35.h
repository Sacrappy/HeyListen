// SPDX-License-Identifier: MIT
// (longshot) PsychoV35, the scene tone mapper of RenoDX's zelda-oot-oz mod (github.com/clshortfuse/renodx,
// src/games/zelda-oot-oz/scene_tonemap.slangi and src/shaders/tonemap/psychov/test35.hlsl; Copyright (c) 2025 Carlos
// Lopez Jr., MIT: LICENSE-renodx.md beside this file), with the mod's grading (Settings: hue clip emulation, highlights,
// shadows, contrast, saturation, tone slope, exposure and midgray; Defaults are the mod's), BT.2020 target with the exact
// fit. scene_tonemap_pass.h emits it as IR, its settings read from the renderer's bank; tests/test_psychov35.py runs it on
// floats against the HLSL's path.
//
// With both anchors grey (0.18 and the game's own exposure of it, times white), the cones' anchor power is one number and
// the response, at a compression of 1, a logistic of the log of each cone's ratio: the HLSL's log-domain evaluation of it
// with the same result. Written once over the operations of Ops so the pass and the test share it.

#pragma once

#include <array>

namespace Shader::Optimization::PsychoV35 {

// (ACES(0.18) / 0.18, the game's Narkowicz fit: the vanilla midgray's exposure)
inline constexpr float kReferenceExposure = 1.4827717799416489f;
// (x V'(x) / V(x) at V''(x) = 0, x = 0.120305925: the game's tone slope)
inline constexpr float kConeExponent = 1.3355037565264822f;
inline constexpr float kHueClip = 0.5f;
// (the anchor power's limits: the output midgray above 95% of the peak has no room left for highlights)
inline constexpr float kMinAnchor = 1e-6f, kMaxAnchor = 0.95f;

// BT.709 to the cones over the input anchor (0.18 times the D65 white's cones): each cone's ratio to its anchor
inline constexpr float kToRatio[3][3] = {{1.51324785f, 3.64325256f, 0.39905514f},
                                         {0.55073221f, 4.31962362f, 0.68519973f},
                                         {0.14901612f, 0.51444296f, 4.89209648f}};
// normalized cones (times the D65 white's) to BT.2020, the fit's target, and to BT.709, the output
inline constexpr float kToBt2020[3][3] = {{3.09283412f, -2.19752558f, 0.10469146f},
                                          {-0.24194003f, 1.40626676f, -0.16432673f},
                                          {-0.05344957f, 0.03725273f, 1.01619683f}};
inline constexpr float kToBt709[3][3] = {{5.28169094f, -4.47806552f, 0.19637458f},
                                         {-0.65886152f, 1.86655129f, -0.20768976f},
                                         {-0.09159885f, -0.05987826f, 1.15147711f}};
inline constexpr float kLuminance[3] = {0.2126390059f, 0.7151686788f, 0.0721923154f};
// A cone ratio's neutral (Yf) level over the anchor's: test35.hlsl's PSYCHO35_D65_ALPHA (Yf weights times the D65 white's
// cones, over its Yf), as the input anchor is grey; and that anchor's own Yf (0.18 times the D65 white's)
inline constexpr float kNeutralWeights[3] = {0.698324089f, 0.301675911f, 0.0f};
inline constexpr float kAnchorYf = 0.189072335f;
inline constexpr float kAnchorYfStops = 2.40298981f;   // (log2(1 / kAnchorYf): the highlights' span above it)

// The mod's grading, as the renderer gives it in its constant bank kSettingsBank (vk_draw.cpp uniform_range): one float
// each, at kSetting* times 4 bytes. With the defaults (Defaults below) the tone map is the mod's at its defaults.
inline constexpr unsigned kSettingsBank = 19;
enum : unsigned {
    kSettingHueClip,        // Hue Clip Emulation, 0 to 1 (0.5)
    kSettingHighlights,     // Highlights, Shadows, Contrast and Saturation: the mod's 0 to 100 times 0.02 (1)
    kSettingShadows,
    kSettingContrast,
    kSettingPurity,
    kSettingConeExponent,   // Match Tone Slope's kConeExponent (else 1) times Tone Slope (0 to 100 times 0.02)
    kSettingAnchorScale,    // Exposure, times kReferenceExposure with Midgray on Vanilla (else 1): 0 black
    // (the scene pass's, around the tone map: not Settings)
    kSettingPeak,           // the peak over the scene's white: the mod's Peak over its Game brightness, at least 1
    kSettingBloom,          // Bloom, the mod's 0 to 100 times 0.02 (1: the game's)
    kSettingSceneScale,     // the scene's white over the UI's: the mod's Game brightness over its UI Brightness
    // (the output encoder pass's, output_encode_pass.h)
    kSettingColorSpace,     // Global Color Space: 0 Vanilla (the game's matrix), 1 BT.709, 2 DCI-P3 (D65)
    kSettingOutputScale,    // the UI's white over PQ's 10000 nits: the mod's UI Brightness / 10000
    kSettingCount
};

template <class F>
struct Settings {
    F hue_clip, highlights, shadows, contrast, purity, cone_exponent, anchor_scale;
};

/// The mod's defaults: hue clip emulation 50%, the grading sliders neutral, the game's tone slope and midgray
template <class Ops>
Settings<typename Ops::F> Defaults(Ops& o) {
    return {o.Imm(kHueClip), o.Imm(1.0f), o.Imm(1.0f), o.Imm(1.0f), o.Imm(1.0f), o.Imm(kConeExponent),
            o.Imm(kReferenceExposure)};
}

// Ops: F (a float), B (a bool), and Imm, Add, Sub, Mul, Fma, Div, Neg, Abs, Min, Max, Sqrt, Log2, Exp2, Select(B, F, F),
// Gt, Lt, Eq, And
template <class Ops>
std::array<typename Ops::F, 3> SceneToneMap(Ops& o, const std::array<typename Ops::F, 3>& scene, typename Ops::F gain,
                                            typename Ops::F peak, const Settings<typename Ops::F>& settings) {
    using F = typename Ops::F;
    using V = std::array<F, 3>;
    const F zero = o.Imm(0.0f), one = o.Imm(1.0f);
    const auto min3 = [&](const V& v) { return o.Min(o.Min(v[0], v[1]), v[2]); };
    const auto max3 = [&](const V& v) { return o.Max(o.Max(v[0], v[1]), v[2]); };
    const auto mean3 = [&](const V& v) { return o.Mul(o.Add(o.Add(v[0], v[1]), v[2]), o.Imm(1.0f / 3.0f)); };
    const auto dot3 = [&](const V& a, const V& b) { return o.Fma(a[2], b[2], o.Fma(a[1], b[1], o.Mul(a[0], b[0]))); };
    const auto mat = [&](const float m[3][3], const V& v) {
        V r;
        for (int i = 0; i < 3; i++)
            r[i] = o.Fma(o.Imm(m[i][2]), v[2], o.Fma(o.Imm(m[i][1]), v[1], o.Mul(o.Imm(m[i][0]), v[0])));
        return r;
    };

    // Hue clip emulation: the game's fit bends hues as it clips them; keep half of that bend, at the input's luminance
    V u = scene, magnitude, fit;
    for (int i = 0; i < 3; i++) {
        magnitude[i] = o.Abs(u[i]);
        const F m = magnitude[i];
        fit[i] = o.Max(zero, o.Min(one, o.Div(o.Mul(m, o.Fma(m, o.Imm(2.51f), o.Imm(0.03f))),
                                              o.Fma(m, o.Fma(m, o.Imm(2.43f), o.Imm(0.59f)), o.Imm(0.14f)))));
    }
    const F fit_min = min3(fit), fit_range = o.Sub(max3(fit), fit_min);
    const auto neutral = o.Eq(fit_range, zero);
    const F span = o.Select(neutral, one, fit_range), low = min3(magnitude), wide = o.Sub(max3(magnitude), low);
    V bent;
    for (int i = 0; i < 3; i++) {
        F moved = o.Fma(o.Div(o.Sub(fit[i], fit_min), span), wide, low);
        moved = o.Select(o.Lt(u[i], zero), o.Neg(moved), moved);
        moved = o.Select(neutral, u[i], moved);
        bent[i] = o.Fma(o.Sub(moved, u[i]), settings.hue_clip, u[i]);
    }
    const V weights{o.Imm(kLuminance[0]), o.Imm(kLuminance[1]), o.Imm(kLuminance[2])};
    const F y = dot3(weights, u), bent_y = dot3(weights, bent);
    const auto keeps = o.And(o.Gt(y, zero), o.Gt(bent_y, zero));
    const F to_y = o.Div(y, bent_y);
    for (int i = 0; i < 3; i++) u[i] = o.Select(keeps, o.Mul(bent[i], to_y), u[i]);

    // The cones' response around the anchors: in at 0.18, out at the game's exposure of 0.18 (midgray as the game has it)
    // times the anchor scale (Exposure, and Midgray's choice)
    const F anchor = o.Max(o.Imm(kMinAnchor),
                           o.Min(o.Imm(kMaxAnchor), o.Div(o.Mul(o.Mul(gain, settings.anchor_scale), o.Imm(0.18f)), peak)));
    const F rest = o.Sub(one, anchor);
    const F exponent = o.Div(o.Mul(settings.contrast, settings.cone_exponent), rest);
    const F offset = o.Log2(o.Div(anchor, rest));
    V ratio = mat(kToRatio, u), response, deficit;

    // Highlights and shadows (test35.hlsl): the neutral (Yf) level graded around the anchor's, the cones scaled with it.
    // A setting at 1 leaves the level as it is (its Select), so the defaults keep the ungraded path exactly.
    const F anchor_yf = o.Imm(kAnchorYf);
    const V alpha{o.Imm(kNeutralWeights[0]), o.Imm(kNeutralWeights[1]), o.Imm(kNeutralWeights[2])};
    const auto smooth = [&](F t) {   // (t^3 (t (6t - 15) + 10))
        return o.Mul(o.Mul(o.Mul(t, t), t), o.Fma(t, o.Fma(t, o.Imm(6.0f), o.Imm(-15.0f)), o.Imm(10.0f)));
    };
    const auto saturate = [&](F v) { return o.Max(zero, o.Min(one, v)); };
    const auto power = [&](F base, F e) { return o.Exp2(o.Mul(e, o.Log2(o.Max(base, o.Imm(1e-30f))))); };
    const F level_in = o.Mul(dot3(alpha, ratio), anchor_yf);
    F graded = level_in;
    {
        const F h = settings.highlights;
        const F level = o.Div(graded, anchor_yf);
        const auto above = o.Gt(graded, anchor_yf);
        const F blend = smooth(o.Select(above, saturate(o.Div(o.Log2(level), o.Imm(kAnchorYfStops))), zero));
        const F shaped = o.Select(above, power(level, o.Abs(o.Sub(h, one))), one);
        const F factor = o.Fma(blend, o.Sub(shaped, one), one);
        const F highlighted = o.Select(o.Gt(h, one), o.Mul(graded, factor), o.Div(graded, factor));
        graded = o.Select(o.Eq(h, one), graded, highlighted);
    }
    {
        const F s = settings.shadows;
        const F level = o.Div(graded, anchor_yf);
        const F blend = smooth(o.Select(o.Gt(graded, o.Mul(anchor_yf, o.Imm(1.0f / 16.0f))),
                                        saturate(o.Div(o.Log2(level), o.Imm(-4.0f))), one));
        const F a2 = o.Mul(anchor_yf, anchor_yf), a3 = o.Mul(a2, anchor_yf);
        const auto deepen = o.Gt(s, one);
        const F lifted = o.Mul(a3, power(level, o.Select(deepen, o.Sub(o.Imm(2.0f), s), s)));
        const F target = o.Select(deepen, o.Fma(graded, o.Sub(one, a2), lifted), o.Sub(o.Mul(graded, o.Add(one, a2)), lifted));
        graded = o.Select(o.Eq(s, one), graded, o.Fma(blend, o.Sub(target, graded), graded));
    }
    const F level_scale = o.Select(o.Eq(level_in, zero), one, o.Div(graded, level_in));
    for (int i = 0; i < 3; i++) ratio[i] = o.Mul(ratio[i], level_scale);

    // Purity (Saturation over Contrast, test35.hlsl): the cones moved from their neutral level along their direction; a
    // gain above 1 bound so no cone of a valid input goes below zero. At purity equal to contrast, left as it is.
    {
        const F neutral_level = dot3(alpha, ratio);
        const F amount = o.Div(settings.purity, settings.contrast);
        V direction_from_neutral;
        F bounded_amount = amount;
        for (int i = 0; i < 3; i++) {
            direction_from_neutral[i] = o.Sub(ratio[i], neutral_level);
            bounded_amount = o.Select(o.Lt(direction_from_neutral[i], zero),
                                      o.Min(bounded_amount, o.Div(neutral_level, o.Neg(direction_from_neutral[i]))),
                                      bounded_amount);
        }
        const auto bound = o.And(o.Gt(neutral_level, zero), o.Gt(amount, one));
        const auto unchanged = o.Eq(settings.purity, settings.contrast);
        const auto negative_input = o.Lt(min3(ratio), zero);
        for (int i = 0; i < 3; i++) {
            const F free_ratio = o.Fma(amount, direction_from_neutral[i], neutral_level);
            const F bounded_ratio = o.Max(o.Fma(bounded_amount, direction_from_neutral[i], neutral_level), zero);
            const F purified = o.Select(negative_input, free_ratio, o.Select(bound, bounded_ratio, free_ratio));
            ratio[i] = o.Select(unchanged, ratio[i], purified);
        }
    }

    for (int i = 0; i < 3; i++) {
        ratio[i] = o.Max(ratio[i], zero);
        const F latent = o.Fma(exponent, o.Log2(ratio[i]), offset);   // (-inf at a zero cone: response 0, deficit 1)
        response[i] = o.Div(one, o.Add(one, o.Exp2(o.Neg(latent))));
        deficit[i] = o.Div(one, o.Add(one, o.Exp2(latent)));
    }
    const F response_mean = mean3(response), deficit_mean = mean3(deficit);
    const auto bright = o.Gt(response_mean, o.Imm(0.5f));
    V residual;
    for (int i = 0; i < 3; i++)
        residual[i] = o.Select(bright, o.Sub(deficit_mean, deficit[i]), o.Sub(response[i], response_mean));
    const F radius = o.Sqrt(dot3(residual, residual));
    const auto has_radius = o.Gt(radius, zero);
    V direction;
    for (int i = 0; i < 3; i++) direction[i] = o.Select(has_radius, o.Div(residual[i], radius), zero);
    // (the Mean-A2 midpoint: halfway between the source's direction and the response's)
    const F ratio_mean = mean3(ratio);
    V source;
    for (int i = 0; i < 3; i++) source[i] = o.Sub(ratio[i], ratio_mean);
    const F source_radius = o.Sqrt(dot3(source, source));
    V middle;
    for (int i = 0; i < 3; i++) middle[i] = o.Add(o.Div(source[i], source_radius), direction[i]);
    const F middle_length = o.Sqrt(dot3(middle, middle));
    const auto midpoint = o.And(o.And(o.Gt(source_radius, zero), has_radius), o.Gt(middle_length, zero));
    for (int i = 0; i < 3; i++) direction[i] = o.Select(midpoint, o.Div(middle[i], middle_length), direction[i]);

    // The exact fit into BT.2020 under the peak: the residual scaled down to the nearest face
    const F level = o.Mul(peak, response_mean), headroom = o.Mul(peak, deficit_mean), length = o.Mul(peak, radius);
    V offset_lms;
    for (int i = 0; i < 3; i++) offset_lms[i] = o.Mul(length, direction[i]);
    const V target = mat(kToBt2020, offset_lms);
    F scale = one;
    for (int i = 0; i < 3; i++) {
        const F up = o.Div(headroom, target[i]), down = o.Div(level, o.Neg(target[i]));
        scale = o.Min(scale, o.Select(o.Gt(target[i], zero), up, o.Select(o.Lt(target[i], zero), down, one)));
    }
    for (int i = 0; i < 3; i++) offset_lms[i] = o.Mul(offset_lms[i], scale);
    const V out709 = mat(kToBt709, offset_lms);
    V out;
    // (the scene buffer's later passes take no negative; Exposure at 0 is black, as the mod has it)
    const auto black = o.Eq(settings.anchor_scale, zero);
    for (int i = 0; i < 3; i++) out[i] = o.Select(black, zero, o.Max(o.Add(level, out709[i]), zero));
    return out;
}

} // namespace Shader::Optimization::PsychoV35
