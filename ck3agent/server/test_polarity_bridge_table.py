"""The bridge's thread classes, computed from the bridge's own code.

After L13 the only routing question is "game thread or IPC thread" (contract
D1): every handler that reads or mutates game objects runs on the game thread,
the IPC thread serves bridge state. The classifiers that answer it keep their
names. This test does NOT re-type them: it lifts the real functions out of the
bridge sources (the snapshot.cpp classifiers and deferred_drain.cpp's
is_deferred_drain_action), compiles them beside the real registry, runs the
program and reads one row per registered action. A classifier edited in the
bridge changes these rows with no edit here.

What it pins:
  * the PIPELINE mode's polarity table (pre-registered vocabulary, L13 D3):
    pipeline.cpp's classify_polarity, action_routes_to_drain and
    flush_scheduler.cpp's polarity_name, lifted with every classifier they call,
    answer exactly pipeline_polarity_expected.txt (the 439ab5a6 answers), one
    row per registered action; a changed answer fails by name (witness W4);
  * no read of any kind stores a write verdict in the completed ring, and every
    game-thread write stores exactly one (drain_write_verdict.h's seam);
  * the game-thread reads are construction_check, query_fabricate_targets and
    query_create_title_cost;
  * the gateway's two Python mirrors are copies of the bridge, checked here:
    BridgeGateway._GAME_THREAD_READS of is_game_thread_read and
    DEFERRED_DRAIN_READS of is_deferred_drain_action; its _COMMANDS are all
    bridge-internal (served on the IPC thread).
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "bridge" / "src"
INC = REPO / "bridge" / "include"

# L13 FF3 (witness X4): the one C++ tokenizer (bridge/scripts/cpp_strip.py).
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "bridge" / "scripts"))
import cpp_strip  # noqa: E402

#: (file, the head of each function lifted out of it). The head regex must
#: match exactly one definition; a moved function fails here, by name.
FUNCTIONS = (
    ("deferred_drain.cpp", r"bool is_deferred_drain_action\(const char\* action_type\)\s*\{"),
    ("snapshot.cpp", r"bool is_bridge_internal_query\(const char\* action_type\)\s*\{"),
    ("snapshot.cpp", r"bool is_game_state_query\(const char\* action_type\)\s*\{"),
    ("snapshot.cpp", r"bool is_game_thread_action\(const char\* action_type\)\s*\{"),
    ("snapshot.cpp", r"bool is_game_thread_read\(const char\* action_type\)\s*\{"),
    ("snapshot.cpp", r"bool drain_records_write_verdict\(const char\* action_type\)\s*\{"),
)


def _strip_c_comments(src: str) -> str:
    """Comments blanked, literals kept (cpp_strip)."""
    return cpp_strip.strip(src, keep_strings=True)


def _function(src: str, head_re: str) -> str:
    heads = list(re.finditer(head_re, src))
    assert len(heads) == 1, f"{len(heads)} definitions match {head_re}"
    m = heads[0]
    j = cpp_strip.match_close(src, src.index("{", m.end() - 1))   # literals do not count
    assert j >= 0, f"unbalanced body: {head_re}"
    return src[m.start():j + 1]


def _program() -> str:
    bodies = []
    for fname, head in FUNCTIONS:
        src = _strip_c_comments((SRC / fname).read_text(encoding="utf-8"))
        bodies.append(_function(src, head))
    return (
        "#include <cstdio>\n#include <cstring>\n"
        "#include \"drain_write_verdict.h\"\n"
        "namespace bridge {\n"
        "bool is_deferred_drain_action(const char*);\nbool is_bridge_internal_query(const char*);\n"
        "bool is_game_state_query(const char*);\nbool is_game_thread_action(const char*);\n"
        "bool is_game_thread_read(const char*);\nbool drain_records_write_verdict(const char*);\n"
        + "\n".join(bodies) + "\n}  // namespace bridge\n"
        "using namespace bridge;\n"
        # The drain's record seam, run once per action with a successful reply
        # (a fake run of the handler): `stored` counts what reached the ring.
        "static int drained(const char* name) {\n"
        "  int stored = 0;\n"
        "  record_drain_write_verdict(drain_records_write_verdict(name), name, \"rid-1\", 1,\n"
        "      \"{\\\"success\\\":true}\", [&](const Intent&) { ++stored; });\n"
        "  return stored;\n}\n"
        "int main() {\n"
        "#define BRIDGE_ACTION(NAME, HANDLER) "
        "std::printf(\"%s %d %d %d %d %d %d\\n\", NAME, "
        "(int)is_bridge_internal_query(NAME), "
        "(int)is_deferred_drain_action(NAME), (int)is_game_thread_action(NAME), "
        "(int)is_game_state_query(NAME), (int)is_game_thread_read(NAME), drained(NAME));\n"
        "#include \"action_registry.inc\"\n"
        "#undef BRIDGE_ACTION\n"
        "  return 0;\n}\n")


def _registry_names(debug_verbs: bool = False) -> list[str]:
    """The registered actions in registry order: the release build's, or with
    debug_verbs the -DCK3_BRIDGE_DEBUG_VERBS build's."""
    reg = _strip_c_comments((SRC / "action_registry.inc").read_text(encoding="utf-8"))
    names, debug = [], False
    for line in reg.splitlines():
        s = line.strip()
        if s.startswith("#ifdef CK3_BRIDGE_DEBUG_VERBS"):
            debug = True
        elif s.startswith("#endif"):
            debug = False
        else:
            m = re.match(r'BRIDGE_ACTION\("([^"]+)"', s)
            if m and (debug_verbs or not debug):
                names.append(m.group(1))
    return names


