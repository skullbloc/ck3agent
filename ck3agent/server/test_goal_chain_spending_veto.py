"""The toolkit against a seat that must create a kingdom needing a duchy and gold (a seated 1066
Munster game; the comparison in a second one).

  1  the spending veto dropped the AI's d_leinster 3 times (191, 191, 169 gold)   the veto spares the goal's chain
  2  the GOAL line silent while k_ireland waited on a duchy                         a harness's goal_line.chain_line
  3  months to afford with no word on what the raised armies cost                   months_to_target.armies_raised
  4  an ally's incoming calls to arms joined by the AI, no veto for them            the call_in kind (ctest 25a-h)
  5  title prices in the notes; the turn-one veto note silent on titles             notes.json
  6  the death band: the two named rulers added to the calibration rows             fixtures/ruler_deaths.json

No game.
"""
from __future__ import annotations

import asyncio
import copy
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))

import news as N  # noqa: E402
import notes as NT  # noqa: E402
import redesign as R  # noqa: E402
import ruler_death as RD  # noqa: E402
import seat_events as SE  # noqa: E402

MURCHAD, BRIAN = 31762, 36303
K_IRELAND, D_MUNSTER, D_LEINSTER, D_MEATH, D_ULSTER = 461, 470, 471, 472, 473
ALLY = 36358


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh():
    N.reset()
    yield
    N.reset()


class ChainGw:
    """A bridge with Murchad at 1071.11.07: k_ireland's check names the tier rule (2 duchy titles, 1 held), its
    de-jure duchies d_munster (his), d_leinster, d_meath, d_ulster. `chain_ids`: the bridge reads spare_title_ids back
    (a bridge); False: an older bridge that keeps only spare_title_id."""

    def __init__(self, held_duchies=1, chain_ids=True, check_ok=True):
        self.writes, self.reads, self.health = [], [], "ok"
        self.held, self.chain_ids, self.check_ok = held_duchies, chain_ids, check_ok
        self.table = {"vetoes": [], "decision_keys": [], "spare_title_id": 0, "spare_title_ids": []}

    def game(self):
        return {"date": "1071.11.07", "paused": True}

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if p.get("op") == "set" and p["what"] not in self.table["vetoes"]:
            self.table["vetoes"].append(p["what"])
        if p.get("what") == "create_title" and p.get("op") == "set":
            first = int(p.get("spare_title_id") or 0)
            more = [int(x) for x in str(p.get("spare_title_ids") or "").split(",") if x]
            self.table["spare_title_id"] = first
            self.table["spare_title_ids"] = ([first] if first else []) + [x for x in more if x != first] \
                if self.chain_ids else ([first] if first else [])
        return {"success": True}

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_de_jure":
            if (params or {}).get("title_key") == "k_ireland":
                kids = [(D_MUNSTER, "d_munster", MURCHAD), (D_LEINSTER, "d_leinster", 71911),
                        (D_MEATH, "d_meath", 0), (D_ULSTER, "d_ulster", 0),
                        (18269, "x_script_845", 0)]      # live 1072.01.01: a script's title of duchy tier
                return {"title": {"title_id": K_IRELAND, "key": "k_ireland"},
                        "de_jure_children": [{"title_id": i, "key": k, "tier": 3, "holder_id": h} for i, k, h in kids]}
            return {"title": {"title_id": K_IRELAND, "key": "k_ireland", "name": "Ireland"}}
        if action == "query_create_title_cost":
            if not self.check_ok:
                return {}
            reasons = ["Petty King Murchad does not have gold 325"]
            if self.held < 2:
                reasons = ["", f"Duchy Titles Required: 2 (currently: {self.held})",
                           "Kingdom Titles Required: 1 (currently: 0)"] + reasons
            return {"success": True, "engine_valid": False, "engine_reasons": reasons}
        if action == "query_ai_veto":
            ruler = {"character_id": (params or {}).get("character_id") or MURCHAD, **copy.deepcopy(self.table)}
            if not self.chain_ids:
                ruler.pop("spare_title_ids")
            return {"success": True, "rulers": [ruler], "records": []}
        return {}


def _spending(gw):
    return run(SE.do_ai_veto(gw, {"op": "set", "what": "spending"}, MURCHAD, {"id": MURCHAD}))


# ── 1. the create_title veto spares the goal's chain ───────────────────────────────────────────────────────────

def test_1_the_chain_is_the_games_tier_rule_and_the_unheld_dejure_duchies():
    ch = run(R.goal_chain(ChainGw(), MURCHAD, "k_ireland"))
    assert ch["option"] == {"tier": "duchy", "need": 2, "have": 1, "short": 1}
    assert [t["title_key"] for t in ch["titles"]] == ["d_leinster", "d_meath", "d_ulster"]   # d_munster is his


