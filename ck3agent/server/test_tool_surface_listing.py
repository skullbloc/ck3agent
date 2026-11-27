"""The 13-tool surface a seat reads. Every number in it is a code constant, every
closed set an enum read from the code, every argument described, the clock actions
absent when an environment owns the clock, and the whole listing under its budget."""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import mcp_server as ms
import personality_axes as PA
import redesign as R
import tool_text as TT
import tools as T

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
TOOLS13 = {"observe", "query", "discover", "inspect", "cbs", "plan_war", "succession_plan", "set_phase",
           "steer", "declare_war", "do", "session", "pass_turn"}
#: The surface budget (at most 45 KB, down from 52 KB), measured as the dump
#: the brief measures: json.dumps(indent=1) of [{name, description, inputSchema}].
DUMP_BUDGET = 45_000


def _defs():
    return {t["name"]: t for t in ms.registered_tool_definitions()}


def _texts():
    """(where, text) for every description a seat reads: each tool's and each argument's."""
    for name, t in _defs().items():
        yield name, t["description"]
        for p, sch in (t["input_schema"].get("properties") or {}).items():
            if isinstance(sch.get("description"), str):
                yield f"{name}.{p}", sch["description"]


def _src(rel):
    return (REPO / rel).read_text(encoding="utf-8")


def _code_numbers() -> set:
    nums = {T.CHARACTERS_DEFAULT_ROWS, T.MARRIAGE_DEFAULT_ROWS, T.CONQUEST_TARGETS_DEFAULT_ROWS,
            T.MAP_GRAPH_HOPS_DEFAULT, T.MAP_GRAPH_MAX_NODES_DEFAULT, ms.QUERY_BATCH_MAX, T.QUERY_REPLY_KB,
            R.PENDING_RING_SIZE, R.PENDING_RING_SIZE + 1, R.PENDING_LIST_MAX, R.INSPECT_MAX_RESULTS_DEFAULT,
            R.SCHEME_TARGETS_DEFAULT, R.WAR_SCORE_MIN, R.WAR_SCORE_MAX, R.STRADDLE_AGE, R.SPEED_MIN,
            R.SPEED_MAX, R.WAIT_CAP_S, PA.AXIS_MIN, PA.AXIS_MAX, len(PA.AXES)}
    for g in R.PHASE_GENOME.values():
        nums |= set(g.values())
    return nums


def test_exactly_the_13_tools_are_registered():
    assert set(_defs()) == TOOLS13


def test_every_number_a_seat_reads_is_a_code_constant():
    allowed = _code_numbers()
    bad = []
    for where, text in _texts():
        for m in re.finditer(r"(?<![A-Za-z_\d])-?\d+(?![\d])", text):
            if int(m.group()) not in allowed and int(m.group().lstrip("-")) not in allowed:
                bad.append((where, m.group(), text[max(0, m.start() - 30):m.end() + 10]))
    assert not bad, bad


def test_CONTROL_the_number_check_sees_a_stray_number():
    text = "returns 37 rows"
    assert [m.group() for m in re.finditer(r"(?<![A-Za-z_\d])-?\d+(?![\d])", text)] == ["37"]
    assert 37 not in _code_numbers()


def test_the_constants_the_texts_quote_are_the_owning_codes():
    assert TT.QUERY_BATCH_MAX == ms.QUERY_BATCH_MAX
    assert re.search(r"int max_results = (\d+);", _src("bridge/src/scheme_engine.cpp")).group(1) == \
        str(R.SCHEME_TARGETS_DEFAULT)
    qh = _src("bridge/src/query_handlers.cpp")
    assert f"if (fallback > {R.WAR_SCORE_MAX}) fallback = {R.WAR_SCORE_MAX};" in qh
    assert f"if (fallback < {R.WAR_SCORE_MIN}) fallback = {R.WAR_SCORE_MIN};" in qh
    assert '(attacker_advantage > 0) ? "attacker"' in qh
    sk = qh[qh.index("static int64_t extract_sort_key("):]
    sk = sk[:sk.index("\n}\n")]
    assert set(re.findall(r'strcmp\(sort_field, "([a-z_]+)"\)', sk)) | {"realm_counties"} == set(T.CHARACTER_SORTS)
    sc = qh[qh.index("bool do_search_characters("):]
    sc = sc[:sc.index("\n}\n")]
    assert set(re.findall(r'strcmp\(filter_str, "([a-z_]+)"\)', sc)) == set(T.CHARACTER_FILTERS)
    ring = re.findall(r"static constexpr size_t MAX_COMPLETED = (\d+);", _src("bridge/include/intent_queue.h"))
    assert ring == [str(R.PENDING_RING_SIZE)]
    mg = T.QUERY_DOMAIN_MAP["map_graph"][1]({})
    assert (mg["hops"], mg["max_nodes"]) == (T.MAP_GRAPH_HOPS_DEFAULT, T.MAP_GRAPH_MAX_NODES_DEFAULT)
    ch = T.QUERY_DOMAIN_MAP["characters"][1]({})
    assert ch["max_results"] == T.CHARACTERS_DEFAULT_ROWS
    assert T.QUERY_DOMAIN_MAP["marriage_candidates"][1]({})["max_results"] == T.MARRIAGE_DEFAULT_ROWS
    import inspect as _i
    assert _i.signature(ms.inspect).parameters["max_results"].default == R.INSPECT_MAX_RESULTS_DEFAULT


