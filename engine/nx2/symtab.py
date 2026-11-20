"""A linker map -> a compact symbol table for the crash handler (HeyListen.sym, hookshot_vk.sym).

The game is compiled on each player's PC, so a report's `HeyListen.exe+0x12f44593` means a different function in
every build. The map the linker writes (build.py: /map: or -Wl,-Map=) names every function: the lifted code as
`f_<guest address>` (the same on every PC), the host's functions by their C/C++ names. This script keeps those
names, sorts them by RVA and writes a small binary the game loads at start-up, so a crash's frames can be named
on the player's own PC without reading (or shipping) the 85 MB map.

    python -I nx2/symtab.py <map> <binary> <out.sym> [--cxxfilt <llvm-cxxfilt>]

The table is:
    0   char[8]  "HLSYM\\0\\0\\1"
    8   u32      entries
    12  u32      name bytes
    16  u64      identity of the binary the names are for (PE fields; 0: not checked)
    24  u64      the map's preferred load address
    32  entry[entries]  { u32 rva, u32 name offset }  ascending by rva
    ..  char[name bytes]  NUL-terminated names

Only names the game can print are kept: no `<absolute>`, no local labels (`.L...`), no section names, one name
per address (the lifted `f_...` first, then the shortest), mangled C++ demangled without parameters
(llvm-cxxfilt -n) when that tool is at hand. Standard library only.
"""
import os
import re
import shutil
import struct
import subprocess
import sys

MAGIC = b"HLSYM\x00\x00\x01"
HEADER = struct.Struct("<8sIIQQ")
ENTRY = struct.Struct("<II")
MAX_NAME = 200
MAX_TABLE = 64 << 20

SECTION = re.compile(r"^\s+([0-9a-f]{4}):([0-9a-f]{8})\s+([0-9a-f]+)H?\s+(\S+)\s+(CODE|DATA|BSS|CONST|OVERLAY|DEFSECT)\s*$")
SYMBOL = re.compile(r"^\s+([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{16})\s+(\S+)\s*$")
PREFERRED = re.compile(r"Preferred load address is ([0-9a-fA-F]+)")


def _u16(data, at):
    return struct.unpack_from("<H", data, at)[0]


