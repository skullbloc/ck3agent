"""The seat's veto of the AI's ally calls and perk picks, the perk lever, the monthly
prestige change (seat_events.py, news.py, declare_gates.py, redesign.py, tools.py).

The bridge replies are the shapes bridge/src/ai_veto.cpp (query_ai_veto records with `origin`,
`ally_id`, `war_id`), query_handlers.cpp (query_lifestyle `lifestyles` / `unlockable_now`,
query_character `monthly_change`) and action_handlers.cpp (unlock_perk) write. The case:
a recorded game — the game's AI called allies 70362 and 32832 into the seat's own offensive
war 83886141 at -150 prestige each (duchy tier), and no lever stopped it; every run, the game's
AI spends each lifestyle point the day it exists (re_lifestyle_perks.md §3); the monthly
prestige change was "unread" in every prestige_if.
"""
from __future__ import annotations

import asyncio

import pytest

import declare_gates as DG
import news
import redesign as R
import seat_events as SE
import tools as T
from test_ireland_walk_replies import Gw

ME = 31506
WAR = 83886141          # a recorded game step 24: the seat's offensive war the AI called 70362 into
ALLY, ALLY2 = 70362, 32832
WHATS = SE.AI_VETO_WHATS


def run(c):
    return asyncio.run(c)


def actor():
    return {"id": ME, "name": "Murchad"}


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    yield
    news.reset()


class Veto:
    def __init__(self, records=None):
        self.vetoes, self.records = [], list(records or [])

    def write(self, p):
        w = p["what"]
        if w not in WHATS:
            return {"success": False, "action": "set_ai_veto", "reason": "what_must_be_a_veto_kind"}
        self.vetoes = [x for x in self.vetoes if x != w] + ([w] if p.get("op") == "set" else [])
        return {"success": True, "action": "set_ai_veto", "op": p.get("op"), "what": w}

    def read(self, p):
        cid = p.get("character_id") or ME
        return {"success": True, "action": "query_ai_veto", "hook_installed": True, "disabled_by_env": False,
                "last_seq": len(self.records), "ring_cap": 128, "origin_disagreements": 0,
                "call_ally_type_resolved": True,
                "rulers": [{"character_id": cid, "vetoes": self.vetoes, "dropped": {}, "passed": {}}],
                "records": self.records, "interactions": []}


class VGw(Gw):
    def __init__(self, table, replies=None):
        super().__init__(dict(replies or {}, query_ai_veto=table.read))
        self.table = table

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        if action == "set_ai_veto":
            return self.table.write(dict(params or {}))
        r = self.replies.get(action)
        return r(dict(params or {})) if callable(r) else (r or {"success": True})


def ally_rec(seq, dropped=True, ally=ALLY, origin="game", date="1083.04.02"):
    """A query_ai_veto record of an ally call, as do_query_ai_veto writes it."""
    return {"seq": seq, "date": date, "character_id": ME, "what": "ally_call", "dropped": dropped,
            "ally_id": ally, "war_id": WAR, "target_scope_type": 16, "origin": origin,
            **({"site": "0x3cc49f9"} if origin == "game" else {"site": "0x1a2b3c"}),
            "key": "call_ally_interaction", "gold": 40.0, "prestige": 310.0, "piety": 90.0, "priced": "no",
            "unpriced_why": "no_price_read"}


def perk_rec(seq, dropped=True, key="thoughtful_perk", date="1067.03.01"):
    return {"seq": seq, "date": date, "character_id": ME, "what": "perk", "dropped": dropped, "origin": "game",
            "site": "0x3cbc74b", "key": key, "gold": 84.0, "prestige": 600.0, "piety": 0.0, "priced": "no",
            "unpriced_why": "no_price_read"}


WARS = {"wars": [{"war_id": WAR, "name": "Murchad's war for Breifne", "primary_attacker": ME,
                  "primary_defender": 29585, "attackers": [ME], "defenders": [29585]}]}


def chars(p):
    cid = p.get("character_id")
    tier = {ALLY: 3, ALLY2: 3}.get(cid, 2)
    return {"id": cid, "name": "x", "alive": True, "ruler_tier": tier, "gold": 84.0, "prestige": 300.0}


