"""nx2/translator_files.py's stamp() over the translator's text: an edit that cannot change a translated program (a comment,
a blank line, trailing white space, indentation) keeps LONGSHOT_SHADER_STAMP, and every edit that can (a token, a literal, a
macro, an include, a file that is not C or C++) moves it, the hard cases of issue 1611 among them: comment markers in a
literal, a quote in a character literal, digit separators, a comment between two tokens, a raw string, a line splice (with
blanks before its line end too).

Every pair is stamped as a whole small translator tree (gpu/vk/{eden,sirit,compat}/ and the three named files) in a
temporary folder, then the real tree's stamped files copied and given comments and blank lines. Each failure names the case.

python -I -B tests/test_translator_stamp_text.py
"""
import importlib.util
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VK = os.path.join(ROOT, "gpu", "vk")

spec = importlib.util.spec_from_file_location("translator_files_text", os.path.join(ROOT, "nx2", "translator_files.py"))
tf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tf)

BASE = {
    "eden/shader_recompiler/x.cpp": b"// SPDX: x\n#include \"x.h\"\n\nnamespace Shader {\nint f(int a) {\n    return a + 1;\n}\n}\n",
    "eden/shader_recompiler/x.h": b"#pragma once\n#define X_ONE 1\nint f(int a);\n",
    "eden/shader_recompiler/ops.inc": b"// the ops\nOPCODE(Add, U32, U32)\n",
    "eden/CMakeLists.txt": b"# eden\nadd_library(eden x.cpp)\n",
    "sirit/src/sirit.cpp": b"#include <vector>\nstatic const char* name = \"sirit\";\n",
    "sirit/LICENSE": b"Licence text\n",
    "compat/common/assert.h": b"#define ASSERT(a) do { if (!(a)) fail(); } while (0)\n",
    "vk_shader.cpp": b"#include \"vk_state.h\"\n// What drives the translator\nstatic int passes = 3;\nint run() { return passes; }\n",
    "vk_bank.cpp": b"int bank_c0(int i) { return i * 4; } // c0's words\n",
    "vk_state.h": b"struct NvnState {\n    int a = 0;   // defaults\n    int big = 1'000;   // a big one\n};\n",
}

FAILED = []


def stamp_of(tmp, changes):
    """The stamp of BASE with changes (rel -> bytes, or None to remove), built in a fresh folder below tmp"""
    root = tempfile.mkdtemp(dir=tmp)
    files = dict(BASE)
    files.update(changes)
    for rel, data in files.items():
        if data is None:
            continue
        p = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
    return tf.stamp(root)


def edit(rel, old, new):
    data = BASE[rel]
    assert data.count(old) == 1, (rel, old)
    return {rel: data.replace(old, new)}


def expect(tmp, what, same, a, b):
    sa, sb = stamp_of(tmp, a), stamp_of(tmp, b)
    if (sa == sb) != same:
        FAILED.append("%s: the stamp %s (%s, %s)" % (what, "moved" if same else "stayed", sa, sb))


def kept(tmp, what, rel, old, new):
    """An edit that cannot change a translation: the stamp stays"""
    expect(tmp, what, True, {}, edit(rel, old, new))


def moved(tmp, what, a, b):
    """Two texts that can translate differently: the stamps differ"""
    expect(tmp, what, False, a, b)


def file_pair(rel, a, b):
    return {rel: a}, {rel: b}


