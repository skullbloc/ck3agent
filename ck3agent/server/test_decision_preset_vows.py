"""Rows from the third seated Munster run, replayed from its recorded replies.

  1a the AI's vow of poverty, never dropped: decision not in the spending preset   the preset's decision kind (gold rule + keys)
  1b renounce read available/valid; the take refused on ai_potential               the bridge's ai_potential read; its conditions
  1c the vow's -20% income read nowhere                                            income_modifiers (economy, income_levers)
  2  "need 7" beside the game's "6 of 8 required Counties"                         more than half; the game's own line wins
  3  a war begun and lost inside one step: no wars_ended row                       the game's war history for the window
  4  wars_at_we_win said 100 while the game's guard read 0-25                      the row stands only on the game's reading
  5  the younger son "leaves_realm" with a duchy the law could not create          stays_vassal below the tier
  6  no gold rule on the 3-option claim letter 0302                                pay_if with the decline as else; variants
  7  retreat refusal explained by the hostile-army rule; a refused order gone      game_reason_why; ORDER_GONE; `more` first

No game; the game's files are read where a row says so (skipped when they are not on this host).
"""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))

import army_orders as AO  # noqa: E402
import event_prices as EP  # noqa: E402
import news as N  # noqa: E402
import partition_forecast as PF  # noqa: E402
import redesign as R  # noqa: E402
import reply_fit as F  # noqa: E402
import seat_events as SE  # noqa: E402

MURCHAD, DONNCHAD, BRIAN = 31762, 71270, 36303
FILES = SE.game_dir() is not None
need_files = pytest.mark.skipif(not FILES, reason="the game's files are not on this host")


def run(c):
    return asyncio.run(c)


# ── 1a. the spending preset drops the decisions that cost gold or income ──────────────────────────────────────

class VetoGw:
    """set_ai_veto / query_ai_veto as the bridge answers them (ai_veto.cpp): the table per kind, decision keys and
    the gold rule; `gold_readback` False models a bridge without the gold rule (the param is ignored, no field read back)."""

    def __init__(self, gold_readback=True, keys=(), vetoes=()):
        self.vetoes, self.keys, self.gold = set(vetoes), list(keys), False
        self.writes, self.gold_readback = [], gold_readback

    def game(self):
        return {"date": "1066.09.15"}

    health = "ok"

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if action == "set_ai_veto":
            if p["op"] == "set":
                self.vetoes.add(p["what"])
                if p["what"] == "decision":
                    self.keys = [k for k in str(p.get("keys") or "").split(",") if k]
                    self.gold = p.get("gold_cost") == 1
            else:
                self.vetoes.discard(p["what"])
            return {"success": True}
        return {"success": True}

    async def read(self, action, params=None):
        if action == "query_ai_veto":
            r = {"character_id": MURCHAD, "vetoes": sorted(self.vetoes), "decision_keys": list(self.keys),
                 "spare_title_id": 0}
            if self.gold_readback:
                r["decision_gold_cost"] = self.gold
            return {"success": True, "rulers": [r]}
        return {}


def _veto_ruler_ok():
    # the reader the preset uses (the table's shape: {"rulers": [...]})
    return SE._veto_ruler({"success": True, "rulers": [{"character_id": MURCHAD, "vetoes": []}]}, MURCHAD)


def test_1a_the_spending_preset_carries_decision_with_the_gold_rule_and_the_income_keys():
    assert "decision" in SE.AI_VETO_PRESETS["spending"]
    assert "decision" not in SE.AI_VETO_LEFT_OUT
    # the two stress reliefs that pay gold through their on_action's event join the three that cost income or prestige
    assert set(SE.SPENDING_DECISION_KEYS) == {"take_vow_of_poverty_decision", "provide_for_the_poor_oath_decision",
                                              "isolate_family_decision", "stress_loss_improvident_decision",
                                              "stress_loss_profligate_decision"}
    if _veto_ruler_ok() is None:
        pytest.skip("the veto table reader takes another shape")
    gw = VetoGw()
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "spending"}, MURCHAD, {"id": MURCHAD}))
    dec = [p for a, p in gw.writes if a == "set_ai_veto" and p["what"] == "decision"]
    assert dec and dec[0]["gold_cost"] == 1 and "take_vow_of_poverty_decision" in dec[0]["keys"].split(",")
    assert env["verdict"] == "applied", env
    d = env["result"]["decisions"]
    assert d["by_gold_cost"] is True and "take_vow_of_poverty_decision" in d["keys"]