@pytest.fixture(scope="module")
def bridge_rows() -> dict[str, dict]:
    cxx = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler: the bridge's thread classes cannot be computed here")
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d, "classes.cpp"), Path(d, "classes")
        src.write_text(_program(), encoding="utf-8")
        b = subprocess.run([cxx, "-std=c++17", "-I", str(SRC), "-I", str(INC),
                            "-I", str(REPO / "bridge" / "third_party"), "-o", str(exe), str(src)],
                           capture_output=True, text=True, timeout=300)
        assert b.returncode == 0, b.stderr[-3000:]
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True,
                             timeout=60).stdout
    rows: dict[str, dict] = {}
    for line in out.splitlines():
        name, bi, dd, gt, gs, gtr, stored = line.split()
        assert name not in rows, f"{name} printed twice"
        rows[name] = {"bridge_internal": bi == "1", "deferred": dd == "1",
                      "game_thread": gt == "1", "game_state": gs == "1",
                      "game_thread_read": gtr == "1", "drain_stored": int(stored)}
    return rows


def _reads(rows: dict) -> dict:
    """The reads by the bridge's own classes: game-state reads, deferred-drain
    reads and game-thread reads."""
    return {n: r for n, r in rows.items()
            if r["game_state"] or r["deferred"] or r["game_thread_read"]}


def test_no_read_of_any_kind_stores_a_verdict(bridge_rows):
    """L12 item 7: every registered READ stores nothing in the completed ring
    when drained, so the ring holds writes only and a read never takes a slot
    from a write's verdict. The control: the game-thread writes are not all
    zero, so the seam is live."""
    reads = _reads(bridge_rows)
    assert len(reads) > 50, "the census found too few reads (fail OPEN)"
    assert {n: r["drain_stored"] for n, r in reads.items() if r["drain_stored"]} == {}
    assert any(r["drain_stored"] for r in bridge_rows.values()
               if r["game_thread"] and not r["game_thread_read"])


def test_one_row_per_registered_action(bridge_rows):
    assert list(bridge_rows) == _registry_names()


def test_the_thread_classes_do_not_overlap(bridge_rows):
    """An action served on the IPC thread (bridge-internal) is not also a
    game-state read or a queued game-thread action: one thread per action."""
    both = {n for n, r in bridge_rows.items()
            if r["bridge_internal"] and (r["game_state"] or r["game_thread"])}
    assert not both, both
    assert not {n for n, r in bridge_rows.items() if r["game_state"] and r["game_thread"]}


#: The reads the bridge serves on the game thread (lane l6, witness #3).
GAME_THREAD_READS = ("construction_check", "query_fabricate_targets", "query_create_title_cost")


def test_the_game_thread_reads_are_reads(bridge_rows):
    """construction_check, query_fabricate_targets and query_create_title_cost run on
    the game thread (they
    stay in is_game_thread_action: queued, drained by the game thread) and are
    its only read subset."""
    for n in GAME_THREAD_READS:
        r = bridge_rows[n]
        assert r["game_thread"] and r["game_thread_read"], (n, r)
    assert {n for n, r in bridge_rows.items() if r["game_thread_read"]} == set(GAME_THREAD_READS)


def test_a_drained_read_stores_no_write_verdict(bridge_rows):
    """A fake run of each handler through the drain's record seam
    (drain_write_verdict.h, deciding by drain_records_write_verdict): a game-thread
    read stores nothing in the completed ring, so inspect(pending) never lists it
    as a finished write; every game-thread write stores exactly one record (the
    control: the seam is not simply dead)."""
    for n in GAME_THREAD_READS:
        assert bridge_rows[n]["drain_stored"] == 0, n
    writes = [n for n, r in bridge_rows.items() if r["game_thread"] and not r["game_thread_read"]]
    assert writes and "construct_building" in writes and "set_lifestyle_focus" in writes
    assert {n: bridge_rows[n]["drain_stored"] for n in writes} == {n: 1 for n in writes}
    assert bridge_rows["query_character"]["drain_stored"] == 0


