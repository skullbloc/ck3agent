"""docs/research.md states facts about the bridge source. Check them.

The B8 defect was a doc and a code change made in parallel that disagreed in
the merged tree, with nothing to notice. Each test below reads the claim from
the doc and the fact from the source and compares them, so either side
moving reds.

  - the env table vs every string literal in bridge/src and bridge/include
    that matches ENV_SHAPE (CK3_*, BRIDGE_*, XDG_*, HOME, DISPLAY, TMPDIR;
    a new name of that shape reds until documented; other names are unseen)
  - every getenv() argument is a literal or a known wrapper parameter, so the
    census cannot be bypassed by an unlisted indirection
  - the handshake's effective_config keys vs the table
  - action_log_kind: the documented management verbs are tagged
    "management" (compiled and run when a C++ compiler exists, plus a
    static read that always runs); the unlogged list is exactly the
    mutating management verbs left over
  - emit_action_log writes session_id and kind, unconditionally
  - the set_phase table vs PHASE_GENOME

Runs in the source tree (release/overlay/docs) and in the export (docs/).
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(__file__))

ROOT = Path(__file__).resolve().parents[2]
BRIDGE = ROOT / "bridge"

# The one C++ tokenizer (bridge/scripts/cpp_strip.py).
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "bridge" / "scripts"))
import cpp_strip  # noqa: E402


def _doc() -> str:
    for rel in ("release/overlay/docs/research.md", "docs/research.md"):
        p = ROOT / rel
        if p.is_file():
            return p.read_text(encoding="utf-8")
    pytest.fail("research.md not found in the source or the export layout")


def _strip_c_comments(src: str) -> str:
    """Comments blanked, literals kept (cpp_strip)."""
    return cpp_strip.strip(src, keep_strings=True)


def _bridge_sources():
    assert BRIDGE.is_dir(), f"no bridge tree at {BRIDGE}"
    for base in (BRIDGE / "src", BRIDGE / "include"):
        for p in sorted(base.rglob("*")):
            if p.suffix in (".cpp", ".cc", ".h", ".hpp", ".inc") and "test" not in p.relative_to(BRIDGE).parts:
                yield p, _strip_c_comments(p.read_text(encoding="utf-8", errors="replace"))


def _function(src: str, head_re: str) -> str:
    m = re.search(head_re, src)
    assert m, f"function not found: {head_re}"
    j = cpp_strip.match_close(src, src.index("{", m.end() - 1))   # literals do not count
    assert j >= 0, f"unbalanced body: {head_re}"
    return src[m.start():j + 1]


def _literals(text: str) -> list:
    return [body for kind, _s, _e, body in cpp_strip.literals(text) if kind == "string"]


def _router() -> str:
    return _strip_c_comments((BRIDGE / "src/dispatch_router.cpp").read_text(encoding="utf-8"))


# ── doc readers ──────────────────────────────────────────────────────────────

def _section(doc: str, heading: str) -> str:
    m = re.search(r"^## " + re.escape(heading) + r"\s*$(.*?)(?=^## |\Z)", doc, re.M | re.S)
    assert m, f"section missing: {heading}"
    return m.group(1)


def doc_env_names(doc: str) -> set:
    names = set()
    for line in _section(doc, "Bridge environment variables").splitlines():
        if line.startswith("| `"):
            names |= set(re.findall(r"`([A-Z][A-Z0-9_]*)`", line.split("|")[1]))
    return names


def doc_logged_lists(doc: str):
    sec = _section(doc, "What is recorded")
    logged = re.search(r"Logged as `management`:(.*?)Not logged:", sec, re.S)
    unlogged = re.search(r"Not logged: reads, and(.*?)(?:\n\s*\n|\Z)", sec, re.S)
    assert logged and unlogged, "the logged / not-logged sentences moved"
    return set(re.findall(r"`([a-z_]+)`", logged.group(1))), set(re.findall(r"`([a-z_]+)`", unlogged.group(1)))


# ── env table ────────────────────────────────────────────────────────────────

ENV_SHAPE = re.compile(r'"((?:CK3|BRIDGE_)[A-Z0-9_]*|XDG_[A-Z_]+|HOME|DISPLAY|TMPDIR)"')
# Parameter names of the bridge's own getenv wrappers; their call sites pass
# literals, which the census sees.
WRAPPER_PARAMS = {"name", "key", "k", "kGameSpeedEnv"}


#: The incoming-proposal mod's debug.log WORDS share the CK3 shape
#: (script_templates.h kProposalLogWords, the line parser's prefix); they are
#: text the bridge reads in the game's log, not variables it reads from its
#: environment. Excluded by prefix, and only while no getenv-shaped call names one.
LOG_WORDS = re.compile(r"^CK3AGENT_PROPOSAL_")


def bridge_env_census() -> dict:
    found: dict = {}
    for p, src in _bridge_sources():
        for m in ENV_SHAPE.finditer(src):
            if LOG_WORDS.match(m.group(1)):
                continue
            found.setdefault(m.group(1), set()).add(p.name)
    return found


def test_the_log_word_exclusion_names_no_environment_read():
    words = 0
    for p, src in _bridge_sources():
        words += len([m for m in ENV_SHAPE.finditer(src) if LOG_WORDS.match(m.group(1))])
        assert not re.search(r'getenv[a-z_]*\s*\(\s*"CK3AGENT_PROPOSAL_', src), p.name
    assert words > 0, "the exclusion is stale: no bridge source carries a proposal log word"


def test_env_table_lists_exactly_what_the_bridge_reads():
    code, doc = set(bridge_env_census()), doc_env_names(_doc())
    assert code - doc == set(), f"read by the bridge, missing from research.md: {sorted(code - doc)}"
    assert doc - code == set(), f"in research.md, not in the bridge source: {sorted(doc - code)}"


def test_every_getenv_argument_is_a_literal_or_a_known_wrapper_param():
    bad = []
    for p, src in _bridge_sources():
        for m in re.finditer(r"(?<![A-Za-z0-9_])(?:secure_)?getenv\s*\(\s*([^)]*?)\s*\)", src):
            arg = m.group(1)
            if not (re.fullmatch(r'"[^"]*"', arg) or arg in WRAPPER_PARAMS):
                bad.append(f"{p.name}: getenv({arg})")
    assert not bad, f"getenv with an argument the census cannot see: {bad}"


def test_handshake_effective_config_is_the_table_minus_the_two_named():
    body = _function(_strip_c_comments((BRIDGE / "src/ipc_server.cpp").read_text(encoding="utf-8")),
                     r"static std::string handshake_provenance_fields\(\)\s*\{")
    keys = set(_literals(re.search(r"kKeys\[\]\s*=\s*\{(.*?)\};", body, re.S).group(1)))
    want = {n for n in doc_env_names(_doc()) if n.startswith(("CK3_", "BRIDGE_"))}
    want -= {"CK3_BRIDGE_BOOT_LOG"}          # CK3MAP_BIN has no underscore after CK3
    assert keys == want, (sorted(keys - want), sorted(want - keys))


def test_boot_log_row_matches_instance_open_boot_log():
    """The row once said a non-strict build writes the boot log even when
    unset. The code gates on CK3_BRIDGE_BOOT_LOG == "1" alone, in every build."""
    row = next(l for l in _doc().splitlines() if l.startswith("| `CK3_BRIDGE_BOOT_LOG`"))
    assert "even when unset" not in row and "STRICT" not in row.upper().replace("CK3_BRIDGE_BOOT_LOG", "")
    assert "Unset, no boot log" in row
    body = _function(_strip_c_comments((BRIDGE / "src/instance_paths.cpp").read_text(encoding="utf-8")),
                     r"int instance_open_boot_log\(\)\s*\{")
    assert 'getenv("CK3_BRIDGE_BOOT_LOG")' in body and 'strcmp(v, "1") == 0' in body
    assert "STRICT" not in body, "the boot log must not depend on the strict-build setting"


# ── action log ───────────────────────────────────────────────────────────────

# Management verbs that only read. Everything else in is_bridge_management_verb
# changes state and must be in research.md's logged or not-logged list.
READ_ONLY_MGMT = {
    "who_holds_the_lock", "get_bridge_state", "get_offset_validation", "get_tick_info",
    "tick_diff",
    "memory_probe", "resolve_global",
    "pre_validate_war", "construction_check", "get_budget_state",
}


def _kind_fn(router: str) -> str:
    return _function(router, r"static const char\* action_log_kind\(const char\* action_type\)\s*\{")


def _mgmt_fn(router: str) -> str:
    return _function(router, r"bool is_bridge_management_verb\(const char\* action_type\)\s*\{")


def test_kLoggedMgmt_is_the_documented_management_list():
    body = _kind_fn(_router())
    arr = re.search(r"kLoggedMgmt\[\]\s*=\s*\{(.*?)\};", body, re.S)
    assert arr, "kLoggedMgmt moved"
    logged_doc, _ = doc_logged_lists(_doc())
    assert set(_literals(arr.group(1))) == logged_doc
    # tagged before the management exclusion can drop it
    assert body.index('return "management"') < body.index("is_bridge_management_verb(")


def test_not_logged_list_is_every_other_mutating_management_verb():
    router = _router()
    mgmt = set(_literals(_mgmt_fn(router)))
    logged_doc, unlogged_doc = doc_logged_lists(_doc())
    assert unlogged_doc == mgmt - logged_doc - READ_ONLY_MGMT, (
        sorted(unlogged_doc - (mgmt - logged_doc - READ_ONLY_MGMT)),
        sorted((mgmt - logged_doc - READ_ONLY_MGMT) - unlogged_doc))


def test_action_log_kind_RUN_tags_management_verbs():
    cxx = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler; the static kLoggedMgmt test above still ran")
    router = _router()
    prog = ("#include <cstring>\n#include <cstdio>\n"
            "static bool is_game_state_query(const char*) { return false; }\n"
            + _mgmt_fn(router) + "\n" + _kind_fn(router) + "\n"
            "int main(int c, char** v) { for (int i = 1; i < c; ++i) {"
            " const char* k = action_log_kind(v[i]); std::printf(\"%s=%s\\n\", v[i], k ? k : \"null\"); }"
            " return 0; }\n")
    logged_doc, unlogged_doc = doc_logged_lists(_doc())
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d, "k.cpp"), Path(d, "k")
        src.write_text(prog)
        subprocess.run([cxx, "-std=c++17", "-o", str(exe), str(src)], check=True,
                       capture_output=True, text=True)
        verbs = sorted(logged_doc | unlogged_doc) + ["send_interaction"]
        out = subprocess.run([str(exe), *verbs], check=True, capture_output=True, text=True).stdout
    got = dict(line.split("=", 1) for line in out.split())
    for v in logged_doc:
        assert got[v] == "management", v
    for v in unlogged_doc:
        assert got[v] == "null", v
    assert got["send_interaction"] == "action"   # control: a plain action stays "action"


def test_emit_action_log_writes_session_id_and_kind_on_every_dispatch():
    router = _router()
    emit = _function(router, r"static void emit_action_log\(const char\* verb, const char\* kind,[^)]*\)\s*\{")
    assert re.search(r'line \+= "\\",\\"session_id\\":\\"";\s*append_escaped\(session \? session->session_id\.c_str\(\)', emit), \
        "emit_action_log no longer writes session_id"
    assert re.search(r'line \+= "\\",\\"kind\\":\\"";\s*line \+= kind;', emit), \
        "emit_action_log no longer writes kind"
    wrap = _function(router, r"bool dispatch_action_with_response\(const char\* action_type,[^)]*\)\s*\{")
    m = re.search(r"if \(const char\* kind = action_log_kind\(action_type\)\) \{\s*[^}]*emit_action_log\(action_type, kind,", wrap)
    assert m, "the action log call moved or gained a condition"
    head = re.sub(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'', '""', wrap[:m.start()])
    assert head.count("{") - head.count("}") == 1, "the action log call is nested (e.g. under `if (ok) {`)"
    assert head.rstrip()[-1:] in (";", "}", "{"), "the action log call is guarded (e.g. `if (ok) if (...)`)"


# ── set_phase table ──────────────────────────────────────────────────────────

# The doc table's columns are the axes in bridge order: the one axis table.
from personality_axes import AXES  # noqa: E402


def phase_table(section: str) -> tuple[dict, list[str]]:
    """({phase: {axis: value}}, problems) for the doc's set_phase table. Strict:
    a header, a row or the prose count with a column more or less than the
    code's axes is a problem (zip would drop the extra column silently)."""
    rows, probs = {}, []
    head = [c.strip() for c in next(l for l in section.splitlines() if l.startswith("| Phase |")).strip("|").split("|")]
    if len(head) - 1 != len(AXES):
        probs.append(f"header names {len(head) - 1} axis columns, the code has {len(AXES)}: {head[1:]}")
    for a, h in zip(AXES, head[1:]):
        if a[:4] != h.rstrip(".")[:4]:
            probs.append(f"column {h!r} is not the axis {a!r}")
    n = re.search(r"writes these (\d+) personality axes", section)
    if not n or int(n.group(1)) != len(AXES):
        probs.append(f"the prose says {n.group(1) if n else 'no'} axes, the code has {len(AXES)}")
    for line in section.splitlines():
        m = re.match(r"\| `([a-z_]+)` \|(.*)\|\s*$", line)
        if m:
            vals = [int(x) for x in m.group(2).split("|")]
            if len(vals) != len(AXES):
                probs.append(f"{m.group(1)}: {len(vals)} values, the code has {len(AXES)} axes")
            rows[m.group(1)] = dict(zip(AXES, vals))
    return rows, probs