def test_1a_the_seats_own_keys_are_kept_and_an_old_bridge_is_named():
    if _veto_ruler_ok() is None:
        pytest.skip("the veto table reader takes another shape")
    gw = VetoGw(gold_readback=False, keys=["isolate_family_decision"], vetoes=["decision"])
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "spending"}, MURCHAD, {"id": MURCHAD}))
    dec = [p for a, p in gw.writes if a == "set_ai_veto" and p["what"] == "decision"][0]
    assert dec["keys"].split(",")[0] == "isolate_family_decision"
    assert env["result"]["decisions"]["gold_cost_unread"] == SE.GOLD_RULE_ABSENT


def test_1a_CONTROL_a_veto_on_every_decision_is_left_as_it_is():
    if _veto_ruler_ok() is None:
        pytest.skip("the veto table reader takes another shape")
    gw = VetoGw(vetoes=["decision"])
    run(SE.do_ai_veto(gw, {"op": "set", "what": "spending"}, MURCHAD, {"id": MURCHAD}))
    assert not [p for a, p in gw.writes if a == "set_ai_veto" and p["what"] == "decision"]


def test_1a_a_keyed_decision_set_keeps_the_gold_rule():
    if _veto_ruler_ok() is None:
        pytest.skip("the veto table reader takes another shape")
    gw = VetoGw(keys=["take_vow_of_poverty_decision"], vetoes=["decision"])
    gw.gold = True
    run(SE.do_ai_veto(gw, {"op": "set", "what": "decision", "keys": ["isolate_family_decision"]}, MURCHAD,
                      {"id": MURCHAD}))
    p = [p for a, p in gw.writes if a == "set_ai_veto"][-1]
    assert p.get("gold_cost") == 1 and gw.gold is True


@need_files
def test_1a_the_vow_is_priced_by_its_income_not_free():
    a = {"what": "decision", "key": "take_vow_of_poverty_decision"}
    SE.decision_row_price(a, {"actor_id": MURCHAD})
    assert "-20%" in a["price"] and "free" not in a["price"]
    b = {"what": "decision", "key": "pet_dog_decision"}
    SE.decision_row_price(b, {"actor_id": MURCHAD})
    assert str(b["price"]).startswith("free (no cost block")            # CONTROL: a free one stays free


# ── 1b. a decision the seat's take would be refused on reads so, with the condition ────────────────────────────

RENOUNCE_ROW = {"name": "renounce_vow_of_poverty_decision", "shown": True, "valid": True, "cost_ok": True,
                "afford_ok": True, "available": False, "ai_potential": False,
                "failures": ["ai_potential_not_met"], "cost": {}}


@need_files
def test_1b_the_row_names_the_ai_potential_conditions():
    row = R._decision_row(dict(RENOUNCE_ROW))
    b = row["blocked"]
    assert b["gate"] == "ai_potential_not_met"
    assert "monthly_character_expenses > monthly_character_income" in b["summary"]
    assert EP.decision_ai_potential("renounce_vow_of_poverty_decision")[0] == \
        "has_character_modifier = vow_of_poverty_modifier"


def test_1b_RED_the_base_row_said_available():
    """The recorded reply (turn 11, turn 12): available/valid/cost_ok with no ai_potential read; the row was not blocked."""
    base = dict(RENOUNCE_ROW, available=True, failures=[])
    base.pop("ai_potential")
    assert R._decision_row(base)["blocked"] is None


# ── 1c. an income modifier is read where income is read ─────────────────────────────────────────────────────

class DecGw:
    def __init__(self, shown):
        self.shown = shown

    async def read(self, action, params=None):
        if action == "list_decisions_annotated":
            return {"valid_only": False, "considered": 1, "decisions": [
                dict(RENOUNCE_ROW, shown=self.shown)]}
        return {}


@need_files
def test_1c_the_vow_is_listed_with_its_cost_and_the_check_on_renouncing():
    rows = run(R.income_modifiers(DecGw(True), MURCHAD))
    assert rows and rows[0]["modifier"] == "vow_of_poverty_modifier" and rows[0]["effect"] == "monthly income -20%"
    assert rows[0]["blocked"]["gate"] == "ai_potential_not_met"
    assert run(R.income_modifiers(DecGw(False), MURCHAD)) == []            # CONTROL: not shown, not held