def test_1_the_rule_met_no_chain():
    """CONTROL: two duchies held, the check lists no tier line: nothing but the goal is spared."""
    assert run(R.goal_chain(ChainGw(held_duchies=2), MURCHAD, "k_ireland")) == {"titles": []}


def test_1_the_preset_spares_the_goal_and_its_chain_and_says_so(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_ireland")
    gw = ChainGw()
    env = _spending(gw)
    assert env["verdict"] == "applied", env
    (ct,) = [p for _a, p in gw.writes if p.get("what") == "create_title"]
    assert ct["spare_title_id"] == K_IRELAND and ct["spare_title_ids"] == f"{D_LEINSTER},{D_MEATH},{D_ULSTER}"
    g = env["result"]["goal_title"]
    assert g["spared"] is True and g["chain"] == {"spared": ["d_leinster", "d_meath", "d_ulster"]}
    assert "d_leinster" in g["why"] and "2 duchy titles, you hold 1" in g["why"]


def test_1_an_old_bridge_keeps_one_title_and_the_reply_says_the_chain_is_dropped(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_ireland")
    env = _spending(ChainGw(chain_ids=False))
    g = env["result"]["goal_title"]
    assert g["spared"] is True and g["chain"]["spared"] == []
    assert g["chain"]["not_spared"] == ["d_leinster", "d_meath", "d_ulster"] and "spares one title" in g["chain"]["why"]


def test_1_CONTROL_no_goal_title_no_chain_read(monkeypatch):
    monkeypatch.delenv(SE.GOAL_TITLE_ENV, raising=False)
    gw = ChainGw()
    _spending(gw)
    assert not [a for a, _p in gw.reads if a in ("query_de_jure", "query_create_title_cost")]
    assert all("spare_title_ids" not in p for _a, p in gw.writes)


def test_1_a_check_that_did_not_answer_spares_the_goal_only_and_says_why(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_ireland")
    gw = ChainGw(check_ok=False)
    env = _spending(gw)
    (ct,) = [p for _a, p in gw.writes if p.get("what") == "create_title"]
    assert "spare_title_ids" not in ct and "did not answer" in env["result"]["goal_title"]["chain_unread"]


def _rec(seq, dropped, key="d_leinster", tid=D_LEINSTER, priced=True):
    r = {"seq": seq, "character_id": MURCHAD, "what": "create_title", "title_id": tid, "dropped": dropped,
         "origin": "game", "date": "1071.05.26", "gold": 191.17, "prestige": 665.11}
    if key:
        r["key"] = key
    r.update({"price": {"gold": 162.5}, "priced": "yes"} if priced else {"priced": "pending"})
    return r


def test_1_every_dropped_creation_names_the_title_its_price_and_its_call():
    """The recorded row (1071.05.26 and 11.02, 191.17 gold) as the news shows it now."""
    (row,) = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, True), _rec(2, True)],
                                                     "actor_id": MURCHAD})["ai_vetoed"]
    assert row["key"] == "d_leinster" and row["title_id"] == D_LEINSTER and row["price"] == {"gold": 162.5}
    assert row["next"] == {"tool": "do", "args": {"intent": "mint_title", "args": {"title_key": "d_leinster"}}}
    assert row["attempts"] == 2


def test_1_a_key_the_bridge_did_not_read_still_names_the_title_by_id():
    (row,) = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, True, key=None)]})["ai_vetoed"]
    assert row["title_id"] == D_LEINSTER and "title_id names it" in row["key_unread"]


def test_1_a_drop_not_yet_priced_carries_the_last_price_read(monkeypatch):
    monkeypatch.setitem(N._CREATION_PRICE, "d_leinster", {"currency": "gold", "cost": 162.5, "date": "1071.05.01",
                                                          "actor_id": MURCHAD})
    (row,) = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, True, priced=False)],
                                                     "actor_id": MURCHAD})["ai_vetoed"]
    assert row["price_last_read"] == {"cost": 162.5, "currency": "gold", "date": "1071.05.01"}


def test_1_an_old_bridges_drop_of_a_chain_title_says_so(monkeypatch):
    SE.SPARED_CHAIN[MURCHAD] = {"key": "k_ireland", "ids": [], "keys": ["d_leinster"],
                                "rule": "the game's tier rule asks 2 duchy titles, you hold 1"}
    (row,) = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, True)],
                                                     "actor_id": MURCHAD})["ai_vetoed"]
    assert row["goal_chain"] is True and "k_ireland, your goal title, still needs" in row["note"]


