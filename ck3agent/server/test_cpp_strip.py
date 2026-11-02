"""The one C++ tokenizer, and the census that keeps it the only one (L13 FF3,
witness X4/X5).

bridge/scripts/cpp_strip.py decides what a comment and a literal are for every
C++ checker in the repo. Seven hand-rolled copies each got one case wrong (a
backslash in a raw string read as an escape put check_no_va_output out of phase
for 600 lines; a digit separator read as a char literal; a `//` in a string
read as a comment), and an out-of-phase checker reports greener.

The census walks every tracked .py file's AST (enumerated from the tree, not a
list of names) and flags a function that parses C++ comments or literals by
hand, by any of four shapes:
  tokenizer       a comment marker ("//", "/*", "*/", or "/" and "*") beside a
                  quote character ('"', "'", ...);
  literal-walker  a quote character beside a backslash (escape handling);
  regex-block     a regex call whose pattern matches /* ... */;
  regex-line      a regex call whose pattern matches // to the line end;
  line-split      split / rsplit / partition / find / index / startswith on "//".
It FAILS OPEN: a file it cannot parse is a red row. Every flagged function must
be in cpp_strip.py or in EXEMPT below, each with its reason; an EXEMPT entry the
census no longer flags is a red row too (stale).

What the census cannot see: a parser spelled with other constants (a char code,
a constant from another module, a table), or one in a non-Python file. The
mutants below are the shapes it does see.
"""
from __future__ import annotations

import ast
import re
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "bridge" / "scripts"))
import cpp_strip  # noqa: E402

HOME = "bridge/scripts/cpp_strip.py"

#: (file, unit) -> why it is not a second C++ tokenizer; a unit is a function,
#: a lambda (<lambda:LINE>), a class body (class NAME) or the module body
#: (<module>). A path prefix (a directory, or one file) covers every unit in it.
EXEMPT = {
    "re/specs/": "Paradox script (game .txt) parsers: `#` comments, not C++",
    ("ck3agent/server/test_instruction_surface.py", "_close"): "parses Python source (redesign.py), not C++",
    ("ck3agent/server/test_instruction_surface.py", "_split_top"): "parses Python source (redesign.py), not C++",
    ("release/export.py", "strip_cmake_comments"): "CMake `#` comments, not C++",
    ("release/export.py", "strip_line_comments"):
        "REFUSES any line whose code holds a quote or a /*: a refusal, not a parse",
    ("release/gen_public_reference.py", "preprocess"):
        "NOT clean (witness r4 m13): besides directive arguments it cuts `//` off every "
        "line before its verb scan, string-blind; the release lane's file, left to its "
        "owner and named here so it is not mistaken for a reader of directives only",
    ("re/scripts/windows/gen_win_offsets.py", "walk"):
        "reads re/offsets headers line by line; no string literal there holds // (checked below)",
    ("re/scripts/windows/gen_win_offsets.py", "windows_tables"):
        "reads re/offsets headers line by line; no string literal there holds // (checked below)",
    ("re/scripts/windows/layout_probe.py", "parse_header"):
        "reads re/offsets headers line by line; no string literal there holds // (checked below)",
    ("scripts/gen_struct_docs.py", "parse_offsets"):
        "reads ck3_offsets.h's comment text on purpose (the generated docs carry it)",
    "ck3agent/server/test_cpp_strip.py":
        "this census's own shape definitions and its mutant fixtures",
    ("release/overlay/ck3agent/_config.py", "<module>"):
        "a JSON/TOML string-escape table, not a C++ parser",
    ("ck3agent/_config.py", "<module>"):
        "the same file where the public export ships it (release/export.py lays "
        "release/overlay on the root): a JSON/TOML string-escape table, not a C++ parser",
    ("scripts/node_stream.py", "<module>"): "an rtmp:// URL pattern, not a comment",
    ("scripts/registered_speed.py", "<module>"):
        "_INT_RHS: an integer right-hand side optionally followed by a comment, on one "
        "declaration line; it reads the declaration, not literals, and this file is a "
        "pre-registered census input kept byte-stable",
    ("scripts/acceptance_start_date_year.py", "check_consumer"):
        "reads each line's comment half on purpose (its stale-comment row); evidence "
        "acceptance for START_DATE_YEAR's grading, left byte-stable",
}