# ── 2. the counties: one number, the game's ─────────────────────────────────────────────────────────────────

def test_2_the_rule_is_more_than_half_as_the_games_lines_read():
    # the game's own "required Counties" lines in the recorded runs: total -> required
    for total, need, tier in ((2, 2, 3), (3, 2, 3), (4, 3, 3), (6, 4, 3), (14, 8, 4), (117, 94, 5)):
        assert R.dejure_need(total, tier) == need, (total, tier)
    assert "more than half" in R.MINT_DEJURE_RULE


def test_2_the_games_line_is_the_counties_row():
    import test_mint_price as TM
    gw = TM.Gateway(TM.price_reply(cost=325.0, have=40.0, engine_valid=False,
                                   reasons=["You only have 6 of 8 required Counties", "Petty King Murchad does "
                                            "not have gold 325"]),
                    dejure=dict(TM.DEJURE, held=6, total=14))
    row, blk = run(R.title_requirements(gw, MURCHAD, "k_ireland"))
    dj = next(q for q in row["requirements"] if q["requirement"] == "de_jure_counties")
    assert (dj["have"], dj["need"]) == (6, 8) and dj["source"] == R.GAME_COUNTIES_SOURCE
    assert row["measured"]["need"] == 8 and "6/8" in blk["summary"]
    assert R.game_counties_of_reasons(["Petty King Murchad does not have gold 325"]) is None


# ── 3. a war begun and ended inside one step ─────────────────────────────────────────────────────────────────

class HistGw:
    def __init__(self, wars):
        self.wars, self.reads = wars, []

    async def read_many(self, calls):
        out = []
        for a, p in calls:
            self.reads.append((a, p))
            side = "attacker_id" if "attacker_id" in p else "defender_id"
            out.append({"success": True, "wars": [w for w in self.wars if w.get(side) == p[side]]})
        return out


DUBLIN = {"war_id": 34, "war_name": "Munsterian-Dublin Vassalization War", "outcome": "defender_victory",
          "end_date": "1068.05.30", "attacker_id": MURCHAD, "defender_id": 31808, "cb_type": "vassalization_cb",
          "target_titles": []}


def test_3_the_lost_war_of_the_window_has_its_row():
    gw = HistGw([DUBLIN, dict(DUBLIN, war_id=4, end_date="1067.01.02")])
    rows = run(N.wars_ended_in_window(gw, {"date": "1067.10.26"}, {"date": "1068.06.14", "actor_id": MURCHAD},
                                      set()))
    assert [r["war_id"] for r in rows] == [34]                            # the 1067.01 war is outside the window
    r = rows[0]
    assert r["outcome"] == "defender_victory" and r["my_side"] == "attacker" and r["_cb"] == "vassalization_cb"
    assert N._war_lost(r)


def test_3_CONTROL_a_war_already_named_is_not_named_twice():
    gw = HistGw([DUBLIN])
    assert run(N.wars_ended_in_window(gw, {"date": "1067.10.26"}, {"date": "1068.06.14", "actor_id": MURCHAD},
                                      {34})) == []


# ── 4. we_win only on the game's own reading ──────────────────────────────────────────────────────────────────

def test_4_the_cache_alone_no_longer_offers_we_win(monkeypatch):
    seen = []

    async def reading(gw, me, enemy):
        seen.append((me, enemy))
        # the game's own listing on 1076.05.12 (turn 18's refusal): our score at least 0, below 25
        return R.end_war_reading(["LEADS", "OPEN_WHITE_PEACE", "OPEN_WE_YIELD", "SCORE_GE_0", "SCORE_GE_M25",
                                  "SCORE_GE_M50", "SCORE_GE_M100"])

    monkeypatch.setattr(R, "end_war_open_terms", reading)
    snap = {"actor_id": MURCHAD, "war_scores": {33554479: 100},
            "wars": {33554479: {"name": "Peasant Uprising", "attacker_id": 41369, "defender_id": MURCHAD,
                                "is_attacker": False}}}
    assert run(N.we_win_rows(None, snap)) == [] and seen == [(MURCHAD, 41369)]

    async def opens(gw, me, enemy):
        return R.end_war_reading(["LEADS", "OPEN_WE_WIN", "OPEN_WHITE_PEACE", "OPEN_WE_YIELD", "SCORE_GE_100"])

    monkeypatch.setattr(R, "end_war_open_terms", opens)
    rows = run(N.we_win_rows(None, snap))
    assert rows[0]["next_call"]["args"]["args"] == {"enemy_id": 41369, "terms": "we_win"}
    import inspect
    assert "we_win_rows(gw, snap)" in inspect.getsource(N.block)            # the news block's one path


