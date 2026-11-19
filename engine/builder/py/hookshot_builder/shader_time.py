"""The builder's note under the bar while the game prepares its shaders (the build's last step).

The note said "Usually about 45 minutes" for the whole build whatever happened: a PC whose graphics card has no mesh shaders, or a
slow or small CPU, took twice that in the shaders' step alone (112 minutes at 97%, Discord 11-18) and the note never changed. The
game's @@progress lines (gpu/vk/vk_main.cpp: the programs by what they cost, then the pipelines) say how far the step is, so the
time left comes from the time it has taken so far, as the compile's does. The same text is in the WPF builder (ShaderStep.cs Note).
"""

MIN_SECONDS = 120   # (the first minutes are the game opening the shader list and the driver warming up: no estimate yet)
MIN_FRACTION = 0.03


def clock(secs):
    secs = int(secs)
    return "%d:%02d" % (secs // 60, secs % 60)


def left_text(seconds):
    """'About 12 minutes left' (as window.py's time_left, which rounds the same)"""
    if seconds < 45:
        return "Less than a minute left"
    if seconds < 90:
        return "About a minute left"
    m = int(round(seconds / 60))
    return "About %d h %d min left" % (m // 60, m % 60) if m >= 60 else "About %d minutes left" % m


def shaders_note(elapsed, fraction, tail=""):
    """The note while the shaders are prepared: the time taken, and the time left once the bar says enough to go by.
    elapsed: seconds since the step began; fraction: the step's @@progress (0..1) or None when none came yet"""
    head = "Preparing the shaders %s" % clock(elapsed)
    if fraction is None or fraction < MIN_FRACTION or elapsed < MIN_SECONDS:
        return "%s · working out how long this takes. It is 10 to 40 minutes on most PCs, more with fewer CPU cores or a graphics card without mesh shaders%s" % (head, tail)
    fraction = min(fraction, 0.999)
    left = elapsed * (1.0 - fraction) / fraction
    return "%s · %s%s" % (head, left_text(left), tail)