QUOTES = {'"', "'", "\"'", "'\"", '\\"'}
MARKERS = {"//", "/*", "*/"}
#: A regex that matches a // comment to the line end, in any spelling the
#: census has met: //.*, //[^\n]*, //[^\r\n]*, \/\/.*, /{2}.*, /[/].*
_LINE = re.compile(r"(?://|\\/\\/|/\{2\}|/\[/\])(?:\.\*|\.\+|\[\^)")
#: A regex that matches a /* ... */ comment: /\*, /[*] or \/\* opening, and
#: \*/, [*]/ or \*\/ closing, in one constant.
_BLOCK_OPEN = re.compile(r"/\\\*|/\[\*\]|\\/\\\*")
_BLOCK_CLOSE = re.compile(r"\\\*/|\[\*\]/|\\\*\\/")
_SPLITTERS = {"split", "rsplit", "partition", "rpartition", "find", "rfind", "index", "rindex",
              "startswith", "endswith", "count"}
_UNITS = (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)


def _unit_name(node: ast.AST) -> str:
    if isinstance(node, ast.Module):
        return "<module>"
    if isinstance(node, ast.ClassDef):
        return f"class {node.name}"
    if isinstance(node, ast.Lambda):
        return f"<lambda:{node.lineno}>"
    return node.name


def _units(tree: ast.Module) -> dict:
    """{unit name: [nodes that belong to it]}: every node is attributed to its
    innermost enclosing function, lambda, class body or the module. Bare string
    statements (docstrings, prose) are not code and are left out."""
    out: dict[str, list] = {}

    def visit(node: ast.AST, unit: str) -> None:
        for child in ast.iter_child_nodes(node):
            if isinstance(child, ast.Expr) and isinstance(child.value, ast.Constant) \
                    and isinstance(child.value.value, str):
                continue
            if isinstance(child, _UNITS):
                name = _unit_name(child)
                if isinstance(child, ast.ClassDef) or name not in out:
                    out.setdefault(name, [])
                visit(child, name)
                continue
            out.setdefault(unit, []).append(child)
            visit(child, unit)

    visit(tree, "<module>")
    return out


def _folded(n: ast.AST):
    """A string the AST can compute without running anything (witness r5): a str
    constant, a bytes constant (latin-1), chr(<int>), and "+" of such strings,
    so "/" + "/", chr(47) and b"//" read as the "//" they spell. Not seen, and
    named here so nobody assumes otherwise: a re.X pattern whose marker is split
    by whitespace, a string built in a loop or a join, a marker read from a file
    or another module, and a slice compared with a computed string."""
    if isinstance(n, ast.Constant):
        if isinstance(n.value, str):
            return n.value
        if isinstance(n.value, bytes):
            return n.value.decode("latin-1")
        return None
    if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id == "chr" \
            and len(n.args) == 1 and isinstance(n.args[0], ast.Constant) \
            and isinstance(n.args[0].value, int) and 0 <= n.args[0].value < 0x110000:
        return chr(n.args[0].value)
    if isinstance(n, ast.BinOp) and isinstance(n.op, ast.Add):
        a, b = _folded(n.left), _folded(n.right)
        return a + b if isinstance(a, str) and isinstance(b, str) else None
    return None


