#!/usr/bin/env python3
"""List every environment switch HeyListen reads, and regenerate the reference documentation.

    python -I engine/scripts/list_switches.py            # rewrite engine/docs/switches.json and docs/SWITCHES.md
    python -I engine/scripts/list_switches.py --check    # exit 1 when either file is out of date (the test's check)

A switch is any environment variable the engine, the renderer, the builder or the lifter reads: `getenv("...")`,
`std::getenv`, `_wgetenv`, the code's own helpers (`env_on`, `env_int`, `env_flag`, `env_switch`, `EnvSwitch`, and the
small wrappers that take a name and call `getenv` on it), table-driven reads (`getenv(table[i].env)`), and
`os.environ`/`os.environ.get`/`os.environ[...]` in the Python the builder and the lifter run.

For every name the generator records where it is read, the default and the accepted values when the code shows them,
and the explanation from the source comment nearest the read -- or the existing description in
`engine/docs/development.md`, `engine/readme.md` or another document. A switch with no comment and no description is
written as `undocumented: <what the code does>`, never guessed.

Engine tests (`engine/tests`) and the vendored trees other than a64-kit are outside the scan: their environment
variables are test fixtures and third-party libraries, not HeyListen switches.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(HERE)
REPO = os.path.dirname(ENGINE)
JSON_PATH = os.path.join(ENGINE, "docs", "switches.json")
MD_PATH = os.path.join(REPO, "docs", "SWITCHES.md")

C_EXT = {".c", ".h", ".cpp", ".cc", ".hpp"}
PY_EXT = {".py"}
# (folders whose own environment is test scaffolding or a vendored library, not a HeyListen switch)
C_SKIP_DIRS = {"tests", "work", "__pycache__", "sixel", "sirit", "boost", "ankerl", "fmt", "host_shaders"}
PY_SKIP_DIRS = {"tests", "work", "__pycache__", "eden", "sirit", "prompt_faces"}

# Environment names are the strings we treat as switches. Third-party variables kept in the scan:
NAME_RE = re.compile(r"^(?:RX|HOS|HL|HOOKSHOT|A64|EDEN|SUYU|WGPU|OOT|NVN|TOTK|MESA|RADV|SDL|LONGSHOT|XDG|CRASH|"
                     r"DISPLAY|WAYLAND|GAMESCOPE|SteamDeck|EMSDK|VULKAN_SDK|GH_TOKEN|RUNNER_TEMP|APPDIR|HOME|TMPDIR|"
                     r"USERPROFILE|USERNAME|LOCALAPPDATA|APPDATA|LANG|INCLUDE|LIB|PYTHONIOENCODING|CXX)_?[A-Za-z0-9_]*$")

# The code's own readers (besides getenv itself). A name read only through one of these still counts.
HELPER_RE = re.compile(r"\b(?:env_on|env_int|env_flag|env_switch|EnvSwitch)\s*\(\s*\"([^\"]+)\"")
GETENV_RE = re.compile(r"\b(?:getenv|std::getenv)\s*\(\s*\"([^\"]+)\"")
WGETENV_RE = re.compile(r"\b_wgetenv\s*\(\s*L\"([^\"]+)\"")
ENVISH_RE = re.compile(r"\b(?:getenv|std::getenv|_wgetenv)\s*\(\s*\(?\s*(?:const\s+char\s*\*\s*)?([A-Za-z_]\w*)\s*\)?\s*\)")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def blank_comments(text):
    """Comments blanked out, everything else (and every line/column) kept: a getenv in a comment is not a read."""
    def repl(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    # (strings first: a // inside a URL or a path is not a comment)
    parts = re.split(r'("(?:\\.|[^"\\\n])*")', text)
    for i in range(0, len(parts), 2):
        parts[i] = re.sub(r"/\*.*?\*/|//[^\n]*", repl, parts[i], flags=re.S)
    return "".join(parts)


def comment_blocks(text):
    """Comments as blocks: [(first_line, last_line, text)]; consecutive or adjacent comment lines merged."""
    raw = []
    for m in re.finditer(r"/\*.*?\*/|//[^\n]*", text, flags=re.S):
        first = text.count("\n", 0, m.start()) + 1
        last = text.count("\n", 0, m.end()) + 1
        body = m.group(0)
        body = re.sub(r"/\*+|\*+/", " ", body)
        body = re.sub(r"(?m)^\s*//+ ?", "", body)
        body = re.sub(r"(?m)^\s*\*+ ?", "", body)
        body = re.sub(r"\s+", " ", body).strip()
        raw.append((first, last, body))
    blocks = []
    for first, last, body in raw:
        if blocks and first - blocks[-1][1] <= 1:
            blocks[-1] = (blocks[-1][0], max(blocks[-1][1], last), (blocks[-1][2] + " " + body).strip())
        else:
            blocks.append((first, last, body))
    return blocks


def func_ranges(code):
    """[(name, first_line, last_line)] for the C/C++ function bodies, by brace matching."""
    out = []
    for m in re.finditer(r"(?m)^[ \t]*(?:[A-Za-z_][\w:<>,*&\s]*?)\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*(?:const\s*)?\{", code):
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "catch", "return"):
            continue
        i = code.find("{", m.start())
        depth, j = 0, i
        while j < len(code):
            if code[j] == "{":
                depth += 1
            elif code[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        first = code.count("\n", 0, m.start()) + 1
        last = code.count("\n", 0, j) + 1
        out.append((name, first, last))
    return out


def wrappers(files):
    """Functions and constructors that take an environment name from a literal and read it with getenv on a parameter."""
    found = {}
    for path in files:
        text = read(path)
        code = blank_comments(text)
        lines = code.split("\n")
        for name, first, last in func_ranges(code):
            body = "\n".join(lines[first - 1:last])
            for m in ENVISH_RE.finditer(body):
                param = m.group(1)
                sig = re.search(r"\b" + re.escape(name) + r"\s*\(([^;{}()]*)\)", code)
                if not sig:
                    continue
                params = [p.strip() for p in sig.group(1).split(",")]
                for k, p in enumerate(params):
                    bare = re.sub(r"\b(?:const|char|wchar_t|std::string|auto)\b|[*&\[\]\s]", "", p)
                    if bare == param:
                        found.setdefault(name, set()).add(k)
                        break
    return found


def table_names(code):
    """Names read from a table (`getenv(info[i].env)`, `getenv(list[i])`): the literals in the table's initializer."""
    names = {}
    for m in re.finditer(r"\b(?:getenv|std::getenv|_wgetenv)\s*\(\s*\(?\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:\.|->)?\s*([A-Za-z_]\w*)?\s*\)", code):
        table = m.group(1)
        # the table's initializer: NAME... = { ... } (brace matched)
        im = re.search(r"\b" + re.escape(table) + r"\s*(?:\[[^\]]*\])*\s*=\s*\{", code)
        if not im:
            continue
        i = code.find("{", im.start())
        depth, j = 0, i
        while j < len(code):
            if code[j] == "{":
                depth += 1
            elif code[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        block = code[i:j]
        lits = [x for x in re.findall(r'"([A-Za-z][A-Za-z0-9_]{2,})"', block) if NAME_RE.match(x)]
        if lits:
            names.setdefault(table, []).extend(lits)
    return names


def scan_c():
    """[(name, relpath, line)] for every C/C++ read."""
    occ = []
    files = []
    for dp, dn, fn in os.walk(ENGINE):
        dn[:] = [d for d in dn if d not in C_SKIP_DIRS]
        for f in fn:
            if os.path.splitext(f)[1] in C_EXT:
                files.append(os.path.join(dp, f))
    macros = {}
    for path in files:
        code = blank_comments(read(path))
        for m in re.finditer(r'(?m)^\s*#\s*define\s+([A-Za-z_]\w*)\s+"([^"]+)"', code):
            macros.setdefault(m.group(1), m.group(2))
        for m in re.finditer(r"(?m)^\s*#\s*define\s+([A-Za-z_]\w*)\s+\(?(-?\d+)\)?\s*(?:$|//)", code):
            MACROS.setdefault(m.group(1), m.group(2))
    wrap = wrappers(files)
    for path in files:
        rel = os.path.relpath(path, ENGINE).replace(os.sep, "/")
        text = read(path)
        code = blank_comments(text)
        tables = table_names(code)
        for i, line in enumerate(code.split("\n"), 1):
            hits = set(GETENV_RE.findall(line)) | set(HELPER_RE.findall(line)) | set(WGETENV_RE.findall(line))
            for m in re.finditer(r"\b(?:getenv|std::getenv|_wgetenv)\s*\(\s*([A-Za-z_]\w*)\s*\)", line):
                name = macros.get(m.group(1))
                if name and NAME_RE.match(name):
                    hits.add(name)
            for w, idxs in wrap.items():
                for call in re.finditer(r"\b" + re.escape(w) + r"\s*\(([^()]*)\)", line):
                    parts = split_args(call.group(1))
                    for k in idxs:
                        if k < len(parts):
                            lit = re.search(r'"([^"]+)"', parts[k])
                            if lit:
                                hits.add(lit.group(1))
            for n in hits:
                occ.append((n, rel, i))
            for m in re.finditer(r"\b(?:getenv|std::getenv|_wgetenv)\s*\(\s*\(?\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:\.|->)?\s*([A-Za-z_]\w*)?\s*\)", line):
                for n in tables.get(m.group(1), []):
                    occ.append((n, rel, i))
    return occ


def split_args(s):
    parts, depth, cur = [], 0, ""
    for ch in s:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    return parts


PY_RE = re.compile(r"(?:os\.environ(?:\.get)?|os\.getenv|environ(?:\.get)?|\benv)\s*(?:\.get|\.setdefault)?\s*[\(\[]\s*"
                   r"[\"']([A-Za-z_][A-Za-z0-9_]*)[\"']")


def scan_py():
    occ = []
    for dp, dn, fn in os.walk(ENGINE):
        dn[:] = [d for d in dn if d not in PY_SKIP_DIRS]
        for f in fn:
            if os.path.splitext(f)[1] not in PY_EXT:
                continue
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, ENGINE).replace(os.sep, "/")
            for i, line in enumerate(read(path).split("\n"), 1):
                for m in PY_RE.finditer(line):
                    if NAME_RE.match(m.group(1)):
                        occ.append((m.group(1), rel, i))
    return occ


