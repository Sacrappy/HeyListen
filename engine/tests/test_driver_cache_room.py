"""The drivers' shader caches get room for what the preparation builds (host/hos_main.c driver_cache_room).

The pipelines the notes predict are built into the driver's own disk cache (vk_draw.cpp predict_run, driver_cache_only). NVIDIA's
and Mesa's caches hold 1 GiB by default and clean themselves down to it; a full preparation filled NVIDIA's to the limit and the
pipelines dropped were compiled again in play. The Linux main raises both limits before anything loads Vulkan (the window, SDL,
comes after), and keeps a value the player set (setenv's last argument 0). This checks the source.
python -I -B tests/test_driver_cache_room.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    fn = src[src.index("static void driver_cache_room(void) {"):]
    fn = fn[:fn.index("\n}\n")]
    for name in ("__GL_SHADER_DISK_CACHE_SIZE", "MESA_SHADER_CACHE_MAX_SIZE"):
        m = re.search(r'setenv\("%s", "([^"]+)", (\d)\)' % name, fn)
        if not m:
            raise SystemExit("driver_cache_room does not set " + name)
        if m.group(2) != "0":
            raise SystemExit(name + " is set over what the player chose")
    size = int(re.search(r'setenv\("__GL_SHADER_DISK_CACHE_SIZE", "(\d+)"', fn).group(1))
    if size < 4 << 30:
        raise SystemExit("NVIDIA's cache limit is under 4 GiB: a full preparation (predicted pipelines and all) does not fit")
    main_fn = src[src.rindex("int main(int argc, char **argv) {"):]
    if "driver_cache_room();" not in main_fn:
        raise SystemExit("the Linux main does not call driver_cache_room")
    if main_fn.index("driver_cache_room();") > main_fn.index("hos_display_init()"):
        raise SystemExit("driver_cache_room runs after the window (SDL) may have loaded Vulkan")
    print("PASS: the drivers' shader caches get room before Vulkan loads")


if __name__ == "__main__":
    main()
