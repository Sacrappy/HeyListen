"""A 5.1 mix also puts the centre in the front pair, so voices survive a device that drops the centre (host/nn_audio.c,
issue 82).

The real au_spread_centre and au_centre_spread_on from host/nn_audio.c are compiled with a small driver: in a six-channel
mix the centre goes to front left and right at -3 dB as well and every other channel is untouched; a stereo mix is never
changed; and HOS_AUDIO_CENTRE=0 turns the spread off. No device is opened.

python -I -B tests/test_audio_centre_spread.py [--cxx clang++]   (--cxx cl: MSVC, from a developer prompt)
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define FRAME 240
HELPER
static void set_env(const char *v) {
#ifdef _WIN32
    _putenv_s("HOS_AUDIO_CENTRE", v ? v : "");
#else
    if (v) setenv("HOS_AUDIO_CENTRE", v, 1);
    else unsetenv("HOS_AUDIO_CENTRE");
#endif
}
static int failed;
static void check(float got, float want, const char *what) {
    if (fabsf(got - want) > 1e-5f) {
        fprintf(stderr, "FAIL: %s: %f (want %f)\n", what, got, want);
        failed = 1;
    }
}
int main() {
    float out[6][FRAME], ref[6][FRAME];
    for (int ch = 0; ch < 6; ch++)
        for (int i = 0; i < FRAME; i++) out[ch][i] = ref[ch][i] = (float)(ch + 1) * 0.01f + (float)i * 0.001f;

    memcpy(out, ref, sizeof out);
    au_spread_centre(out, 0, 1);   /* (stereo: nothing to spread) */
    for (int ch = 0; ch < 6; ch++)
        for (int i = 0; i < FRAME; i++) check(out[ch][i], ref[ch][i], "stereo mix changed");

    memcpy(out, ref, sizeof out);
    au_spread_centre(out, 1, 0);   /* (HOS_AUDIO_CENTRE=0) */
    for (int ch = 0; ch < 6; ch++)
        for (int i = 0; i < FRAME; i++) check(out[ch][i], ref[ch][i], "spread off changed the mix");

    memcpy(out, ref, sizeof out);
    au_spread_centre(out, 1, 1);
    for (int i = 0; i < FRAME; i++) {
        check(out[0][i], ref[0][i] + 0.7071f * ref[2][i], "front left misses the phantom centre");
        check(out[1][i], ref[1][i] + 0.7071f * ref[2][i], "front right misses the phantom centre");
        for (int ch = 2; ch < 6; ch++) check(out[ch][i], ref[ch][i], "a channel past the front pair changed");
    }

    set_env(nullptr);
    if (!au_centre_spread_on()) { fprintf(stderr, "FAIL: unset HOS_AUDIO_CENTRE is off\n"); failed = 1; }
    set_env("");
    if (!au_centre_spread_on()) { fprintf(stderr, "FAIL: empty HOS_AUDIO_CENTRE is off\n"); failed = 1; }
    set_env("1");
    if (!au_centre_spread_on()) { fprintf(stderr, "FAIL: HOS_AUDIO_CENTRE=1 is off\n"); failed = 1; }
    set_env("0");
    if (au_centre_spread_on()) { fprintf(stderr, "FAIL: HOS_AUDIO_CENTRE=0 is on\n"); failed = 1; }
    return failed;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    audio = (ROOT / "host/nn_audio.c").read_text(encoding="utf-8")
    i = audio.index("/* (hookshot) Issue 82: the centre also goes")
    helper = audio[i:audio.index("/* The device:", i)]
    assert "void au_spread_centre(" in helper and "HOS_AUDIO_CENTRE" in helper, \
        "host/nn_audio.c has no centre spread"
    # (the mix spreads the centre after the sink's channel map and before the limiter: a dropped centre is still played,
    #  and the added signal is limited with the rest)
    frame = audio[audio.index("static void render_frame(int16_t *pcm, int channels) {"):]
    frame = frame[:frame.index("\n}")]
    assert "au_spread_centre(out, six, centre_spread)" in frame, "render_frame does not spread the centre"
    assert frame.index("if (sink_mix >= 0") < frame.index("au_spread_centre(out, six") < frame.index("if (lim_enabled)"), \
        "render_frame spreads the centre before the sink map or after the limiter"
    # (the log says what the 5.1 output does, so a player's log shows it)
    opened = audio[audio.index("static int output_try("):]
    opened = opened[:opened.index("\n}")]
    assert "the centre also to the front pair" in opened, "output_try does not log the centre spread"
    # (the switch is written where a player looks for one)
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    assert "HOS_AUDIO_CENTRE=0" in readme, "readme.md does not describe HOS_AUDIO_CENTRE=0"

    with tempfile.TemporaryDirectory(prefix="audio-centre-spread-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.cpp", work / "fixture.exe"
        source.write_text(FIXTURE.replace("HELPER", helper), encoding="utf-8")
        if os.path.splitext(os.path.basename(args.cxx))[0].lower() == "cl":
            command = [args.cxx, "/nologo", "/std:c++17", "/W4", "/WX", "/EHsc", str(source), "/Fe:" + str(exe),
                       "/Fo:" + str(work) + os.sep]
        else:
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(exe)]
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: a 5.1 mix spreads its centre to the front pair at -3 dB; stereo is untouched and HOS_AUDIO_CENTRE=0 "
          "turns it off")


if __name__ == "__main__":
    main()
