"""recomplauncher.json as a release carries it: the repository's file with prepare.version set to the release's tag.

RecompLauncher builds the game again (the prepare step) only when prepare.version changes, and the game a release builds is that
release's engine. With the repository's fixed "1", a player who set up once kept playing the game the first release built while
the launcher showed the newer release. So each release's zip and AppImage carry their own tag there (the repository's file keeps
"1": it is not a release). The rest of the file is unchanged.

    python3 -I tools/release/launcher_manifest.py <recomplauncher.json> <tag> <out>

make_launcher_zip.py (the Windows zip) and engine/scripts/linux/appimage.sh (the AppImage) both write it through here, and
check_release_linux.py compares the AppImage's copy with it.
"""
import json
import sys


def for_release(manifest_bytes, tag):
    """The bytes a release carries: the same JSON, prepare.version the tag"""
    doc = json.loads(manifest_bytes.decode("utf-8"))
    doc["prepare"]["version"] = tag
    text = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
    return text.encode("utf-8")


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: python3 -I tools/release/launcher_manifest.py <recomplauncher.json> <tag> <out>")
    source, tag, out = sys.argv[1:]
    with open(source, "rb") as f:
        stamped = for_release(f.read(), tag)
    with open(out, "wb") as f:
        f.write(stamped)
    return 0


if __name__ == "__main__":
    sys.exit(main())
