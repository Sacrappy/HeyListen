"""A host pointer import takes its memory type from its own pointer and the buffer's requirements (Longshot a48592e3):
vkGetMemoryHostPointerPropertiesEXT is asked for each pointer (no type kept from the first), the type is one the buffer allows
too, and a buffer larger than the imported bytes is refused."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    a = p.parse_args()
    s = (a.source / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    start = s.index("bool import_pool(")
    body = s[start:s.index("\n} // namespace", start)]
    if "static uint32_t type" in body:
        raise SystemExit("import_pool keeps the first pointer's memory type for every pointer")
    for want in ["VkMemoryHostPointerPropertiesEXT hp{", "vkGetBufferMemoryRequirements(dev.device, p.buffer, &req);",
                 "hp.memoryTypeBits & req.memoryTypeBits", "req.size > bytes", "destroy_import_buffer(p.buffer);"]:
        if want not in body:
            raise SystemExit("import_pool lacks " + want)
    if body.index("vkGetBufferMemoryRequirements") < body.index("vkCreateBuffer(") or \
            body.index("vkGetBufferMemoryRequirements") > body.index("vkAllocateMemory("):
        raise SystemExit("the buffer's requirements are not asked between its creation and the allocation")
    print("host pointer import: per-pointer memory type, buffer requirements checked: ok")


if __name__ == "__main__":
    main()
