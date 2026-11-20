"""(HeyListen) Frame generation keeps Native HDR's picture at 10 bits: under Native HDR the game presents PQ codes in RGB10A2,
and frame generation copied them into RGBA8 frames (fg.frames) and made its frames in RGBA8 (set.frame): two bits lost,
banding in the dark and in the sky on every frame it showed, the game's own too. With the present PQ, vk_framegen.cpp's
ensure_size makes both in A2B10G10R10 (no RGBA16F image: the same 32 bits a pixel), and the copy and the blend write them
through fg_copy.comp / fg_synth.comp compiled with FG_PQ (an rgb10_a2 destination). AMD FSR 3.1's frame interpolation is
given that format (its output is written without a format). SDR keeps RGBA8.

The shader headers: fg_copy_pq_comp_spv.h and fg_synth_pq_comp_spv.h name the hash of the shader they were made from (their
first line), and their destination images are RGB10A2 in the SPIR-V; the plain headers' destinations stay RGBA8. With
glslangValidator the headers are compared with its output byte for byte (the plain headers, which have no hash line, too).
--write makes the PQ headers.

python -I -B tests/test_fg_pq_frames.py [--glslang glslangValidator] [--write]
"""
import argparse
import hashlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"
FRAMEGEN = ROOT / "gpu" / "vk" / "vk_framegen.cpp"

# (shader, plain header and its variable, PQ header and its variable, glslang's target: each plain header's own, which the
# PQ one is made with too)
SHADERS_PQ = (("fg_copy.comp", "fg_copy_comp_spv.h", "FG_COPY_COMP_SPV", "fg_copy_pq_comp_spv.h", "FG_COPY_PQ_COMP_SPV", "vulkan1.3"),
              ("fg_synth.comp", "fg_synth_comp_spv.h", "FG_SYNTH_COMP_SPV", "fg_synth_pq_comp_spv.h", "FG_SYNTH_PQ_COMP_SPV",
               "vulkan1.2"))
RGBA8, RGB10A2 = 4, 11   # (SPIR-V's image formats: Rgba8, Rgb10A2)


def source_line(comp, var, env):
    digest = hashlib.sha256(comp.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    return ("// %s sha256 %s (tests/test_fg_pq_frames.py --write: glslangValidator -V --target-env %s -DFG_PQ --vn %s)\n"
            % (comp.name, digest, env, var)).encode()


def compile_header(glslang, comp, out, var, defines, env):
    r = subprocess.run([glslang, "-V", "--target-env", env, *defines, "--vn", var, "-o", str(out), str(comp)],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("%s does not compile (%s):\n%s%s" % (comp.name, " ".join(defines) or "as it is", r.stdout, r.stderr))
    return out.read_bytes()


def image_formats(header):
    """the formats of the storage images in the header's SPIR-V (OpTypeImage, Sampled 2)"""
    words = [int(w, 16) for w in re.findall(r"0x[0-9a-fA-F]{8}", header)]
    assert words and words[0] == 0x07230203, "not SPIR-V"
    formats, i = [], 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        assert count and i + count <= len(words)
        if op == 25 and words[i + 7] == 2:   # (OpTypeImage: result, sampled type, dim, depth, arrayed, ms, sampled, format)
            formats.append(words[i + 8])
        i += count
    return formats


def check_sources():
    fg = FRAMEGEN.read_text(encoding="utf-8")
    ensure = fg[fg.index("bool ensure_size(VkCommandBuffer cmd"):]
    ensure = ensure[:ensure.index("\n}\n")]
    for need in ("fg.pq == pq", "frame_format(pq)", "make_img(fg.frames[i], w, h, format,", "make_img(set.frame[k], w, h, format,"):
        assert need in ensure, "ensure_size: %r is missing (the frames' format follows the present's PQ)" % need
    assert "VK_FORMAT_R8G8B8A8_UNORM" not in ensure, "ensure_size: an RGBA8 frame left"
    fmt = fg[fg.index("VkFormat frame_format(bool pq)"):]
    fmt = fmt[:fmt.index("\n}\n")]
    assert "VK_FORMAT_A2B10G10R10_UNORM_PACK32" in fmt and "VK_FORMAT_R8G8B8A8_UNORM" in fmt, "frame_format: RGB10A2 for PQ"
    assert "VK_FORMAT_R16G16B16A16_SFLOAT" not in fmt, "frame_format: no RGBA16F frames (no intermediate of that size)"
    record = fg[fg.index("bool framegen_record(VkCommandBuffer cmd"):]
    record = record[:record.index("\n}\n")]
    assert "ensure_size(cmd, (uint32_t)w, (uint32_t)h, pq)" in record, "framegen_record: the frames made for the present's PQ"
    assert "const Pipe& copy = fg.pq ? fg.copy_pq : fg.copy;" in record and \
        "const Pipe& synth = fg.pq ? fg.synth_pq : fg.synth;" in record, "framegen_record: the 10-bit copy and blend for PQ"
    assert "pass(cmd, fg.copy," not in record and "pass(cmd, fg.synth," not in record, "framegen_record: an RGBA8 pass left"
    assert "in.color_format = frame_format(fg.pq);" in fg, "fsr3fi_frame: FSR 3.1 given the frames' own format"
    for _, _, _, pq_header, var, _ in SHADERS_PQ:
        assert ('#include "shaders/%s"' % pq_header) in fg and var in fg, "vk_framegen.cpp: %s" % pq_header


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--glslang", default=shutil.which("glslangValidator"))
    p.add_argument("--write", action="store_true", help="make the PQ headers from the shaders")
    args = p.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="fg-pq-") as tmp:
        for comp_name, plain, plain_var, pq, pq_var, env in SHADERS_PQ:
            comp = SHADERS / comp_name
            text = comp.read_text(encoding="utf-8")
            assert "#ifdef FG_PQ\nlayout(binding = " in text.replace("\r\n", "\n") and "rgb10_a2) uniform writeonly image2D dst;" in text, \
                "%s: FG_PQ's rgb10_a2 destination" % comp_name
            if args.write:
                if not args.glslang:
                    raise SystemExit("--write needs glslangValidator")
                (SHADERS / pq).write_bytes(source_line(comp, pq_var, env) +
                                           compile_header(args.glslang, comp, Path(tmp) / pq, pq_var, ["-DFG_PQ"], env))
            header = (SHADERS / pq).read_bytes()
            first, _, rest = header.partition(b"\n")
            assert first + b"\n" == source_line(comp, pq_var, env), "%s was not made from %s as it is (--write)" % (pq, comp_name)
            assert image_formats(rest.decode()) == [RGB10A2], "%s: its destination is not RGB10A2" % pq
            assert image_formats((SHADERS / plain).read_text(encoding="utf-8")) == [RGBA8], "%s: its destination is not RGBA8" % plain
            if args.glslang:
                assert compile_header(args.glslang, comp, Path(tmp) / pq, pq_var, ["-DFG_PQ"], env) == rest, \
                    "%s is not glslang's output for %s with FG_PQ" % (pq, comp_name)
                assert compile_header(args.glslang, comp, Path(tmp) / plain, plain_var, [], env) == \
                    (SHADERS / plain).read_bytes(), "%s is not glslang's output for %s as it is" % (plain, comp_name)
    print("fg PQ frames: RGB10A2 frames and made frames for Native HDR's PQ, 10-bit copy and blend, headers %s" %
          ("compiled and compared" if args.glslang else "hashed (no glslang)"))


if __name__ == "__main__":
    main()
