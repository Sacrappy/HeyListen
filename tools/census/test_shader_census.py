"""Tests for the shader census, using a synthetic NUP container only."""

from __future__ import annotations

import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import shader_census as census


BOM = b"\xff\xfe"
GRSC = b"grsc"


def make_bnsh(stages):
    """Build one BNSH with a single variation holding ``stages`` programs."""
    if not stages:
        raise ValueError("at least one stage is required")
    if len(stages) > len(census.BNSH_PROGRAM_SLOTS):
        raise ValueError("too many stages")

    variation_at = 0x100
    program_at = 0x140
    slot_at = 0x190
    cursor = slot_at + 32 * len(stages)
    built = []

    for stage, code in stages:
        control = bytearray(0x60)
        control[:4] = census.CONTROL_MAGIC
        struct.pack_into("<H", control, census.NVN_CONTROL_STAGE_OFFSET, stage)
        control_at = cursor
        code_at = control_at + len(control)
        cursor = code_at + len(code)
        built.append((control_at, code_at, bytes(control), code))

    data = bytearray(cursor)
    data[0:4] = census.BNSH_MAGIC
    data[census.BNSH_BOM_OFFSET : census.BNSH_BOM_OFFSET + 2] = BOM
    data[census.BNSH_GRSC_OFFSET : census.BNSH_GRSC_OFFSET + 4] = GRSC
    struct.pack_into("<I", data, census.BNSH_SIZE_OFFSET, len(data))
    struct.pack_into("<I", data, census.BNSH_VARIATION_COUNT_OFFSET, 1)
    struct.pack_into(
        "<Q", data, census.BNSH_VARIATION_OFFSET_OFFSET, variation_at
    )
    struct.pack_into(
        "<Q",
        data,
        variation_at + census.BNSH_PROGRAM_OFFSET_IN_VARIATION,
        program_at,
    )

    for index, (control_at, code_at, control, code) in enumerate(built):
        this_slot = slot_at + 32 * index
        struct.pack_into(
            "<Q",
            data,
            program_at + census.BNSH_PROGRAM_SLOTS[index],
            this_slot,
        )
        struct.pack_into(
            "<Q", data, this_slot + census.BNSH_SLOT_CONTROL_OFFSET, control_at
        )
        struct.pack_into(
            "<Q", data, this_slot + census.BNSH_SLOT_CODE_OFFSET, code_at
        )
        struct.pack_into(
            "<I", data, this_slot + census.BNSH_SLOT_CODE_SIZE, len(code)
        )
        struct.pack_into(
            "<I", data, this_slot + census.BNSH_SLOT_CONTROL_SIZE, len(control)
        )
        data[control_at : control_at + len(control)] = control
        data[code_at : code_at + len(code)] = code
    return bytes(data)


def make_nup(blob, name="Shader/Test.bfsha", title_id=0x0400486024DEC000):
    """Build a minimal one-title, one-file NUP around ``blob``."""
    title_count = 1
    dir_count = 1
    file_count = 1

    dirs_at = census.NUP_HEADER_SIZE + census.TOC_SIZE + census.TITLE_SIZE
    files_at = dirs_at + census.DIR_SIZE
    children_at = files_at + census.FILE_SIZE
    strings_at = children_at + 4
    encoded_name = name.encode("utf-8")
    strings_size = len(encoded_name)
    data_at = (strings_at + strings_size + 0x1F) & ~0x1F
    total = data_at + len(blob)
    strings_rel = strings_at - census.NUP_HEADER_SIZE

    data = bytearray(total)
    data[0:4] = census.NUP_MAGIC
    struct.pack_into("<Q", data, 0x40, total)

    toc = census.NUP_HEADER_SIZE
    data[toc : toc + 4] = census.TOC_MAGIC
    struct.pack_into(
        "<IQQQQ",
        data,
        toc + 4,
        title_count,
        dir_count,
        file_count,
        strings_rel,
        strings_size,
    )

    struct.pack_into(
        "<QIIII",
        data,
        census.TITLE_OFFSET,
        title_id,
        0,
        0,
        0,
        1,
    )

    struct.pack_into("<4I", data, dirs_at, 0, 0, 0, 1)
    struct.pack_into(
        "<IIQQQ",
        data,
        files_at,
        0,
        len(encoded_name),
        len(blob),
        data_at,
        len(blob),
    )
    data[files_at + 32 : files_at + 64] = hashlib.sha256(blob).digest()
    data[files_at + 64] = 0
    data[files_at + 65] = 0

    struct.pack_into("<I", data, children_at, 1)
    data[strings_at : strings_at + strings_size] = encoded_name
    data[data_at:] = blob
    return bytes(data)


def make_sarc(member):
    data_offset = 0x30
    data = bytearray(data_offset + len(member))
    data[0:4] = b"SARC"
    struct.pack_into("<I", data, 0x08, len(data))
    struct.pack_into("<I", data, 0x0C, data_offset)
    struct.pack_into("<H", data, 0x1A, 1)
    struct.pack_into("<II", data, 0x28, 0, len(member))
    data[data_offset:] = member
    return bytes(data)