# ----------------------------------------------------------------------------- descriptions from the documents

def md_rows(path, table):
    """Markdown table rows -> [(first cell, rest)]. `table` keeps every row's text by name."""
    for line in read(path).split("\n"):
        s = line.strip()
        if not s.startswith("|"):
            continue
        cells = [c.strip() for c in s.strip("|").split("|")]
        if len(cells) < 2 or set(cells[0]) <= set("-: "):
            continue
        desc = " ".join(c for c in cells[1:] if c).strip()
        if not desc:
            continue
        for name in re.findall(r"\b([A-Z][A-Z0-9_]{2,})\b", cells[0]):
            if NAME_RE.match(name) and name not in table:
                table[name] = (desc, "development.md")


def md_paragraphs(path, table, source, require_assign=False):
    text = read(path)
    text = re.sub(r"\n(?=[-*] |\d+\. )", "\n\n", text)      # (a bullet list is one entry each, not one paragraph)
    prev = ""
    for para in re.split(r"\n\s*\n", text):
        if para.strip().startswith("|"):
            continue
        flat = re.sub(r"^\s*```[A-Za-z]*\s*|\s*```\s*$", "", para.strip())
        flat = re.sub(r"\s+", " ", flat).strip()
        if not flat:
            continue
        # (a shell or command-line sample is no description of a variable)
        if re.match(r"(?i)^(cd|sudo|python|py|git|cmake|make|ninja|export|set|powershell|bash|sh|\.\/)\b", flat):
            prev = flat
            continue
        # (a code sample ("```ini RX_X=1 ```") belongs to the paragraph that introduces it)
        if len(flat) < 90 and re.search(r"[A-Z][A-Z0-9_]{2,}=", flat) and prev:
            flat = (prev + " " + flat).strip()
        found = set(re.findall(r"\b([A-Z][A-Z0-9_]{2,})\b", flat))
        found = {n for n in found if NAME_RE.match(n)}
        if len(found) >= 4:                                # (a list of switches is no one switch's description)
            prev = flat
            continue
        for name in found:
            if require_assign and not re.search(re.escape(name) + r"\s*=", flat):
                continue
            if name not in table:
                table[name] = (flat, source)
        prev = flat


def doc_descriptions():
    table = {}
    dev = os.path.join(ENGINE, "docs", "development.md")
    if os.path.exists(dev):
        md_rows(dev, table)
        md_paragraphs(dev, table, "development.md")
    readme = os.path.join(ENGINE, "readme.md")
    if os.path.exists(readme):
        md_paragraphs(readme, table, "readme.md")
    # (the other design notes carry a switch or two; the raw perf audit is a log, not a description)
    for sub in ("docs", "docs/native-engine", "docs/release"):
        d = os.path.join(ENGINE, sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith(".md") and f != "perf-audit.md":
                md_paragraphs(os.path.join(d, f), table, sub + "/" + f, require_assign=True)
    return table


def condense(text, name, limit=900):
    """A long paragraph trimmed to the sentences around the name."""
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) <= limit:
        return text
    sentences = re.split(r"(?<=[.!?])\s+(?=[A-Z(])", text)
    keep, on = [], False
    for s in sentences:
        if name in s:
            on = True
        if on:
            keep.append(s)
            if sum(len(x) + 1 for x in keep) >= limit * 0.7:
                break
    out = " ".join(keep) if keep else text
    if len(out) > limit:
        out = out[:limit].rsplit(" ", 1)[0] + " ..."
    return out


def tidy(text):
    text = re.sub(r"\s+", " ", text).strip()
    text = re.sub(r"^[-=*_]{2,}\s*", "", text)          # ("---- (TICK-021) ...")
    text = re.sub(r"^[-*]\s+", "", text)                # (a bullet's marker)
    text = text.replace("( ", "(").replace(" )", ")")
    if text and text.count("(") > text.count(")"):
        text = re.sub(r"^[(\[]\s*", "", text)
    # (capitalise the first word, not the 'h' of "(hookshot)")
    m = re.search(r"[A-Za-z]", text)
    if m:
        i = m.start()
        text = text[:i] + text[i].upper() + text[i + 1:]
    if text and text[-1] not in ".!?":
        text += "."
    return text


def focus(text, name, limit=900):
    """A long comment block about several switches: the sentences around this one (no other switch named)."""
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) <= 600:
        return text
    others = set(re.findall(r"\b([A-Z][A-Z0-9_]{4,})\b", text))
    others = {o for o in others if NAME_RE.match(o) and o != name}
    if not others:
        return condense(text, name, limit)
    sentences = re.split(r"(?<=[.!?])\s+(?=[A-Z(])", text)
    hits = [i for i, s in enumerate(sentences) if name in s]
    if not hits:
        return condense(text, name, limit)
    lo, hi = max(0, hits[0] - 1), min(len(sentences), hits[-1] + 2)
    return condense(" ".join(sentences[lo:hi]), name, limit)


STOP_WORDS = {"read", "reads", "once", "set", "off", "on", "default", "before", "switch", "switches", "env",
              "environment", "variable", "the", "a", "an", "to", "is", "it", "in", "and", "or", "of", "for", "by",
              "with", "as", "at", "from", "this", "that", "case", "see", "below", "above", "same", "than", "when",
              "its", "be", "are", "was", "were", "not", "no", "yes", "only", "here", "there"}


def weak(text, name):
    """A "comment" that only repeats the name ("RX_X, read once") is no explanation."""
    words = re.sub(r"[^a-z ]", " ", text.replace(name, " ").lower()).split()
    return len([w for w in words if w not in STOP_WORDS]) < 5


# ----------------------------------------------------------------------------- defaults, values, what the code does

