"""Image buffer, storage image and texture buffer arrays are arrays in the SPIR-V, without game files or a GPU.

A handle the shader reads at a dynamic offset of a constant buffer is a descriptor array (Shader::Info's count), and the
renderer writes that many descriptors to its binding (vk_draw.cpp). tests/image_arrays_ir.cpp builds such programs and
the same ones with single descriptors, runs them through the real texture pass and SPIR-V back end, and writes their
modules. Each must translate, pass spirv-val (Vulkan 1.3) and declare every binding with the count Info gives it: an
OpTypeArray of that many images for a count above 1, the image itself for 1. Image buffers and storage images are read and
written or added to atomically, texture buffers and textures fetched (plain, and with the fetch bounds of vk_vendor.h).
Programs whose device indexes the arrays nonuniformly must carry NonUniform decorations (an atomic's texel pointer too)
and that kind's capability.

python -I -B nx2/test_image_arrays.py --headers <SPIRV-Headers/include> --spirv-val spirv-val [--cxx clang++]
    [--tree <a checkout>] [--base-tree <a checkout>] [--keep <folder>]
--tree takes the back end from another checkout (main's: the test fails there). --base-tree builds the fixture against
that checkout's back end too, and requires every module of a program with single descriptors (the -1 cases) to be byte
for byte the same: a change that declares arrays leaves every other program as it was. --keep copies the modules.
Builds the same Eden IR/SPIR-V and Sirit sources as the renderer, with its compatibility headers.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"]
# (spirv-val: SPIRV-Tools comes with the Linux runner's packages, not on Windows)
RUN_ALL_SKIP = {"windows": "needs spirv-val: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
KINDS = ('texbuf', 'imgbuf', 'tex', 'img')   # (Eden's binding order: texture buffers, image buffers, textures, images)
# The descriptor arrays (count above 1) each program has, in binding order; the -1 programs have none
ARRAYS = {
    'imgbuf-2': [('imgbuf', 2)],
    'img-3': [('img', 3)],
    'texbuf-2': [('texbuf', 2)],
    'imgbuf-atomic-2': [('imgbuf', 2)],
    'img-atomic-3': [('img', 3)],
    'mixed': [('texbuf', 2), ('imgbuf', 2), ('tex', 2), ('img', 3)],
}
# (each kind's capability for nonuniform indexing)
NONUNIFORM_CAPABILITY = {'texbuf': 5311, 'imgbuf': 5312, 'tex': 5307, 'img': 5309}
OP_CAPABILITY, OP_TYPE_IMAGE, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_ARRAY = 17, 25, 27, 28
OP_TYPE_POINTER, OP_CONSTANT, OP_VARIABLE, OP_IMAGE_TEXEL_POINTER, OP_DECORATE = 32, 43, 59, 60, 71
DIM_BUFFER, UNIFORM_CONSTANT, DECORATION_BINDING, DECORATION_NON_UNIFORM = 5, 0, 33, 5300
CASES = 28   # (12 programs, each plain and nonuniform; texbuf-1, texbuf-2, mixed-1 and mixed also with fetch bounds)


def build_fixture(cxx, flags, sources, exe):
    """What one compiler call with every source builds, made one object a source, as many at once as there are CPUs,
    then linked: the same program, faster, and each object one a compiler cache can keep"""
    (exe.parent / 'obj').mkdir()
    objs = [exe.parent / 'obj' / ('%03d.o' % i) for i in range(len(sources))]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
        list(pool.map(lambda job: subprocess.run([cxx, *flags, '-c', str(job[0]), '-o', str(job[1])], check=True),
                      zip(sources, objs)))
    subprocess.run([cxx, *map(str, objs), '-o', str(exe)], check=True)


def emit(args, tree, folder):
    """The fixture built against tree's back end and run into folder: {case: (Info's counts by kind) or 'refused: ...'}"""
    vk = tree / 'gpu/vk'
    sources = sorted({p for directory in ('eden/shader_recompiler/backend/spirv',
                      'eden/shader_recompiler/frontend/ir', 'eden/shader_recompiler/ir_opt', 'sirit/src')
                      for p in (vk / directory).rglob('*.cpp')})
    flags = ['-std=c++20', '-include', str(vk / 'compat' / 'hookshot_env.h'), '-O1', '-D_CRT_SECURE_NO_WARNINGS',
             '-DVK_NO_PROTOTYPES', '-DMKWVK_INCLUDES=2']
    for header in ('algorithm', 'cstdio', 'cstdlib', 'cstring', 'functional', 'string', 'vector'):
        flags += ['-include', header]
    flags += ['-I' + str(p) for p in (vk / 'compat', tree / 'gpu/compat', vk / 'eden',
              tree / 'gpu/third_party/suyu', vk / 'sirit/include', vk / 'sirit/src', args.headers)]
    exe = folder / 'fixture.exe'
    build_fixture(args.cxx, flags, [ROOT / 'tests/image_arrays_ir.cpp', *sources], exe)
    modules = folder / 'modules'
    run = subprocess.run([str(exe), str(modules)], check=True, capture_output=True, text=True)
    cases = {}
    for line in run.stdout.splitlines():
        name, rest = line.split(' ', 1)
        if rest.startswith('refused: '):
            cases[name] = rest
            continue
        fields = dict(field.split(':') for field in rest.split()[1:])
        cases[name] = {kind: [int(n) for n in fields[kind].split(',') if n] for kind in KINDS}
    return modules, cases


def declared(path):
    """The module's descriptors of images (UniformConstant variables with a binding), in binding order:
    {kind: [count, ...]}; its ids decorated NonUniform, its capabilities and its atomics' texel pointers"""
    data = path.read_bytes()
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    images, sampled, arrays, constants, pointers, variables, bindings = {}, set(), {}, {}, {}, {}, {}
    capabilities, non_uniform, texel_pointers = set(), set(), []
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        operands = words[i + 1:i + count]
        if op == OP_CAPABILITY:
            capabilities.add(operands[0])
        elif op == OP_TYPE_IMAGE:
            images[operands[0]] = (operands[2], operands[6])   # (dim, sampled: 1 sampled, 2 storage)
        elif op == OP_TYPE_SAMPLED_IMAGE:
            sampled.add(operands[0])
        elif op == OP_TYPE_ARRAY:
            arrays[operands[0]] = (operands[1], operands[2])
        elif op == OP_CONSTANT:
            constants[operands[1]] = operands[2]
        elif op == OP_TYPE_POINTER:
            pointers[operands[0]] = (operands[1], operands[2])
        elif op == OP_VARIABLE:
            variables[operands[1]] = (operands[0], operands[2])
        elif op == OP_IMAGE_TEXEL_POINTER:
            texel_pointers.append(operands[1])
        elif op == OP_DECORATE and operands[1] == DECORATION_BINDING:
            bindings[operands[0]] = operands[2]
        elif op == OP_DECORATE and operands[1] == DECORATION_NON_UNIFORM:
            non_uniform.add(operands[0])
        i += count
    found = {kind: [] for kind in KINDS}
    for variable in sorted((v for v in variables if v in bindings), key=bindings.get):
        pointer, storage = variables[variable]
        if storage != UNIFORM_CONSTANT:
            continue
        element, count = pointers[pointer][1], 1
        if element in arrays:
            element, count = arrays[element][0], constants[arrays[element][1]]
        if element in sampled:
            kind = 'tex'
        else:
            dim, use = images[element]
            kind = ('texbuf' if use == 1 else 'imgbuf') if dim == DIM_BUFFER else 'img'
        found[kind].append(count)
    return found, non_uniform, capabilities, texel_pointers


