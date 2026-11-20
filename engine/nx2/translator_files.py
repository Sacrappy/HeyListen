# Copyright (C) 2026 Sacrappy

"""What the shader caches are good for: the shader translator's sources, and the stamp made of them.

nx2/build.py compiles the stamp into the renderer (HOOKSHOT_SHADER_STAMP), where translator_stamp (gpu/vk/vk_shader.cpp) folds
in what the translator is told of the GPU and the environment, and the sizes and C++ library of the records. The program cache
(hookshot_programs.bin), the pipeline notes (hookshot_pipelines.bin) and the Vulkan pipeline cache files are dropped whole when
it changes, and every player translates every program again at the next start, so the stamp holds the sources that can change
a translated program:

- gpu/vk/eden, gpu/vk/sirit and gpu/vk/compat, every file: Eden's recompiler, the SPIR-V emitter and what they are built with;
- vk_shader.cpp: what drives them (the environment they read, the passes, the program keys and the answers kept with them);
- vk_bank.cpp: the driver constant banks c0 and c2 the translation reads constants from;
- vk_state.h: NvnState, which those two read (its arrays and its defaults: the seeds are translated from a blank one).

Not vk_api.h (Longshot 26a2eab0, #370). The translator reaches the host through MkwVkHost's gpu_read, gpu_guest, guest_base and
guest_size, which carry the game's memory, and depth_minus_w, whose answer translator_stamp holds itself: what they answer is
the host's code (host/), which the stamp never held, and a change to how they are called changes vk_shader.cpp or vk_bank.cpp.
The NVN types NvnState is made of are gpu/render/rx_nvn.h's, which vk_api.h includes but which the stamp never held either.
The rest of vk_api.h (the menu's option ids, the controls, the prompts, MkwVkRenderer, the renderer callbacks) the translator
does not use, and the menu, input and audio rows added there made every player translate every program again. vk_api.h is
still in the *state* stamp (HOOKSHOT_STATE_STAMP, the pipeline notes' layout), which is a different question.

Files are stamped with Windows line ends as Unix ones, and a C or C++ file (C_FAMILY) by its tokens (normal(), Longshot issue
1611, Jeff Aigner's 4fc22424): its comments, blank lines, trailing white space and indentation leave the stamp as they leave the
compiled translator. Any other file, and a C or C++ file the scanner is not sure of, is stamped byte for byte, so a doubt can
only make the stamp move more often, never less. tests/test_shader_stamp_inputs.py pins the list: a change to it means stating
why, in the docstring above.
"""
import hashlib
import os
import re

DIRS = ("eden", "sirit", "compat")                       # (gpu/vk/<dir>/: every file below)
FILES = ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h")   # (gpu/vk/<file>: after the folders' files, in this order)
C_FAMILY = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc", ".inl")   # (stamped by normal(); the rest byte for byte)
NORMAL_TAG = b"\0tokens 1\0"   # (before a normal form in the hash, so it is never taken for a file's bytes)


class _Unsure(Exception):
    """A text normal() does not read with certainty: the file is stamped byte for byte"""


