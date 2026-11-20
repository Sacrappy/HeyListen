"""The precompile's stall rule fires whether or not every pipeline was handed out, and then the workers take no new one
(Hookshot 4ae22a7a: every worker inside AMD's driver on a pipeline it never returned from held the start for good)."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    a = p.parse_args()
    s = (a.source / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    if "else if (stall_s > 0 && pre->next.load() >= total" in s:
        raise SystemExit("the stall rule waits for every pipeline to be handed out")
    at = s.index("else if (stall_s > 0 && now - last_change")
    if "pre->abandoned = true;" not in s[at:at + 600]:
        raise SystemExit("the stall rule does not abandon the pipelines not yet handed out")
    if "pre->abandoned.load(" not in s:
        raise SystemExit("the workers do not stop taking pipelines once abandoned")
    print("precompile stall rule: ok")


if __name__ == "__main__":
    main()
