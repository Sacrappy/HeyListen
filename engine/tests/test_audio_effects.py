"""Compile the actual effect code (standard reverb, delay, mix biquad filter, splitter destination biquads) from
host/nn_audio.c around authored samples and parameter packets. No game, SDK or output device is used."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("audio_block_helper", ROOT / "tests/test_audio_deadline.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    source = (ROOT / "host/nn_audio.c").read_text(encoding="utf-8")
    constants = []
    for name in ("FRAME", "RATE", "AU_MAGIC", "MAX_VOICES", "MAX_MIXES", "MAX_SPLITTERS", "MAX_SPLIT_DESTS",
                 "MAX_CH", "MAX_BUF", "MAX_WB", "MAX_REL"):
        matches = re.findall(r"^#define " + name + r"\s+[^\n]+", source, re.M)
        assert len(matches) == 1, name
        constants.extend(matches)
    types = source[source.index("enum { K_VOICE"):source.index("static CRITICAL_SECTION lock;")]
    effects = source[source.index("/* ---- I3DL2 reverb (on a sub mix"):source.index("/* ---- the mixer")]
    queue = source[source.index("enum { OP_VOICE_RELEASE"):source.index("static int64_t last_update_ns;")]
    setters = source[source.index("/* ---- the other effects"):source.index("/* aux effects pass through")]
    actual = ["\n".join(constants), types, """
static Voice voices[MAX_VOICES];
static Mix mixes[MAX_MIXES];
static Splitter splitters[MAX_SPLITTERS];
static int sink_mix = -1;
static void release_wb(Voice *voice, uint64_t buffer) { (void)voice; (void)buffer; }
static float fixture_src[MAX_CH];   /* the authored source: every source frame holds these */
static int next_source_frame(Voice *v) { memcpy(v->cur, fixture_src, sizeof v->cur); return 1; }
""", effects]
    actual.extend(helper.block(source, marker) for marker in (
        "static void put_handle(", "static int get_handle(", "static float farg(", "static int voice_index(",
        "static int mix_index(", "static int splitter_index(", "static Splitter *splitter_of(",
        "static void ret_float(", "static Reverb *reverb_of("))
    actual.extend([queue, helper.block(source, "static void apply("), helper.block(source, "static void submit("),
                   helper.block(source, "static void commit("), helper.block(source, "static void set_split_biquad("),
                   helper.block(source, "static void render_voice("), setters])
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="audio-effects-") as temporary:
        folder = Path(temporary)
        (folder / "actual_audio_effects.inc").write_text("\n\n".join(actual), encoding="utf-8")
        executable = folder / "fixture.exe"
        command = [args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   "-I" + str(folder), str(ROOT / "tests/audio_effects_test.c"), "-lm", "-o", str(executable)]
        for command in (command, [str(executable)]):
            result = subprocess.run(command, creationflags=flags, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