def test_every_argument_is_described():
    missing = [f"{n}.{p}" for n, t in _defs().items()
               for p, sch in (t["input_schema"].get("properties") or {}).items()
               if not (isinstance(sch.get("description"), str) and sch["description"].strip())]
    assert not missing, missing


def test_the_closed_sets_are_enums_read_from_the_code():
    q = _defs()["query"]["input_schema"]["properties"]
    assert q["sort"]["enum"] == [""] + list(T.CHARACTER_SORTS)
    assert q["sort_order"]["enum"] == ["", "desc", "asc"]
    assert "scope" not in q            # The map's region is inspect(subject="map")'s
    assert _defs()["inspect"]["input_schema"]["properties"]["layers"]["items"]["enum"] == list(__import__(
        "map_view").LAYERS)
    assert set(q["detail"]["enum"]) >= set(T.CONQUEST_TARGETS_DETAILS)
    d = _defs()["discover"]["input_schema"]["properties"]
    assert set(d["action"]["enum"]) == {""} | set(T.TARGETS_ACTIONS)
    sp = _defs()["set_phase"]["input_schema"]["properties"]["phase"]
    assert all(p in sp["description"] for p in R.PHASES)
    args = _defs()["do"]["input_schema"]["properties"]["args"]["description"]
    for (intent, key), vals in __import__("do_intent_args").arg_values().items():
        assert f"{key}={'|'.join(vals)}" in args, (intent, key)


def test_no_pydantic_titles_and_no_harness_wording():
    blob = json.dumps(list(_defs().values()))
    assert '"title": "Character Id"' not in blob
    for word in ("REQUIRED in multi-actor", "formatters", "nine integers", "SPEC"):
        assert word not in blob, word


def test_the_surface_is_within_its_budget():
    tools = [{"name": t["name"], "description": t["description"], "inputSchema": t["input_schema"]}
             for t in _defs().values()]
    size = len(json.dumps(tools, indent=1))
    assert size <= DUMP_BUDGET, size


_CLOCKLESS = """
import json, sys
sys.path.insert(0, {here!r})
import mcp_server as ms
t = {{d["name"]: d for d in ms.registered_tool_definitions()}}["session"]
print(json.dumps({{"enum": t["input_schema"]["properties"]["action"]["enum"],
                  "props": sorted(t["input_schema"]["properties"]), "desc": t["description"]}}))
"""


def _session_listing(env_extra: dict) -> dict:
    env = {k: v for k, v in os.environ.items() if k != R.CLOCK_OWNER_ENV_VAR}
    env.update(env_extra)
    out = subprocess.run([sys.executable, "-c", _CLOCKLESS.format(here=str(HERE))], env=env,
                         capture_output=True, text=True, timeout=120, cwd=str(HERE))
    assert out.returncode == 0, out.stderr[-2000:]
    return json.loads(out.stdout.strip().splitlines()[-1])


def test_a_clockless_seat_is_not_offered_the_clock():
    got = _session_listing({R.CLOCK_OWNER_ENV_VAR: R.CLOCK_OWNER_ENV})
    assert not set(got["enum"]) & set(R.SESSION_CLOCK_ACTIONS)
    assert set(got["enum"]) == set(R.SESSION_ACTIONS) - set(R.SESSION_CLOCK_ACTIONS)
    assert not {"speed", "seconds", "game_days"} & set(got["props"])
    assert "clock_owned_by_env" in got["desc"] and "wait unpauses" not in got["desc"]


def test_CONTROL_without_the_switch_the_clock_is_offered():
    got = _session_listing({})
    assert set(R.SESSION_CLOCK_ACTIONS) <= set(got["enum"]) and "speed" in got["props"]
    assert "wait unpauses" in got["desc"]


def test_a_clockless_seat_is_refused_a_clock_action_and_nothing_is_written(monkeypatch):
    import asyncio

    class Gw:
        health = "ok"
        writes = []

        async def write(self, a, p=None):
            self.writes.append(a)
            return {"success": True}

        async def read(self, a, p=None):
            return {}

        def game(self):
            return {"date": "1066.9.15"}
    monkeypatch.setenv(R.CLOCK_OWNER_ENV_VAR, R.CLOCK_OWNER_ENV)
    for action in R.SESSION_CLOCK_ACTIONS:
        gw = Gw()
        env = asyncio.run(R.session_impl(gw, action=action))
        assert env["blocked"]["gate"] == "clock_owned_by_env" and gw.writes == [], action