_IDENT = frozenset(b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$") | frozenset(range(0x80, 0x100))
_DIGITS = frozenset(b"0123456789")
_PREFIXES = (b"u8", b"u", b"U", b"L")                                # (of a string or character literal)
_SPLIT = b" /*\n*/ "   # (a comment over lines between two tokens: no normal form holds a comment otherwise)
_WIDTH = {None: 0, b" ": 1, _SPLIT: 2, b"\n": 3}
_RAW = (b"R", b"u8R", b"uR", b"UR", b"LR")                           # (of a raw string: never read, the file is unsure)
# (a header name in angle brackets is no string to the scanner: one holding a comment marker or a quote is unsure)
_ANGLED = re.compile(rb"^[ \t]*#[ \t]*(?:include|include_next|import)[ \t]*<[^>\n]*(?://|/\*|[\"'])|__has_include[ \t]*\([ \t]*<[^>\n]*(?://|/\*|[\"'])",
                     re.M)
_BLANK_SPLICE = re.compile(rb"\\[ \t\f\v]+\n")   # (a backslash, blanks, a line end: GCC and clang splice it, and warn)


def _literal(data, i):
    """The end (one past the closing quote) of the string or character literal whose quote is at i. A line break or a line
    splice inside it, or no end, is unsure"""
    q, j, n = data[i], i + 1, len(data)
    while j < n:
        c = data[j]
        if c == q:
            return j + 1
        if c == 0x0A:
            raise _Unsure()
        if c == 0x5C:   # (\: an escape, never a splice here)
            if j + 1 >= n or data[j + 1] == 0x0A:
                raise _Unsure()
            j += 2
            continue
        j += 1
    raise _Unsure()


def _tokens(data):
    """data (a C or C++ file, Unix line ends) as its tokens, as translation phase 3 leaves them: each comment one space, a run
    of white space one space, or one line break when it holds one (a line break ends a directive; how many, and the spaces
    around them, change nothing). A comment over lines with tokens on both sides on its lines is a separator of its own
    (_SPLIT): whether a # after it starts a directive is the compiler's choice, so it is neither a space nor a line break.
    Literals, line splices and every other byte are kept as they are. Raises _Unsure for what
    it does not read with certainty: a raw string, a line break in a literal, a comment ended or continued by a splice, a
    splice beside a / or a *, a backslash with blanks before its line end (a splice to GCC and clang), a header name in angle
    brackets holding a comment marker or a quote."""
    if _ANGLED.search(data) or _BLANK_SPLICE.search(data):
        raise _Unsure()
    out = bytearray()
    space = None   # (the white space before the next token: None, b" ", _SPLIT or b"\n", each wider than the one before)
    n, i = len(data), 0

    def widen(to):
        nonlocal space
        if _WIDTH[to] > _WIDTH[space]:
            space = to

    def token(b):
        nonlocal space
        if space is not None and out:
            out.extend(space)
        space = None
        out.extend(b)

    while i < n:
        c = data[i]
        if c == 0x20 or c == 0x09:
            widen(b" ")
            i += 1
        elif c == 0x0A:
            widen(b"\n")
            i += 1
        elif c == 0x5C and data.startswith(b"\\\n", i):   # (a line splice: kept, it is no white space)
            if (space is None and out and out[-1] in b"/*") or (i + 2 < n and data[i + 2] in b"/*"):
                raise _Unsure()
            token(b"\\\n")
            i += 2
        elif data.startswith(b"//", i):
            j = data.find(b"\n", i)
            j = n if j < 0 else j
            if data[j - 1] == 0x5C:   # (the comment goes on over the splice)
                raise _Unsure()
            widen(b" ")
            i = j
        elif data.startswith(b"/*", i):
            j = data.find(b"*/", i + 2)
            if j < 0 or b"\\\n" in data[i + 2:j]:
                raise _Unsure()
            widen(_SPLIT if b"\n" in data[i + 2:j] else b" ")
            i = j + 2
        elif c == 0x22 or c == 0x27:   # (" or ': a literal; a digit separator ' is read with its number below)
            j = _literal(data, i)
            token(data[i:j])
            i = j
        elif c in _DIGITS or (c == 0x2E and i + 1 < n and data[i + 1] in _DIGITS):   # (a pp-number)
            j = i + 1
            while j < n:
                d = data[j]
                if d in b"eEpP" and j + 1 < n and data[j + 1] in b"+-":
                    j += 2
                elif d in _IDENT or d == 0x2E:
                    j += 1
                elif d == 0x27 and j + 1 < n and data[j + 1] in _IDENT:   # (1'000)
                    j += 2
                else:
                    break
            token(data[i:j])
            i = j
        elif c in _IDENT:
            j = i + 1
            while j < n and data[j] in _IDENT:
                j += 1
            word = data[i:j]
            if j < n and data[j] == 0x22 and word in _RAW:
                raise _Unsure()
            if j < n and data[j] in b"\"'" and word in _PREFIXES:
                k = _literal(data, j)
                token(data[i:k])
                i = k
            else:
                token(word)
                i = j
        else:
            token(data[i:i + 1])
            i += 1
    return bytes(out)


def normal(rel, data):
    """What stamp() hashes of a stamped file (rel, its path below gpu/vk, / separated; data, its bytes with Unix line ends):
    a C or C++ file's tokens after NORMAL_TAG, or the bytes as they are for any other file and for a file _tokens is not sure
    of"""
    if not rel.lower().endswith(C_FAMILY):
        return data
    try:
        return NORMAL_TAG + _tokens(data)
    except _Unsure:
        return data


def files(vk):
    """The stamped files below gpu/vk (vk), in the stamp's order: the folders' files sorted, then FILES as listed (appended,
    not sorted in: a file added to a folder must not move the ones after it)"""
    found = [os.path.join(d, f) for top in DIRS for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
    return sorted(found) + [os.path.join(vk, n) for n in FILES]


def stamp(vk):
    """HOOKSHOT_SHADER_STAMP's 16 hex digits for the translator below gpu/vk (vk)"""
    h = hashlib.sha1(usedforsecurity=False)
    for f in files(vk):
        rel = os.path.relpath(f, vk).replace("\\", "/")
        with open(f, "rb") as src:
            h.update(rel.encode() + normal(rel, src.read().replace(b"\r\n", b"\n")))
    return h.hexdigest()[:16]
