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


# ── v4fix B2: a second crown title (set v3-registered-2 s3) ──────────────────
# Charles 12015 held k_france and k_aquitaine; all 25 of the king's observes said
# "intact" with one heir, and at his death k_aquitaine went to Carloman 15319
# (89 -> 44 counties). The two replies below are the bridge's shapes, keys from
# the emitters: query_succession_simulation (do_query_succession_simulation,
# query_handlers.cpp `r.key("primary_title_id")`, `r.key("heirs")`, each heir
# `character_id/name/is_player_heir/title_count/titles`, each title
# `title_id/tier/key/name/succession_type`) and query_title_succession_map
# (do_query_title_succession_map: `primary_title_id`, `primary_heir_id`,
# `primary_heir_name`, `titles[]` `title_id/title_key/title_name/tier/
# succession_law/title_division/projected_heir_id/projected_heir_name/
# splits_away`, `any_splits`). The values are the 867 core dump's (ivar867):
# each kingdom's own law vector resolves (order, division) = (unset, unset), so
# the map writes succession_law "unset" / title_division "n/a" and the
# simulation's partition switch (the primary title's division) is off, giving
# every title to Louis 14577; the engine's per-title heir cache names Louis first
# on k_france 7052 and Carloman 15319 first on k_aquitaine 7259.

LOUIS, CARLOMAN = 14577, 15319


def _s3_sim():
    return {"action": "query_succession_simulation", "character_id": 12015,
            "primary_title_id": 7052, "primary_title_tier": 4, "total_titles": 4,
            "succession_types": {"partition": 2, "single_heir": 2, "elective": 0},
            "player_heir": {"character_id": LOUIS, "name": "Louis", "titles_kept": 4,
                            "titles_lost": 0, "is_partition": False, "partition_type": "none"},
            "heirs": [{"character_id": LOUIS, "name": "Louis", "is_player_heir": True,
                       "title_count": 4, "titles": [
                           {"title_id": 7052, "tier": 4, "key": "k_france", "name": "k_france",
                            "succession_type": "single_heir"},
                           {"title_id": 7259, "tier": 4, "key": "k_aquitaine", "name": "k_aquitaine",
                            "succession_type": "single_heir"},
                           {"title_id": 7054, "tier": 2, "key": "c_ile_de_france",
                            "name": "c_ile_de_france", "succession_type": "partition"},
                           {"title_id": 7152, "tier": 2, "key": "c_orleans", "name": "c_orleans",
                            "succession_type": "partition"}]}],
            "titles_lost": [], "status": "ok"}


def _s3_map(aquitaine_heir=CARLOMAN):
    def t(tid, key, tier, law, div, hid, hname):
        return {"title_id": tid, "title_key": key, "title_name": key, "tier": tier,
                "succession_law": law, "title_division": div, "projected_heir_id": hid,
                **({"projected_heir_name": hname} if hname else {}),
                "splits_away": hid not in (LOUIS, -1)}
    rows = [t(7052, "k_france", 4, "unset", "n/a", LOUIS, "Louis"),
            t(7259, "k_aquitaine", 4, "unset", "n/a", aquitaine_heir,
              {CARLOMAN: "Carloman", LOUIS: "Louis"}.get(aquitaine_heir)),
            t(7054, "c_ile_de_france", 2, "partition", "partition", LOUIS, "Louis"),
            t(7152, "c_orleans", 2, "partition", "partition", LOUIS, "Louis")]
    return {"action": "query_title_succession_map", "character_id": 12015,
            "primary_title_id": 7052, "primary_title_tier": 4, "primary_heir_id": LOUIS,
            "primary_heir_name": "Louis", "titles": rows,
            "any_splits": any(r["splits_away"] for r in rows), "status": "ok"}


def _s3_readers(sim, sp):
    import inject_state
    gw = Gw({"query_succession_simulation": sim, "query_title_succession_map": sp,
             "query_character": {"id": 12015, "name": "Charles", "alive": True}})
    plan_env = asyncio.run(R.succession_plan_impl(gw, character_id=12015))
    obs_env = asyncio.run(R.observe_impl(gw, character_id=12015))
    return (plan_env, obs_env, inject_state.partition_line(sim, {}, sp))