def test_phase_table_matches_PHASE_GENOME():
    redesign = pytest.importorskip("redesign")
    rows, probs = phase_table(_section(_doc(), "`set_phase` presets"))
    assert probs == []
    assert rows == redesign.PHASE_GENOME


def test_CONTROL_the_phase_table_check_reds_on_an_extra_column_and_a_wrong_count():
    """The check must see what zip() hid: a tenth column, a wrong axis count in
    the prose, a row one value short. Each mutant of the real section is red."""
    sec = _section(_doc(), "`set_phase` presets")
    assert phase_table(sec)[1] == []
    extra = "\n".join(l + " 60 |" if re.match(r"\| `[a-z_]+` \|", l) else
                      (l + " domin. |" if l.startswith("| Phase |") else l) for l in sec.splitlines())
    assert any("axis columns" in p for p in phase_table(extra)[1])
    assert any("values" in p for p in phase_table(extra)[1])
    assert any("prose says" in p for p in phase_table(re.sub(r"these \d+ personality", "these 10 personality", sec))[1])
    short = re.sub(r"(\| `expansion` \|.*?)\| -?\d+ \|\s*$", r"\1|", sec, flags=re.M)
    assert any("expansion: 8 values" in p for p in phase_table(short)[1])


# ── strictness controls: every doc table a test here guards ──────────────────
#
# A test that claims to guard a doc table must red when the table drifts. Each
# control writes a mutated copy of research.md (a row dropped, a row added) to a
# temp file, points _doc at it, and runs the guard: it must fail. The set_phase
# table has its own control above (a column added).