def statement(code_lines, line, span=4):
    """The statement around a read, joined: its own continuation lines, not the neighbours' (a default of another
    switch on the line above must not be taken for this one)."""
    lo = line - 1
    for _ in range(span):                     # (back over a statement that wraps)
        if lo <= 0:
            break
        prev = code_lines[lo - 1].rstrip()
        if not prev or prev.endswith((";", "{", "}", ":")):
            break
        lo -= 1
    hi = line
    for _ in range(span + 4):
        if hi >= len(code_lines):
            break
        s = code_lines[hi - 1].rstrip()
        if s.endswith((";", "{", "}", ":")) or (hi > line and not s.startswith((" ", "\t")) and s and
                                                 not s.lstrip().startswith(("?", ":"))):
            break
        hi += 1
    return "\n".join(code_lines[lo:hi])


def read_default(win, name):
    q = re.escape(name)
    m = re.search(r"env_int\s*\(\s*\"" + q + r"\"\s*,\s*([-\w]+)\s*\)", win)
    if m:
        return num_or_own(m.group(1)), "integer"
    m = re.search(r"env_flag\s*\(\s*\"" + q + r"\"\s*,\s*([-\w]+)\s*\)", win)
    if m:
        return num_or_own(m.group(1)), "on/off"
    m = re.search(r"EnvSwitch\s*\(\s*\"" + q + r"\"\s*,\s*([-\w]+)\s*\)", win)
    if m:
        return ("on" if m.group(1) == "true" else "off" if m.group(1) == "false" else m.group(1)), "on/off"
    # (read into a local first: the use of it on the following lines gives the default)
    m = re.search(r"(?:const\s+)?char\s*\*\s*(\w+)\s*=\s*(?:std::)?(?:getenv|_wgetenv)\s*\(\s*L?\"" + q + r"\"\s*\)", win)
    if m:
        v = re.escape(m.group(1))
        tail = win[m.end():]
        if re.search(r"!\s*" + v + r"\b[^;]*\|\|", tail) or re.search(r"!\s*\(\s*" + v + r"\s*&&", tail):
            return "on (unless set to 0, false, off or no)", "on/off"
        m2 = re.search(v + r"\s*\?\s*[^:;]{0,60}?:\s*([-\w.\"]+)", tail)
        if m2 and re.search(r"(?:atoi|atoll|atof|strtol|strtoull|strtod|strtoul)\s*\(\s*" + v, tail):
            return num_or_own(m2.group(1)), "integer"
        if re.search(r"if\s*\(\s*!\s*" + v, tail):
            return "on (unset = off)", "on/off"
        if re.search(v + r"\s*!=\s*(?:nullptr|NULL|0)", tail) or re.search(r"if\s*\(\s*" + v + r"\s*\)", tail):
            return "off (unset)", "on/off"
        if re.search(r"(?:atoi|atoll|atof|strtol|strtoull|strtod|strtoul)\s*\(\s*" + v, tail):
            return "0 (unset)", "integer"
    m = re.search(r"!\s*(?:getenv|env_flag|env_on)\s*\(\s*\"" + q + r"\"[^;]*\|\|", win)
    if m:
        return "on (unless set to 0, false, off or no)", "on/off"
    m = re.search(r"(?:getenv|env_on|env_flag|env_int|env_switch)\s*\(\s*\"" + q + r"\"\s*\)[^;?]*\?\s*(.*?):\s*"
                  r"([-\w.\"]+)\s*[;)]", win, re.S)
    if m:
        expr, dflt = m.group(1), m.group(2)
        return num_or_own(dflt), ("integer" if re.search(r"atoi|atoll|atof|strto", expr) else "text")
    if re.search(r"env_switch\s*\(\s*\"" + q + r"\"\s*\)", win):
        return "-1 (unset)", "integer"
    if re.search(r"env_(?:on|flag)\s*\(\s*\"" + q + r"\"\s*\)", win):
        return "off (unset)", "on/off"
    if re.search(r"getenv\s*\(\s*\"" + q + r"\"\s*\)\s*!=\s*(?:nullptr|NULL|0)", win):
        return "off (unset)", "on/off"
    if re.search(r"\(\s*(?:getenv|env_[a-z]+)\s*\(\s*\"" + q + r"\"\s*\)", win) or \
       re.search(r"\bif\s*\(\s*(?:getenv|env_[a-z]+)\s*\(\s*\"" + q + r"\"\s*\)\s*\)", win):
        return "off (unset)", "on/off"
    if re.search(r"(?:atoi|atoll|atof|strtoull|strtol|strtod)\s*\(\s*(?:getenv|env_[a-z]+)\s*\(\s*\"" + q + r"\"", win):
        return "0 (unset)", "integer"
    return "unset", "text"


MACROS = {}


def num_or_own(v):
    v = v.strip().strip('"')
    if re.match(r"^-?\d+$", v):
        return v
    if v in MACROS and re.match(r"^-?\d+$", MACROS[v]):
        return MACROS[v]
    if v == "":
        return "empty (unset)"
    if v in ("true", "false"):
        return v
    return "the engine's own value"


def values_for(kind, win, desc, name=""):
    """A short values line: what the code accepts, or the options the comment names."""
    text = win + " " + (desc or "")
    opts = []
    for m in re.finditer(r"=\s*([-\w]{1,12})(?:\s*\(([^)]{2,40})\))?", text):
        v = m.group(1)
        if v.lower() in ("0", "1", "2", "3", "4", "5", "6", "8", "true", "false", "off", "on", "no", "yes") and v not in opts:
            opts.append(v)
    if kind == "on/off":
        base = "on or off (0, false, off, no and empty are off)"
        return base + (("; the description names " + ", ".join("`" + o + "`" for o in opts)) if opts else "")
    if kind == "integer":
        return "a whole number" + ((" (" + ", ".join("`" + o + "`" for o in opts) + " in the description)") if opts else "")
    if re.search(r"_(?:FILE|DIR|PATH|HOME)$", name) or " a folder " in text or " a path " in text:
        return "a path (text)"
    return "text" + ((" (" + ", ".join("`" + o + "`" for o in opts) + " named)") if opts else "")


def enclosing(code, line, funcs):
    for name, first, last in funcs:
        if first <= line <= last:
            return name
    return None


def undocumented(name, rel, line, code_lines, funcs, style):
    fn = enclosing(blank_comments("\n".join(code_lines)), line, funcs)
    here = "%s:%d" % (rel, line)
    win = statement(code_lines, line)
    snippet = code_lines[line - 1].strip() if 0 <= line - 1 < len(code_lines) else ""
    snippet = re.sub(r"\s+", " ", snippet)
    if len(snippet) > 120:
        snippet = snippet[:117] + "..."
    var = re.search(r"=\s*(?:static\s+)?(?:const\s+)?(?:bool|int|uint32_t|uint64_t|size_t|auto)\s+(\w+)\s*=", win)
    var = var.group(1) if var else None
    usage = ""
    if var:
        tail = "\n".join(code_lines[line:line + 6])
        if re.search(r"if\s*\(\s*!\s*" + re.escape(var) + r"\b[^)]*\)\s*return", tail):
            usage = "; with it unset, the code returns before the path it guards"
        elif re.search(r"if\s*\(\s*" + re.escape(var) + r"\b", tail):
            usage = "; it guards the block after the read"
        elif re.search(r"return\s+" + re.escape(var) + r"\b", tail):
            usage = "; it is returned as the choice"
        elif re.search(re.escape(var) + r"\s*\?", tail):
            usage = "; it picks between two branches"
    quote = (": `%s`" % snippet) if snippet else ""
    if style == "on/off":
        what = "an on/off switch"
    elif style == "integer":
        what = "a number"
    elif re.search(r"_FILE$|_DIR$|_HOME$|_PATH$", name) or "join(" in snippet or "path" in snippet:
        what = "a path"
    else:
        what = "a text value"
    if fn:
        return ("undocumented: %s (in `%s()`) reads it as %s%s%s. No comment in the source says what it changes."
                % (here, fn, what, quote, usage))
    return ("undocumented: %s reads it as %s%s%s. No comment in the source says what it changes."
            % (here, what, quote, usage))


# ----------------------------------------------------------------------------- areas, audiences, the top table