def snap(date, records, vetoes=(), **kw):
    veto = Veto(records)
    veto.vetoes = list(vetoes)
    base = dict(actor_id=ME, date=date, chr_sect={"gold": 0.0, "prestige": 0.0, "piety": 0.0},
                wars={"wars": []}, claims={"claims": []}, council={"council": []}, heir=None,
                schemes={"schemes": []}, held={"held_titles": []}, council_tasks=[], laws={"active_laws": []},
                alliances={"alliances": []}, ai_veto=veto.read({}))
    base.update(kw)
    return news.snapshot(**base)


def two(a, b, replies=None):
    gw = Gw(dict({"query_wars": WARS, "query_character": chars}, **(replies or {})))
    run(news.block(gw, 0, a, offers_served=False))
    return run(news.block(gw, 0, b, offers_served=False))


# ── Ally calls ─────────────────────────────────────────────────────────
def test_1_the_veto_kinds_include_ally_call_and_perk_and_the_listing_says_so():
    assert "ally_call" in WHATS and "perk" in WHATS
    import do_intent_args
    rows = do_intent_args.args_table() if hasattr(do_intent_args, "args_table") else None
    import tool_text
    txt = str(rows) + tool_text.DO + str(getattr(tool_text, "DO_ARGS", ""))
    import mcp_server as ms
    tools = run(ms.mcp.list_tools())
    dump = str([t.model_dump(mode="json") for t in tools])
    assert "ally_call|perk" in dump and "lifestyle: args.{focus, perk}" in dump, txt[:0]


def test_1_set_ally_call_is_sent_and_read_back():
    t = Veto()
    gw = VGw(t)
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "ally_call"}, 0, actor()))
    assert env["verdict"] == "applied" and env["result"]["vetoes"] == ["ally_call"], env
    assert gw.writes == [("set_ai_veto", {"character_id": ME, "op": "set", "what": "ally_call"})]
    assert "do call_ally" in env["result"]["applies_to"] and "never dropped" in env["result"]["applies_to"]


def test_1_the_bridge_refusals_of_the_new_kinds_name_their_gates():
    for code, gate in (("what_must_be_a_veto_kind", "invalid_argument"),
                       ("call_ally_interaction_unresolved", "engine_unreachable")):
        assert SE.REFUSALS[code] == gate and R.RAW_REASON_TO_GATE[code] == gate


def test_1_a_dropped_ally_call_is_news_with_the_ally_its_price_and_the_seats_own_call():
    out = two(snap("1083.01.01", []), snap("1083.05.01", [ally_rec(1), ally_rec(2, date="1083.04.20")],
                                           vetoes=["ally_call"]))
    (row,) = out["ai_vetoed"]
    assert row["what"] == "ally_call" and row["ally_id"] == ALLY and row["war_id"] == WAR and row["attempts"] == 2
    assert row["prestige_if_joined"] == -150          # a duke, your offensive war (-150 each)
    assert row["next"] == {"tool": "do", "args": {"intent": "call_ally", "args": {"war_id": WAR, "ally_id": ALLY}}}
    assert "ai_acted" not in out


def test_1_an_unvetoed_ai_call_is_ai_acted_with_its_price_and_the_veto():
    out = two(snap("1083.01.01", []), snap("1083.05.01", [ally_rec(1, dropped=False, ally=ALLY2)]))
    (a,) = out["ai_acted"]
    assert a["what"] == "ally_call" and a["ally_id"] == ALLY2 and a["prestige_if_joined"] == -150
    assert a["veto"]["args"] == {"intent": "ai_veto", "args": {"op": "set", "what": "ally_call"}}
    assert "price_unread" not in a and "key" not in a


def test_1_the_seats_own_call_recorded_by_the_bridge_is_not_the_ais():
    out = two(snap("1083.01.01", []), snap("1083.05.01", [ally_rec(1, dropped=False, origin="bridge")],
                                           vetoes=["ally_call"]))
    assert "ai_acted" not in out and "ai_vetoed" not in out