def _shapes(nodes: list) -> list[str]:
    consts = {v for v in (_folded(n) for n in nodes) if isinstance(v, str)}
    out = []
    quoted = bool(consts & QUOTES)
    if quoted and (consts & MARKERS or {"/", "*"} <= consts):
        out.append("tokenizer")
    if quoted and "\\" in consts:
        out.append("literal-walker")
    if any(_LINE.search(c) for c in consts):
        out.append("regex-line")
    if any(_BLOCK_OPEN.search(c) and _BLOCK_CLOSE.search(c) for c in consts):
        out.append("regex-block")
    for n in nodes:
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr in _SPLITTERS:
            if any(_folded(a) == "//" for a in n.args):
                out.append("line-split")
        if isinstance(n, ast.Compare) and any(isinstance(o, (ast.In, ast.NotIn)) for o in n.ops):
            if _folded(n.left) in MARKERS:
                out.append("line-split")
    return sorted(set(out))


def census(sources: dict[str, str]) -> tuple[dict, list[str]]:
    """{(file, unit): shapes} for every hand-rolled parser, and the files that
    did not parse (fail OPEN). A unit is a function, a lambda (<lambda:LINE>),
    a class body (class NAME) or the module body (<module>)."""
    found, unparsed = {}, []
    for rel, text in sources.items():
        try:
            tree = ast.parse(text)
        except (SyntaxError, ValueError) as e:
            unparsed.append(f"{rel}: {e}")
            continue
        for unit, nodes in _units(tree).items():
            shapes = _shapes(nodes)
            if shapes:
                found[(rel, unit)] = shapes
    return found, unparsed


def _tracked_py() -> dict[str, str]:
    try:
        names = subprocess.run(["git", "ls-files", "*.py"], cwd=REPO, capture_output=True,
                               text=True, check=True, timeout=60).stdout.split()
    except (OSError, subprocess.SubprocessError):
        names = []
    if not names:      # an export without .git: walk the tree (fails open: more, not fewer)
        names = [str(p.relative_to(REPO)) for p in REPO.rglob("*.py")
                 if ".git" not in p.parts and "build" not in p.parts]
    out = {}
    for n in names:
        p = REPO / n
        if p.is_file():
            out[n] = p.read_text(encoding="utf-8", errors="replace")
    return out


def _exempt(key: tuple) -> bool:
    rel, _ = key
    return key in EXEMPT or any(isinstance(k, str) and rel.startswith(k) for k in EXEMPT)


def test_the_tokenizer_selftest_passes(capsys):
    assert cpp_strip.selftest() == 0, capsys.readouterr().out


#: The census floor per tree: the research tree tracks several hundred .py files;
#: the public export (no release/overlay: release/export.py lays it on the root)
#: ships the server, the bridge scripts and its tests, about a hundred and ten.
RESEARCH = (REPO / "release" / "overlay").is_dir()
PY_FLOOR = 200 if RESEARCH else 100


def test_no_second_cpp_tokenizer_remains():
    sources = _tracked_py()
    assert len(sources) > PY_FLOOR, f"only {len(sources)} .py files found (fail OPEN)"
    found, unparsed = census(sources)
    assert unparsed == [], unparsed
    rogue = {k: v for k, v in found.items() if k[0] != HOME and not _exempt(k)}
    assert rogue == {}, ("hand-rolled C++ comment/literal parsers outside cpp_strip.py: "
                         f"{rogue}; read through bridge/scripts/cpp_strip.py, or exempt with a reason")


def test_every_exemption_is_still_a_flagged_function():
    sources = _tracked_py()
    found, _ = census(sources)
    # A file this layout does not carry (the public export ships no re/specs,
    # release/ or scripts/) cannot be judged here; one it carries must be flagged.
    stale = [k for k in EXEMPT if isinstance(k, tuple) and k[0] in sources and k not in found]
    assert stale == [], f"EXEMPT entries the census no longer flags (delete them): {stale}"
    for k in EXEMPT:
        if isinstance(k, str) and any(f.startswith(k) for f in sources):
            assert any(f.startswith(k) for f, _ in found), f"EXEMPT prefix {k} covers nothing"