AREAS = [
    ("telemetry", "Telemetry and crash reports", ("HL_TELEMETRY", "HOS_TELEMETRY", "HL_PERF", "HOS_CRASH", "HL_CRASH", "CRASH_")),
    ("audio", "Audio", ("HOS_AUDIO", "RX_AUDIO", "SDL_AUDIO", "HOS_MIC")),
    ("input", "Input and controllers", ("HOS_INPUT", "HOS_HID", "HOS_PAD", "HOS_MOTION", "HOS_MOUSE", "HOS_JOY",
                                        "HOS_CONTROLLER", "SDL_GAMECONTROLLER", "SDL_JOYSTICK")),
]

LOG_TOKENS = {"LOG", "TRACE", "DUMP", "STATS", "PROBE", "PROFILE", "SAMPLE", "WATCH", "SCREENSHOT", "SHOT", "SHOTS",
              "FRAMELOG", "PERF", "TIMING", "DEBUG", "VERBOSE", "CHECK", "CHECKPOINTS", "VALIDATION", "HANG", "SPY",
              "TIMES", "TELEMETRY"}
MEM_TOKENS = {"TEX", "TEXTURE", "ASTC", "BC", "VRAM", "MIRROR", "TARGET", "POOL", "MEM", "MEMORY", "PAGE", "PAGES",
              "HEAP", "UPLOAD", "WRITEBACK", "WB", "CAPTURE", "GUEST", "IMPORT", "SPARSE", "DMABUF", "COMMIT", "EVICT",
              "SPARE", "IMAGE", "TILE", "STORAGE", "SHADOW", "DESC", "ALLOC", "GPU"}
SHADER_TOKENS = {"SHADER", "SPIRV", "IR", "LOWER", "TRANSLATE", "WGSL", "GLSL", "F16", "F32", "MMA", "COOPMAT", "PIPE",
                 "PROG", "PROGRAM", "PCACHE", "PRECOMPILE", "COMPUTE", "DISPATCH", "MESH", "DRAW", "TASK", "VERTEX",
                 "INDEX", "SSBO", "UBO", "BIND", "BARRIER", "SYNC", "MSYNC", "FLUSH", "STAGE", "BAKE", "SPECIALIZE",
                 "VARIANT", "KEY", "MATRIX"}
RENDER_TOKENS = {"UPSCALE", "DLSS", "FSR", "FRAMEGEN", "FG", "TAA", "TAAU", "SCALE", "SHARPEN", "PRESENT", "PACE",
                 "WINDOW", "DISPLAY", "FULLSCREEN", "VSYNC", "VRR", "HDR", "RES", "ASPECT", "HUD", "OVERLAY", "UI",
                 "FRAME", "REFLEX", "LATENCY", "JITTER", "MOVIE", "VIDEO", "OCC", "OCCLUSION", "RT", "REFRESH",
                 "TONEMAP", "GSYNC", "OUTPUT", "TV", "NETWORK"}
HOST_NAMES = {"SDL_VIDEODRIVER", "SDL_VIDEO_DRIVER", "DISPLAY", "WAYLAND_DISPLAY", "GAMESCOPE_WAYLAND_DISPLAY",
              "MESA_VK_ENABLE_SUBMIT_THREAD", "RADV_FORCE_VRS_CONFIG_FILE", "SteamDeck", "HOME", "TMPDIR", "USER",
              "USERPROFILE", "USERNAME", "LOCALAPPDATA", "APPDATA", "XDG_CURRENT_DESKTOP", "XDG_DATA_HOME",
              "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE", "CRASH_TEST_CURL", "LONGSHOT_PREPARE_ONLY", "LOWER_TEST_LIST_OK",
              "NVN_NO_POOL", "TOTK_MINIMAL_LAYOUT", "SUYU_RECOMP_STAGING_LAYOUT", "SUYU_RECOMP_FPX_SHADOW_LOG",
              "SUYU_RECOMP_STRICT"}
BUILD_NAMES = {"RX_CPM", "RX_TARGET", "RX_RELEASE", "RX_PGO", "RX_MARCH", "RX_JOBS", "RX_MAPS", "WGPU_NATIVE", "EMSDK",
               "VULKAN_SDK", "INCLUDE", "LIB", "LANG", "PYTHONIOENCODING", "RUNNER_TEMP", "GH_TOKEN", "APPDIR",
               "CXX", "RX_HOT_FLAGS", "RX_HOT_OPT", "RX_NO_ORDER", "RX_SOFT_WRITE_WATCH", "RX_VK_ONLY", "RX_VK_PGO",
               "A64_SITE_PROFILE"}
BUILD_DIRS = ("nx2/", "scripts/", "builder/", "third_party/")


def classify(name, files):
    parts = set(name.split("_"))
    all_build = all(f.startswith(BUILD_DIRS) for f in files)
    if name.startswith(("A64_", "HOOKSHOT_")) or name in BUILD_NAMES or (all_build and not name.startswith(("HOS_", "HL_"))):
        if name.startswith("A64_"):
            return "build", "Lift (A64_*)", "developer"
        if name.startswith("HOOKSHOT_"):
            return "build", "Builder and toolchain", "developer"
        return "build", "Build behaviour and paths", "developer"
    if name.startswith(("HL_TELEMETRY", "HOS_TELEMETRY", "HL_PERF", "HOS_CRASH", "HL_CRASH", "CRASH_")):
        return "telemetry", "Telemetry" if "TELEMETRY" in name else "Crash reports and crash window", "diagnostic"
    for a, prefixes in (("audio", ("HOS_AUDIO", "RX_AUDIO", "SDL_AUDIO", "HOS_MIC")),
                        ("input", ("HOS_INPUT", "HOS_HID", "HOS_PAD", "HOS_MOTION", "HOS_MOUSE", "HOS_JOY",
                                   "HOS_CONTROLLER", "SDL_GAMECONTROLLER", "SDL_JOYSTICK"))):
        if name.startswith(prefixes):
            group = audio_group(name) if a == "audio" else input_group(name)
            return a, group, "player" if name in PLAYER_USE else "diagnostic"
    if name.startswith("HL_"):
        return "native", native_group(name), ("diagnostic" if parts & LOG_TOKENS else
                                              "player" if name in PLAYER_USE else "developer")
    if parts & LOG_TOKENS:
        return "logging", log_group(name), "diagnostic"
    if parts & {"FPS", "FRAMERATE"}:
        if name.startswith("HL_"):
            return "native", "Timing and frame rate", "developer"
        return "host", "Frame rate and timing", "player" if name in PLAYER_USE else "developer"
    if parts & MEM_TOKENS:
        group = "Textures, uploads and write-backs" if parts & {"TEX", "TEXTURE", "ASTC", "BC", "UPLOAD", "WRITEBACK",
                                                                "WB", "IMAGE", "TILE"} else \
                "VRAM, pools and mirrors" if parts & {"VRAM", "MIRROR", "TARGET", "POOL", "COMMIT", "EVICT", "SPARE"} \
                else "Guest memory and captures"
        return "memory", group, "player" if name in PLAYER_USE else "developer"
    if parts & SHADER_TOKENS:
        group = "Pipelines and the program cache" if parts & {"PIPE", "PIPELINE", "PROG", "PROGRAM", "PCACHE",
                                                             "PRECOMPILE", "CACHE", "COMPILE", "SPECIALIZE", "VARIANT",
                                                             "KEY"} else \
                "Mesh, draws and synchronization" if parts & {"MESH", "DRAW", "TASK", "VERTEX", "INDEX", "DESC", "SSBO",
                                                              "UBO", "BIND", "BARRIER", "SYNC", "MSYNC", "FLUSH",
                                                              "STAGE", "DISPATCH", "COMPUTE"} else "Shader translation"
        return "shaders", group, "player" if name in PLAYER_USE else "developer"
    if parts & RENDER_TOKENS:
        group = "Frame generation" if parts & {"FRAMEGEN", "FG"} else \
                "Upscaling (DLSS, FSR, TAAU)" if parts & {"UPSCALE", "DLSS", "FSR", "TAA", "TAAU", "SHARPEN", "SCALE",
                                                          "NETWORK"} else \
                "HDR, V-Sync and presentation" if parts & {"HDR", "VSYNC", "VRR", "PACE", "PRESENT", "WINDOW", "DISPLAY",
                                                           "FULLSCREEN", "RES", "ASPECT", "REFRESH", "JITTER", "REFLEX",
                                                           "LATENCY", "UI", "HUD", "OVERLAY", "OUTPUT"} else "Rendering and GPU"
        audience = "player" if name in PLAYER_USE else "diagnostic" if parts & {"PRESENT", "WINDOW", "DISPLAY"} else "developer"
        return "renderer", group, audience
    if name.startswith("HOS_") or name in HOST_NAMES:
        if name.startswith("HOS_NVN"):
            return "host", "NVN graphics driver", "developer"
        if parts & {"THREAD", "THREADS", "VCORE", "VCORES", "CORE", "CORES", "SLEEP", "JOB", "JOBTRACE", "PRIO",
                    "AFFINITY", "SPIN", "MULTIWAIT", "CV"}:
            return "host", "Threads and scheduling", "player" if name in PLAYER_USE else "developer"
        if parts & {"FILE", "FILES", "PATH", "DIR", "MODS", "SAVE", "SAVES", "HOME", "TMPDIR", "ENV", "APPDATA",
                    "LOCALAPPDATA", "USERPROFILE", "USERNAME", "XDG", "APPDIR"}:
            return "host", "Files, paths and folders", "player" if name in PLAYER_USE else "developer"
        if parts & {"WINDOW", "DISPLAY", "DPI", "FULLSCREEN", "WAYLAND", "SDL", "GAMESCOPE"}:
            return "renderer", "HDR, V-Sync and presentation", "player" if name in PLAYER_USE else "diagnostic"
        return "host", "System and services", "player" if name in PLAYER_USE else "developer"
    return "renderer", "Rendering and GPU", "player" if name in PLAYER_USE else "developer"