def test_1_a_defensive_war_call_costs_nothing():
    wars = {"wars": [dict(WARS["wars"][0], primary_attacker=29585, primary_defender=ME,
                          attackers=[29585], defenders=[ME])]}
    out = two(snap("1083.01.01", []), snap("1083.05.01", [ally_rec(1)], vetoes=["ally_call"]),
              replies={"query_wars": wars})
    assert out["ai_vetoed"][0]["prestige_if_joined"] == 0


def test_1_the_first_news_names_the_ally_call_and_perk_vetoes():
    out = run(news.block(Gw({}), 0, snap("1066.09.15", []), offers_served=False))
    ai = out["ai_spends"]
    if "turn_one_notes" in out:   # with the turn-one notes beside it the block keeps one veto call naming every kind
        kinds = ai["veto"]["args"]["args"]["what"]
        assert "ally_call" in kinds and "perk" in kinds and "allies_called_into_your_wars" not in ai
    else:
        assert ai["allies_called_into_your_wars"]["lever"]["args"] == {"intent": "ai_veto",
                                                                      "args": {"op": "set", "what": "ally_call"}}
        assert ai["perks"]["lever"]["args"]["args"]["what"] == "perk"
    full = news.ai_spends()
    assert full["allies_called_into_your_wars"]["lever"]["args"]["args"]["what"] == "ally_call"
    assert full["perks"]["lever"]["args"]["args"]["what"] == "perk"
    assert "ai_veto what=ally_call" in R.CALL_ALLY_AI_NO_LEVER and "no call stops it" not in R.CALL_ALLY_AI_NO_LEVER


def test_1_the_vetoes_follow_the_seat_to_a_new_ruler():
    t = Veto()
    gw = VGw(t)
    prev, now = {"actor_id": ME, "ai_vetoes": ["ally_call", "perk"]}, {"actor_id": 40000, "ai_vetoes": []}
    row = run(news.carry_vetoes(gw, 0, prev, now))
    assert ("set_ai_veto", {"character_id": 40000, "op": "set", "what": "ally_call"}) in gw.writes
    assert row["carried"] == ["ally_call", "perk"]


# ── Perks ──────────────────────────────────────────────────────────────
LIFESTYLE = {"lifestyle_xp": {"diplomacy_lifestyle": 1040}, "perks": [], "perk_count": 0,
             "current_focus": "diplomacy_foreign_affairs_focus", "focus_lifestyle": "diplomacy_lifestyle",
             "lifestyles": [{"lifestyle": "diplomacy_lifestyle", "xp": 1040.5, "xp_per_point": 1000, "points": 1,
                             "xp_to_next_point": 960}],
             "unlockable_now": ["thoughtful_perk"],
             "next_perks": [{"perk": "thoughtful_perk", "lifestyle": "diplomacy_lifestyle"}], "status": "ok"}


def test_2_a_dropped_perk_pick_is_news_and_the_points_wait_with_the_unlock_call():
    out = two(snap("1067.01.01", []), snap("1067.04.01", [perk_rec(1)], vetoes=["perk"]),
              replies={"query_lifestyle": LIFESTYLE})
    (row,) = out["ai_vetoed"]
    assert row["what"] == "perk" and row["key"] == "thoughtful_perk"
    assert row["next"] == {"tool": "do", "args": {"intent": "lifestyle", "args": {"perk": "thoughtful_perk"}}}
    pp = out["perk_points"]
    assert pp["points"] == {"diplomacy_lifestyle": 1} and pp["unlockable_now"] == ["thoughtful_perk"]
    assert pp["next"]["args"]["args"] == {"perk": "thoughtful_perk"}


def test_2_the_first_news_read_names_the_waiting_points_too():
    """A recorded game: 32095 (perk vetoed) held a point at 1068.09.25; his first news read was the baseline and
    carried no perk_points row."""
    gw = Gw({"query_wars": WARS, "query_character": chars, "query_lifestyle": LIFESTYLE})
    out = run(news.block(gw, 0, snap("1068.09.25", [], vetoes=["perk"]), offers_served=False))
    assert out.get("first_observe") and out["perk_points"]["unlockable_now"] == ["thoughtful_perk"]