def test_the_line_split_exemptions_hold_no_string_with_a_comment_marker():
    """The claim three EXEMPT reasons make, checked: no string literal in the
    re/offsets headers holds `//`, so a line split there cannot cut a string."""
    heads = sorted((REPO / "re" / "offsets").glob("*.h"))
    if not heads:
        pytest.skip("no re/offsets headers in this layout")
    bad = [(h.name, body) for h in heads
           for _k, _s, _e, body in cpp_strip.literals(h.read_text(errors="replace")) if "//" in body]
    assert bad == []


@pytest.mark.parametrize("name, text", [
    ("tokenizer", 'def s(t):\n    if t.startswith("//"):\n        pass\n    if t[0] == \'"\':\n        pass\n'),
    ("tokenizer by slash and star", 'def s(t, i):\n    return t[i] == "/" and t[i+1] == "*" or t[i] == \'"\'\n'),
    ("literal-walker", 'def s(t, i):\n    while t[i] != \'"\':\n        i += 2 if t[i] == "\\\\" else 1\n'),
    ("regex-block", 'import re\ndef s(t):\n    return re.sub(r"/\\*.*?\\*/", "", t)\n'),
    ("line-split", 'def s(t):\n    return [l.split("//")[0] for l in t]\n'),
    ("regex-line", 'import re\ndef s(t):\n    return re.sub(r"//[^\\n]*", "", t)\n'),
    # witness r5: spellings the AST can compute (_folded)
    ("line-split by concatenation", 'def s(t):\n    return [l.split("/" + "/")[0] for l in t]\n'),
    ("line-split by chr", 'def s(t):\n    return [l.split(chr(47) + chr(47))[0] for l in t]\n'),
    ("regex-line in bytes", 'import re\ndef s(t):\n    return re.sub(rb"//[^\\n]*", b"", t)\n'),
])
def test_MUTANT_a_planted_parser_is_flagged(name, text):
    found, _ = census({"scripts/planted.py": text})
    assert ("scripts/planted.py", "s") in found, (name, found)


#: L13 FF4 (witness T3): the shapes the function-only census missed, each in
#: the unit that holds it.
@pytest.mark.parametrize("name, text, unit", [
    ("pattern in a module constant (the witness's run 2)",
     'import re\nPAT = r"//[^\\n]*"\ndef s(t): return re.sub(PAT, "", t)\n', "<module>"),
    ("pattern in a class constant",
     'import re\nclass K:\n    LINE = re.compile(r"//[^\\n]*")\n', "class K"),
    ("a lambda", 'import re\nstrip = lambda t: re.sub(r"//.*", "", t)\n', "<lambda:2>"),
    ("//[^\\r\\n]*", 'import re\ndef s(t): return re.sub(r"//[^\\r\\n]*", "", t)\n', "s"),
    ("\\/\\/.*", 'import re\ndef s(t): return re.sub(r"\\/\\/.*", "", t)\n', "s"),
    ("/[*].*?[*]/", 'import re\ndef s(t): return re.sub(r"/[*].*?[*]/", "", t)\n', "s"),
    ('"//" in a line, then a slice',
     'def s(l):\n    if "//" in l:\n        l = l[:l.find("/" + "/")]\n    return l\n', "s"),
])
def test_MUTANT_T3_a_shape_the_function_census_missed_is_flagged(name, text, unit):
    found, _ = census({"scripts/planted.py": text})
    assert ("scripts/planted.py", unit) in found, (name, found)


def test_CONTROL_a_docstring_naming_a_comment_regex_is_not_flagged():
    found, _ = census({"scripts/x.py": 'def s():\n    """strips r"//[^\\n]*" comments"""\n    return 1\n'})
    assert found == {}


def test_CONTROL_a_function_that_only_names_a_path_is_not_flagged():
    found, _ = census({"scripts/x.py": 'def s():\n    return "bridge/src/*.cpp and \\"quoted\\""\n'})
    assert found == {}


def test_MUTANT_an_unparsable_file_reds_fail_open():
    _, unparsed = census({"scripts/broken.py": "def (:\n"})
    assert unparsed and "scripts/broken.py" in unparsed[0]