def audio_group(name):
    p = set(name.split("_"))
    if p & LOG_TOKENS:
        return "Audio diagnostics"
    return "Output device and mixing" if p & {"DEVICE", "CHANNELS", "OUTPUT", "SAMPLE", "FRAMES", "MUTE", "VOLUME"} \
        else "Mixing and effects"


def input_group(name):
    p = set(name.split("_"))
    if p & {"SCRIPT", "LIVE", "BACKGROUND", "EXCLUSIVE", "FRAME", "CLOCK", "FROM", "PLAY"}:
        return "Scripted and background input"
    if p & {"MOUSE", "KEY", "KEYBOARD", "SENS"}:
        return "Keyboard and mouse"
    if p & LOG_TOKENS:
        return "Input diagnostics"
    return "Controllers and motion"


def native_group(name):
    p = set(name.split("_"))
    if p & {"MAINLOOP", "LOOP"}:
        return "Main loop"
    if p & {"FPS", "FRAME", "STEP", "TIMING", "SLEEP", "HIGH"}:
        return "Timing and frame rate"
    if p & {"TRACE", "REC", "HOOK", "PATCH", "GFX", "SCAN", "GATES"}:
        return "Hooks and tracing"
    if p & LOG_TOKENS:
        return "Native engine diagnostics"
    return "Native engine"


def log_group(name):
    p = set(name.split("_"))
    if p & {"DUMP", "SHOT", "SHOTS", "SCREENSHOT", "FRAMELOG", "CAPTURE", "PNG"}:
        return "Dumps and screenshots"
    if p & {"PROFILE", "SAMPLE", "TIMING", "PERF", "STATS", "TIMES", "PROBE", "FRAME"}:
        return "Profiling and timing"
    if p & {"CHECK", "CHECKS", "CHECKPOINTS", "VALIDATION", "VERIFY", "TRAP"}:
        return "Checks and validation"
    return "Logs and traces"


