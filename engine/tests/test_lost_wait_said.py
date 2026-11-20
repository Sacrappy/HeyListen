"""A capture's timeline wait that comes back VK_ERROR_DEVICE_LOST is said as a lost device, not as a wait that timed out
(gpu/vk/vk_replay.cpp vk_timeline_wait, wait_lost). Ported in spirit from Longshot d61aa82b (#670): HeyListen's replay has the one wait,
so the check reads its source. No GPU or game."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if "void wait_lost(const char* what, uint64_t v) {" not in src:
        raise SystemExit("vk_replay.cpp: wait_lost is missing")
    at = src.index("bool vk_timeline_wait_for(uint64_t v, uint32_t timeout_ms) {")
    body = src[at:at + 2500]
    for need in ("const VkResult wres = vkWaitSemaphores(dev.device, &wi, (uint64_t)timeout_ms * 1000000ull);",
                 "wres == VK_TIMEOUT", "wres == VK_ERROR_DEVICE_LOST", 'wait_lost("a capture\'s wait for write-back", v);'):
        if need not in body:
            raise SystemExit("vk_timeline_wait_for lacks: " + need)
    if "(void)vk_timeline_wait_for(v, 250);" not in src:
        raise SystemExit("vk_timeline_wait does not keep its 250 ms bound")
    if "came back VK_ERROR_DEVICE_LOST: the GPU device is lost" not in src:
        raise SystemExit("the lost-device line is missing")
    print("a timeline wait that is device-lost is said as a lost device")


if __name__ == "__main__":
    main()
