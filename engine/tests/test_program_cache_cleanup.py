"""Count module lifetimes through the production cache loader, without a GPU or game.

Auto-discovered by tests/run_all.py. Optional local checks:
  python -I -B tests/test_program_cache_cleanup.py --cxx cl --sanitize
  python -I -B tests/test_program_cache_cleanup.py --cxx g++ --sanitize
Use --source with an older vk_shader.cpp for the negative control.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = None
SANITIZE = False
SOURCE = ROOT / 'gpu/vk/vk_shader.cpp'


def line(text, marker):
    start = text.index(marker)
    return text[start:text.index('\n', start)]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class CacheModuleCleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = COMPILER or shutil.which('g++') or shutil.which('clang++') or shutil.which('cl')
        if not compiler:
            raise unittest.SkipTest('no C++20 compiler')
        cls.temporary = tempfile.TemporaryDirectory(prefix='hookshot-cache-cleanup-')
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name).resolve()
        source = SOURCE.read_text(encoding='utf-8')
        header = (ROOT / 'gpu/vk/vk_shader.h').read_text(encoding='utf-8')
        structs = '\n'.join(block(header, marker) + ';' for marker in ('struct Stage {', 'struct Program {'))
        structs += '\n' + block(source, 'struct DiskRecord {') + ';'
        functions = line(source, 'std::atomic<int64_t> st_module_ns') + '\n'   # (the preparation's profile counters, which make_module and program_cache_save add to)
        functions += '\n'.join(block(source, marker) for marker in (
            'VkShaderModule make_module(', 'uint64_t answers_hash(', 'bool answers_fit(', 'void idx_put('))
        # (the records' packing: kKindZstd and what record_pack and record_unpack use. zstd is stubbed in the test: it packs nothing)
        functions += '\n' + line(source, 'constexpr uint32_t kKindZstd')
        functions += '\n' + block(source, 'bool pack_on(') + '\n' + block(source, 'struct ZstdThread {') + ';'
        functions += '\n' + block(source, 'ZstdThread& zstd_thread(') + '\n' + line(source, 'std::atomic<int64_t> st_pack_raw')
        functions += '\n' + block(source, 'void record_pack(') + '\n' + block(source, 'bool record_unpack(')
        serialization = source[source.index('void put_bytes('):source.index('void program_cache_save(')]
        functions += '\n' + serialization + block(source, 'void program_cache_save(')
        functions += '\n' + block(source, 'void program_cache_read(')
        functions += '\n' + block(source, 'void program_cache_load(')
        (directory / 'cache_types.inc').write_text(structs, encoding='utf-8')
        (directory / 'cache_functions.inc').write_text(functions, encoding='utf-8')
        cls.executable = directory / 'check.exe'
        test_source = ROOT / 'tests/program_cache_cleanup_test.cpp'
        if Path(compiler).stem.lower() == 'cl':
            command = [compiler, '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/D_CRT_SECURE_NO_WARNINGS',
                       '/I' + str(directory), '/I' + str(ROOT / 'third_party/zstd/lib'), str(test_source), '/Fe:' + str(cls.executable)]
            if SANITIZE:
                command += ['/fsanitize=address', '/Zi']
        else:
            command = [compiler, '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                       '-I' + str(directory), '-I' + str(ROOT / 'third_party/zstd/lib'), str(test_source), '-o', str(cls.executable)]
            if SANITIZE:
                command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_second_module_failure(self):
        self.run_case('second')

    def test_first_module_failure(self):
        self.run_case('first')

    def test_truncated_second_stage(self):
        self.run_case('truncated')

    def test_empty_second_shader(self):
        self.run_case('empty')

    def test_valid_record_retains_modules(self):
        self.run_case('valid')

    def test_independent_records_continue(self):
        self.run_case('mixed')

    def test_answer_past_the_stages_refused(self):
        self.run_case('answer_stage')

    def test_answer_past_the_slots_refused(self):
        self.run_case('answer_desc')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--source', type=Path, default=SOURCE)
    args, remaining = parser.parse_known_args()
    COMPILER, SANITIZE, SOURCE = args.cxx, args.sanitize, args.source
    unittest.main(argv=[__file__, *remaining])