def test_4_RED_the_base_offered_it_from_the_cache(monkeypatch):
    """The recorded row (step 17): war_score_yours 100 and the we_win call, from the cache alone. A reading that does
    not answer now gives no row (never the cache's word)."""
    async def unread(gw, me, enemy):
        return None

    monkeypatch.setattr(R, "end_war_open_terms", unread)
    snap = {"actor_id": MURCHAD, "war_scores": {33554479: 100},
            "wars": {33554479: {"attacker_id": 41369, "defender_id": MURCHAD, "is_attacker": False}}}
    assert run(N.we_win_rows(None, snap)) == []


# ── 5. below the tier: the heir's vassal ─────────────────────────────────────────────────────────────────────

def _kids():
    return [{"id": BRIAN, "alive": True, "is_female": False, "traits": []},
            {"id": DONNCHAD, "alive": True, "is_female": False, "traits": []}]


def test_5_no_duchy_of_his_own_the_younger_son_stays():
    f = PF.forecast(ruler_tier=3, succession_law=PF.CONFEDERATE, gender_law="male_preference_law",
                    primary_heir_id=BRIAN, children=_kids(), held_counties=3, tier_shares=[])
    s = f["younger_shares"][0]
    assert s["id"] == DONNCHAD and s["fate"] == "stays_vassal" and s["why"] == PF.STAYS_BELOW_TIER


def test_5_CONTROL_a_duchy_the_law_creates_still_leaves():
    f = PF.forecast(ruler_tier=3, succession_law=PF.CONFEDERATE, gender_law="male_preference_law",
                    primary_heir_id=BRIAN, children=_kids(), held_counties=3,
                    tier_shares=[{"title": "d_ulster", "title_id": 400, "created": True}])
    assert f["younger_shares"][0]["fate"] == "leaves_realm"


def test_5_d_leinster_at_1_of_2_is_not_created():
    """The 1071 forecast's d_leinster: 1 of its 2 counties in the realm; the game requires 2 (more than half)."""
    assert 1 < R.dejure_need(2, 3)


# ── 6. the claim letter's variants and its third option ──────────────────────────────────────────────────────

@need_files
def test_6_options_matched_by_what_they_do():
    assert SE.free_option("court_chaplain_task.0302") == "court_chaplain_task.0301.b"
    assert SE.free_option("court_chaplain_task.0301") == "court_chaplain_task.0301.b"
    assert SE.same_effect_option("court_chaplain_task.0301", "court_chaplain_task.0301.a",
                                 "court_chaplain_task.0302") == "court_chaplain_task.0302.a"
    assert SE.same_effect_option("court_chaplain_task.0302", "court_chaplain_task.0302.b",
                                 "court_chaplain_task.0301") is None           # the duchy claim: no 0301 twin
    v = SE.variant_rule("court_chaplain_task.0301", "court_chaplain_task.0302",
                        {"character_id": MURCHAD, "op": "set", "event": "court_chaplain_task.0301",
                         "answer": "pay_if", "option": "court_chaplain_task.0301.a", "currency": "gold",
                         "amount": 91})
    assert v == {"character_id": MURCHAD, "op": "set", "event": "court_chaplain_task.0302", "answer": "pay_if",
                 "option": "court_chaplain_task.0302.a", "currency": "gold", "amount": 91,
                 "else": "court_chaplain_task.0301.b"}
    d = SE.variant_rule("court_chaplain_task.0301", "court_chaplain_task.0302",
                        {"op": "set", "event": "court_chaplain_task.0301", "answer": "decline"})
    assert d == {"op": "set", "event": "court_chaplain_task.0302", "answer": "option",
                 "option": "court_chaplain_task.0301.b"}


class RuleGw:
    def __init__(self):
        self.writes, self.rules = [], []

    def game(self):
        return {"date": "1078.11.01"}

    health = "ok"

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if action == "set_event_rule" and p.get("op") == "set":
            self.rules = [r for r in self.rules if r["event"] != p["event"]] + [
                {"event": p["event"], "answer": p["answer"]}]
        return {"success": True}

    async def read(self, action, params=None):
        if action == "query_event_rules":
            return {"success": True, "seats": [{"character_id": MURCHAD, "rules": list(self.rules)}], "picks": []}
        if action in ("query_character", "query_council"):
            return {}
        return {}