def _drop_first(doc: str, heading: str, row: str) -> str:
    sec = _section(doc, heading)
    line = next(l for l in sec.splitlines() if l.startswith(row))
    return doc.replace(sec, sec.replace(line + "\n", "", 1), 1)


def _add_row(doc: str, heading: str, row: str) -> str:
    sec = _section(doc, heading)
    last = [l for l in sec.splitlines() if l.startswith("| `")][-1]
    return doc.replace(sec, sec.replace(last + "\n", last + "\n" + row + "\n", 1), 1)


def _logged(doc: str, f) -> str:
    sec = _section(doc, "What is recorded")
    m = re.search(r"Logged as `management`:(.*?)Not logged:", sec, re.S)
    return doc.replace(m.group(1), f(m.group(1)), 1)


MUTANTS = [
    ("env row dropped", test_env_table_lists_exactly_what_the_bridge_reads,
     lambda d: _drop_first(d, "Bridge environment variables", "| `CK3")),
    ("env row added", test_env_table_lists_exactly_what_the_bridge_reads,
     lambda d: _add_row(d, "Bridge environment variables", "| `CK3_NOT_READ_BY_THE_BRIDGE` | nothing |")),
    ("logged verb dropped", test_kLoggedMgmt_is_the_documented_management_list,
     lambda d: _logged(d, lambda s: s.replace("`pause_game`, ", "", 1))),
    ("logged verb added", test_kLoggedMgmt_is_the_documented_management_list,
     lambda d: _logged(d, lambda s: "`not_a_verb`, " + s)),
    ("unlogged verb added", test_not_logged_list_is_every_other_mutating_management_verb,
     lambda d: d.replace("Not logged: reads, and", "Not logged: reads, and `not_a_verb`,", 1)),
    ("phase row dropped", test_phase_table_matches_PHASE_GENOME,
     lambda d: _drop_first(d, "`set_phase` presets", "| `rebuild`")),
]


@pytest.mark.parametrize("name,guard,mutate", MUTANTS, ids=[m[0] for m in MUTANTS])
def test_CONTROL_each_guard_reds_on_a_mutated_copy_of_the_doc(name, guard, mutate, tmp_path, monkeypatch):
    doc = _doc()
    bad = mutate(doc)
    assert bad != doc, f"{name}: the mutation did not apply (the control would be vacuous)"
    copy = tmp_path / "research.md"
    copy.write_text(bad, encoding="utf-8")
    monkeypatch.setitem(globals(), "_doc", lambda: copy.read_text(encoding="utf-8"))
    with pytest.raises(AssertionError):
        guard()