def test_the_rows_read_the_classifiers_not_a_copy(bridge_rows):
    """Spot rows that each come from a different classifier."""
    assert bridge_rows["query_cbs_for_target"]["deferred"]            # deferred drain
    assert bridge_rows["change_realm_law"]["game_thread"]             # game-thread action
    assert bridge_rows["construction_check"]["game_thread_read"]      # game-thread read
    assert bridge_rows["get_tick_info"]["bridge_internal"]            # bridge-internal
    assert bridge_rows["query_character"]["game_state"]               # game-state read


def _gateway_module():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    return pytest.importorskip("mcp_server")


def test_the_gateway_game_thread_reads_mirror_the_bridge(bridge_rows):
    """mcp_server.BridgeGateway._GAME_THREAD_READS routes discover's game-thread
    reads through the gateway's action path; it is a copy of the bridge's
    is_game_thread_read, checked here."""
    ms = _gateway_module()
    bridge_reads = {n for n, r in bridge_rows.items() if r["game_thread_read"]}
    assert set(ms.BridgeGateway._GAME_THREAD_READS) == bridge_reads


def test_the_gateway_deferred_reads_mirror_the_bridge(bridge_rows):
    """mcp_server.DEFERRED_DRAIN_READS picks the action path, the 12 s budget and
    the gateway's single-flight (and discover's path); it is a copy of the
    bridge's is_deferred_drain_action, checked here."""
    ms = _gateway_module()
    bridge_deferred = {n for n, r in bridge_rows.items() if r["deferred"]}
    assert bridge_deferred, "no deferred-drain action found (fail OPEN)"
    assert set(ms.DEFERRED_DRAIN_READS) == bridge_deferred


def test_the_gateway_commands_are_bridge_internal(bridge_rows):
    """BridgeGateway._COMMANDS (dispatched on the action path) are all served on
    the IPC thread by the bridge, and registered."""
    ms = _gateway_module()
    for n in ms.BridgeGateway._COMMANDS:
        assert n in bridge_rows, n
        assert bridge_rows[n]["bridge_internal"], n


# ── the pipeline mode's polarity table (pre-registered; witness W4) ───────────

#: (file, head) of everything classify_polarity / action_routes_to_drain /
#: polarity_name call, lifted as they stand. Polarity comes from the real
#: flush_scheduler.h.
PIPELINE_FUNCTIONS = FUNCTIONS[:-1] + (
    ("session.cpp", r"bool action_is_readonly\(const char\* action_type\)\s*\{"),
    ("session.cpp", r"bool action_is_mutation\(const char\* action_type\)\s*\{"),
    ("dispatch_router.cpp", r"bool is_bridge_management_verb\(const char\* action_type\)\s*\{"),
    ("flush_scheduler.cpp", r"const char\* polarity_name\(Polarity p\)\s*\{"),
    ("pipeline.cpp", r"Polarity classify_polarity\(const char\* action_type\)\s*\{"),
    ("pipeline.cpp", r"bool action_routes_to_drain\(const char\* action_type\)\s*\{"),
)

EXPECTED_POLARITY = Path(__file__).resolve().parent / "pipeline_polarity_expected.txt"


def _pipeline_program() -> str:
    bodies = []
    for fname, head in PIPELINE_FUNCTIONS:
        src = _strip_c_comments((SRC / fname).read_text(encoding="utf-8"))
        bodies.append(_function(src, head))
    decls = ("bool is_deferred_drain_action(const char*);\nbool is_bridge_internal_query(const char*);\n"
             "bool is_game_state_query(const char*);\nbool is_game_thread_action(const char*);\n"
             "bool is_game_thread_read(const char*);\nbool action_is_readonly(const char*);\n"
             "bool action_is_mutation(const char*);\nbool is_bridge_management_verb(const char*);\n"
             "Polarity classify_polarity(const char*);\nbool action_routes_to_drain(const char*);\n")
    return (
        "#include <cstdio>\n#include <cstring>\n#include \"flush_scheduler.h\"\n"
        "namespace bridge {\n" + decls + "\n".join(bodies) + "\n}  // namespace bridge\n"
        "using namespace bridge;\n"
        "int main() {\n"
        "#define BRIDGE_ACTION(NAME, HANDLER) "
        "std::printf(\"%s %s %d\\n\", NAME, polarity_name(classify_polarity(NAME)), "
        "(int)action_routes_to_drain(NAME));\n"
        "#include \"action_registry.inc\"\n"
        "#undef BRIDGE_ACTION\n"
        "  return 0;\n}\n")