@need_files
def test_6_a_gold_rule_on_0302_takes_the_decline_as_else_and_covers_0301(monkeypatch):
    async def none(*a, **k):
        return None

    monkeypatch.setattr(SE, "letter_cost_warning", none)
    monkeypatch.setattr(SE, "letter_facts", none)
    gw = RuleGw()
    env = run(SE.do_event_rule(gw, {"op": "set", "event": "court_chaplain_task.0302", "answer": "pay_if",
                                    "option": "court_chaplain_task.0302.a", "currency": "gold", "amount": 91},
                               MURCHAD, {"id": MURCHAD}))
    assert env["verdict"] == "applied", env
    sent = [p for a, p in gw.writes if a == "set_event_rule"]
    assert sent[0]["else"] == "court_chaplain_task.0301.b"
    assert env["result"]["event_check"]["else_by_effect"] == "court_chaplain_task.0301.b"
    assert sent[1]["event"] == "court_chaplain_task.0301" and sent[1]["option"] == "court_chaplain_task.0301.a"
    assert env["result"]["variants"][0]["set"] is True


@need_files
def test_6_RED_the_base_refused_pay_if_on_0302(monkeypatch):
    """The recorded refusal (turn 11): needs_two_options. With the free option unread the refusal still stands."""
    monkeypatch.setattr(SE, "free_option", lambda e: None)
    env = run(SE.do_event_rule(RuleGw(), {"op": "set", "event": "court_chaplain_task.0302", "answer": "pay_if",
                                          "option": "court_chaplain_task.0302.a", "amount": 91}, MURCHAD,
                               {"id": MURCHAD}))
    assert env["blocked"]["raw_reason"] == "needs_two_options"


def test_6_the_claim_letter_levers_are_gold_rules():
    calls = N.letter_rule_calls(91)
    assert calls["court_chaplain_task.0302"]["args"]["args"]["answer"] == "pay_if"
    assert calls["court_chaplain_task.0302"]["args"]["args"]["else"] == N.CLAIM_DECLINE_OPTION


# ── 7. small ones ────────────────────────────────────────────────────────────────────────────────────────────

def test_7_the_retreat_refusal_gives_its_own_cause():
    w = AO.game_reason_why("Cannot disband while in Retreat")
    assert w and "retreat" in w and "hostile" not in w
    assert AO.game_reason_why("hostile Armies are around") is None        # CONTROL: that one keeps the rule


def test_7_page_one_opens_with_the_page_line_and_the_later_blocks():
    env = {"ok": True, "tool": "observe", "result": {"news": {"a" * 10: ["x" * 300] * 40, "b": ["y" * 300] * 40}},
           "warnings": [], "errors": [], "bridge_health": "ok"}
    out = F.paginate(env, "observe")
    assert list(out["result"])[0] == "more"
    assert list(out["result"]["more"])[:2] == ["page", "on_later_pages"]


def test_3_wars_ended_carries_the_window_rows():
    """The news path: wars_ended (the diff) adds the window's history rows."""
    gw = HistGw([DUBLIN])
    rows = run(N.wars_ended(gw, {"date": "1067.10.26", "actor_id": MURCHAD},
                            {"date": "1068.06.14", "actor_id": MURCHAD}, {}, {}))
    assert [r["war_id"] for r in rows] == [34] and rows[0]["seen"] == N.WINDOW_SEEN


class ArmyGw:
    health = "ok"

    def game(self):
        return {"date": "1068.06.14"}

    async def write(self, action, params=None):
        return {"success": False, "reason": "refused_by_game", "game_reason": "Cannot disband while in Retreat",
                "around": [], "army_id": 304}

    async def read(self, action, params=None):
        return {"armies": []}


def test_7_the_disband_refusal_through_the_call():
    env = run(AO.do_army(ArmyGw(), {"action": "disband", "army_id": 304}, MURCHAD, {"id": MURCHAD},
                         environ={"CK3_BRIDGE_ARMY_ORDERS": "1"}))
    s = env["blocked"]["summary"]
    assert "retreating" in s and "hostile" not in s, s
