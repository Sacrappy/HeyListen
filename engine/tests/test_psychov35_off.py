"""Upscaler Off with Native PsychoV35 (HeyListen, after Longshot b80a68f1): the source rules of its RGBA16F path.

HeyListen's Off is vk_draw.cpp off_stand_in over the typed upscale (shaders/taau_upscale.comp), not upstream's stretch in
vk_temporal.cpp; test_taau_typed_dispatch.py runs the typed upscale itself (RGBA16F pipeline, history format) with spies.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
from pathlib import Path

RUN_ALL_ARGS = []
ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def need(body, texts, what):
    for text in texts:
        if text not in body:
            raise SystemExit('%s: %r is missing' % (what, text))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    draw = (args.root / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    temporal = (args.root / 'gpu/vk/vk_temporal.cpp').read_text(encoding='utf-8')

    off = function(draw, 'static bool off_stand_in(')
    # RGBA16F only under the PsychoV35 tone mapper, and only a matching pair: never the packed path's views on RGBA16F images
    need(off, ('const bool rgba = out->format == VK_FORMAT_R16G16B16A16_SFLOAT && scene_tonemap_on();',
               'if (out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && !rgba) return false;',
               'if (!in || in->format != out->format) return false;',
               'texture_view_of(in_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_R16G16B16A16_SFLOAT)',
               'texture_view_of(out_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, VK_FORMAT_R16G16B16A16_SFLOAT)',
               'if (!input_view || !output_view) return false;',
               'typed_upscale(cmd, input_view, output_view, out, level, p.key, "Upscaler Off", false)',
               '!upscaler_off()'), 'off_stand_in')
    if 'if (!known || rgba) {' not in off or off.index('if (!in || in->format != out->format)') > off.index('typed_upscale('):
        raise SystemExit('off_stand_in: an RGBA16F pair is not matched before the typed upscale draws')

    typed = function(draw[draw.rindex('static bool typed_upscale(VkCommandBuffer cmd,'):], 'static bool typed_upscale(')
    # (Off is a non-temporal call: the RGBA16F shader may not ask for the temporal flag, only for the tone mapper)
    need(typed, ('if (out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && !(rgba && scene_tonemap_on())) return false;',
                 'mi.pCode = rgba ? TAAU_UPSCALE_RGBA_COMP_SPV : TAAU_UPSCALE_COMP_SPV;'), 'typed_upscale')

    scene = function(temporal, 'bool temporal_scene_format(VkFormat format) {')
    # Off takes the scene whatever RX_VK_PSYCHOV35_DLSS says; the engines only while that switch allows them
    need(scene, ('scene_tonemap_on()', '(upscaler_off() || (enabled && ur::takes_psychov35(temporal_mode())))'),
         'temporal_scene_format')
    print('PsychoV35 Off: matching RGBA16F pair, typed RGBA16F views and shader, scene admitted under Off pass')


if __name__ == '__main__':
    main()