# The switches players are actually told to try (the README's troubleshooting, the development guide's "first thing to
# try", the issues). Ordered: the top table takes the first TOP_TABLE entries.
PLAYER_USE = [
    ("RX_VK_MESH_NOCULL", "Steam Deck or AMD card freezes and closes in Kokiri Forest or the Great Deku Tree (a "
                          "`gfx_0.0.0 timeout` in the kernel log); every model is still drawn."),
    ("RX_VK_SKIP_CODE", "The same Steam Deck freeze, with the models of the program left out instead (a workaround: "
                        "key models go missing)."),
    ("RX_VK_ASTC_RGBA8", "AMD cards: blocky textures, solid rectangles in the sky, or unusually dark courtyard "
                         "surfaces (uses more graphics memory)."),
    ("RX_VK_ASTC_CPU", "A Ryzen 7000/9000 desktop APU resets the GPU or freezes for seconds while an area loads: "
                       "textures are decoded on the CPU instead."),
    ("RX_VK_MESH_G3", "A Steam Deck or AMD card draws wrong or hangs in a mesh scene: `=0` is the earlier "
                      "translation, the first thing to try."),
    ("RX_VK_FORCE_MESH_FALLBACK", "The picture is missing on a card or driver whose mesh shaders do not work: "
                                  "`=1` draws with the fallback path."),
    ("RX_VK_NO_PREDICT", "A start or a new area stalls or goes wrong while pipelines are predicted: `=1` skips the "
                         "prediction and builds a pipeline when a draw first needs it."),
    ("RX_VK_FIFO", "Tearing, or a present mode that misbehaves on the driver: set it to present in FIFO (V-Sync), as "
                   "a card or driver with V-Sync forced does."),
    ("RX_VK_KEEP_RETIRED", "Windows, a card that loses the device around area loads (issue 1057): `=0` gives "
                           "retired memory pools back instead of keeping them."),
    ("RX_VK_NO_F16", "GTX 10xx, RX 400/500 or another card without 16-bit float shaders: `=1` lowers the f16 math "
                     "to 32 bits."),
    ("RX_VK_HANDLE_TABLES", "Dark or green blocks where shadow maps should be read (issue 52): `=0` is the earlier "
                            "translation."),
    ("RX_VK_NO_INTEL_SERIAL", "Windows on an Intel UHD or Arc: a driver fault or a long stall when shaders and "
                              "pipelines are made. `=1` builds them side by side for the run."),
    ("RX_VK_SPLIT_UBFLUSH", "Intel without VRAM mirrors (the iGPUs): white blocks and flashes in Kokiri Forest: "
                            "`=0` writes uniforms only in the last part of a split submission."),
    ("RX_VK_SPLIT_BEFORE_DRAW", "Intel Arc: white shards after a submission that begins with mesh draws: the "
                                "split ends only before a plain draw."),
    ("RX_VK_READBACK_STRICT", "The picture pulses brighter for a moment (auto-exposure) on RADV or another driver: "
                              "`=1040,c0` (or `=1`) waits for the light meter's write-back to land."),
    ("RX_VK_METER_ZERO", "White flashes or the exposure stepping brighter and darker in dark scenes: `=0` stops clearing "
                         "the light meter's sums on the GPU before they are added to (on by default)."),
    ("RX_VK_METER_HOLD", "White flashes or the exposure stepping in dark scenes: `=0` stops giving the game the light "
                         "meter's last result again when it reads zeros (on by default)."),
    ("RX_VK_WAIT_OWN", "HUD icons flashing mid-screen or a minimap drawn with another texture on a Radeon handheld or "
                       "iGPU: `=1` makes each present wait for its frame's GPU work (on by default on AMD integrated "
                       "GPUs, `=0` turns it off)."),
    ("RX_VK_NO_PROGRAM_CACHE", "Shaders are translated wrongly after a driver or card change (a bad cache): reads "
                               "and writes no program cache for one run."),
    ("RX_VK_NO_DRAWN_PAGE_MAP", "HUD labels go blank after a zone change, or black surfaces flicker in Zora's Domain: "
                                "`=1` is the earlier page-map behaviour."),
    ("RX_VK_NO_DRAWN_WRITEBACK", "The ground garbles after some minutes in one area: `=1` stops writing small drawn "
                                 "textures back."),
    ("RX_VK_WB_MAX_KB", "Low frame rate in Hyrule Field from copying large GPU-written buffers the CPU never reads: "
                        "`=1024` copies only the small results back."),
    ("RX_VK_DEVICE", "The wrong graphics card is picked (a hybrid laptop): `RX_VK_DEVICE=<n>` chooses the one the "
                     "log lists as `vk: device <n>`."),
    ("HOS_OUTPUT_RES", "The output resolution is wrong or the game starts at a size the screen cannot show: `0` to "
                       "`4` pick a resolution for one run."),
    ("HOS_ASPECT", "An ultrawide monitor: `HOS_ASPECT=2.3889` gives the wider 21:9 picture for one run."),
    ("HOS_WINDOW_SIZE", "The window is too large or too small for the screen: `1280x720` (or another size) for "
                        "one run."),
    ("HOS_DPI_UNAWARE", "Windows scaling makes the picture or the mouse wrong: the process ignores DPI scaling."),
    ("HOS_LANGUAGE", "The game is in the wrong language for one run (`HOS_LANGUAGE=<code>` from `language.txt`)."),
    ("HOS_DYNRES", "The dynamic resolution makes the picture soft: `=0` holds 100% for one run."),
    ("HOS_TEXT_SPEED", "Text is too slow or too fast for one run: `0` to `3`."),
    ("HL_MAINLOOP", "Testing the native main loop (`=1`); the fallback if a 0.4.x crash is suspected in the "
                    "loop's step order."),
    ("HOS_AUDIO", "No sound device works and the game will not start: `=0` opens none (the mixer still runs)."),
    ("HOS_AUDIO_NO_EFFECTS", "The sound is distorted and the cause may be the effects: `=1` leaves out reverbs and "
                             "filters for a comparison."),
    ("HOS_AUDIO_NO_REVERB", "Only the reverbs are suspected: `=1` leaves them out."),
    ("HOS_AUDIO_CHANNELS", "Stereo or 5.1 is wrong for the speakers: `2` or `6` for one run."),
    ("HOS_HD_RUMBLE", "A controller's rumble is wrong or too strong: `=0` sends the plain motor rumble."),
    ("RX_VK_FRAMEGEN", "Frame generation causes artifacts or a wrong frame rate: `=0` turns it off for one run."),
    ("RX_VK_UPSCALE", "The upscaler causes artifacts: `=0` to `2` choose the filter for one run."),
    ("RX_VK_GPU_CLOCK", "A Steam Deck's battery drains too fast: `=0` leaves the GPU's clocks to the driver."),
    ("RX_VK_MIRROR_MB", "Out of VRAM on a card with little memory: `=0` (or `RX_VK_MIRROR=0`) turns the VRAM "
                        "mirrors off."),
    ("RX_VK_VRAM_MB", "The texture cache uses too much VRAM: a lower budget in MB."),
    ("RX_VK_COMMIT_RESERVE_MB", "Windows runs out of commit (RAM plus page file): the shortfall comes off the "
                                "textures' budget; `=0` turns the reserve off."),
    ("HOS_MODS", "Mods live somewhere other than the game folder's `mods` folder."),
    ("HOS_OOT_CHEATS", "The Debug tab's tools should show without turning them on in the menu."),
    ("HOS_ENV_FILE", "The switches should come from another file than `hookshot_env.txt`."),
    ("RX_VK_LOW_SPEC", "A 4 GB card (a GTX 1650) starts with the Minimum preset, frame generation and upscaling "
                       "off."),
    ("RX_VK_MOVIE_UPSCALE", "Movies stutter or look wrong: `linear` replaces their neural upscale."),
    ("RX_VK_UI_PROMPTS", "Screenshots need one set of button prompts: `pad` or `keys`."),
    ("RX_VK_UI_GAME_FONT", "The menu font is unreadable: `=0` draws it with the included font."),
    ("HOS_MOTION_DEBUG", "Motion controls behave wrongly: `=1` logs what the game gets twice a second."),
    ("HOS_MIC_LOG", "The microphone is not picked up: `=1` logs what the game reads, once a second."),
    ("HOS_SHORT_SLEEP_TIMER", "Short sleeps cost CPU time: `=1` waits on the timer instead of yielding."),
]
PLAYER_USE = dict((n, u) for n, u in PLAYER_USE)
TOP_TABLE = [
    "RX_VK_MESH_NOCULL", "RX_VK_SKIP_CODE", "RX_VK_ASTC_RGBA8", "RX_VK_ASTC_CPU", "RX_VK_MESH_G3",
    "RX_VK_FORCE_MESH_FALLBACK", "RX_VK_NO_F16", "RX_VK_HANDLE_TABLES", "RX_VK_NO_INTEL_SERIAL",
    "RX_VK_SPLIT_UBFLUSH", "RX_VK_READBACK_STRICT", "RX_VK_NO_PROGRAM_CACHE", "RX_VK_NO_DRAWN_PAGE_MAP",
    "RX_VK_NO_DRAWN_WRITEBACK", "RX_VK_WB_MAX_KB", "RX_VK_DEVICE", "HOS_OUTPUT_RES", "HOS_ASPECT",
    "HOS_WINDOW_SIZE", "HOS_DPI_UNAWARE", "HOS_LANGUAGE", "HL_MAINLOOP", "HOS_AUDIO", "HOS_AUDIO_CHANNELS",
    "RX_VK_FRAMEGEN", "RX_VK_UPSCALE", "RX_VK_GPU_CLOCK", "RX_VK_MIRROR_MB",
]
# The value the top table shows for each of them (the entry's own text says the rest).
TOP_VALUE = {
    "RX_VK_MESH_NOCULL": "`2c56dc1006942f74`", "RX_VK_SKIP_CODE": "`2c56dc1006942f74`", "RX_VK_ASTC_RGBA8": "`1`",
    "RX_VK_ASTC_CPU": "`1`", "RX_VK_MESH_G3": "`0`", "RX_VK_FORCE_MESH_FALLBACK": "`1`", "RX_VK_NO_F16": "`1`",
    "RX_VK_HANDLE_TABLES": "`0`", "RX_VK_NO_INTEL_SERIAL": "`1`", "RX_VK_SPLIT_UBFLUSH": "`0`",
    "RX_VK_READBACK_STRICT": "`1040,c0`", "RX_VK_NO_PROGRAM_CACHE": "set (`1`)", "RX_VK_NO_DRAWN_PAGE_MAP": "`1`",
    "RX_VK_NO_DRAWN_WRITEBACK": "`1`", "RX_VK_WB_MAX_KB": "`1024`", "RX_VK_DEVICE": "`<n>`",
    "HOS_OUTPUT_RES": "`0` to `4`", "HOS_ASPECT": "`2.3889`", "HOS_WINDOW_SIZE": "`1280x720`",
    "HOS_DPI_UNAWARE": "set (`1`)", "HOS_LANGUAGE": "`<code>`", "HL_MAINLOOP": "`1`", "HOS_AUDIO": "`0`",
    "HOS_AUDIO_CHANNELS": "`2` or `6`", "RX_VK_FRAMEGEN": "`0`", "RX_VK_UPSCALE": "`0` to `2`",
    "RX_VK_GPU_CLOCK": "`0`", "RX_VK_MIRROR_MB": "`0`",
}

ALLOWED_FILE_PREFIXES = ("RX_", "HOS_", "HL_", "HOOKSHOT_", "EDEN_", "NVN_", "TOTK_", "SDL_")
ENV_ONLY = {"HOS_ENV_FILE", "HOS_CRASH_TEST", "HL_TELEMETRY_URL"}


def set_via(name, area):
    """Where a switch can be set: hookshot_env.txt, the environment (or HOS_ENV_FILE), or the build's environment."""
    if area == "build" or name.startswith(("A64_", "OOT_")):
        return "In the build's environment (the builder or the command line), before the build runs."
    bad = name in ENV_ONLY or not name.startswith(ALLOWED_FILE_PREFIXES)
    if name.startswith("SDL_") and ("_LIB" in name or "GL_DRIVER" in name):
        bad = True
    if name.endswith(("_FILE", "_DIR")) or name in ("RX_VK_FRAME_LOG", "HOS_PRESENT_TRACE"):
        bad = bad or name != "RX_VK_DLSS_DIR"
    if bad:
        return ("In the process environment (or in a file named by `HOS_ENV_FILE`); `hookshot_env.txt` refuses this "
                "one.")
    return "In `hookshot_env.txt`, or in the environment."


# ----------------------------------------------------------------------------- building the records

_file_cache = {}