def test_1_the_chain_is_sent_again_when_the_titles_held_change(monkeypatch):
    """d_leinster minted: the rule is met, so the other duchies stop being spared (the AI would buy a third)."""
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_ireland")
    gw = ChainGw()
    _spending(gw)
    assert SE.SPARED_CHAIN[MURCHAD]["ids"] == [D_LEINSTER, D_MEATH, D_ULSTER]
    gw.held = 2
    snap = {"actor_id": MURCHAD, "ai_vetoes": gw.table["vetoes"]}
    rs = run(N.resync_goal_chain(gw, snap, ["d_leinster"]))
    assert rs["spared"] == ["k_ireland"] and rs["chain"] == [] and gw.writes[-1][1] == {"character_id": MURCHAD, "op": "set", "what": "create_title",
                                                       "spare_title_id": K_IRELAND}
    assert gw.table["spare_title_ids"] == [K_IRELAND]


def test_1_CONTROL_no_resend_when_nothing_changed_or_no_title_of_duchy_tier(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_ireland")
    gw = ChainGw()
    _spending(gw)
    n = len(gw.writes)
    snap = {"actor_id": MURCHAD, "ai_vetoes": gw.table["vetoes"]}
    assert run(N.resync_goal_chain(gw, snap, ["d_meath"])) is None
    assert run(N.resync_goal_chain(gw, snap, ["c_ossory"])) is None and len(gw.writes) == n


def test_1_the_heir_carries_the_chain(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_ireland")
    gw = ChainGw()
    prev = {"actor_id": MURCHAD, "ai_vetoes": ["create_title"]}
    snap = {"actor_id": BRIAN, "ai_vetoes": []}
    row = run(N.carry_vetoes(gw, MURCHAD, prev, snap))
    (ct,) = [p for _a, p in gw.writes if p.get("what") == "create_title"]
    # computed for the heir: in this fake d_munster is still Murchad's, so it is one Brian does not hold
    assert ct["character_id"] == BRIAN and ct["spare_title_ids"] == f"{D_MUNSTER},{D_LEINSTER},{D_MEATH},{D_ULSTER}"
    assert row["goal_chain"]["spared"] == ["d_munster", "d_leinster", "d_meath", "d_ulster"]
    assert ("query_create_title_cost", {"actor_id": BRIAN, "title_key": "k_ireland"}) in gw.reads


# ── 3. what the raised armies cost, beside the realised net ─────────────────────────────────────────────────────

def test_3_armies_raised_at_both_reads_give_the_upkeep_and_the_months_stood_down():
    """A Munster run's 1082-85 shape: net -0.56 a month with 1,800 men raised: standing down is worth 5.4 a month."""
    mt = R.months_to_target(294.61, {"monthly": -0.56, "from": "1083.06.29", "to": "1084.06.29",
                                     "armies_men": [1800, 1800]})
    ar = mt["armies_raised"]
    assert mt["months"] is None and ar["men"] == 1800 and ar["upkeep_a_month"] == 5.4
    assert ar["if_stood_down"] == {"net_monthly": 4.84, "months": 61}
    assert "GOLD_COST_PER_SOLDIER" in ar["basis"] and "not read" in ar["basis"]


def test_3_raised_only_now_gives_the_upkeep_and_no_stood_down_count():
    """The step's net did not pay a whole step of that upkeep (no army at the step's start): no what-if."""
    mt = R.months_to_target(100.0, {"monthly": 4.0, "from": "1083.06.29", "to": "1084.06.29",
                                    "armies_men": [0, 900]})
    assert mt["months"] == 25 and mt["armies_raised"]["upkeep_a_month"] == 2.7
    assert "if_stood_down" not in mt["armies_raised"]


def test_3_CONTROL_no_army_raised_no_field():
    for men in ([0, 0], None, [None, None]):
        mt = R.months_to_target(100.0, {"monthly": 4.0, "from": "1083.06.29", "to": "1084.06.29",
                                        **({"armies_men": men} if men is not None else {})})
        assert "armies_raised" not in mt


def test_3_the_step_read_keeps_the_men_raised_at_both_reads(monkeypatch):
    prev = {"actor_id": MURCHAD, "date": "1083.06.29", "gold": 10.0, "armies_men": 1800}
    snap = {"actor_id": MURCHAD, "date": "1084.06.29", "gold": 3.6, "armies_men": 1700}
    N._resource_change(prev, snap, "gold", {})
    assert N.realised_net(MURCHAD)["armies_men"] == [1800, 1700]


# ── 4. incoming calls to arms ──────────────────────────────────────────────────────────────────────────────────

def _call(seq, dropped, war=184549402):
    return {"seq": seq, "character_id": MURCHAD, "what": "call_in", "called_by": ALLY, "war_id": war,
            "dropped": dropped, "origin": "game", "date": "1082.06.02", "gold": 136.0, "prestige": 900.0,
            "key": "call_ally_interaction", "priced": "no", "unpriced_why": "call_in_no_price"}


def test_4_call_in_is_a_kind_the_preset_leaves_out_with_its_reason():
    assert "call_in" in SE.AI_VETO_WHATS and "call_in" not in SE.AI_VETO_PRESETS["spending"]
    assert "defensive ones too" in SE.AI_VETO_LEFT_OUT["call_in"]
    assert SE.REFUSALS["call_in_interactions_unresolved"] == "engine_unreachable"


def test_4_an_accepted_call_names_the_caller_the_war_and_what_joining_costs():
    snap = {"actor_id": MURCHAD, "armies_men": 1650,
            "wars": {184549402: {"war_id": 184549402, "name": "Moravian War for Suðrland", "attacker_id": ALLY,
                                 "defender_id": 40000}}}
    (a,) = SE.veto_news({"ai_veto_records": []}, {**snap, "ai_veto_records": [_call(1, False)]})["ai_acted"]
    assert a["called_by"] == ALLY and a["war"] == "Moravian War for Suðrland" and a["attacker_id"] == ALLY
    assert a["joining_costs"]["upkeep_a_month"] == -4.95 and "answered at once" in a["sent_by"]
    assert a["veto"] == {"tool": "do", "args": {"intent": "ai_veto", "args": {"op": "set", "what": "call_in"}}}
    assert "key" not in a and "price_unread" not in a


def test_4_a_dropped_call_is_one_row_per_caller_and_war():
    (row,) = SE.veto_news({"ai_veto_records": []}, {"actor_id": MURCHAD,
                                                     "ai_veto_records": [_call(1, True), _call(2, True)]})["ai_vetoed"]
    assert row["what"] == "call_in" and row["attempts"] == 2 and row["called_by"] == ALLY
    assert "never reached your ruler" in row["note"] and "war_unread" in row


# ── 5. notes: no title price a read gives; the veto note names the goal chain ──────────────────────────────────

def test_5_the_default_notes_carry_no_title_price():
    doc = NT.load()
    for n in doc["notes"]:
        for stale in ("212.5", "425", "325"):
            assert stale not in n["text"], (n["id"], stale)


def test_5_the_turn_one_veto_note_says_it_stops_titles_other_than_the_goals():
    doc = NT.load()
    (t,) = [n["text"] for n in doc["notes"] if n["id"] == "ai-spends-for-you"]
    assert "titles other than your goal and those it needs" in t and "what=spending" in t
    assert len(json.dumps(NT.turn_one_texts(doc), ensure_ascii=False).encode()) <= 1700


# ── 6. the death band over the two named rulers ────────────────────────────────────────────────────────────────

def test_6_the_two_named_deaths_are_in_the_rows_and_inside_the_band():
    rows = json.load(open(HERE / "fixtures" / "ruler_deaths.json"))["rows"]
    for game in ("AJ", "AK"):
        first = next(r for r in rows if r["game"] == game and r["ruler"] == MURCHAD)
        y = RD.estimate(first["age"], first["health"])["years"]
        assert first["died"] is True and y["p25"] < first["hi"] <= y["p90"], (game, first, y)


# ── two wordings seen live (a recorded run, 1072.05.31) ──────────────────────────────────────────────────

def test_1_live_the_ai_s_creation_of_a_spared_chain_title_is_named_so_with_no_veto_offered(monkeypatch):
    """Live: d_leinster created for Murchad on 1072.05.13 read "no veto set for it" and offered the create_title
    veto that was set (the resync of the same news had already emptied the chain)."""
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_ireland")
    gw = ChainGw()
    _spending(gw)
    gw.held = 2
    run(N.resync_goal_chain(gw, {"actor_id": MURCHAD, "ai_vetoes": gw.table["vetoes"]}, ["d_leinster"]))
    (a,) = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, False)],
                                                   "actor_id": MURCHAD})["ai_acted"]
    assert a["goal_chain"] is True and "veto" not in a and "k_ireland, your goal title, needed" in a["sent_by"]


def test_1_CONTROL_a_title_never_spared_still_offers_the_veto(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_ireland")
    (a,) = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, False, key="d_desmond", tid=480)],
                                                   "actor_id": MURCHAD})["ai_acted"]
    assert "goal_chain" not in a and a["veto"]["args"]["args"]["what"] == "create_title"


def test_1_live_the_chain_met_row_says_the_goal_alone_is_spared(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_ireland")
    gw = ChainGw()
    _spending(gw)
    gw.held = 2
    rs = run(N.resync_goal_chain(gw, {"actor_id": MURCHAD, "ai_vetoes": gw.table["vetoes"]}, ["d_leinster"]))
    assert rs["spared"] == ["k_ireland"] and rs["chain"] == [] and "spares k_ireland alone again" in rs["note"]
