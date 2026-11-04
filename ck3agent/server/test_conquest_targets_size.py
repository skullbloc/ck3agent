"""L20-B item 2 (smoke17 B4): query(domain="conquest_targets") at its default is
under 15,000 characters as the seat reads it.

THE SMOKE. probe-paused.jsonl line 21 (B4.conq, Cui 13191, 0867.02.20):
  query(domain="conquest_targets", character_id=13191) -> 38,481 characters,
  compact {counties_listed 25, counties_omitted 54, own_realm_counties_dropped 309}
The reply's result is committed as fixtures/smoke17_b4_conquest_targets_13191.json.
Measured on it: ~1,500 characters a row as tool-result text (indent=2, the probe's
`chars`), 70% of each row the `declare` and `declare_options` blocks, plus the
bridge's 1,500-character `note`.

THE BRIDGE REPLY these rows feed the server is rebuilt from that result: its 25
rows carry every key the 2026.12.0 compact kept, and the three it dropped are put
back as do_query_conquest_targets emits them (query_handlers.cpp:
holder_top_liege_independent, is_kin, already_at_war_opposing); the 54 omitted
rows are copies of its rows (4 of them conquerable, as conquerable_count 29 says) and the 309 own-realm rows carry
holder_top_liege_id == ruler_id, the counts the smoke's `compact` reported.
"""

import asyncio
import copy
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

ms = pytest.importorskip("mcp_server")
import tools as T  # noqa: E402
from test_query_tool import bridge  # noqa: E402,F401  (the fake-bridge fixture)

FIX = json.loads((HERE / "fixtures" / "smoke17_b4_conquest_targets_13191.json").read_text())
CUI = 13191
LIMIT = 15_000


def seat_chars(env) -> int:
    """The size the seat reads: the tool-result text (the probe's measure)."""
    return len(json.dumps(env, indent=2, ensure_ascii=False))


def _bridge_reply() -> dict:
    res = copy.deepcopy(FIX["result"])
    comp = res.pop("compact")
    duchies = res["duchies"]
    for d in duchies:
        for rc in d["remaining_counties"]:
            rc.setdefault("holder_top_liege_independent", rc.get("is_independent", True))
            rc.setdefault("is_kin", False)
            rc.setdefault("already_at_war_opposing", False)
    rows = [rc for d in duchies for rc in d["remaining_counties"]]
    # all 25 listed rows were conquerable; of the 54 omitted, conquerable_count
    # (29) - 25 = 4 were too, and the other 50 were not
    still_conquerable = res["conquerable_count"] - comp["counties_listed"]
    extra = []
    for i in range(comp["counties_omitted"]):
        rc = copy.deepcopy(rows[i % len(rows)])
        rc["county_id"] = 900000 + i
        rc["conquerable_now"] = i < still_conquerable
        extra.append(rc)
    duchies.append({"duchy_id": 899999, "duchy_key": "d_rest", "duchy_name": "Rest",
                    "expansion_target": True, "remaining_counties": extra})
    own = []
    for i in range(comp["own_realm_counties_dropped"]):
        rc = copy.deepcopy(rows[0])
        rc.update(county_id=800000 + i, holder_id=CUI + 1, holder_top_liege_id=CUI,
                  conquerable_now=False)
        own.append(rc)
    duchies.insert(0, {"duchy_id": 799999, "duchy_key": "d_own", "duchy_name": "Own",
                       "partially_held": True, "remaining_counties": own})
    return res


#: 2026.12.0's detail="full" county keys (git show 85be8d78:ck3agent/server/tools.py)
OLD_FULL_COUNTY_KEYS = ("county_id", "county_key", "county_name", "holder_id", "holder_name",
                        "holder_top_liege_id", "is_independent", "is_allied", "is_truced",
                        "adjacent", "conquerable_now", "conquerable_now_source",
                        "engine_valid_cbs", "block_reason", "declare", "declare_options")