def program_of(case):
    """The program a case translates (its name without the profile's suffix)"""
    return case.removesuffix('-nonuniform').removesuffix('-bounds')


def check(args, modules, cases):
    """What is wrong with each case's module, as lines"""
    failures = []
    for name, info in cases.items():
        program = program_of(name)
        if isinstance(info, str):
            failures.append('%s: translation %s' % (name, info))
            continue
        module = modules / (name + '.spv')
        valid = subprocess.run([args.spirv_val, '--target-env', 'vulkan1.3', str(module)], capture_output=True, text=True)
        if valid.returncode:
            said = (valid.stdout + valid.stderr).strip().splitlines() or ['exit %d' % valid.returncode]
            failures.append('%s: spirv-val: %s' % (name, said[0]))
        want = ARRAYS.get(program, [])
        made = [(kind, count) for kind in KINDS for count in info[kind] if count > 1]
        if made != want:
            failures.append('%s: the texture pass made the arrays %s, not %s' % (name, made, want))
        found, non_uniform, capabilities, texel_pointers = declared(module)
        if found != info:
            failures.append('%s: the module declares %s where Shader::Info has %s' % (name, found, info))
        if name.endswith('-nonuniform') and want:
            if not non_uniform:
                failures.append('%s: no NonUniform decoration' % name)
            if not set(texel_pointers) <= non_uniform:
                failures.append('%s: an atomic\'s texel pointer is not decorated NonUniform' % name)
            for kind in {kind for kind, _ in want} - {k for k, c in NONUNIFORM_CAPABILITY.items() if c in capabilities}:
                failures.append('%s: no %s nonuniform indexing capability' % (name, kind))
        elif non_uniform:
            failures.append('%s: NonUniform decorations without arrays indexed nonuniformly' % name)
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--spirv-val', default='spirv-val')
    parser.add_argument('--tree', type=Path, default=ROOT, help='the checkout whose back end is tested')
    parser.add_argument('--base-tree', type=Path, help='a checkout whose single-descriptor modules must be the same')
    parser.add_argument('--keep', type=Path, help='a folder to copy the modules to')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='image-arrays-') as temp:
        folder = Path(temp).resolve()
        (folder / 'tree').mkdir()
        modules, cases = emit(args, args.tree.resolve(), folder / 'tree')
        if args.keep:
            shutil.copytree(modules, args.keep, dirs_exist_ok=True)
        failures = check(args, modules, cases)
        if len(cases) != CASES:
            failures.append('%d cases, not %d: %s' % (len(cases), CASES, sorted(cases)))
        singles = sorted(name for name in cases if program_of(name).endswith('-1'))
        if args.base_tree:
            (folder / 'base').mkdir()
            base_modules, base_cases = emit(args, args.base_tree.resolve(), folder / 'base')
            for name in singles:
                ours, theirs = modules / (name + '.spv'), base_modules / (name + '.spv')
                if not theirs.exists() or not ours.exists() or ours.read_bytes() != theirs.read_bytes():
                    failures.append('%s: not byte for byte the base tree\'s module' % name)
            for name in sorted(set(base_cases) - set(singles)):
                base = base_cases[name]
                print('base tree %s: %s' % (name, base if isinstance(base, str) else '%d bytes' % (
                    (base_modules / (name + '.spv')).stat().st_size)))
    for name, info in sorted(cases.items()):
        print('%s: %s' % (name, info if isinstance(info, str) else ' '.join(
            '%s %s' % (kind, info[kind]) for kind in KINDS if info[kind])))
    if failures:
        for failure in failures:
            print('FAIL: ' + failure)
        raise SystemExit(1)
    print('PASS: image buffer, storage image and texture buffer arrays declared as arrays; %d modules validate%s' % (
        len(cases), '; %d single-descriptor modules byte for byte the base tree\'s' % len(singles) if args.base_tree else ''))


if __name__ == '__main__':
    main()
