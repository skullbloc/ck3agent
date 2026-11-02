"""One succession verdict, one definition (cleanup 2026.11 L14, surface-eval finding 8).

succession_plan, observe's succession block and the live-state hook state whether
the next succession passes intact. Each calls succession_verdict.verdict; these
rows drive the three with the same bridge replies and compare what they say. The
replies are the two states recorded live on the 867 save (ck3agent/hooks/fixtures,
live867 l7-hook-1 and l10fx5-live-2), not shapes written here: the simulation's
`heirs` rows are the bridge's own (do_query_succession_simulation), which the
server read under a key the bridge never emits (`eligible_heirs`) until L14.
"""
import asyncio
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "hooks"))

import succession_verdict as SV  # noqa: E402
R = pytest.importorskip("redesign")

FIX = os.path.join(HERE, "..", "hooks", "fixtures")


def _replies(name):
    if not os.path.isdir(FIX):
        pytest.skip("the 867 recordings (ck3agent/hooks/fixtures) are research-tree only")
    with open(os.path.join(FIX, f"state_867_{name}.json"), encoding="utf-8") as f:
        return json.load(f)["replies"]


class Gw:
    health = "ok"

    def __init__(self, replies):
        self.replies = replies

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        r = self.replies.get(action)
        return dict(r) if isinstance(r, dict) else {}

    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def delta_records_since_last(self):
        return []

    def game(self):
        return {"tick": 1, "date": "867.1.2", "paused": True, "speed": 1}


CASES = {"duke_at_war": (False, 14255), "liege_with_factions": (True, None)}


@pytest.mark.parametrize("name", sorted(CASES))
def test_the_verdict_reads_the_simulation_the_bridge_emits(name):
    r = _replies(name)
    v = SV.verdict(r["query_succession_simulation"], r["query_succession"], SV.map_summary({}))
    intact, breaker = CASES[name]
    assert v["heirs_source"] == "simulation"
    assert v["passes_intact"] is intact, v
    assert (v["breaker"] or {}).get("id") == breaker, v
    assert v["primary"]["id"] == r["query_succession_simulation"]["player_heir"]["character_id"]


@pytest.mark.parametrize("name", sorted(CASES))
def test_succession_plan_observe_and_the_hook_say_the_same(name):
    import inject_state
    r = _replies(name)
    plan = asyncio.run(R.succession_plan_impl(Gw(r), character_id=0))["result"]
    obs = asyncio.run(R.observe_impl(Gw(r), character_id=0))["result"]["succession"]
    line = inject_state.partition_line(r["query_succession_simulation"], r["query_succession"], None)
    assert plan["passes_intact"] is obs["passes_intact"] is CASES[name][0], (plan, obs)
    assert plan["breaker"] == obs["breaker"]
    assert plan["eligible_heir_count"] == obs["eligible_heir_count"]
    assert ("passes intact" in line) is CASES[name][0], line


def test_no_reply_states_a_retired_succession_field():
    """One vocabulary: no succession reply carries scatters / is_partition /
    passes_single_heir / any_splits / scatter_breaker."""
    r = _replies("duke_at_war")
    plan = asyncio.run(R.succession_plan_impl(Gw(r), character_id=0))
    obs = asyncio.run(R.observe_impl(Gw(r), character_id=0))
    text = json.dumps(plan) + json.dumps(obs["result"]["succession"])
    for word in ("scatters", "is_partition", "passes_single_heir", "any_splits", "scatter_breaker"):
        assert f'"{word}"' not in text, word


def test_inspect_has_no_succession_preview_subject():
    assert "succession_preview" not in R.INSPECT_SUBJECTS


def test_succession_plan_states_no_field_the_bridge_never_sends():
    """L14 ruling (c): primary_keeps_kingdom and realm_above_partition_floor were
    read off the succession simulation, which never sends them
    (do_query_succession_simulation), so the reply always said false / true.
    A constant is not a reading; the reply carries neither. Fixture-free, so it
    also runs in the public export's layout."""
    gw = Gw({"query_character": {"id": 777, "name": "Ruler", "alive": True},
             "query_succession_simulation": {"heirs": []}})
    env = asyncio.run(R.succession_plan_impl(gw, character_id=777))
    assert isinstance(env.get("result"), dict), env
    text = json.dumps(env)
    for word in ("primary_keeps_kingdom", "realm_above_partition_floor"):
        assert f'"{word}"' not in text, word


def _map(*titles, any_splits=False, primary=5):
    """query_title_succession_map's title-keyed shape (map_summary's docstring)."""
    return {"status": "ok", "primary_title_id": titles[0][0], "primary_heir_id": primary,
            "primary_heir_name": "Heir", "any_splits": any_splits,
            "titles": [{"title_id": tid, "title_key": key, "title_name": key, "tier": 2,
                        "succession_law": "partition", "title_division": "partition",
                        "projected_heir_id": hid, "projected_heir_name": f"h{hid}",
                        "splits_away": hid != primary} for tid, key, hid in titles]}


