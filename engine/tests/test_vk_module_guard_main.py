"""The renderer's own vkCreateShaderModule pointer is guarded on Windows: a driver fault while it reads a shader becomes a
failed shader (null module), a log line, a saved .spv and one message, not a silent close (Hookshot b9b48dfd)."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    a = p.parse_args()
    s = (a.source / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    for want in ["guarded_create_shader_module", "__try {", "driver_create_shader_module(d, ci, a, m)", "EXCEPTION_ACCESS_VIOLATION",
                 "heylisten_driver_fault_", "RX_VK_NO_MODULE_GUARD", "*m = VK_NULL_HANDLE;",
                 "vkx::vkCreateShaderModule = guarded_create_shader_module;"]:
        if want not in s:
            raise SystemExit("vk_main.cpp lacks " + want)
    at = s.index("load_device(dev.device);")
    tail = s[at:at + 400]
    if tail.index("guard_shader_modules();") > tail.index("fix_fallback_modules();"):
        raise SystemExit("the module guard must be installed before the fallback fixer chains onto it")
    print("shader module guard in vk_main.cpp: ok")


if __name__ == "__main__":
    main()
