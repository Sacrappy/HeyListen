"""AMD: a graphics or compute pipeline the driver's compiler fails on is built once more with DISABLE_OPTIMIZATION_BIT
(Hookshot f1eb23c9), never for the memory errors nor the cold ask's COMPILE_REQUIRED, and RX_VK_NO_UNOPT_RETRY turns it off."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    a = p.parse_args()
    s = (a.source / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    at = s.index("static bool retry_without_optimizer(")
    body = s[at:s.index("}", at)]
    for want in ["0x1002", "VK_ERROR_OUT_OF_HOST_MEMORY", "VK_ERROR_OUT_OF_DEVICE_MEMORY", "VK_PIPELINE_COMPILE_REQUIRED",
                 "VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT", "RX_VK_NO_UNOPT_RETRY"]:
        if want not in s[at - 200:at + 900]:
            raise SystemExit("retry_without_optimizer lacks " + want)
    if s.count("guarded_compute_once(") < 4:
        raise SystemExit("guarded_compute does not retry through guarded_compute_once")
    if "if (retry_without_optimizer(r, ci.flags))" not in s:
        raise SystemExit("build_graphics does not retry a failed pipeline without the optimizer")
    print("pipeline retry without the optimizer: ok")


if __name__ == "__main__":
    main()