def test_kept(tmp):
    kept(tmp, "a comment line added to vk_shader.cpp", "vk_shader.cpp", b"static int", b"// one more note\nstatic int")
    kept(tmp, "a comment's words changed in vk_shader.cpp", "vk_shader.cpp", b"// What drives the translator",
         b"// What drives Eden's recompiler")
    kept(tmp, "a comment after code changed (vk_bank.cpp)", "vk_bank.cpp", b"// c0's words", b"// the words of c0")
    kept(tmp, "a block comment added over lines (eden)", "eden/shader_recompiler/x.cpp", b"int f(",
         b"/* f adds one:\n * it is the test's function\n */\nint f(")
    kept(tmp, "a block comment between tokens on one line", "eden/shader_recompiler/x.cpp", b"return a + 1;",
         b"return a /* the argument */ + 1;")
    kept(tmp, "a block comment over lines after code, before a line break", "eden/shader_recompiler/x.cpp",
         b"return a + 1;", b"return a + 1; /* one\n   more */")
    kept(tmp, "a comment in a header (vk_state.h)", "vk_state.h", b"// defaults", b"// the blank state's defaults")
    kept(tmp, "a comment in an .inc table", "eden/shader_recompiler/ops.inc", b"// the ops", b"// the IR's ops, one a line")
    kept(tmp, "blank lines added", "vk_shader.cpp", b"static int passes = 3;\n", b"\n\nstatic int passes = 3;\n\n\n")
    kept(tmp, "trailing white space", "vk_shader.cpp", b"static int passes = 3;\n", b"static int passes = 3;   \t\n")
    kept(tmp, "indentation changed", "eden/shader_recompiler/x.cpp", b"    return a + 1;", b"\treturn  a + 1;")
    kept(tmp, "Windows line ends", "vk_bank.cpp", b"\n", b"\r\n")
    kept(tmp, "a comment in a macro's line", "eden/shader_recompiler/x.h", b"#define X_ONE 1\n",
         b"#define X_ONE 1 // one\n")
    kept(tmp, "an apostrophe in a comment after digit separators", "vk_state.h", b"// a big one", b"// it's a big one")


def test_moved(tmp):
    k = "vk_shader.cpp"
    moved(tmp, "a token", {}, edit(k, b"passes = 3", b"passes = 4"))
    moved(tmp, "a line of code added", {}, edit(k, b"static int passes = 3;", b"static int passes = 3;\nstatic int more = 1;"))
    moved(tmp, "a string literal", {}, edit("sirit/src/sirit.cpp", b"\"sirit\"", b"\"Sirit\""))
    moved(tmp, "white space inside a string literal", {}, edit("sirit/src/sirit.cpp", b"\"sirit\"", b"\"sirit  \""))
    moved(tmp, "a macro's value", {}, edit("eden/shader_recompiler/x.h", b"X_ONE 1", b"X_ONE 2"))
    moved(tmp, "an #include", {}, edit(k, b"\"vk_state.h\"", b"\"vk_state2.h\""))
    moved(tmp, "a system #include", {}, edit("sirit/src/sirit.cpp", b"<vector>", b"<array>"))
    moved(tmp, "a comment marker inside a string literal",
          *file_pair(k, b"const char* s = \"a // b\";\n", b"const char* s = \"a // c\";\n"))
    moved(tmp, "a block comment marker inside a string literal",
          *file_pair(k, b"const char* s = \"/* a */\";\n", b"const char* s = \"/* b */\";\n"))
    moved(tmp, "code after a quote in a character literal",
          *file_pair(k, b"char q = '\"'; int a = 1; // \"\n", b"char q = '\"'; int a = 2; // \"\n"))
    moved(tmp, "code after an escaped quote in a string",
          *file_pair(k, b"const char* s = \"\\\" // \"; int a = 1;\n", b"const char* s = \"\\\" // \"; int a = 2;\n"))
    moved(tmp, "a digit separator's digits", *file_pair(k, b"int a = 1'000;\n", b"int a = 1'001;\n"))
    moved(tmp, "a comment between two tokens is a space (int/**/x against intx)",
          *file_pair(k, b"int/**/x;\n", b"intx;\n"))
    moved(tmp, "white space between two tokens where it counts (a + +b against a ++b)",
          *file_pair(k, b"int y = a + +b;\n", b"int y = a ++b;\n"))
    moved(tmp, "a macro's parameter list against its body (F(x) against F (x))",
          *file_pair(k, b"#define F(x) x\n", b"#define F (x) x\n"))
    moved(tmp, "a line break that ends a directive",
          *file_pair(k, b"#define A 1\n+ 2;\n", b"#define A 1 + 2;\n"))
    moved(tmp, "a comment over lines between two tokens, against a space (a # after it may start a directive)",
          *file_pair(k, b"int a; /* x\n */ #define B 1\n", b"int a; /* x */ #define B 1\n"))
    moved(tmp, "a comment over lines between two tokens, against a line break (in a directive it is a space)",
          *file_pair(k, b"#define A 1 /* x\n */ + 2\n", b"#define A 1\n + 2\n"))
    moved(tmp, "a raw string's lines that look like comments",
          *file_pair(k, b"const char* g = R\"(a\n// b\n)\";\n", b"const char* g = R\"(a\n// c\n)\";\n"))
    moved(tmp, "a raw string holding quotes and a comment marker on one line",
          *file_pair(k, b"const char* g = R\"x(a \" // b \" )x\";\n", b"const char* g = R\"x(a \" // c \" )x\";\n"))
    moved(tmp, "code after a comment continued by a splice that opens a block comment",
          *file_pair(k, b"// c \\\nfoo /*\nint a = 1; // */\n", b"// c \\\nfoo /*\nint a = 2; // */\n"))
    moved(tmp, "a raw string's blank lines",
          *file_pair(k, b"const char* g = R\"x(a\n)x\";\n", b"const char* g = R\"x(a\n\n)x\";\n"))
    moved(tmp, "a macro continued over a line splice",
          *file_pair(k, b"#define A(x) \\\n    ((x) + 1)\n", b"#define A(x) \\\n    ((x) + 2)\n"))
    moved(tmp, "the line after a comment that ends in a splice (part of the comment, or code if the splice is not seen)",
          *file_pair(k, b"int a; // note \\\nint b = 1;\n", b"int a; // note \\\nint b = 2;\n"))
    moved(tmp, "a comment ended by a backslash and a blank (GCC and clang splice them: the next line is comment)",
          *file_pair(k, b"int a; // note\nint b = 1;\n", b"int a; // note \\ \nint b = 1;\n"))
    moved(tmp, "a block comment ended early by a star, a backslash and a blank before the line end, then a slash",
          *file_pair(k, b"/* x *\\ \n/ int b = 1; // */\n", b"/* x *\\ \n/ int b = 2; // */\n"))
    moved(tmp, "code in an #if 0 block (left as it is)",
          *file_pair(k, b"#if 0\nint a = 1;\n#endif\n", b"#if 0\nint a = 2;\n#endif\n"))
    moved(tmp, "a block comment end inside a string in code",
          *file_pair(k, b"/* a */ const char* s = \"*/\"; int a = 1;\n", b"/* a */ const char* s = \"*/\"; int a = 2;\n"))
    moved(tmp, "a CMake file's comment (not C or C++: byte for byte)", {}, edit("eden/CMakeLists.txt", b"# eden", b"# Eden"))
    moved(tmp, "a licence file (byte for byte)", {}, edit("sirit/LICENSE", b"Licence text", b"Licence  text"))
    moved(tmp, "a file renamed", {}, {"eden/shader_recompiler/x.cpp": None,
                                      "eden/shader_recompiler/y.cpp": BASE["eden/shader_recompiler/x.cpp"]})
    moved(tmp, "a comment-only file added (a new file is a new path)", {}, {"eden/shader_recompiler/z.h": b"// nothing yet\n"})


