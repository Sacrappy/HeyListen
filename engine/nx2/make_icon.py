"""The app icon from a picture: an .ico for the builder and a .res the game's exe links in (nx2/build.py).

    python nx2/make_icon.py <picture.png> [out folder, default work]

The picture is the user's and stays out of the repository, so these go to work/. Needs Pillow.
"""
import io
import os
import struct
import sys

from PIL import Image

SIZES = (16, 24, 32, 48, 64, 128, 256)


def square(im, size):
    """fitted in a transparent square, not stretched"""
    im = im.convert("RGBA")
    k = size / max(im.size)
    fit = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(fit, ((size - fit.width) // 2, (size - fit.height) // 2), fit)
    return out


def png_of(im):
    b = io.BytesIO()
    im.save(b, "PNG", optimize=True)
    return b.getvalue()


def res_entry(kind, name, data, flags=0x1010):
    header = struct.pack("<II", len(data), 32) + struct.pack("<HHHH", 0xFFFF, kind, 0xFFFF, name)
    header += struct.pack("<IHHII", 0, flags, 0x0409, 0, 0)
    return header + data + b"\0" * (-len(data) & 3)


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work")
    im = Image.open(src)
    images = [(s, png_of(square(im, s))) for s in SIZES]

    head = struct.pack("<HHH", 0, 1, len(images))
    offset = len(head) + 16 * len(images)
    entries, body = b"", b""
    group = head
    for i, (s, data) in enumerate(images):
        b = s & 255
        entries += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(data), offset + len(body))
        group += struct.pack("<BBBBHHIH", b, b, 0, 0, 1, 32, len(data), i + 1)
        body += data
    with open(os.path.join(out, "hookshot.ico"), "wb") as f:
        f.write(head + entries + body)

    res = res_entry(0, 0, b"", 0)   # (a .res file starts with an empty entry)
    for i, (s, data) in enumerate(images):
        res += res_entry(3, i + 1, data)   # RT_ICON
    res += res_entry(14, 1, group, 0x1030)   # RT_GROUP_ICON: resource 1, what the window class loads
    with open(os.path.join(out, "hookshot.res"), "wb") as f:
        f.write(res)
    print("make_icon: %d sizes, %d KB" % (len(images), len(res) >> 10))


if __name__ == "__main__":
    main()