def file_info(rel):
    if rel not in _file_cache:
        path = os.path.join(ENGINE, rel)
        text = read(path)
        code = blank_comments(text)
        _file_cache[rel] = (code.split("\n"), comment_blocks(text), func_ranges(code), text.split("\n"))
    return _file_cache[rel]


GENERIC_TOKENS = {"RX", "VK", "HOS", "HL", "HOOKSHOT", "EDEN", "SUYU", "A64", "WGPU", "OOT", "NVN", "TOTK", "SDL",
                  "NO", "NOT", "ON", "OFF", "USE", "WITH", "FOR", "THE", "AND", "ALL", "NEW", "OLD", "FROM", "BY",
                  "AT", "IN", "IS", "SET", "SETUP", "MODE", "TEST", "DEBUG"}


def relevant(comment, name):
    """A comment 8 lines away is only about the switch when it names one of its distinctive words."""
    words = [w for w in name.split("_") if len(w) > 2 and w not in GENERIC_TOKENS]
    low = comment.lower()
    return any(w.lower() in low for w in words)


def best_comment(name, places):
    """The source comment nearest a read that names the switch; else the closest one about it (8 lines)."""
    named, near = [], []
    for rel, line in places:
        _, blocks, _, _ = file_info(rel)
        for k, (first, last, body) in enumerate(blocks):
            if not body:
                continue
            dist = max(0, first - line, line - last)
            if re.search(r"(?<![\w$/\\])" + re.escape(name) + r"(?![A-Za-z0-9_])", body):
                named.append((dist, rel, first, body))
            elif dist <= 1 or (dist == 2 and relevant(body, name)):
                near.append((dist, rel, first, body))
    for bucket in (named, near):
        if bucket:
            bucket.sort(key=lambda x: (x[0], x[1], x[2]))
            dist, rel, first, body = bucket[0]
            return extend_back(rel, first, body), rel
    return None, None


def extend_back(rel, first, body):
    """A fragment ("1 / 2 when ...") gets the comment block just above it, so the sentence has its start."""
    if not body or not (body[0].islower() or body[0].isdigit() or body[0] in "(["):
        return body
    _, blocks, _, raw = file_info(rel)
    for i, (f, last, prev) in enumerate(blocks):
        if f == first and i > 0:
            between = raw[blocks[i - 1][1]:f - 1]
            if all((not x.strip()) or x.lstrip().startswith("#") for x in between) and f - blocks[i - 1][1] <= 3:
                return (prev + " " + body).strip()
            break
    return body


def build_records():
    occ = scan_c() + scan_py()
    by_name = {}
    for name, rel, line in occ:
        by_name.setdefault(name, set()).add((rel, line))
    missing = (set(PLAYER_USE) | set(TOP_TABLE)) - set(by_name)
    if missing:
        raise SystemExit("these player switches are in the lists but no longer read in the source: %s"
                         % ", ".join(sorted(missing)))
    docs = doc_descriptions()
    records = []
    for name in sorted(by_name):
        places = sorted(by_name[name])
        files = sorted({p for p, _ in places})
        rel, line = places[0]
        code_lines, blocks, funcs, _ = file_info(rel)
        if rel.endswith(".py"):
            win = statement(code_lines, line)
            default, kind = "unset", "text"
            m = re.search(r"\.get\s*\(\s*[\"']" + re.escape(name) + r"[\"']\s*,\s*([^)]+)\)", win)
            if m:
                default = m.group(1).strip().strip("'\"")
                if re.match(r"^\d+$", default):
                    kind = "integer"
            if re.search(r"\b(?:if|while)\b[^;\n]*" + re.escape(name), win) or \
               re.search(r"not\s+(?:os\.environ|environ)", win):
                kind, default = "on/off", "off (unset)"
            if re.search(r"(?:int|float)\s*\(\s*(?:os\.environ|environ|\benv)", win):
                kind = "integer"
        else:
            win = statement(code_lines, line)
            default, kind = read_default(win, name)
        # (a name that says it holds text, not a flag: `HOS_WINDOW_SIZE=1920x1080` is no on/off switch)
        if kind == "on/off" and (re.search(r"_(?:SIZE|RES|ASPECT|LANGUAGE|PROMPTS|FONT|SCRIPT|LIST|KEYS|NAME|URL|"
                                           r"DIR|FILE|PATH|HOME|LIVE|MODE|TAGS)$", name) or
                                 name in ("HOS_AUDIO_DEVICE", "RX_VK_DLSS_DIR", "RX_VK_FSR_DIR")):
            kind = "text"
            if default in ("off (unset)", "0 (unset)"):
                default = "unset"
        desc, source = docs.get(name, (None, None))
        doc_source = source if desc else None
        comment, cpath = best_comment(name, places)
        good_comment = comment if comment and not weak(comment, name) else None
        if desc:
            explanation = condense(desc, name)
            source = source or "document"
        elif good_comment:
            explanation = focus(good_comment, name)
            source = "comment at %s" % cpath
        else:
            explanation = undocumented(name, rel, line, code_lines, funcs, kind)
            source = "undocumented"
        documented = not explanation.startswith("undocumented:")
        explanation = tidy(explanation) if documented else explanation
        area, group, audience = classify(name, files)
        records.append({
            "name": name,
            "area": area,
            "group": group,
            "audience": audience,
            "files": [{"path": p, "line": n} for p, n in places],
            "default": default,
            "values": values_for(kind, win, explanation if documented else "", name),
            "explanation": explanation,
            "comment": tidy(focus(good_comment, name)) if good_comment else None,
            "documented_in": doc_source,
            "source": source,
            "documented": documented,
            "set_via": set_via(name, area),
            "use": PLAYER_USE.get(name),
        })
    return records


AREA_TITLES = {
    "renderer": "Renderer: upscaling, frame generation, HDR, V-Sync and presentation",
    "shaders": "Shaders and pipelines",
    "memory": "Memory and textures",
    "audio": "Audio",
    "input": "Input and controllers",
    "native": "Native engine (`HL_*`)",
    "host": "Host and OS (`HOS_*` and the system's variables)",
    "telemetry": "Telemetry and crash reports",
    "logging": "Diagnostics and logging",
    "build": "Build and lift time (`A64_*`, `RX_*` and `HOOKSHOT_*` the builder reads)",
}
AREA_ORDER = list(AREA_TITLES)
GROUP_ORDER = {
    "renderer": ["Upscaling (DLSS, FSR, TAAU)", "Frame generation", "HDR, V-Sync and presentation", "Rendering and GPU"],
    "shaders": ["Shader translation", "Pipelines and the program cache", "Mesh, draws and synchronization"],
    "memory": ["Textures, uploads and write-backs", "VRAM, pools and mirrors", "Guest memory and captures"],
    "audio": ["Output device and mixing", "Mixing and effects", "Audio diagnostics"],
    "input": ["Controllers and motion", "Keyboard and mouse", "Scripted and background input", "Input diagnostics"],
    "native": ["Main loop", "Timing and frame rate", "Hooks and tracing", "Native engine diagnostics", "Native engine"],
    "host": ["Window and display", "Frame rate and timing", "Threads and scheduling", "Files, paths and folders",
             "NVN graphics driver", "System and services"],
    "telemetry": ["Telemetry", "Crash reports and crash window"],
    "logging": ["Logs and traces", "Dumps and screenshots", "Profiling and timing", "Checks and validation"],
    "build": ["Lift (A64_*)", "Builder and toolchain", "Build behaviour and paths"],
}


def use_sentence(rec):
    if rec["use"]:
        return rec["use"]
    if rec["audience"] == "player":
        return ("Set it for one run when the symptom its description names shows up, then remove the line; it is a "
                "workaround, not a setting.")
    if rec["audience"] == "diagnostic":
        return ("For a bug report or a measurement: set it only when a maintainer asks, and keep the log, dump or "
                "numbers it writes.")
    return "Not for players: an internal experiment or A/B switch, changed only by a developer."