def _u32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def pe_identity(binary):
    """The fields a stale .sym would not share with this binary (TimeDateStamp, SizeOfImage, CheckSum); 0 when
    the binary is not a PE (Linux: the table is trusted as built beside the game)"""
    try:
        with open(binary, "rb") as f:
            head = f.read(4096)
    except OSError:
        return 0
    if len(head) < 0x40 or head[:2] != b"MZ":
        return 0
    at = _u32(head, 0x3C)
    if at + 92 > len(head) or head[at:at + 4] != b"PE\0\0":
        return 0
    timestamp = _u32(head, at + 8)
    size_of_image = _u32(head, at + 80)
    checksum = _u32(head, at + 88)
    value = ((timestamp << 32) | size_of_image) ^ (checksum * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    return value or 1


def find_cxxfilt(cc=None):
    """llvm-cxxfilt beside the compiler, on PATH, or LLVM_CXXFILT"""
    for candidate in (os.environ.get("LLVM_CXXFILT"),
                      os.path.join(os.path.dirname(cc), "llvm-cxxfilt.exe") if cc else None,
                      os.path.join(os.path.dirname(cc), "llvm-cxxfilt") if cc else None,
                      shutil.which("llvm-cxxfilt"), shutil.which("llvm-cxxfilt.exe"),
                      shutil.which("c++filt")):
        if candidate and os.path.isfile(candidate):
            return candidate
    return None


def _demangle(names, tool):
    """{mangled: demangled without parameters}; names the tool does not know stay as they are"""
    mangled = sorted({n for n in names if n.startswith("_Z")})
    if not tool or not mangled:
        return {}
    try:
        proc = subprocess.run([tool, "-n"], input="\n".join(mangled) + "\n", capture_output=True,
                              text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return {}
    out = proc.stdout.splitlines()
    if len(out) != len(mangled):
        return {}
    return {a: b for a, b in zip(mangled, out) if b and b != a and not any(ch.isspace() for ch in b)}


def _readable(name):
    """A name the report can hold as one token: printable ASCII, no space (spaces become _)"""
    out = []
    for ch in name[:MAX_NAME]:
        if ch.isspace():
            out.append("_")
        elif 0x21 <= ord(ch) < 0x7F:
            out.append(ch)
    return "".join(out)


def _score(name):
    """The name to keep where several share an address: a lifted function first, then a stable C/C++ name"""
    return (0 if name.startswith("f_") else 1, 0 if "." not in name else 1,
            0 if "$" not in name else 1, 1 if name.startswith(".") else 0, len(name))


def read_map(path, cxxfilt=None):
    """The map's function names as {rva: name}, and its preferred load address"""
    preferred = 0x140000000
    found_preferred = False
    best = {}
    with open(path, errors="replace") as f:
        for line in f:
            if not found_preferred:
                m = PREFERRED.search(line)
                if m:
                    preferred = int(m.group(1), 16)
                    found_preferred = True
                    continue
            if SECTION.match(line):
                continue
            m = SYMBOL.match(line)
            if not m:
                continue
            name, address, obj = m.group(3), int(m.group(4), 16), m.group(5)
            if obj == "<absolute>" or name.startswith(".") or len(name) <= 1:
                continue
            rva = address - preferred
            if rva <= 0:
                continue
            name = _readable(name)
            if not name:
                continue
            old = best.get(rva)
            if old is None or _score(name) < _score(old):
                best[rva] = name
    names = _demangle(set(best.values()), cxxfilt)
    for rva, name in best.items():
        best[rva] = names.get(name, name)
    return best, preferred


def write_table(path, functions, preferred, identity):
    """Writes the binary table; returns the bytes written (0, and no file, when the map gave no names: an ELF map
    another linker wrote, or a link that named nothing)"""
    rvas = sorted(functions)
    if not rvas:
        try:
            os.remove(path)
        except OSError:
            pass
        return 0
    pool = bytearray()
    offsets = {}
    for rva in rvas:
        name = functions[rva]
        offsets[rva] = len(pool)
        pool += name.encode("ascii", "replace") + b"\0"
    if len(pool) > MAX_TABLE or len(rvas) > MAX_TABLE // ENTRY.size:
        raise ValueError("the symbol table is too large: %d names, %d bytes" % (len(rvas), len(pool)))
    body = b"".join(ENTRY.pack(rva & 0xFFFFFFFF, offsets[rva]) for rva in rvas)
    with open(path, "wb") as f:
        f.write(HEADER.pack(MAGIC, len(rvas), len(pool), identity & 0xFFFFFFFFFFFFFFFF,
                            preferred & 0xFFFFFFFFFFFFFFFF))
        f.write(body)
        f.write(pool)
    return HEADER.size + len(body) + len(pool)


def write_map(map_path, binary_path, out_path, cxxfilt=None):
    """The one call build.py makes after a link: <map> and <binary> -> <out_path>"""
    functions, preferred = read_map(map_path, cxxfilt)
    identity = pe_identity(binary_path)
    return write_table(out_path, functions, preferred, identity)


def main():
    import argparse
    p = argparse.ArgumentParser(description="a linker map -> the crash handler's symbol table")
    p.add_argument("map")
    p.add_argument("binary")
    p.add_argument("out")
    p.add_argument("--cxxfilt", default=None)
    p.add_argument("--no-demangle", action="store_true", help="keep C++ names mangled (tests: no tool needed)")
    a = p.parse_args()
    size = write_map(a.map, a.binary, a.out, None if a.no_demangle else (a.cxxfilt or find_cxxfilt()))
    print("%s: %d bytes" % (a.out, size))


if __name__ == "__main__":
    main()