def _query(bridge, **kw):
    bridge.replies["query_conquest_targets"] = _bridge_reply()
    return asyncio.run(ms.query(domain="conquest_targets", character_id=CUI, **kw))


def test_CONTROL_the_rebuilt_reply_through_the_old_shape_is_the_smokes_size(bridge):
    """The fixture is faithful: detail='full' at the smoke's 25 rows is the
    2026.12.0 shape, and it lands within 10% of the 38,481 the smoke measured."""
    env = _query(bridge, detail="full", max_results=25)
    # detail="full" now carries every emitted key (L20 r3, witness item 4); the
    # 2026.12.0 row is the projection on its own key list (tools.py at 85be8d78)
    for d in env["result"]["duchies"]:
        d["remaining_counties"] = [{k: rc[k] for k in OLD_FULL_COUNTY_KEYS if k in rc}
                                   for rc in d["remaining_counties"]]
    n = seat_chars(env)
    assert abs(n - FIX["chars"]) < FIX["chars"] * 0.10, n
    assert env["result"]["compact"]["counties_omitted"] == 54
    assert env["result"]["compact"]["own_realm_counties_dropped"] == 309


def test_the_default_reply_is_under_15k(bridge):
    env = _query(bridge)
    assert env["ok"] is True, env
    n = seat_chars(env)
    assert n < LIMIT, n
    comp = env["result"]["compact"]
    assert comp["counties_listed"] == T.CONQUEST_TARGETS_DEFAULT_ROWS == 10
    assert comp["counties_omitted"] == 79 - 10


def test_a_default_row_carries_what_a_seat_decides_on(bridge):
    env = _query(bridge)
    duchy = env["result"]["duchies"][0]
    assert {"duchy_id", "duchy_name", "de_jure_kingdom_key"} <= set(duchy)
    row = duchy["remaining_counties"][0]
    assert {"county_id", "county_name", "holder_id", "holder_top_liege_id", "adjacent",
            "conquerable_now", "conquerable_now_source", "declare_args"} <= set(row), row
    # declare_war's own argument names, so the row is declared as it stands
    assert set(row["declare_args"]) == {"target_id", "cb", "cb_title_id"}
    assert row["declare_args"]["cb"] == "county_conquest_cb"
    assert "declare" not in row and "declare_options" not in row
    assert row["declare_option_count"] == 2
    assert "note" not in env["result"]


def test_the_engine_cb_list_is_a_count_and_the_list_under_detail_full(bridge):
    reply = _bridge_reply()
    rc = next(r for d in reply["duchies"] for r in d["remaining_counties"] if r["conquerable_now"])
    rc.update(conquerable_now_source="engine_county_conquest_cb",
              engine_valid_cbs=["county_conquest_cb", "claim_cb"])
    lean = T.compact_conquest_targets(copy.deepcopy(reply))
    full = T.compact_conquest_targets(copy.deepcopy(reply), detail="full")

    def find(out):
        return next(r for d in out["duchies"] for r in d["remaining_counties"]
                    if r["county_id"] == rc["county_id"])
    assert find(lean)["engine_valid_cb_count"] == 2 and "engine_valid_cbs" not in find(lean)
    assert find(full)["engine_valid_cbs"] == ["county_conquest_cb", "claim_cb"]
    assert full["note"] == reply["note"]


def test_max_results_raises_the_rows(bridge):
    env = _query(bridge, max_results=40)
    assert env["result"]["compact"]["counties_listed"] == 40


def test_an_unknown_detail_is_refused_and_nothing_is_read(bridge):
    bridge.replies["query_conquest_targets"] = _bridge_reply()
    env = asyncio.run(ms.query(domain="conquest_targets", character_id=CUI, detail="brief"))
    assert env["ok"] is False and env["blocked"]["gate"] == "invalid_argument", env
    assert "query_conquest_targets" not in bridge.actions()