def test_real_tree(tmp):
    """The real translator's files: comments and blank lines added to vk_shader.cpp, the first recompiler source and
    vk_state.h keep the stamp, and a token changed in vk_shader.cpp moves it"""
    copy = os.path.join(tmp, "real", "vk")
    for f in tf.files(VK):
        rel = os.path.relpath(f, VK)
        dst = os.path.join(copy, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(f, dst)
    before = tf.stamp(copy)
    first_cpp = next(f for f in tf.files(copy) if f.endswith(".cpp"))

    def append(path, text):
        with open(path, "ab") as f:
            f.write(text)
    append(os.path.join(copy, "vk_shader.cpp"), b"\n// (a note added by tests/test_translator_stamp_text.py)\n\n")
    append(first_cpp, b"\n/* a note\n   over two lines */\n")
    append(os.path.join(copy, "vk_state.h"), b"\n\n   \n")
    after = tf.stamp(copy)
    if after != before:
        FAILED.append("the real tree: comments and blank lines at the end of vk_shader.cpp, %s and vk_state.h moved the "
                      "stamp (%s, %s)" % (os.path.relpath(first_cpp, copy), before, after))
    append(os.path.join(copy, "vk_shader.cpp"), b"static int tests_translator_stamp_text = 1;\n")
    if tf.stamp(copy) == after:
        FAILED.append("the real tree: a line of code added to vk_shader.cpp kept the stamp")


def main():
    tmp = tempfile.mkdtemp(prefix="stamp-text-")   # (about 3 MB: the real tree's stamped sources once, and small trees)
    try:
        test_kept(tmp)
        test_moved(tmp)
        test_real_tree(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILED:
        print("FAIL: %d case(s)" % len(FAILED))
        for f in FAILED:
            print("  " + f)
        return 1
    print("ok: the translator stamp keeps comment, blank-line and white-space edits and moves with every other")
    return 0


if __name__ == "__main__":
    sys.exit(main())
