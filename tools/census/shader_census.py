"""Census of the NVN shader programs stored inside a Switch 2 .nup package.

The census reads the package index, walks the romfs tree in place, and only
loads the files that can carry shader programs (Shader/, Layout2Shader/,
Effect/ and Lib/).  Nothing is extracted and no game data is
written next to the source tree: the only output is a CSV file, which by
default goes to the ignored ``out/`` directory.

Each row describes one distinct shader-stage program, identified by the
SHA-256 of its NVN control block and machine code.  ``count`` says how many
times that program was referenced by a variation (or a BAHS compute entry).

Usage::

    python tools/census/shader_census.py package.nup [package2.nup] \\
        [--csv out/shader_census.csv] [--all-titles]

The format facts used here (NUP table layout, BNSH variation records, NVN
control magic) are read-only observations from the reference tree; the code
is original to HeyListen.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import os
import struct
import sys
from dataclasses import dataclass, field


NUP_MAGIC = b"NUP\x01"
TOC_MAGIC = b"TOC\x01"
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
SARC_MAGIC = b"SARC"
BNSH_MAGIC = b"BNSH"
BAHS_MAGIC = b"BAHS"
CONTROL_MAGIC = b"\x34\x12\x76\x98"

NUP_HEADER_SIZE = 0x80
TOC_SIZE = 0x30
TITLE_OFFSET = 0xB0
TITLE_SIZE = 0x40
DIR_SIZE = 24
FILE_SIZE = 80

BNSH_GRSC_OFFSET = 96
BNSH_BOM_OFFSET = 12
BNSH_SIZE_OFFSET = 28
BNSH_VARIATION_COUNT_OFFSET = 124
BNSH_VARIATION_OFFSET_OFFSET = 128
BNSH_VARIATION_SIZE = 64
BNSH_PROGRAM_OFFSET_IN_VARIATION = 16
BNSH_PROGRAM_SLOTS = (8, 16, 24, 32, 40, 48, 56, 64, 72)
BNSH_SLOT_CONTROL_OFFSET = 8
BNSH_SLOT_CODE_OFFSET = 16
BNSH_SLOT_CODE_SIZE = 24
BNSH_SLOT_CONTROL_SIZE = 28

NVN_CONTROL_STAGE_OFFSET = 0x56
NVN_CONTROL_MIN_SIZE = 0x60
NVN_CONTROL_MAX_SIZE = 0x1000

BAHS_CODE_BASE_OFFSET = 0x1C
BAHS_CONTROL_SIZE = 0xD8
BAHS_CODE_OFFSET_FROM_CONTROL = 0x68
BAHS_LENGTH_OFFSET = 0x38
BAHS_EXTENT_OFFSET = 0x40
BAHS_MAX_CODE_SIZE = 16 << 20

SHADER_TOP_DIRS = {"Shader", "Layout2Shader", "Effect", "Lib"}
# Substrings of a file name that mark a shader-bearing container.  Matching
# on substrings rather than suffixes catches ``X.vsetb.byml.zs`` in Effect/.
SHADER_MARKERS = (".bfsha", ".sarc", ".vsetb", ".sharcb", ".bnsh")

STAGE_NAMES = {
    0: "vertex",
    1: "fragment",
    2: "geometry",
    3: "tess-control",
    4: "tess-evaluation",
    5: "compute",
    6: "task",
    7: "mesh",
}

MAX_GAME_BLOB = 1 << 31


class CensusError(Exception):
    """A package or shader container could not be read."""


def _require_zstd():
    try:
        import zstandard
    except ImportError as exc:  # pragma: no cover - exercised only without the dep
        raise CensusError(
            "the 'zstandard' package is required to read compressed game files "
            "(python -m pip install --user zstandard)"
        ) from exc
    return zstandard


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def _zstd_decompress(data: bytes, max_output_size: int | None = None) -> bytes:
    zstd = _require_zstd()
    decompressor = zstd.ZstdDecompressor()
    try:
        if max_output_size is None:
            return decompressor.decompress(data)
        return decompressor.decompress(data, max_output_size=max_output_size)
    except zstd.ZstdError as exc:
        raise CensusError("zstd frame did not decompress: %s" % exc) from exc


def decompress_game_blob(data: bytes, dictionaries: list[bytes]) -> bytes:
    """Unpack a game zstd file, trying the package dictionaries in order."""
    if data[:4] != ZSTD_MAGIC:
        return data
    zstd = _require_zstd()
    try:
        return _zstd_decompress(data, MAX_GAME_BLOB)
    except CensusError:
        pass
    failures = 0
    for dictionary in dictionaries:
        try:
            zdict = zstd.ZstdCompressionDict(
                dictionary, dict_type=zstd.DICT_TYPE_FULLDICT
            )
            return zstd.ZstdDecompressor(dict_data=zdict).decompress(
                data, max_output_size=MAX_GAME_BLOB
            )
        except (zstd.ZstdError, ValueError):
            failures += 1
    raise CensusError(
        "no package dictionary could unpack a zstd file (%d tried)" % failures
    )


@dataclass(frozen=True)
class NupFile:
    name: str
    size: int
    offset: int
    stored_size: int
    codec: int
    flags: int
    digest: bytes


@dataclass(frozen=True)
class NupTitle:
    index: int
    title_id: int
    root: int
    exefs: int
    romfs: int
    version: int


class NupPackage:
    """Read-only index over a .nup package."""

    def __init__(self, path: str):
        self.path = path
        self._fh = open(path, "rb")
        try:
            self._load()
        except Exception:
            self._fh.close()
            raise

    def close(self) -> None:
        self._fh.close()

    def __enter__(self) -> "NupPackage":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def _load(self) -> None:
        header = self._fh.read(NUP_HEADER_SIZE)
        if len(header) < NUP_HEADER_SIZE or header[:4] != NUP_MAGIC:
            raise CensusError("%s is not a NUP package" % self.path)
        self.file_size = _u64(header, 0x40)
        have = os.fstat(self._fh.fileno()).st_size
        if self.file_size and have < self.file_size:
            raise CensusError(
                "%s is incomplete: %d bytes present, %d declared"
                % (self.path, have, self.file_size)
            )

        toc = self._fh.read(TOC_SIZE)
        if len(toc) < TOC_SIZE or toc[:4] != TOC_MAGIC:
            raise CensusError("%s has no TOC at 0x80" % self.path)
        title_count = _u32(toc, 4)
        dir_count = _u64(toc, 8)
        file_count = _u64(toc, 16)
        strings_rel = _u64(toc, 24)
        strings_size = _u64(toc, 32)

        dirs_at = NUP_HEADER_SIZE + TOC_SIZE + TITLE_SIZE * title_count
        files_at = dirs_at + DIR_SIZE * dir_count
        children_at = files_at + FILE_SIZE * file_count
        strings_at = NUP_HEADER_SIZE + strings_rel
        meta_size = strings_at + strings_size
        self._fh.seek(0)
        meta = self._fh.read(meta_size)
        if len(meta) < meta_size:
            raise CensusError("%s ends inside its package index" % self.path)

        self.strings = meta[strings_at : strings_at + strings_size]
        self.titles: list[NupTitle] = []
        for i in range(title_count):
            base = TITLE_OFFSET + TITLE_SIZE * i
            title_id, root, exefs, romfs, version = struct.unpack_from(
                "<QIIII", meta, base
            )
            self.titles.append(
                NupTitle(i, title_id, root, exefs, romfs, version)
            )

        self.dirs: list[tuple[int, int, int, int]] = [
            struct.unpack_from("<4I", meta, dirs_at + DIR_SIZE * i)
            for i in range(dir_count)
        ]
        self.files: list[NupFile] = []
        for i in range(file_count):
            base = files_at + FILE_SIZE * i
            name_off, name_len, size, offset, stored = struct.unpack_from(
                "<IIQQQ", meta, base
            )
            self.files.append(
                NupFile(
                    name=self._decode_name(name_off, name_len),
                    size=size,
                    offset=offset,
                    stored_size=stored,
                    codec=meta[base + 64],
                    flags=meta[base + 65],
                    digest=meta[base + 32 : base + 64],
                )
            )

        child_count = max(0, (strings_at - children_at) // 4)
        if child_count:
            self.children = struct.unpack_from(
                "<%dI" % child_count, meta, children_at
            )
        else:
            self.children = ()

    def _decode_name(self, offset: int, length: int) -> str:
        return self.strings[offset : offset + length].decode(
            "utf-8", errors="replace"
        )

    def _read_at(self, offset: int, length: int) -> bytes:
        self._fh.seek(offset)
        data = self._fh.read(length)
        if len(data) != length:
            raise CensusError(
                "%s ended early at 0x%x (wanted %d bytes)"
                % (self.path, offset, length)
            )
        return data

    def read_file(self, entry: NupFile) -> bytes:
        """Return a file's bytes, removing only the package's outer layer."""
        if entry.codec == 1:
            stored = entry.stored_size or entry.size
            raw = self._read_at(entry.offset, stored)
            return _zstd_decompress(raw, entry.size or MAX_GAME_BLOB)

        length = entry.stored_size or entry.size
        raw = self._read_at(entry.offset, length)
        if not entry.name.lower().endswith(".zs") and raw[:4] == ZSTD_MAGIC:
            try:
                plain = _zstd_decompress(raw, entry.size or None)
            except CensusError:
                return raw
            if not entry.size or len(plain) == entry.size:
                return plain
        return raw

    def walk(self, dir_index: int, prefix: str = ""):
        """Yield (path, NupFile) for every file below a directory."""
        yield from self._walk(dir_index, prefix, set())

    def _walk(self, dir_index: int, prefix: str, seen: set[int]):
        if dir_index < 0 or dir_index >= len(self.dirs) or dir_index in seen:
            return
        seen.add(dir_index)
        name_off, name_len, first, count = self.dirs[dir_index]
        for child in self.children[first : first + count]:
            if child < len(self.dirs):
                cname_off, cname_len = self.dirs[child][:2]
                name = self._decode_name(cname_off, cname_len)
                yield from self._walk(child, prefix + name + "/", seen)
            else:
                file_index = child - len(self.dirs)
                if 0 <= file_index < len(self.files):
                    entry = self.files[file_index]
                    yield prefix + entry.name, entry

    def select_title(
        self, selector: str | None = None, all_titles: bool = False
    ) -> list[NupTitle]:
        if all_titles:
            return list(self.titles)
        if selector is None:
            if not self.titles:
                raise CensusError("%s has no titles" % self.path)
            return [max(self.titles, key=lambda t: t.version)]
        wanted = selector.lower()
        for title in self.titles:
            if (
                str(title.index) == selector
                or str(title.version) == selector
                or "%016x" % title.title_id == wanted
            ):
                return [title]
        raise CensusError("%s has no title %s" % (self.path, selector))


def sarc_entries(data: bytes):
    """Yield each member of a SARC archive."""
    if data[:4] != SARC_MAGIC or len(data) < 0x20:
        raise CensusError("not a SARC archive")
    data_offset = _u32(data, 0x0C)
    count = _u16(data, 0x1A)
    for i in range(count):
        base = 0x20 + 16 * i
        if base + 16 > len(data):
            break
        start, end = struct.unpack_from("<II", data, base + 8)
        begin = data_offset + start
        finish = data_offset + end
        if begin > finish or finish > len(data):
            raise CensusError("SARC member %d is outside the archive" % i)
        yield data[begin:finish]


def read_dictionaries(package: NupPackage, pack_entry: NupFile | None) -> list[bytes]:
    if pack_entry is None:
        return []
    raw = package.read_file(pack_entry)
    pack = decompress_game_blob(raw, [])
    if pack[:4] != SARC_MAGIC:
        return []
    return list(sarc_entries(pack))


def shader_candidates(package: NupPackage, romfs: int):
    """Return ((path, entry) for shader-bearing files, dictionary entry)."""
    candidates: list[tuple[str, NupFile]] = []
    dictionary_entry: NupFile | None = None
    for path, entry in package.walk(romfs):
        lower = path.lower()
        if lower == "pack/zsdic.pack.zs":
            dictionary_entry = entry
            continue
        if _is_shader_path(path):
            candidates.append((path, entry))
    return candidates, dictionary_entry


def _is_shader_path(path: str) -> bool:
    parts = path.split("/")
    if len(parts) < 2 or parts[0] not in SHADER_TOP_DIRS:
        return False
    lower = path.lower()
    return any(marker in parts[-1].lower() for marker in SHADER_MARKERS)


def container_blobs(raw: bytes, dictionaries: list[bytes]):
    """Yield shader containers from a file, unpacking zstd and SARC layers.

    A game ``.zs`` file is normally one zstd layer, but an older dumper can
    wrap it in a second package-level frame, so zstd is peeled a few times.
    """
    plain = raw
    for _ in range(4):
        if plain[:4] != ZSTD_MAGIC:
            break
        plain = decompress_game_blob(plain, dictionaries)
    if plain[:4] == SARC_MAGIC:
        for member in sarc_entries(plain):
            yield from container_blobs(member, dictionaries)
    else:
        yield plain


@dataclass(frozen=True)
class StageProgram:
    variation: int
    stage: int
    control: bytes
    code: bytes


def parse_bnsh(data: bytes):
    """Yield the stage programs of each BNSH variation."""
    if (
        len(data) < 0x100
        or data[:4] != BNSH_MAGIC
        or data[BNSH_BOM_OFFSET : BNSH_BOM_OFFSET + 2] != b"\xff\xfe"
        or data[BNSH_GRSC_OFFSET : BNSH_GRSC_OFFSET + 4] != b"grsc"
    ):
        return
    body_size = _u32(data, BNSH_SIZE_OFFSET)
    if body_size < 0x100 or body_size > len(data):
        return
    count = _u32(data, BNSH_VARIATION_COUNT_OFFSET)
    variations_at = _u64(data, BNSH_VARIATION_OFFSET_OFFSET)
    if variations_at == 0 or count == 0:
        return

    for variation in range(count):
        var_at = variations_at + variation * BNSH_VARIATION_SIZE
        if var_at + BNSH_VARIATION_SIZE > body_size:
            break
        program_at = _u64(data, var_at + BNSH_PROGRAM_OFFSET_IN_VARIATION)
        if not program_at or program_at + 80 > body_size:
            continue
        for slot in BNSH_PROGRAM_SLOTS:
            slot_at = program_at + slot
            if slot_at + 8 > body_size:
                continue
            stage_at = _u64(data, slot_at)
            if not stage_at or stage_at + 32 > body_size:
                continue
            control_at = _u64(data, stage_at + BNSH_SLOT_CONTROL_OFFSET)
            code_at = _u64(data, stage_at + BNSH_SLOT_CODE_OFFSET)
            code_size = _u32(data, stage_at + BNSH_SLOT_CODE_SIZE)
            control_size = _u32(data, stage_at + BNSH_SLOT_CONTROL_SIZE)
            if not (
                NVN_CONTROL_MIN_SIZE
                <= control_size
                <= NVN_CONTROL_MAX_SIZE
            ):
                continue
            if control_at + control_size > body_size:
                continue
            if code_at + code_size > body_size:
                continue
            control = data[control_at : control_at + control_size]
            if control[:4] != CONTROL_MAGIC:
                continue
            stage = _u16(control, NVN_CONTROL_STAGE_OFFSET)
            code = data[code_at : code_at + code_size]
            yield StageProgram(variation, stage, control, code)


def parse_bahs(data: bytes):
    """Yield the compute programs of an agl BAHS shader archive."""
    if data[:4] != BAHS_MAGIC or len(data) < BAHS_CODE_BASE_OFFSET + 4:
        return
    code_base = _u32(data, BAHS_CODE_BASE_OFFSET)
    search_at = 0
    while True:
        control_at = data.find(CONTROL_MAGIC, search_at)
        if control_at < 0:
            return
        search_at = control_at + 4
        if (
            control_at < BAHS_CODE_OFFSET_FROM_CONTROL
            or control_at + BAHS_CONTROL_SIZE > len(data)
        ):
            continue
        stage = _u16(data, control_at + NVN_CONTROL_STAGE_OFFSET)
        code_size = _u32(data, control_at + BAHS_LENGTH_OFFSET)
        first_offset, extent = struct.unpack_from(
            "<II", data, control_at + BAHS_EXTENT_OFFSET
        )
        if (
            stage != 5
            or not code_size
            or extent > BAHS_MAX_CODE_SIZE
            or first_offset > extent
            or code_size > extent
        ):
            continue
        code_at = code_base + _u32(
            data, control_at - BAHS_CODE_OFFSET_FROM_CONTROL
        )
        if code_at + extent > len(data):
            continue
        control = data[control_at : control_at + BAHS_CONTROL_SIZE]
        code = data[code_at : code_at + extent]
        yield StageProgram(0, stage, control, code)


def _bnsh_blobs(blob: bytes):
    """Find every complete BNSH object in a blob, including concatenated ones."""
    search_at = 0
    while True:
        at = blob.find(BNSH_MAGIC, search_at)
        if at < 0:
            return
        search_at = at + 4
        if at + BNSH_GRSC_OFFSET + 4 > len(blob):
            continue
        if (
            blob[at + BNSH_BOM_OFFSET : at + BNSH_BOM_OFFSET + 2]
            != b"\xff\xfe"
            or blob[at + BNSH_GRSC_OFFSET : at + BNSH_GRSC_OFFSET + 4]
            != b"grsc"
        ):
            continue
        size = _u32(blob, at + BNSH_SIZE_OFFSET)
        if size < 0x100 or at + size > len(blob):
            continue
        yield blob[at : at + size]
        search_at = at + size


@dataclass
class ProgramRecord:
    sha256: str
    stage: int
    control_size: int
    code_size: int
    count: int = 0
    packages: set[str] = field(default_factory=set)
    sources: set[str] = field(default_factory=set)

    @property
    def size(self) -> int:
        return self.control_size + self.code_size

    @property
    def stage_name(self) -> str:
        return STAGE_NAMES.get(self.stage, "stage-%d" % self.stage)


class Census:
    def __init__(self) -> None:
        self.programs: dict[str, ProgramRecord] = {}
        self.variations = 0
        self.references = 0

    def add(self, package: str, source: str, program: StageProgram) -> None:
        digest = hashlib.sha256(program.control + program.code).hexdigest()
        record = self.programs.get(digest)
        if record is None:
            record = ProgramRecord(
                sha256=digest,
                stage=program.stage,
                control_size=len(program.control),
                code_size=len(program.code),
            )
            self.programs[digest] = record
        record.count += 1
        record.packages.add(package)
        record.sources.add(source)
        self.references += 1

    def note_variation(self) -> None:
        self.variations += 1

    def rows(self):
        for record in sorted(
            self.programs.values(),
            key=lambda r: (r.stage, -r.size, r.sha256),
        ):
            yield record


@dataclass
class PackageStats:
    package: str
    title: str
    candidates: int = 0
    variations: int = 0
    programs: int = 0
    references: int = 0


def census_package(
    path: str,
    census: Census,
    selector: str | None = None,
    all_titles: bool = False,
) -> list[PackageStats]:
    results: list[PackageStats] = []
    with NupPackage(path) as package:
        titles = package.select_title(selector, all_titles)
        for title in titles:
            stats = PackageStats(
                package=os.path.basename(path),
                title="%016x v%d" % (title.title_id, title.version),
            )
            candidates, dictionary_entry = shader_candidates(
                package, title.romfs
            )
            stats.candidates = len(candidates)
            try:
                dictionaries = read_dictionaries(package, dictionary_entry)
            except CensusError as exc:
                print(
                    "  warning: %s: %s" % (stats.package, exc),
                    file=sys.stderr,
                )
                dictionaries = []

            before_programs = len(census.programs)
            before_references = census.references
            for source, entry in candidates:
                try:
                    raw = package.read_file(entry)
                except CensusError as exc:
                    print(
                        "  warning: %s: %s" % (source, exc),
                        file=sys.stderr,
                    )
                    continue
                try:
                    for blob in container_blobs(raw, dictionaries):
                        for bnsh in _bnsh_blobs(blob):
                            seen_variation = False
                            for program in parse_bnsh(bnsh):
                                census.add(stats.package + ":" + stats.title, source, program)
                                seen_variation = True
                            if seen_variation:
                                census.note_variation()
                                stats.variations += 1
                        for program in parse_bahs(blob):
                            census.add(stats.package + ":" + stats.title, source, program)
                except CensusError as exc:
                    print(
                        "  warning: %s: %s" % (source, exc),
                        file=sys.stderr,
                    )
                    continue

            stats.programs = len(census.programs) - before_programs
            stats.references = census.references - before_references
            print(
                "%s [%s]: %d candidate files, %d variations, %d new programs, "
                "%d references"
                % (
                    stats.package,
                    stats.title,
                    stats.candidates,
                    stats.variations,
                    stats.programs,
                    stats.references,
                )
            )
            results.append(stats)
    return results


CSV_COLUMNS = (
    "sha256",
    "stage",
    "stage_name",
    "size",
    "control_size",
    "code_size",
    "count",
    "packages",
    "sources",
)


def _csv_rows(census: Census):
    yield CSV_COLUMNS
    for record in census.rows():
        yield (
            record.sha256,
            str(record.stage),
            record.stage_name,
            str(record.size),
            str(record.control_size),
            str(record.code_size),
            str(record.count),
            "|".join(sorted(record.packages)),
            "|".join(sorted(record.sources)),
        )


def csv_lines(census: Census):
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    for row in _csv_rows(census):
        writer.writerow(row)
    yield from buffer.getvalue().splitlines()


def write_csv(census: Census, path: str) -> None:
    directory = os.path.dirname(os.path.abspath(path))
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerows(_csv_rows(census))


def print_totals(census: Census) -> None:
    print("Totals by stage:")
    stages: dict[int, list[ProgramRecord]] = {}
    for record in census.programs.values():
        stages.setdefault(record.stage, []).append(record)
    for stage in sorted(stages):
        records = stages[stage]
        name = STAGE_NAMES.get(stage, "stage-%d" % stage)
        print(
            "  %-16s (%d): %6d programs, %12d bytes, %6d references"
            % (
                name,
                stage,
                len(records),
                sum(r.size for r in records),
                sum(r.count for r in records),
            )
        )
    print(
        "  %-16s    : %6d programs, %12d bytes, %6d references"
        % (
            "all stages",
            len(census.programs),
            sum(r.size for r in census.programs.values()),
            census.references,
        )
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Census NVN shader programs inside Switch 2 .nup packages."
    )
    parser.add_argument("packages", nargs="+", help=".nup package(s) to scan")
    parser.add_argument(
        "--csv",
        default=os.path.join("out", "shader_census.csv"),
        help="CSV destination (default: out/shader_census.csv)",
    )
    parser.add_argument(
        "--title",
        help="title index, version or 16-hex id (default: newest version)",
    )
    parser.add_argument(
        "--all-titles",
        action="store_true",
        help="scan every title in each package instead of only the newest",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        _require_zstd()
    except CensusError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    census = Census()
    failures = 0
    for path in args.packages:
        try:
            census_package(path, census, args.title, args.all_titles)
        except (CensusError, OSError) as exc:
            failures += 1
            print("error: %s: %s" % (path, exc), file=sys.stderr)

    if census.programs:
        print_totals(census)
    else:
        print("No shader programs found.")

    try:
        write_csv(census, args.csv)
    except OSError as exc:
        print("error: cannot write %s: %s" % (args.csv, exc), file=sys.stderr)
        failures += 1
    else:
        print(
            "Wrote %d program rows to %s"
            % (len(census.programs), args.csv)
        )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
