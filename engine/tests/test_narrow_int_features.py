"""The translator emits 8- and 16-bit integers only where the device has them (gpu/vk/vk_shader.cpp make_profile), and the
program cache's stamp keeps programs made without them apart, changed only on such a device (disk_stamp).

A module that declares the Int8 or Int16 capability on a device without shaderInt8 / shaderInt16 is invalid
(VUID-VkShaderModuleCreateInfo-pCode-08740) and the driver need not accept it: the profile said every device had both. With
shaderInt16 masked at device creation the validation layer named Int16 in 159 modules of the title, New Game and its first
play (RX 9070 XT, RADV 26.2.6, VVL 1.4.363). Without them Eden converts through 32 bits. Source checks; no game or GPU required.

python -I -B tests/test_narrow_int_features.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    """From marker to the brace closing its first one"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    src = (ROOT / "gpu" / "vk" / "vk_shader.cpp").read_text(encoding="utf-8")
    profile = block(src, "Shader::Profile make_profile(bool wide)")
    if "p.support_int8 = dev.vk12.shaderInt8 != VK_FALSE;" not in profile:
        raise SystemExit("FAIL: the profile's 8-bit integers do not follow the device's shaderInt8")
    if "p.support_int16 = dev.features.shaderInt16 != VK_FALSE;" not in profile:
        raise SystemExit("FAIL: the profile's 16-bit integers do not follow the device's shaderInt16")
    for line in profile.splitlines():
        code = line.split("//")[0]
        if ("support_int8" in code or "support_int16" in code) and "= true" in code:
            raise SystemExit("FAIL: the profile sets 8- or 16-bit integers on whatever the device has: " + line.strip())
    print("profile: 8- and 16-bit integers from shaderInt8 and shaderInt16")
    stamp = block(src, "uint64_t disk_stamp()")
    at = stamp.find("if (!dev.vk12.shaderInt8 || !dev.features.shaderInt16) {")
    if at < 0:
        raise SystemExit("FAIL: the program cache's stamp does not tell programs made without 8- or 16-bit integers apart")
    added = block(stamp[at:], "if (!dev.vk12.shaderInt8 || !dev.features.shaderInt16) {")
    if "add(&narrow, sizeof narrow);" not in added:
        raise SystemExit("FAIL: the stamp's narrow-integer word is not added inside its condition")
    rest = stamp.replace(added, "")
    if "narrow" in rest:
        raise SystemExit("FAIL: the narrow-integer word is added outside its condition: every GPU's stamp would change")
    print("stamp: changed only on a device without shaderInt8 or shaderInt16")
    print("PASS")


if __name__ == "__main__":
    main()
