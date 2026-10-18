"""rc2 B8: docs/research.md states facts about the bridge source. Check them.

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


def _doc() -> str:
    for rel in ("release/overlay/docs/research.md", "docs/research.md"):
        p = ROOT / rel
        if p.is_file():
            return p.read_text(encoding="utf-8")
    pytest.fail("research.md not found in the source or the export layout")


def _strip_c_comments(src: str) -> str:
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _bridge_sources():
    assert BRIDGE.is_dir(), f"no bridge tree at {BRIDGE}"
    for base in (BRIDGE / "src", BRIDGE / "include"):
        for p in sorted(base.rglob("*")):
            if p.suffix in (".cpp", ".cc", ".h", ".hpp", ".inc") and "test" not in p.relative_to(BRIDGE).parts:
                yield p, _strip_c_comments(p.read_text(encoding="utf-8", errors="replace"))


def _function(src: str, head_re: str) -> str:
    m = re.search(head_re, src)
    assert m, f"function not found: {head_re}"
    j, depth, n = src.index("{", m.end() - 1), 0, len(src)
    while j < n:
        c = src[j]
        if c in "\"'":                       # braces inside literals do not count
            k = j + 1
            while k < n and src[k] != c:
                k += 2 if src[k] == "\\" else 1
            j = k + 1
            continue
        depth += {"{": 1, "}": -1}.get(c, 0)
        if depth == 0:
            return src[m.start():j + 1]
        j += 1
    raise AssertionError(f"unbalanced body: {head_re}")


def _literals(text: str) -> list:
    return re.findall(r'"([^"\\]*)"', text)


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


def bridge_env_census() -> dict:
    found: dict = {}
    for p, src in _bridge_sources():
        for m in ENV_SHAPE.finditer(src):
            found.setdefault(m.group(1), set()).add(p.name)
    return found


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
    """rc2 B1: the row once said a non-strict build writes the boot log even when
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
    "evaluate_named_trigger", "tick_diff", "pending_events", "notifications",
    "query_pending_events", "event_probabilities", "memory_probe", "resolve_global",
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

AXES = ["honor", "greed", "rationality", "energy", "boldness", "zeal",
        "vengefulness", "compassion", "sociability", "dominance"]


def test_phase_table_matches_PHASE_GENOME():
    redesign = pytest.importorskip("redesign")
    rows = {}
    for line in _section(_doc(), "`set_phase` presets").splitlines():
        m = re.match(r"\| `([a-z_]+)` \|(.*)\|\s*$", line)
        if m:
            rows[m.group(1)] = dict(zip(AXES, (int(x) for x in m.group(2).split("|"))))
    assert rows == redesign.PHASE_GENOME