def test_2_no_perk_veto_no_lifestyle_read():
    gw = Gw({"query_wars": WARS, "query_character": chars, "query_lifestyle": LIFESTYLE})
    run(news.block(gw, 0, snap("1067.01.01", []), offers_served=False))
    out = run(news.block(gw, 0, snap("1067.04.01", [perk_rec(1, dropped=False)]), offers_served=False))
    assert "perk_points" not in out and not [r for r in gw.reads if r[0] == "query_lifestyle"]
    (a,) = out["ai_acted"]
    assert a["what"] == "perk" and a["key"] == "thoughtful_perk"


def test_2_lifestyle_with_a_perk_is_unlock_perk_and_without_one_the_focus():
    act, p = T.resolve_act(R._intent_to_act_params("lifestyle", {"perk": "ducal_conquest_perk"}, ME))
    assert act == "unlock_perk" and p == {"actor_id": ME, "perk": "ducal_conquest_perk"}
    act, p = T.resolve_act(R._intent_to_act_params("lifestyle", {"focus": "diplomacy_foreign_affairs_focus"}, ME))
    assert act == "set_lifestyle_focus" and p["focus"] == "diplomacy_foreign_affairs_focus"


UNLOCKED = {"success": True, "action": "unlock_perk", "perk": "thoughtful_perk", "lifestyle": "diplomacy_lifestyle",
            "game_check": True, "points_before": 1, "points_after": 0, "perks_before": 0, "perks_after": 1,
            "held_after": True}
REFUSED = {"success": False, "action": "unlock_perk", "perk": "ducal_conquest_perk", "lifestyle": "diplomacy_lifestyle",
           "game_check": False, "points_before": 0, "points_after": 0, "perks_before": 1, "perks_after": 1,
           "held_after": False, "reason": "perk_points_needed"}


@pytest.mark.parametrize("reply,ok,gate", [(UNLOCKED, True, None), (REFUSED, False, "engine_refused")])
def test_2_the_unlock_reply_is_the_verdict(reply, ok, gate):
    gw = VGw(Veto(), replies={"unlock_perk": reply})
    env = run(R.do_impl(gw, intent="lifestyle", args={"perk": reply["perk"]}, character_id=ME))
    assert ("unlock_perk", {"actor_id": ME, "perk": reply["perk"]}) in [(a, {k: v for k, v in p.items()
                                                                          if k in ("actor_id", "perk")})
                                                                         for a, p in gw.writes]
    assert env["ok"] is ok, env
    if gate:
        assert env["blocked"]["gate"] == gate and env["blocked"]["raw_reason"] == "perk_points_needed"


def test_2_the_unlock_refusals_name_their_gates():
    for code in ("unlock_perk_disabled", "perk_required", "perk_not_found", "no_lifestyle_focus",
                 "perk_points_needed", "parent_perk_missing", "perk_already_held",
                 "game_check_refused_perk_trigger", "lifestyle_unread", "perk_command_unmapped"):
        assert code in R.RAW_REASON_TO_GATE, code


# ── The monthly change ────────────────────────────────────────────────
def test_3_prestige_if_carries_the_monthly_change_and_the_months_to_afford():
    opt = {"cb": "claim_cb", "checked": {"prestige": {"cost": 150.0}}}
    p = DG.prestige_outcomes(opt, 110.0, 4.93)
    assert p["monthly"] == 4.93 and p["months_to_afford"] == 9          # ceil(40 / 4.93)
    assert DG.prestige_outcomes(opt, 200.0, 4.93)["months_to_afford"] == 0
    assert DG.prestige_outcomes(opt, 110.0, -1.2)["months_to_afford"] == "never at this rate"
    assert DG.prestige_outcomes(opt, 110.0)["monthly"] == "unread" and "months_to_afford" not in \
        DG.prestige_outcomes(opt, 110.0)


