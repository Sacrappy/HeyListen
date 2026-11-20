# Copyright (C) 2026 Sacrappy

"""What the shader caches' translator stamp is made of (nx2/translator_files.py, HOOKSHOT_SHADER_STAMP of nx2/build.py), run
on tiny authored files without building the renderer: a translator edit changes it; an edit to a file the translator does not
read (another renderer source, vk_api.h's host interface and menu ids) keeps it, so every player's translated programs are kept.

python -I -B tests/test_shader_stamp_inputs.py
"""
import importlib.util
from pathlib import Path
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("translator_files_under_test", ROOT / "nx2" / "translator_files.py")
files = importlib.util.module_from_spec(spec)
spec.loader.exec_module(files)

build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
assert re.search(r"^import translator_files$", build, re.M), "nx2/build.py no longer imports translator_files"
assert 'shader_stamp = "-DHOOKSHOT_SHADER_STAMP=0x%sull" % translator_files.stamp(vk)' in build, \
    "nx2/build.py's HOOKSHOT_SHADER_STAMP is no longer translator_files.stamp(vk): update this check"
assert files.DIRS == ("eden", "sirit", "compat") and files.FILES == ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h"), \
    "the stamped files changed: say in nx2/translator_files.py why each one is in or out, then update this check"

with tempfile.TemporaryDirectory(prefix="shader-stamp-bank-") as tmp:
    vk = Path(tmp) / "gpu/vk"
    for rel in ("eden/a.cpp", "sirit/a.cpp", "compat/a.h", "vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h", "vk_draw.cpp"):
        f = vk / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b"authored input\n")
    stamp = lambda: files.stamp(str(vk))
    first = stamp()
    assert re.fullmatch(r"[0-9a-f]{16}", first)
    (vk / "vk_draw.cpp").write_bytes(b"unrelated renderer edit\n")
    assert stamp() == first, "unrelated renderer source entered shader stamp"
    (vk / "vk_api.h").write_bytes(b"enum { HL_OPT_AUDIO_OUTPUT = 57 };\n")
    assert stamp() == first, "a vk_api.h edit (the host interface, the menu's option ids) changed the shader stamp"
    changed = first
    (vk / "vk_bank.cpp").write_bytes(b"changed bank semantics\n")
    assert stamp() != changed, "bank-only edit did not invalidate shader stamp"
    changed = stamp()
    (vk / "vk_bank.cpp").write_bytes(b"changed bank semantics\r\n")
    assert stamp() == changed, "existing CRLF normalization changed"
    for rel in ("vk_shader.cpp", "vk_state.h", "eden/a.cpp", "sirit/a.cpp", "compat/a.h"):
        (vk / rel).write_bytes(b"changed\n")
        assert stamp() != changed, "a %s edit kept the shader stamp" % rel
        changed = stamp()
    (vk / "eden/b.h").write_bytes(b"")
    assert stamp() != changed, "a file added to the recompiler kept the shader stamp"
    (vk / "eden/b.h").unlink()
    assert stamp() == changed

# the pipeline notes' state stamp (HOOKSHOT_STATE_STAMP): the same rule, vk_api.h out, PipeNote's headers in
assert 'translator_files.state_stamp(GPU)' in build, "nx2/build.py's HOOKSHOT_STATE_STAMP is no longer translator_files.state_stamp(GPU)"
assert files.STATE_FILES == ("gpu/vk/vk_state.h", "gpu/vk/vk_draw.h", "gpu/render/rx_nvn.h"), \
    "the state stamp's files changed: say in nx2/translator_files.py why each one is in or out, then update this check"
with tempfile.TemporaryDirectory(prefix="state-stamp-") as tmp:
    gpu = Path(tmp)
    for rel in ("vk/vk_state.h", "vk/vk_draw.h", "render/rx_nvn.h", "vk/vk_api.h", "vk/vk_draw.cpp"):
        f = gpu / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b"authored input\n")
    state = lambda: files.state_stamp(str(gpu))
    first = state()
    assert re.fullmatch(r"[0-9a-f]{16}", first)
    (gpu / "vk/vk_api.h").write_bytes(b"enum { HOS_OPT_AUDIO_OUTPUT = 57 };\n")
    (gpu / "vk/vk_draw.cpp").write_bytes(b"unrelated\n")
    assert state() == first, "a vk_api.h edit changed the state stamp"
    for rel in ("vk/vk_state.h", "vk/vk_draw.h", "render/rx_nvn.h"):
        before = state()
        (gpu / rel).write_bytes(b"changed\r\n")
        assert state() != before, "a %s edit kept the state stamp" % rel
        after = state()
        (gpu / rel).write_bytes(b"changed\n")
        assert state() == after, "CRLF normalization of the state stamp changed"

# program_cache_stamp() (vk_shader.cpp disk_stamp) holds the compiled-in translator stamp and run-time facts, and nothing of vk_api.h
shader = (ROOT / "gpu" / "vk" / "vk_shader.cpp").read_text(encoding="utf-8")
start = shader.index("uint64_t disk_stamp() {")
body = shader[start:shader.index("return h;", start)]
assert "HOOKSHOT_SHADER_STAMP" in body, "disk_stamp no longer holds HOOKSHOT_SHADER_STAMP"
for name in ("MKWVK_API_VERSION", "HOS_OPT", "HOOKSHOT_STATE_STAMP", "kStateStamp"):
    assert name not in body, "program_cache_stamp() takes %s: a vk_api.h edit would move it" % name
print("PASS: the shader stamp changes for the translator's sources and keeps the caches for vk_api.h and other renderer sources")