def _expected_polarity(debug_verbs: bool = False) -> dict[str, tuple[str, int]]:
    """The release rows (`<action> <polarity> <routes>`), or with debug_verbs the
    debug build's extra rows (`debug <action> <polarity> <routes>`)."""
    rows: dict[str, tuple[str, int]] = {}
    for line in EXPECTED_POLARITY.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split()
        is_debug = fields[0] == "debug"
        if is_debug:
            fields = fields[1:]
        name, pol, routes = fields
        if is_debug != debug_verbs:
            continue
        assert name not in rows, f"{name} listed twice in {EXPECTED_POLARITY.name}"
        rows[name] = (pol, int(routes))
    return rows


def _compute_pipeline_rows(debug_verbs: bool) -> dict[str, tuple[str, int]]:
    cxx = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler: the pipeline's polarity table cannot be computed here")
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d, "polarity.cpp"), Path(d, "polarity")
        src.write_text(_pipeline_program(), encoding="utf-8")
        b = subprocess.run([cxx, "-std=c++17"] + (["-DCK3_BRIDGE_DEBUG_VERBS"] if debug_verbs else [])
                           + ["-I", str(SRC), "-I", str(INC),
                            "-I", str(REPO / "bridge" / "third_party"), "-o", str(exe), str(src)],
                           capture_output=True, text=True, timeout=300)
        assert b.returncode == 0, b.stderr[-3000:]
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True,
                             timeout=60).stdout
    rows: dict[str, tuple[str, int]] = {}
    for line in out.splitlines():
        name, pol, routes = line.split()
        assert name not in rows, f"{name} printed twice"
        rows[name] = (pol, int(routes))
    return rows


@pytest.fixture(scope="module")
def pipeline_rows() -> dict[str, tuple[str, int]]:
    return _compute_pipeline_rows(debug_verbs=False)


@pytest.fixture(scope="module")
def debug_pipeline_rows() -> dict[str, tuple[str, int]]:
    return _compute_pipeline_rows(debug_verbs=True)


def test_the_debug_verbs_polarity_rows_are_pinned(debug_pipeline_rows):
    """L13 FF2 (witness m-8): the -DCK3_BRIDGE_DEBUG_VERBS build (the host's
    build) answers the release rows unchanged, and each debug verb answers its
    `debug` row in pipeline_polarity_expected.txt (439ab5a6's answers). The
    debug set is the registry's #ifdef block, so a verb added there without a
    row fails here by name."""
    release = _expected_polarity()
    debug = _expected_polarity(debug_verbs=True)
    extra = [n for n in _registry_names(debug_verbs=True) if n not in release]
    assert len(extra) >= 5 and sorted(extra) == sorted(debug), (
        f"debug verbs registered {sorted(extra)}, pinned {sorted(debug)}")
    want = {**release, **debug}
    changed = {n: {"now": got, "registered": want.get(n)}
               for n, got in debug_pipeline_rows.items() if want.get(n) != got}
    assert not changed, changed
    assert list(debug_pipeline_rows) == _registry_names(debug_verbs=True)


def test_the_pipeline_polarity_table_is_the_pre_registered_one(pipeline_rows):
    """Every registered action's (polarity, routes-to-drain) is the row in
    pipeline_polarity_expected.txt, named when it is not."""
    expected = _expected_polarity()
    assert len(expected) > 100, "the expected table is nearly empty (fail OPEN)"
    changed = {n: {"now": got, "registered": expected.get(n)}
               for n, got in pipeline_rows.items() if expected.get(n) != got}
    assert not changed, changed
    assert sorted(expected) == sorted(pipeline_rows), (
        "actions in the expected table and not registered, or the reverse: "
        f"{sorted(set(expected) ^ set(pipeline_rows))}")


def test_the_pipeline_table_covers_the_registry_and_every_polarity(pipeline_rows):
    """One row per release-registered action, and the table is not degenerate:
    the four L13 pinned rows read what they published before L13."""
    assert list(pipeline_rows) == _registry_names()
    assert {p for p, _ in pipeline_rows.values()} == {"any", "paused", "tick_read", "tick_write"}
    assert pipeline_rows["query_conquest_targets"] == ("tick_read", 1)
    for n in ("query_war_declarations", "query_council_tasks", "query_debug_flags"):
        assert pipeline_rows[n][0] == "paused", (n, pipeline_rows[n])