def test_3_the_purse_reads_monthly_change_and_an_older_bridge_reads_none_not_zero():
    chr_ = {"gold": 84.0, "prestige": 600.0, "piety": 10.0, "income_breakdown": {"total_monthly": 2.9},
            "monthly_change": {"prestige": 4.92934, "piety": 2.35, "prestige_tail": 0.0}}
    assert DG.purse_of(chr_)["monthly_prestige"] == 4.92934
    old = dict(chr_)
    old.pop("monthly_change")
    assert DG.purse_of(old)["monthly_prestige"] is None and DG.monthly_change_of(old) is None


def test_3_the_news_purse_prices_the_monthly_prestige():
    mc = {"monthly_change": {"prestige": 3.0, "piety": 1.0}}
    a = snap("1067.01.01", [], chr_sect={"gold": 0.0, "prestige": 300.0, "piety": 50.0, **mc})
    b = snap("1067.07.01", [], chr_sect={"gold": 0.0, "prestige": 318.0, "piety": 56.0, **mc})
    out = two(a, b)
    pr = out["purse"]["prestige"]
    assert pr["income"]["monthly"] == 3.0 and abs(pr["income"]["expected"] - 17.84) < 0.01, pr
    assert "unexplained" not in pr and "cause" not in pr


def _standing(char):
    """observe_impl's `standing` over a query_character reply (test_observe_money's harness)."""
    import test_observe_money as TOM
    oa, op = R._resolve_actor, R._dejure_pair

    async def actor(_g, cid, _t):
        return ({"id": cid or 1}, None)

    async def pair(_g, _c, keys, **kw):
        return [{"total": 1, "held": 1, "title_key": k} for k in keys]

    async def go():
        R._resolve_actor, R._dejure_pair = actor, pair
        try:
            env = await R.observe_impl(TOM._GW(char), character_id=1, depth="brief")
        finally:
            R._resolve_actor, R._dejure_pair = oa, op
        return (env.get("result") or {}).get("standing")
    return run(go())


@pytest.mark.parametrize("mc,want", [
    ({"prestige": 1.02, "piety": 3.8, "prestige_tail": 0.0}, {"prestige_monthly": 1.02, "piety_monthly": 3.8}),
    ({"prestige": 0.0, "piety": 0.0, "prestige_tail": 0.0}, {"prestige_monthly": 0.0, "monthly_note": True}),
    (None, {"monthly_unread": True}),
])
def test_3_observe_standing_shows_the_monthly_change(mc, want):
    """v1: Murchad 1066.10.14 (1.02 / 3.8); the load day (0 / 0, P4); an older bridge (no monthly_change)."""
    char = {"gold": 84.0, "prestige": 600.0, "piety": 400.0, "income_breakdown": {"total_monthly": 2.34},
            **({"monthly_change": mc} if mc is not None else {})}
    st = _standing(char)
    assert st["prestige"] == 600
    for k, v in want.items():
        assert (k in st) if v is True else st.get(k) == v, (k, st)
    if mc and mc["prestige"]:
        assert "monthly_note" not in st and "monthly_unread" not in st


# ── from the live records (the test host, the 1066 save) ──────────────
import json  # noqa: E402
from pathlib import Path  # noqa: E402

LIVE = json.loads((Path(__file__).parent / "fixtures" / "ally_call_perk_replies.json").read_text())["rows"]


def test_live_the_bridges_own_ally_call_and_the_ais_dropped_law_read_as_they_happened():
    """v1: the seat's call_ally for Haraldr (29767, ally calls vetoed) recorded origin=bridge and not dropped;
    the AI's crown_authority_1 for Murchad dropped twice (origin game, site 0x4cbe240 in the Laws pass)."""
    raw = LIVE["V.census"]["reply"]
    recs = raw["records"]
    own = [r for r in recs if r["what"] == "ally_call"]
    assert [(r["origin"], r["dropped"], r["character_id"], r["ally_id"]) for r in own] == [("bridge", False, 29767, 36529)]
    assert raw["origin_disagreements"] == 0
    a = snap("1066.09.15", [], ai_veto={"success": True, "rulers": raw["rulers"], "records": []})
    b = news.snapshot(actor_id=29767, date="1068.10.31", chr_sect={"gold": 0.0, "prestige": 0.0, "piety": 0.0},
                      wars={"wars": []}, claims={"claims": []}, council={"council": []}, heir=None,
                      schemes={"schemes": []}, held={"held_titles": []}, council_tasks=[], laws={"active_laws": []},
                      alliances={"alliances": []}, ai_veto=raw)
    a["actor_id"] = 29767
    out = two(a, b)
    assert not [x for x in out.get("ai_acted") or [] if x["what"] == "ally_call"]      # the seat's own, not the AI's
    m = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [r for r in recs if r["character_id"] == ME]})
    (row,) = m["ai_vetoed"]
    assert row["what"] == "law" and row["attempts"] == 2 and row["price"] == {"prestige": 101.0}