def write_nup(tmp_path, blob, name="Shader/Test.bfsha"):
    path = tmp_path / "synthetic.nup"
    path.write_bytes(make_nup(blob, name))
    return str(path)


def test_nup_header_walk_and_read(tmp_path):
    blob = make_bnsh([(0, b"\x01\x02\x03\x04")])
    path = write_nup(tmp_path, blob)

    with census.NupPackage(path) as package:
        assert package.file_size == os.path.getsize(path)
        assert len(package.titles) == 1
        assert package.titles[0].version == 1
        title = package.select_title()[0]
        entries = list(package.walk(title.romfs))
        assert [p for p, _ in entries] == ["Shader/Test.bfsha"]
        assert package.read_file(entries[0][1]) == blob


def test_parse_bnsh_yields_each_stage():
    vertex_code = b"\xaa" * 12
    fragment_code = b"\xbb" * 20
    blob = make_bnsh([(0, vertex_code), (1, fragment_code)])

    programs = list(census.parse_bnsh(blob))
    assert [(p.stage, p.code) for p in programs] == [
        (0, vertex_code),
        (1, fragment_code),
    ]
    assert programs[0].control[:4] == census.CONTROL_MAGIC


def test_census_package_and_csv(tmp_path, capsys):
    vertex_code = b"\xaa" * 12
    fragment_code = b"\xbb" * 20
    blob = make_bnsh([(0, vertex_code), (1, fragment_code)])
    path = write_nup(tmp_path, blob)
    out = tmp_path / "census.csv"

    crawler = census.Census()
    stats = census.census_package(path, crawler)
    assert len(stats) == 1
    assert stats[0].candidates == 1
    assert stats[0].variations == 1
    assert stats[0].programs == 2
    assert len(crawler.programs) == 2

    assert census.main([path, "--csv", str(out)]) == 0
    text = out.read_text(encoding="utf-8").splitlines()
    assert text[0] == ",".join(census.CSV_COLUMNS)
    assert len(text) == 3

    rows = [line.split(",") for line in text[1:]]
    assert {row[1] for row in rows} == {"0", "1"}
    expected_vertex = hashlib.sha256(
        make_control(0) + vertex_code
    ).hexdigest()
    assert {row[0] for row in rows} == {
        expected_vertex,
        hashlib.sha256(make_control(1) + fragment_code).hexdigest(),
    }
    for row in rows:
        assert row[3] == str(int(row[4]) + int(row[5]))
        assert row[6] == "1"

    stdout = capsys.readouterr().out
    assert "Totals by stage:" in stdout
    assert "vertex" in stdout
    assert "fragment" in stdout


def make_control(stage):
    control = bytearray(0x60)
    control[:4] = census.CONTROL_MAGIC
    struct.pack_into("<H", control, census.NVN_CONTROL_STAGE_OFFSET, stage)
    return bytes(control)


def test_shader_path_filter():
    assert census._is_shader_path("Shader/A.bfsha.zs")
    assert census._is_shader_path("Effect/Particles.Nin_Ounce_NVN2.vsetb.byml.zs")
    assert census._is_shader_path("Shader/ApplicationPackage.release.sarc.zs")
    assert census._is_shader_path("Lib/agl/agl_resource.release.sarc.zs")
    assert not census._is_shader_path("Pack/Foo.pack.zs")
    assert not census._is_shader_path("Effect/Foo.bgyml")
    assert not census._is_shader_path("System/Version.txt")


def test_compressed_sarc_layers(tmp_path):
    import zstandard

    blob = make_bnsh([(5, b"\x99" * 8)])
    packed = zstandard.ZstdCompressor().compress(make_sarc(blob))
    path = write_nup(tmp_path, packed, name="Shader/Pack.sarc.zs")

    crawler = census.Census()
    census.census_package(path, crawler)
    assert len(crawler.programs) == 1
    record = next(iter(crawler.programs.values()))
    assert record.stage == 5
    assert record.code_size == 8


def test_parse_bahs_compute():
    code_base = 0x200
    control_at = 0x80
    extent = 16
    data = bytearray(code_base + 0x40 + extent)
    data[0:4] = census.BAHS_MAGIC
    struct.pack_into("<I", data, census.BAHS_CODE_BASE_OFFSET, code_base)
    data[control_at : control_at + 4] = census.CONTROL_MAGIC
    struct.pack_into("<H", data, control_at + census.NVN_CONTROL_STAGE_OFFSET, 5)
    struct.pack_into("<I", data, control_at + census.BAHS_LENGTH_OFFSET, 12)
    struct.pack_into("<II", data, control_at + census.BAHS_EXTENT_OFFSET, 0, extent)
    struct.pack_into(
        "<I", data, control_at - census.BAHS_CODE_OFFSET_FROM_CONTROL, 0x40
    )
    data[code_base + 0x40 :] = b"\x5a" * extent

    programs = list(census.parse_bahs(bytes(data)))
    assert len(programs) == 1
    assert programs[0].stage == 5
    assert programs[0].code == b"\x5a" * extent
    assert len(programs[0].control) == census.BAHS_CONTROL_SIZE


def test_stage_names_cover_task_and_mesh():
    assert census.STAGE_NAMES[6] == "task"
    assert census.STAGE_NAMES[7] == "mesh"