def render_md(records):
    by_area = {a: [r for r in records if r["area"] == a] for a in AREA_ORDER}
    counts = {}
    for r in records:
        c = counts.setdefault(r["area"], [0, 0, 0])
        if r["audience"] == "player":
            c[0] += 1
        elif r["audience"] == "diagnostic":
            c[1] += 1
        else:
            c[2] += 1
    documented = sum(1 for r in records if r["documented"])
    out = []
    out.append("# HeyListen switches")
    out.append("")
    out.append("Every environment switch HeyListen reads: the game, the Vulkan and WebGPU renderers, the Horizon OS "
               "layer, the builder and the lifter. Generated from the source by `engine/scripts/list_switches.py`, "
               "one entry per name.")
    out.append("")
    out.append("`engine/docs/switches.json` holds the same list as data (name, area, files and lines, default, values, "
               "explanation, audience). Do not edit this page by hand: run")
    out.append("")
    out.append("```")
    out.append("python -I engine/scripts/list_switches.py")
    out.append("```")
    out.append("")
    out.append("and commit both files. A test (`engine/tests/test_switches_documented.py`) fails when a switch read in "
               "the source is missing here or the data file is out of date.")
    out.append("")
    out.append("%d switches: %d players may be told to use, %d diagnostic, %d developer-only; %d have a real "
               "explanation and %d are marked `undocumented`." %
               (len(records), sum(1 for r in records if r["audience"] == "player"),
                sum(1 for r in records if r["audience"] == "diagnostic"),
                sum(1 for r in records if r["audience"] == "developer"), documented, len(records) - documented))
    out.append("")

    out.append("## How to set a switch")
    out.append("")
    out.append("Create or edit `hookshot_env.txt` beside `image.bin`, in the game folder (next to the game's executable "
               "on Windows; on Linux the game folder, `~/Games/HeyListen` by default, not its `bin/`). One `NAME=value` "
               "per line, `#` starts a note. A line the game refuses is left out and the log says so.")
    out.append("")
    out.append("- **Windows**: `hookshot_env.txt` in the folder that holds `image.bin` and `hookshot.exe`.")
    out.append("- **Linux**: `hookshot_env.txt` in `~/Games/HeyListen` (or your install folder), not in `bin/`.")
    out.append("- **Steam**: Launch options `NAME=value %command%` (the variable reaches the game's environment).")
    out.append("- **Another file**: `HOS_ENV_FILE=<path>` in the environment makes the game read that file instead of "
               "`hookshot_env.txt`; that file may set anything, including paths.")
    out.append("")
    out.append("`hookshot_env.txt` takes only the game's own switches (names starting `RX_`, `HOS_`, `HL_`, `HOOKSHOT_`, "
               "`EDEN_`, `NVN_`, `TOTK_` or `SDL_`), no value that is a path (except `RX_VK_DLSS_DIR`), and no name "
               "that ends `_FILE` or `_DIR`: those write or read a file and go in the environment or in the "
               "`HOS_ENV_FILE` file. `HOS_ENV_FILE`, `HOS_CRASH_TEST` and `HL_TELEMETRY_URL` are never taken from it.")
    out.append("")
    out.append("The build's switches (the last section) are read by the builder or the lifter, in the environment it "
               "runs in, not by the game.")
    out.append("")

    out.append("## Switches players are most often told to use")
    out.append("")
    out.append("| Switch | Set it to | Symptom it fixes |")
    out.append("|---|---|---|")
    for name in TOP_TABLE:
        use = PLAYER_USE.get(name)
        if use:
            out.append("| `%s` | %s | %s |" % (name, TOP_VALUE.get(name, "set"), use.replace("|", "\\|")))
    out.append("")
    out.append("The entries below say what each one does, its values and default, and when it is worth setting. A "
               "switch marked *diagnostic* writes logs, dumps or numbers for a bug report; *developer* means an "
               "internal experiment. Remove a line once it is no longer needed.")
    out.append("")

    out.append("## Contents")
    out.append("")
    out.append("| Area | Switches | Player | Diagnostic | Developer |")
    out.append("|---|---:|---:|---:|---:|")
    for a in AREA_ORDER:
        c = counts[a]
        out.append("| [%s](#%s) | %d | %d | %d | %d |" %
                   (AREA_TITLES[a].split(":")[0].strip("`"), anchor(AREA_TITLES[a]), sum(c), c[0], c[1], c[2]))
    out.append("")

    for a in AREA_ORDER:
        rs = by_area[a]
        if not rs:
            continue
        out.append("## %s" % AREA_TITLES[a])
        out.append("")
        out.append(AREA_INTRO[a] % len(rs))
        out.append("")
        groups = {}
        for r in rs:
            groups.setdefault(r["group"], []).append(r)
        order = GROUP_ORDER.get(a, [])
        for g in order + sorted(set(groups) - set(order)):
            if g not in groups:
                continue
            out.append("### %s" % g)
            out.append("")
            for r in sorted(groups[g], key=lambda x: x["name"]):
                out.append("#### `%s` - %s" % (r["name"], r["audience"]))
                out.append("")
                out.append(r["explanation"])
                out.append("")
                out.append("**Values:** %s. **Default:** %s." % (r["values"], r["default"]))
                out.append("")
                out.append("**When a player would use it:** %s" % use_sentence(r))
                out.append("")
                out.append("**Set it:** %s **Read at:** %s." % (r["set_via"], where_text(r)))
                out.append("")
    out.append("## See also")
    out.append("")
    out.append("- The builder's and maintainer's guide: [engine/docs/development.md](../engine/docs/development.md)"
               "#switches-for-testing-and-debugging)")
    out.append("- Player troubleshooting: [docs/USER_GUIDE.md](USER_GUIDE.md)")
    out.append("- The same data as JSON: [engine/docs/switches.json](../engine/docs/switches.json)")
    out.append("")
    return "\n".join(out)


def anchor(title):
    s = title.lower()
    s = re.sub(r"[`*]", "", s)
    s = re.sub(r"[^a-z0-9 _-]", "", s)
    return s.replace(" ", "-")


def where_text(rec):
    f = rec["files"]
    first = "`%s:%d`" % (f[0]["path"], f[0]["line"])
    if len(f) == 1:
        return first
    return "%s and %d more place%s" % (first, len(f) - 1, "s" if len(f) > 2 else "")


AREA_INTRO = {
    "renderer": "%d switches of the renderer: the upscaler, frame generation, HDR, V-Sync and what reaches the "
                "screen.",
    "shaders": "%d switches of the shader translator and the pipeline cache.",
    "memory": "%d switches of textures, VRAM, mirrors, guest memory and write-backs.",
    "audio": "%d switches of the mixer and the audio device.",
    "input": "%d switches of the controllers, the mouse and scripted input.",
    "native": "%d switches of the native engine (`HL_*`).",
    "host": "%d switches of the Horizon OS layer, the window and the system.",
    "telemetry": "%d switches of the crash reports, the crash window and the performance summaries.",
    "logging": "%d switches for logs, dumps, profiles and checks.",
    "build": "%d switches read by the builder or the lifter, in their environment.",
}


def dump_json(records):
    data = {
        "schema": 1,
        "generated_by": "engine/scripts/list_switches.py",
        "scope": ("C, C++ and headers under engine/ (engine/tests and vendored trees other than a64-kit excluded) and "
                  "the Python the builder, the lifter and the scripts run: getenv, std::getenv, _wgetenv, the code's "
                  "own env helpers and table-driven reads, and os.environ."),
        "count": len(records),
        "switches": records,
    }
    return json.dumps(data, indent=1, ensure_ascii=False) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--check", action="store_true", help="exit 1 when the two files are not what the source gives")
    a = p.parse_args()
    records = build_records()
    js = dump_json(records)
    md = render_md(records)
    if a.check:
        bad = []
        for path, text in ((JSON_PATH, js), (MD_PATH, md)):
            if not os.path.exists(path):
                bad.append("%s is missing" % os.path.relpath(path, REPO))
            elif read(path) != text:
                bad.append("%s is out of date" % os.path.relpath(path, REPO))
        for b in bad:
            print(b)
        if bad:
            print("run: python -I engine/scripts/list_switches.py")
            return 1
        print("%d switches, both files up to date" % len(records))
        return 0
    for path, text in ((JSON_PATH, js), (MD_PATH, md)):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, REPO), len(text.encode("utf-8"))))
    print("%d switches" % len(records))
    return 0


if __name__ == "__main__":
    sys.exit(main())