def test_live_the_monthly_change_read_is_what_the_next_monthly_pass_added():
    """A recorded prestige series: on 1066.10.14 Murchad's cache filled (1.02 / 3.8) and his prestige rose 500.0 -> 501.02 the
    same day; the observe purse prices it from the read."""
    days = {d[0]["date"]: d[0] for d in LIVE["prestige_series"]["series"]}
    before, after = days["1066.10.13"], days["1066.10.14"]
    assert before["monthly_change"]["prestige"] == 0.0 and after["monthly_change"]["prestige"] == 1.02
    assert round(after["prestige"] - before["prestige"], 2) == 1.02 and round(after["piety"] - before["piety"], 2) == 3.8
    assert DG.monthly_change_of({"monthly_change": after["monthly_change"]}) == {"prestige": 1.02, "piety": 3.8}


def test_live_the_unlock_refusal_and_the_empty_xp_table_at_the_start():
    ls = LIVE["L.me0.raw"]["reply"]
    assert ls["lifestyles"] == [] and ls["focus_lifestyle"] == "diplomacy_lifestyle" and "thoughtful_perk" in \
        [p["perk"] for p in ls["next_perks"]]
    r = LIVE["L.unlock_no_point"]["reply"]
    assert r["blocked"]["raw_reason"] == "perk_points_needed" and r["blocked"]["gate"] == "engine_refused"


def test_live_v4_the_ais_dropped_ally_call_is_news_with_its_price_and_the_seats_call():
    """v4 1067.02.20: Haraldr's (29767, ally_call vetoed) AI call of 36529 into war 0, which he attacks in, dropped
    (origin game, site 0x4cbc2ac). The news row: the ally, the war, -150 (a duke: v1's call_ally price for him),
    the seat's own call_ally."""
    rec = LIVE["perk_veto_drops"]["drops"][0]
    war0 = LIVE["v4.I.seat.wars"]["war0"]
    assert (rec["origin"], rec["dropped"], rec["ally_id"], rec["war_id"]) == ("game", True, 36529, 0)
    gw = Gw({"query_wars": {"wars": [war0]},
             "query_character": lambda p: {"id": p.get("character_id"), "ruler_tier": 3}})
    base = dict(date="1067.01.01", chr_sect={"gold": 0.0, "prestige": 0.0, "piety": 0.0}, wars={"wars": []},
                claims={"claims": []}, council={"council": []}, heir=None, schemes={"schemes": []},
                held={"held_titles": []}, council_tasks=[], laws={"active_laws": []}, alliances={"alliances": []})
    veto = {"success": True, "rulers": [{"character_id": 29767, "vetoes": ["ally_call"]}]}
    a = news.snapshot(actor_id=29767, **base, ai_veto=dict(veto, records=[]))
    b = news.snapshot(actor_id=29767, **dict(base, date="1067.03.14"), ai_veto=dict(veto, records=[rec]))
    run(news.block(gw, 0, a, offers_served=False))
    out = run(news.block(gw, 0, b, offers_served=False))
    (row,) = out["ai_vetoed"]
    assert row["ally_id"] == 36529 and row["war_id"] == 0 and row["prestige_if_joined"] == -150
    assert row["next"] == {"tool": "do", "args": {"intent": "call_ally", "args": {"war_id": 0, "ally_id": 36529}}}
