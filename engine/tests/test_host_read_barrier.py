"""Every replay command buffer ends with a memory dependency into the host's reads: the game reads what the GPU wrote into
its memory once the timeline reaches the submission, and a host wait's access scope is device access only."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
	start = source.index(marker)
	at = source.index("{", start) + 1
	depth = 1
	while depth:
		depth += (source[at] == "{") - (source[at] == "}")
		at += 1
	return source[start:at]


def main():
	p = argparse.ArgumentParser(description=__doc__)
	p.add_argument("--source", type=Path, default=ROOT)
	a = p.parse_args()
	source = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
	if "void host_read_barrier(" not in source:
		raise SystemExit("no memory dependency into the host's reads at a recording's end: a host wait leaves GPU writes unseen")
	bar = block(source, "void host_read_barrier(")
	for want in ["VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT", "VK_ACCESS_2_MEMORY_WRITE_BIT",
				 "b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT", "b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT",
				 "vkCmdPipelineBarrier2(cmd, &d);"]:
		if want not in bar:
			raise SystemExit("host_read_barrier lacks " + want)
	sub = block(source, "void submit_recording(")
	at = sub.index("vkEndCommandBuffer(r.cmd);")
	if "host_read_barrier(r.cmd);" not in sub[:at]:
		raise SystemExit("submit_recording ends r.cmd without host_read_barrier")
	seg = source.index("record_segment(c, j->g);")
	end = source.index("vkEndCommandBuffer(j->r->cmd);", seg)
	if "host_read_barrier(c);" not in source[seg:end]:
		raise SystemExit("a parallel segment's command buffer ends without host_read_barrier")
	print("replay command buffers end with a dependency into host reads: ok")


if __name__ == "__main__":
	main()