def test_verbose_is_the_bridge_reply_unchanged(bridge):
    env = _query(bridge, verbose=True)
    assert env["result"] == _bridge_reply()


# ── L20 r3, witness item 4: detail="full" is a superset of the default row ───
# The keys are read off the emitter, not the fixture (2026.12.0's compacted
# output carries none of is_kin / already_at_war_opposing /
# holder_top_liege_unresolved): every r.key on a remaining_counties row and on a
# duchy row of do_query_conquest_targets (bridge/src/query_handlers.cpp).

import re  # noqa: E402

QH = HERE.parents[1] / "bridge" / "src" / "query_handlers.cpp"
_TOK = re.compile(r'r\.(?:key\("([^"]+)"\)|(begin_object|end_object|begin_array|end_array)\(\))')


def emitted_row_keys():
    """(duchy row keys, county row keys): r.key lines at the depth of a duchies[]
    object and of a remaining_counties[] object, in do_query_conquest_targets."""
    src = QH.read_text(encoding="utf-8", errors="replace")
    a = src.index("bool do_query_conquest_targets(")
    seg = src[src.index('r.key("duchies"); r.begin_array();', a):]
    depth, duchy, county, last = 0, [], [], None
    for m in _TOK.finditer(seg):
        k, op = m.group(1), m.group(2)
        if k is not None:
            last = k
            if depth == 2 and k not in duchy:
                duchy.append(k)
            elif depth == 4 and k not in county:
                county.append(k)
            continue
        depth += 1 if op.startswith("begin") else -1
        if depth == 0:
            break
    return duchy, county


def test_the_emitter_walk_finds_the_rows():
    duchy, county = emitted_row_keys()
    assert {"duchy_id", "remaining_counties", "adjacent"} <= set(duchy), duchy
    assert {"county_id", "is_kin", "already_at_war_opposing", "holder_top_liege_unresolved",
            "holder_top_liege_independent", "declare", "declare_options"} <= set(county), county
    assert "casus_belli" not in county and "scope" not in county      # nested in declare*


def _every_key_row(county):
    row = {k: True for k in county}
    row.update(county_id=4242, county_name="X", holder_id=7, holder_name="H",
               holder_top_liege_id=8, is_independent=False, conquerable_now=True,
               conquerable_now_source="engine", block_reason="", engine_valid_cbs=["claim_cb"],
               declare={"type": "war", "method": "steered", "casus_belli": "county_conquest_cb",
                        "target_id": 7, "cb_title_id": 4242},
               declare_options=[{"casus_belli": "county_conquest_cb", "scope": "county",
                                 "target_id": 7, "cb_title_id": 4242}])
    return row


def test_full_carries_every_emitted_key_and_is_a_superset_of_the_default_row():
    duchy, county = emitted_row_keys()
    drow = {k: True for k in duchy if k != "remaining_counties"}
    drow.update(duchy_id=1, remaining_counties=[_every_key_row(county)])
    reply = {"action": "query_conquest_targets", "ruler_id": CUI, "duchies": [drow], "note": "n"}
    lean = T.compact_conquest_targets(copy.deepcopy(reply))
    full = T.compact_conquest_targets(copy.deepcopy(reply), detail="full")
    (lr,), (fr,) = lean["duchies"][0]["remaining_counties"], full["duchies"][0]["remaining_counties"]
    assert set(county) <= set(fr), sorted(set(county) - set(fr))
    assert set(lr) <= set(fr), sorted(set(lr) - set(fr))
    assert all(fr[k] == v for k, v in lr.items()), {k: (fr.get(k), v) for k, v in lr.items()}
    assert fr["is_kin"] is True and fr["already_at_war_opposing"] is True
    fd, ld = full["duchies"][0], lean["duchies"][0]
    assert set(duchy) <= set(fd), sorted(set(duchy) - set(fd))
    assert set(ld) <= set(fd), sorted(set(ld) - set(fd))