def test_a_second_kingdom_the_map_gives_another_heir_splits():
    """s3: the simulation gives both kingdoms to Louis, the engine's own heir for
    k_aquitaine is Carloman. The verdict is splits, Carloman the breaker taking
    k_aquitaine, and observe, succession_plan and the hook all say it. RED on
    8cef520a: intact, passes_intact True, eligible_heir_count 1 (what the king
    read 25 times)."""
    v = SV.verdict(_s3_sim(), {}, SV.map_summary(_s3_map()))
    assert v["outcome"] == "splits" and v["passes_intact"] is False, v
    assert v["breaker"]["id"] == CARLOMAN and v["breaker"]["source"] == "per_title_map", v
    assert [t["key"] for t in v["breaker"]["takes"]] == ["k_aquitaine"], v
    assert v["eligible_heir_count"] == 2, v
    crown = {r["key"]: r for r in v["crown_titles"]}
    assert set(crown) == {"k_france", "k_aquitaine"}, crown
    assert crown["k_aquitaine"]["simulation_heir_id"] == LOUIS
    assert crown["k_aquitaine"]["map_heir_id"] == CARLOMAN
    assert crown["k_aquitaine"]["agree"] is False and crown["k_france"]["agree"] is True
    plan_env, obs_env, line = _s3_readers(_s3_sim(), _s3_map())
    for r in (plan_env["result"], obs_env["result"]["succession"]):
        assert r["outcome"] == "splits" and r["passes_intact"] is False, r
        assert r["breaker"]["id"] == CARLOMAN, r
        assert r["crown_titles"] == v["crown_titles"] and r["scope"] == SV.SCOPE, r
    codes = [w.get("code") for w in obs_env.get("warnings") or [] if isinstance(w, dict)]
    assert "succession_not_intact" in codes, codes
    assert "does not pass intact" in line and "k_aquitaine" in line, line
    assert "keeps 3 of 4 titles" in line, line


def test_a_second_kingdom_with_no_map_heir_is_undetermined_never_intact():
    """The same realm with the per-title map not read (the bridge's fault reply)
    or projecting nobody on k_aquitaine: the simulation alone cannot see the second
    kingdom's heir, so the verdict is undetermined, naming it. RED on 8cef520a:
    intact."""
    for sp in ({"error": "memory_access_fault"}, _s3_map(aquitaine_heir=-1)):
        v = SV.verdict(_s3_sim(), {}, SV.map_summary(sp))
        assert v["outcome"] == "undetermined" and v["passes_intact"] is None, (sp, v)
        assert "k_aquitaine" in v["undetermined_reason"], v
        plan_env, obs_env, line = _s3_readers(_s3_sim(), sp)
        for env, r in ((plan_env, plan_env["result"]), (obs_env, obs_env["result"]["succession"])):
            assert r["outcome"] == "undetermined" and r["passes_intact"] is None, r
            assert r["undetermined_reason"] == v["undetermined_reason"], r
            codes = [w.get("code") for w in env.get("warnings") or [] if isinstance(w, dict)]
            assert SV.UNDETERMINED_WARNING in codes, codes
        assert line.startswith("- Succession: undetermined") and "k_aquitaine" in line, line


def test_a_second_kingdom_both_reads_give_the_primary_heir_is_intact():
    """The control: the map names Louis on k_aquitaine too. Both reads agree on
    every crown title, so intact stands, and the reply says what it covers."""
    v = SV.verdict(_s3_sim(), {}, SV.map_summary(_s3_map(aquitaine_heir=LOUIS)))
    assert v["outcome"] == "intact" and v["passes_intact"] is True, v
    assert all(r["agree"] is True for r in v["crown_titles"]), v["crown_titles"]
    assert v["scope"] == SV.SCOPE and "after it is not in this verdict" in SV.SCOPE


def test_one_crown_title_and_no_map_keeps_the_simulation_verdict():
    """Only the primary is a crown title (k_africa, the recorded 869 liege), the map
    unread: nothing beside the primary needs checking, so the simulation's intact
    stands (the B2 rule does not turn every unread map into undetermined)."""
    r = _replies("liege_with_factions")
    v = SV.verdict(r["query_succession_simulation"], r["query_succession"], SV.map_summary({}))
    assert v["outcome"] == "intact", v
    assert [c["key"] for c in v["crown_titles"]] == ["k_africa"], v["crown_titles"]