def test_map_fallback_rows_carry_their_titles_and_the_hook_counts_them():
    """L14 FF1 M-3 (the witness's run 8): two titles to heir 5, the simulation
    empty. The map's heir row carries its titles, and the hook's line counts 2."""
    import inject_state
    sp = _map((1, "d_a", 5), (2, "c_b", 5))
    v = SV.verdict({}, {}, SV.map_summary(sp))
    assert v["heirs_source"] == "per_title_map"
    assert len(v["heirs"][0]["titles"]) == 2, v["heirs"]
    assert v["outcome"] == "intact" and v["passes_intact"] is True, v
    line = inject_state.partition_line({}, {}, sp)
    assert "2 titles" in line, line


def test_a_split_with_no_breaker_is_undetermined_and_every_reader_says_it():
    """The map says any_splits but projects every title to the one heir: not
    "false with nobody named" -- undetermined, a typed state each reader states."""
    import inject_state
    sp = _map((1, "d_a", 5), (2, "c_b", 5), any_splits=True)
    v = SV.verdict({}, {}, SV.map_summary(sp))
    assert v["outcome"] == "undetermined" and v["passes_intact"] is None, v
    assert v["breaker"] is None and v["undetermined_reason"], v
    assert "undetermined" in inject_state.partition_line({}, {}, sp)
    gw = Gw({"query_title_succession_map": sp,
             "query_character": {"id": 777, "name": "R", "alive": True}})
    plan = asyncio.run(R.succession_plan_impl(gw, character_id=777))["result"]
    obs_env = asyncio.run(R.observe_impl(gw, character_id=777))
    obs = obs_env["result"]["succession"]
    for r in (plan, obs):
        assert r["outcome"] == "undetermined" and r["passes_intact"] is None, r
        assert r["undetermined_reason"] == v["undetermined_reason"], r
    codes = [w.get("code") for w in obs_env.get("warnings") or [] if isinstance(w, dict)]
    assert "succession_undetermined" in codes and "succession_not_intact" not in codes, codes


def test_a_split_names_its_breaker():
    """The control: a title projected to another heir is splits, with the breaker."""
    v = SV.verdict({}, {}, SV.map_summary(_map((1, "d_a", 5), (2, "c_b", 6), any_splits=True)))
    assert v["outcome"] == "splits" and v["passes_intact"] is False, v
    assert v["breaker"]["id"] == 6, v
    assert set(SV.OUTCOMES) == {"intact", "splits", "undetermined"}


def test_no_heir_read_is_undetermined_never_intact():
    """L14 FF2 N-MAJ-2 (the witness's run 4): the simulation empty and the map's
    read an error. Nobody was read, so nobody is known to take every title:
    undetermined, naming both reads. RED on b84e3634 (intact, passes_intact True)."""
    v = SV.verdict({}, {}, SV.map_summary({"error": "x"}))
    assert v["outcome"] != "intact", v
    assert v["outcome"] == "undetermined" and v["passes_intact"] is None, v
    assert v["undetermined_reason"] == "no heir was read (simulation empty; map: x)", v
    # an available map that projects nobody names that instead
    v = SV.verdict({"heirs": []}, {}, SV.map_summary(_map((1, "d_a", -1))))
    assert v["outcome"] == "undetermined", v
    assert v["undetermined_reason"] == "no heir was read (simulation empty; map: it projects no heir)", v


def test_both_reads_fail_and_observe_and_succession_plan_warn():
    """Both reads answer the bridge's fault reply as it is really sent
    (MEMORY_ACCESS_FAULT_REPLY {"error":"memory_access_fault"}, bridge/include/
    bridge.h). observe and succession_plan both say undetermined, with the
    succession_undetermined warning. RED on b84e3634 (intact, no warning)."""
    fault = {"error": "memory_access_fault"}
    gw = Gw({"query_succession_simulation": fault, "query_title_succession_map": fault,
             "query_character": {"id": 777, "name": "R", "alive": True}})
    plan_env = asyncio.run(R.succession_plan_impl(gw, character_id=777))
    obs_env = asyncio.run(R.observe_impl(gw, character_id=777))
    want = "no heir was read (simulation empty; map: memory_access_fault)"
    for env, r in ((plan_env, plan_env["result"]), (obs_env, obs_env["result"]["succession"])):
        assert r["outcome"] == "undetermined" and r["passes_intact"] is None, r
        assert r["undetermined_reason"] == want, r
        codes = [w.get("code") for w in env.get("warnings") or [] if isinstance(w, dict)]
        assert SV.UNDETERMINED_WARNING in codes, codes


def test_the_hook_says_undetermined_when_no_heir_is_read():
    """L14 int5 (FA2's left-over, N-MAJ-2's third reader): the live-state hook
    printed no succession line at all when no heir was read, so the prompt said
    nothing where observe and succession_plan say undetermined. Now it states the
    same verdict and reason. RED on e262847e (partition_line returned None)."""
    import inject_state
    fault = {"error": "memory_access_fault"}
    line = inject_state.partition_line(fault, {}, fault)
    want = SV.verdict(fault, {}, SV.map_summary(fault))["undetermined_reason"]
    assert line == "- Succession: undetermined (%s)" % want, line
    # every read failed (None): the map was not answered at all
    line = inject_state.partition_line(None, None, None)
    assert line == ("- Succession: undetermined (no heir was read "
                    "(simulation empty; map: no_response))"), line
