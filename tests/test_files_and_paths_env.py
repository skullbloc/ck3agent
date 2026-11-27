"""docs/reference/files-and-paths.md lists every environment variable the package's code reads.

The names are enumerated from the code, never from a list here, two ways, and both must be documented:

* every string constant spelled like one of the package's variables (`CK3_*`, `CK3AGENT_*`, `XDG_*`) in every
  non-test module under `ck3agent/`, by AST: a constant read through a name (`ENV = "CK3_NOTES"`, then
  `os.environ.get(ENV)`) is caught as well as a literal. A short list of reviewed constants that are not
  variables (log words) is exempt, and each exemption must still occur in the code;
* every name read directly (`os.environ.get`, `os.environ[...]`, `os.getenv`, `... in os.environ`) whose argument
  is a string literal, whatever its spelling (`HOME`, `TERM`).

A variable is documented when it is in the first cell of a row of a table under "## Environment variables".
Controls plant a module with a new variable and prove each enumeration sees it.
"""
import ast
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "reference" / "files-and-paths.md"
PKG = ROOT / "ck3agent"
SPELLED = re.compile(r"^(?:CK3|CK3AGENT|XDG)_[A-Z0-9_]+$")
#: constants spelled like a variable that are not one (reviewed): the proposal mod's log words
NOT_VARIABLES = {"CK3AGENT_PROPOSAL_HELD", "CK3AGENT_PROPOSAL_RESOLVED", "CK3AGENT_PROPOSAL_VOID"}


def _modules():
    return sorted(p for p in PKG.rglob("*.py") if not p.name.startswith("test_") and "__pycache__" not in p.parts)


def _spelled(tree: ast.AST) -> set[str]:
    return {n.value for n in ast.walk(tree)
            if isinstance(n, ast.Constant) and isinstance(n.value, str) and SPELLED.match(n.value)}


def _is_environ(n: ast.AST) -> bool:
    return (isinstance(n, ast.Attribute) and n.attr == "environ") or (isinstance(n, ast.Name) and n.id == "environ")


def _read_directly(tree: ast.AST) -> set[str]:
    out = set()
    for n in ast.walk(tree):
        arg = None
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.args:
            if n.func.attr in ("get", "pop", "setdefault") and _is_environ(n.func.value):
                arg = n.args[0]
            elif n.func.attr == "getenv":
                arg = n.args[0]
        elif isinstance(n, ast.Subscript) and _is_environ(n.value) and isinstance(n.ctx, ast.Load):
            arg = n.slice
        elif (isinstance(n, ast.Compare) and len(n.ops) == 1 and isinstance(n.ops[0], (ast.In, ast.NotIn))
              and _is_environ(n.comparators[0])):
            arg = n.left
        if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
            out.add(arg.value)
    return out


def _names() -> dict[str, set[str]]:
    out: dict[str, set[str]] = {}
    for p in _modules():
        tree = ast.parse(p.read_text(encoding="utf-8"))
        for v in _spelled(tree) | _read_directly(tree):
            out.setdefault(v, set()).add(p.relative_to(ROOT).as_posix())
    return out


def _documented() -> set[str]:
    text = DOC.read_text(encoding="utf-8")
    i = text.index("\n## Environment variables\n")
    j = text.find("\n## ", i + 1)
    names = set()
    for ln in text[i:j if j > 0 else None].splitlines():
        if ln.startswith("| `"):
            names |= set(re.findall(r"`([^`]+)`", ln.split(" | ")[0]))
    return names


def test_every_environment_variable_the_code_reads_is_documented():
    names = _names()
    assert len(names) > 40, "the enumeration found almost nothing: it is not reading the code"
    assert {"CK3_NOTES", "CK3_TOOL_PREFIX", "HOME"} <= set(names), "an enumeration lost a known read"
    stale = NOT_VARIABLES - set(names)
    assert not stale, f"reviewed exemptions no longer in the code: {sorted(stale)}"
    doc = _documented()
    missing = sorted(n for n in names if n not in NOT_VARIABLES and n not in doc)
    assert missing == [], f"read by the code, not in files-and-paths.md: {[(m, sorted(names[m])) for m in missing]}"


def test_CONTROL_a_new_variable_is_found_by_each_enumeration():
    """A planted module's variables, one read through a constant and one read directly, are each seen and
    neither is documented: the test above would red on them."""
    tree = ast.parse('import os\nENV = "CK3_PLANTED_NEW"\nx = os.environ.get(ENV)\ny = os.getenv("PLANTED_HOME")\n'
                     'z = "PLANTED_TERM" in os.environ\n')
    assert _spelled(tree) == {"CK3_PLANTED_NEW"}
    assert _read_directly(tree) == {"PLANTED_HOME", "PLANTED_TERM"}
    doc = _documented()
    assert not ({"CK3_PLANTED_NEW", "PLANTED_HOME", "PLANTED_TERM"} & doc)
    assert {"CK3_NOTES", "CK3_GUIDE_DIR", "XDG_CONFIG_HOME"} <= doc       # both tables are read
