"""(TICK-054) The capture pre-copy thread kicked once a frame (RX_VK_PRECOPY_KICK=1 with RX_VK_CAPTURE_PRECOPY=1, and
HOS_PRECOPY_KICK=1 on the host): the thread waits on the kick event instead of its timer; the renderer exports
mkwvk_precopy_kick; the host takes it as an optional export and calls it when the main thread leaves its end-of-frame wait
(lr 0x83acfcf4, OoT's f_83acfb90), after the wait, only with the switch.

python -I -B tests/test_precopy_kick.py
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class PrecopyKickTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mem = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
        cls.rx = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")
        cls.sync = (ROOT / "host" / "nn_os_sync.c").read_text(encoding="utf-8")

    def test_thread_waits_on_the_kick_only_with_the_switch(self):
        main = block(self.mem, "void precopy_main() {")
        self.assertIn("if (precopy_kick_on() && precopy_kick_event) WaitForSingleObject(precopy_kick_event, INFINITE);", main)
        self.assertIn("else if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);", main)
        on = block(self.mem, "bool precopy_kick_on() {")
        self.assertIn('getenv("RX_VK_PRECOPY_KICK")', on)
        # the event is made before the thread starts, with the switch
        self.assertIn("if (precopy_kick_on()) precopy_kick_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);\n        std::thread(precopy_main).detach();", self.mem)

    def test_export_sets_the_event(self):
        kick = block(self.mem, "void mkwvk_precopy_kick(void) {")
        self.assertIn("if (precopy_kick_event) SetEvent(precopy_kick_event)", kick)
        self.assertIn("__declspec(dllexport)", self.mem[self.mem.index("mkwvk_precopy_kick") - 200:self.mem.index("mkwvk_precopy_kick")])

    def test_host_takes_the_optional_export(self):
        self.assertIn('GetProcAddress(m, "mkwvk_precopy_kick")', self.rx)
        self.assertIn("void rx_precopy_kick(void) { if (vk_precopy_kick) vk_precopy_kick(); }", self.rx)

    def test_kick_after_the_end_of_frame_wait_with_the_switch(self):
        wait = block(self.sync, "HLE(_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE) {")
        self.assertIn("if (c->x[30] == 0x83acfcf4ULL && precopy_kick_lr()) { void rx_precopy_kick(void); rx_precopy_kick(); }", wait)
        self.assertLess(wait.index('logged_wait(c, obj(ARG(0), K_EVENT), 0, 0, "light");'), wait.index("rx_precopy_kick();"))
        on = block(self.sync, "static int precopy_kick_lr(void) {")
        self.assertIn('getenv("HOS_PRECOPY_KICK")', on)


if __name__ == "__main__":
    unittest.main()
