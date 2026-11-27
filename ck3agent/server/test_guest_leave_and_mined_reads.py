"""Guest activities, hostile armies and other reads, from the reply shapes 22 seated runs recorded.

Each row drives the code with the reply shapes the runs recorded (turn named on the row). Everything here is
built, not proven live. No game, no model.
"""
import asyncio
import json
from pathlib import Path

import pytest

import activities as A
import army_orders as AO
import do_intent_args
import news
import notes as N
import redesign as R
import refusal_memo as RM
import seat_events
import tool_text
import tools
from test_ireland_walk_replies import Gw
from test_map_digest_and_turn_one_notes import recorded_title  # noqa: F401  (the map's recorded title table)

HERE = Path(__file__).resolve().parent
ME = 31762            # Murchad of Munster, the seat of the mined runs
POPE = 1003


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh(monkeypatch):
    news.reset()

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    yield
    news.reset()


class ActGw(Gw):
    """The `activity` template answers the next marks of `queue` per call (the leave, then the read-back listing)."""

    def __init__(self, queue, guards=None, replies=None):
        super().__init__(replies, "1070.06.01")
        self.queue, self.guards = list(queue), list(guards or [])

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if action == "script_action" and p.get("template") == "activity":
            marks = self.queue.pop(0) if self.queue else []
            guard = self.guards.pop(0) if self.guards else "ran"
            return {"success": True, "template": "activity", "token": "SACT_4", "guard": guard, "marks": marks,
                    "errors": []}
        return {"success": True}


# ── A guest activity (two recorded games, turns 6-14 and turns 44-46) ───────────────────────────────────────

CORONATION_GUEST = ["IN_ACTIVITY", "IN_T_2"]


def test_1_a_guest_reads_what_it_blocks_and_his_way_out():
    a = A.in_activity(CORONATION_GUEST)
    assert a["type"] == "activity_coronation" and a["you_host"] is False
    assert "declaring war" in a["blocks"] and "not peace offers" in a["blocks"]
    assert a["leave"]["args"] == {"intent": "host_activity", "args": {"activity": "leave"}}
    assert a["leave"]["proven"] == "built, not proven live"


def test_1_CONTROL_a_host_is_not_offered_the_leave():
    a = A.in_activity(CORONATION_GUEST + ["HOSTING"])
    assert a["you_host"] is True and "leave" not in a


def test_1_leave_sends_the_template_leave_and_reads_him_out():
    gw = ActGw([CORONATION_GUEST, []])
    env = run(A.do_host_activity(gw, {"activity": "leave"}, ME, {"id": ME}))
    sent = [p for a, p in gw.writes if a == "script_action"]
    assert sent[0] == {"actor_id": ME, "leave": True, "template": "activity"}, sent
    assert sent[1] == {"actor_id": ME, "template": "activity"}            # the read-back listing writes nothing
    assert env["verdict"] == "applied" and "in no activity" in env["result"]["after"], env
    assert env["result"]["was_in"]["type"] == "activity_coronation"
    assert env["result"]["proven"] == "built, not proven live"


def test_1_leave_the_game_ran_but_he_is_still_in_it_is_not_applied():
    gw = ActGw([CORONATION_GUEST, CORONATION_GUEST])
    env = run(A.do_host_activity(gw, {"activity": "leave"}, ME, {"id": ME}))
    assert env["verdict"] == "rejected" and env["blocked"]["raw_reason"] == "still_in_activity"


def test_1_leave_refused_by_the_games_guard_says_why():
    gw = ActGw([CORONATION_GUEST + ["HOSTING"]], guards=["refused"])
    env = run(A.do_host_activity(gw, {"activity": "leave"}, ME, {"id": ME}))
    assert env["verdict"] == "rejected" and "a host cannot leave" in env["blocked"]["summary"], env
    gw = ActGw([[]], guards=["refused"])
    env = run(A.do_host_activity(gw, {"activity": "leave"}, ME, {"id": ME}))
    assert "in no activity" in env["blocked"]["summary"]


def test_1_the_declaration_refusal_names_the_leave_and_the_rule():
    from test_map_digest_and_turn_one_notes import _act_gw
    from test_raised_army_declaration_rule import DA_MAYO
    env = run(R.declare_war_impl(_act_gw(CORONATION_GUEST + ["RAN"]), 31506, declare_args=dict(DA_MAYO),
                                 confirm_straddle=True))
    b = env["blocked"]
    assert b["fix"] == {"tool": "do", "args": {"intent": "host_activity", "args": {"activity": "leave"}}}
    assert "activity=leave" in b["summary"] and "no ai_veto kind stops" in b["summary"]
    assert b["measured"]["in_activity"]["blocks"] == A.IN_ACTIVITY_BLOCKS


def test_1_CONTROL_a_hosts_declaration_refusal_points_at_the_listing():
    from test_map_digest_and_turn_one_notes import _act_gw
    from test_raised_army_declaration_rule import DA_MAYO
    env = run(R.declare_war_impl(_act_gw(CORONATION_GUEST + ["HOSTING", "RAN"]), 31506,
                                 declare_args=dict(DA_MAYO), confirm_straddle=True))
    assert env["blocked"]["fix"] == {"tool": "do", "args": {"action_class": "activities"}}


def test_1_the_args_table_takes_leave_and_the_bridge_template_carries_it():
    assert ("host_activity", "activity") in do_intent_args.arg_values()
    assert do_intent_args.arg_values()[("host_activity", "activity")][-1] == "leave"
    h = (HERE.parents[1] / "bridge" / "include" / "script_templates.h").read_text()
    assert "remove_from_activity = involved_activity return_home = yes" in h   # the game's own Go home effects


def test_1_the_game_files_hold_the_effects_the_template_runs():
    game = Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game"
    ev = game / "events" / "activities" / "activity_system_events.txt"
    if not ev.is_file():
        pytest.skip("no local install")
    t = ev.read_text(encoding="utf-8-sig")
    i = t.index("activity_system.0400.b")
    assert "remove_from_activity = involved_activity" in t[i:i + 300] and "return_home = yes" in t[i:i + 300]
    war = (game / "common" / "character_interactions" / "00_war.txt").read_text(encoding="utf-8-sig")
    assert "exists = involved_activity" in war


def test_1_the_turn_one_note_names_the_guest_activity():
    doc = N.load()
    text = " ".join(N.turn_one_texts(doc))
    assert "guest invitations" in text and "do host_activity activity=leave" in text
    # The leave was seen working in a seated run (a recorded game, turn 22: refused in_activity naming it; the seat left
    # and declared), so the note no longer says it is unproven
    assert "built, not proven live" not in text


# ── "hostile armies are around" (a recorded game turns 5-11) ──────────────────────────────────────────────────

AROUND = [{"army_id": 166, "owner_id": 32287, "men": 900, "province_id": 1500, "barony": "b_ossory",
           "county_id": 501},
          {"army_id": 170, "owner_id": 28892, "men": 120, "province_id": 1511, "barony": "b_kilkenny",
           "county_id": 502}]


class RGw(Gw):
    """Gw whose writes answer `replies[action]` too."""

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return self.replies.get(action, {"success": True})


def _army_gw(reply, location=1491):
    return RGw({"army_disband": reply,
               "query_armies": {"armies": [{"army_id": 261, "troop_count": 547, "location_province_id": location,
                                            "path_steps": 0}]}}, "1068.07.01")


def test_2_the_disband_refusal_names_whose_armies_and_the_rule(monkeypatch):
    monkeypatch.setenv(AO.ENV_ORDERS, "1")
    reply = {"success": False, "reason": "refused_by_game", "army_id": 261,
             "game_reason": "Cannot disband while enemy or hostile Armies are around", "around": AROUND,
             "around_total": 3}
    env = run(AO.do_army(_army_gw(reply), {"action": "disband", "army_id": 261}, ME, {"id": ME}))
    d = env["blocked"]["summary"]
    assert "army 166 of 32287 (900 men) at b_ossory" in d and "army 170 of 28892" in d and "1 more" in d, d
    assert AO.AROUND_RULE in d and "built, not proven live" in d
    assert env["result"]["army_at"] == "1491:0"


def test_2_CONTROL_no_army_around_and_an_older_bridge():
    assert AO.around_words({"around": []}).startswith("no other ruler's army")
    assert AO.around_words({"around": "unread"}) == "the county around your army was not read"
    assert AO.around_words({"game_reason": "x"}) is None          # an older bridge: nothing invented


def test_2_a_check_is_never_answered_from_memory():
    assert RM.do_key(ME, "army", {"action": "disband", "army_id": 261, "check": True}) is None
    assert RM.do_key(ME, "army", {"action": "disband", "army_id": 261}) is not None
    assert RM.do_key(ME, "army", {"action": "disband", "army_id": 261, "check": False}) is not None


def test_2_a_moved_army_is_asked_again():
    env = {"actor": {"id": ME}, "result": {"army_id": 261, "army_at": "1491:0", "raw": {"army_id": 261}}}
    assert run(AO.moved_since(_army_gw({}, location=1500), env)) is True
    assert run(AO.moved_since(_army_gw({}, location=1491), env)) is False
    # unread either way: asked again, never answered from memory
    assert run(AO.moved_since(Gw({"query_armies": {"armies": []}}), env)) is True
    assert run(AO.moved_since(_army_gw({}), dict(env, result={"army_id": 261}))) is True


def test_2_the_do_tool_re_asks_a_moved_army_and_repeats_a_standing_one(monkeypatch):
    ms = pytest.importorskip("mcp_server")
    monkeypatch.setenv(AO.ENV_ORDERS, "1")
    calls = []
    where = {"loc": 1491}
    refused = {"success": False, "reason": "refused_by_game", "army_id": 261,
               "game_reason": "Cannot disband while enemy or hostile Armies are around", "around": []}

    class G(Gw):
        async def read(self, action, params=None):
            if action == "query_armies":
                return {"armies": [{"army_id": 261, "location_province_id": where["loc"], "path_steps": 0}]}
            return await super().read(action, params)

        async def write(self, action, params=None):
            calls.append(action)
            return refused
    gw = G({}, "1068.07.01")

    async def ensure(*_a, **_k):
        return None

    async def now():
        return gw
    monkeypatch.setattr(ms, "_ensure_connected", ensure)
    monkeypatch.setattr(ms, "_gateway_now", now)
    monkeypatch.setattr(ms, "_get_state", lambda: None)
    monkeypatch.setattr(ms, "_state_game", lambda _s: gw.game())      # The memo keys on the tick
    monkeypatch.setattr(ms, "_REFUSALS", RM.Memo())
    args = {"action": "disband", "army_id": 261}
    run(ms.do(intent="army", character_id=ME, args=args))
    again = run(ms.do(intent="army", character_id=ME, args=args))
    assert calls == ["army_disband"] and "repeat_of" in again          # it stands there: answered from memory
    where["loc"] = 1500
    run(ms.do(intent="army", character_id=ME, args=args))
    assert calls == ["army_disband", "army_disband"]                   # it moved: the game is asked again
    run(ms.do(intent="army", character_id=ME, args=dict(args, check=True)))
    run(ms.do(intent="army", character_id=ME, args=dict(args, check=True)))
    assert calls.count("army_disband") == 4                            # a check is never memoised


def test_2_the_note_says_any_other_rulers_army_wherever():
    by = {n["id"]: n for n in N.load()["notes"]}
    t = by["raised-armies-block-declares"]["text"]
    assert "near another ruler's army, wherever" in t and "for as long as they stand" in t and "cbs names the call" in t


# ── A lost or white-peaced war's prestige (a recorded run) ───────────────────────────────────

def _ended(cb, outcome, side="attacker"):
    return {"war_id": 9, "name": "Munsterian-Ossoryan Vassalization War", "outcome": outcome, "my_side": side,
            "_cb": cb}


def test_3_the_price_table_prices_the_defeat_and_the_white_peace():
    assert news.war_end_prestige(_ended("vassalization_cb", "defender_victory"))["expected"] == -350
    assert news.war_end_prestige(_ended("vassalization_cb", "white_peace"))["expected"] == -75
    assert news.war_end_prestige(_ended("duchy_conquest_cb", "defender_victory"))["expected"] == -350


def test_3_CONTROL_a_factor_price_says_unread_and_a_defender_gets_nothing():
    r = news.war_end_prestige(_ended("claim_cb", "defender_victory"))
    assert "expected" not in r and "factor is unread" in r["basis"]
    # a defender's share is the casus belli's own block for that ending (vassalization_cb on_defeat: +150)
    assert news.war_end_prestige(_ended("vassalization_cb", "defender_victory", side="defender")).get(
        "expected") in (150.0, None)
    assert news.war_end_prestige(_ended(None, "defender_victory")) == {}


def test_3_the_purse_row_carries_it_and_the_unexplained_falls():
    prev = {"actor_id": ME, "date": "1068.01.01", "prestige": 500.0}
    snap = {"actor_id": ME, "date": "1068.03.18", "prestige": 148.93}
    ev = news._events_for("prestige", prev, snap, {"wars_ended": [_ended("vassalization_cb", "defender_victory")]})
    row = next(e for e in ev if e["what"].startswith("war ended defender_victory"))
    assert row["expected"] == -350 and "00_vassalization.txt" in row["basis"]


# ── The head of faith's gold (117 checks, 44 sends, 4 accepted) ──────────────────────────────────

def _hof_gw(defender_faith="catholic", gold=600.0, opinion=20):
    return Gw({"query_character": lambda p: ({"id": ME, "faith_key": "catholic", "gold": gold,
                                              "income_breakdown": {"total_monthly": 10.0}}
                                             if p.get("character_id") == ME else
                                             {"id": p.get("character_id"), "faith_key": defender_faith}),
               "query_wars": {"wars": [{"war_id": 44, "primary_attacker": ME, "primary_defender": 32287}]},
               "query_opinion": {"opinion": opinion},
               "query_interaction_check": {"result": {"results": [
                   {"target_id": POPE, "sendable": True, "answer": "no", "accept_score": -58}]}}})


def test_4_the_terms_put_in_the_co_religionist_war_the_rich_band_and_his_opinion():
    t = run(R.hof_gold_terms(_hof_gw(), ME, POPE))
    assert t["attacking_your_faith"] == {"wars": [44], "term": -50}
    assert t["rich"] == {"gold": 600.0, "term_if_treasury_income_is_your_income": -75}   # 600 >= max(300, 120)
    assert t["opinion_term"] == 10.0
    assert "-50 while you are the attacker" in t["terms"] and t["source"].startswith("common/character_interactions")


def test_4_CONTROL_another_faith_and_a_poor_ruler():
    t = run(R.hof_gold_terms(_hof_gw(defender_faith="norse_pagan", gold=100.0), ME, POPE))
    assert t["attacking_your_faith"]["term"] == 0 and t["rich"]["term_if_treasury_income_is_your_income"] == 0


def test_4_discover_acceptance_carries_the_terms():
    env = run(R.acceptance_impl(_hof_gw(), "hof_ask_for_gold_interaction", POPE, ME))
    assert env["result"]["accept_score"] == -58 and env["result"]["acceptance_terms"]["attacking_your_faith"]["term"] == -50
    env = run(R.acceptance_impl(_hof_gw(), "gift_interaction", POPE, ME))
    assert "acceptance_terms" not in env["result"]


def test_4_the_refused_send_and_the_lever_row_state_the_terms():
    env = {"verdict": "rejected", "blocked": {"gate": "recipient_refuses", "raw_reason": "target_would_reject"}}
    R._explain_interaction_refusal(env, "hof_ask_for_gold_interaction")
    assert env["blocked"]["he_accepts_when"] == R.HOF_GOLD_ACCEPT
    assert R.hof_gold_lever({"monthly_income": 10.0, "piety": 300.0})["he_accepts_when"] == R.HOF_GOLD_ACCEPT


def test_4_the_terms_are_the_game_files():
    game = Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game"
    f = game / "common" / "character_interactions" / "00_religious_interactions.txt"
    if not f.is_file():
        pytest.skip("no local install")
    t = f.read_text(encoding="utf-8-sig")
    body = t[t.index("hof_ask_for_gold_interaction = {"):]
    body = body[body.index("ai_accept = {"):body.index("auto_accept = {")]
    for want in ("base = -40", "multiplier = 0.5", "add = -50\n\t\t\tdesc = I_AM_ATTACKING_A_CORELIGIONIST",
                 "add = 30", "add = -20", "add = -25\n\t\t\tdesc = I_AM_RICH", "add = -75\n\t\t\tdesc = I_AM_RICH",
                 "add = -100\n\t\t\tdesc = WE_ARE_RIVALS"):
        assert want in body, want


def test_4_the_note_says_he_refuses_during_such_a_war():
    by = {n["id"]: n for n in N.load()["notes"]}
    assert "attack a ruler of your faith" in by["piety-into-gold"]["text"]


# ── gold_reserve is not a stop (a recorded game, turn 57, 1089.06) ─────────────────────────────────────────

def test_5_every_reserve_text_says_it_is_not_a_stop():
    assert "not a stop" in tool_text.DO and "ai_veto is" in tool_text.DO
    assert "not stop the AI's purchases" in news.RESERVE_PROVEN and "do ai_veto what=spending" in news.RESERVE_PROVEN
    assert "gold_reserve" not in news.ai_spends(levers_only=True)      # the turn-one levers that stop the AI's spending
    assert "Not a stop" in seat_events.RESERVE_NOTE and "ai_veto what=spending" in seat_events.RESERVE_NOTE


# ── Men-at-arms disband (a recorded run) ───────────────────────────────────────────────────

def test_6_the_intent_resolves_to_the_bridges_disband():
    a = {"type": "maa", "actor_id": ME, "action": "disband", "regiment_id": 48, "check": True}
    assert tools.resolve_act(a) == ("maa_disband", {"actor_id": ME, "regiment_id": 48, "check": True})
    assert "disband" in tools.MAA_ACTIONS


def test_6_refused_checked_and_applied():
    class G(Gw):
        def __init__(self, reply):
            super().__init__({}, "1089.06.01")
            self.reply = reply

        async def write(self, action, params=None):
            self.writes.append((action, dict(params or {})))
            return self.reply
    p = {"actor_id": ME, "regiment_id": 48, "check": False}
    env = run(R._do_maa_disband(G({"success": False, "reason": "refused_by_game",
                                   "game_reason": "The regiment is in combat"}), p, {"id": ME}))
    assert env["verdict"] == "rejected" and "in combat" in env["blocked"]["summary"]
    assert env["blocked"].get("detail") == "The regiment is in combat"
    env = run(R._do_maa_disband(G({"success": True, "check": True, "applied": False}), dict(p, check=True), {"id": ME}))
    assert "verdict" not in env and env["result"]["checked_only"] is True
    env = run(R._do_maa_disband(G({"success": True, "applied": True, "regiment_after": "absent"}), p, {"id": ME}))
    assert env["verdict"] == "applied" and env["result"]["proven"].startswith("built, not proven live")
    env = run(R._do_maa_disband(G({"success": True, "applied": True, "regiment_after": "present"}), p, {"id": ME}))
    assert env["verdict"] == "rejected" and env["blocked"]["raw_reason"] == "regiment_not_gone"
    env = run(R._do_maa_disband(G({"success": False, "error": "unknown_action"}), p, {"id": ME}))
    assert "older build" in env["blocked"]["summary"]


def test_6_do_routes_the_disband_and_the_args_table_lists_it():
    seen = []

    class G(Gw):
        async def write(self, action, params=None):
            seen.append((action, dict(params or {})))
            return {"success": True, "applied": True, "regiment_after": "absent"}
    env = run(R.do_impl(G({}, "1089.06.01"), intent="men_at_arms", character_id=ME,
                        args={"action": "disband", "regiment_id": 48}))
    assert seen and seen[-1][0] == "maa_disband" and seen[-1][1]["regiment_id"] == 48, (seen, env)
    assert env["verdict"] == "applied" and env["result"]["proven"] == R.MAA_DISBAND_UNPROVEN, env   # its own verdict
    assert "action=recruit|station|unstation|disband" in do_intent_args.table_text()


def test_6_the_bridge_registers_and_builds_the_command():
    root = HERE.parents[1]
    assert 'BRIDGE_ACTION("maa_disband", do_maa_disband)' in (root / "bridge/src/action_registry.inc").read_text()
    src = (root / "bridge/src/action_handlers.cpp").read_text()
    body = src[src.index("bool do_maa_disband("):]
    body = body[:body.index("\n}\n")]
    assert "DESTROY_MAA_CMD_CTOR" in body and "actor_owns_regiment(actor, full)" in body
    assert body.index("canexec(cmd, &reason)") < body.index("execute(cmd + 0x18)")


# ── The tool prefix rule in the seat prompt ──────────────────────────────────────────────────────


# ── A refused peace offer's score (92 refusals, 38 identical retries) ─────────────────────────────

def test_8_the_refused_offer_carries_his_score_and_what_is_missing():
    from test_activity_income_lever import ECHRAD
    war = {"war_id": 301989948, "name": "Ormond", "primary_attacker": ECHRAD, "primary_defender": 73537,
           "attackers": [ECHRAD], "defenders": [73537]}
    gw = Gw({"query_wars": {"wars": [war]}, "get_active_wars": {"wars": [war]},
             "query_war_score_breakdown": {"total_warscore": 37, "total_warscore_source": "engine_cached"},
             "query_war_history": {"wars": []}, "query_intent_results": {"results": []},
             "query_interaction_check": {"result": {"results": [
                 {"target_id": 73537, "sendable": True, "answer": "no", "accept_score": -62}]}}}, "1095.02.12")

    async def refuse(action, params=None, **kw):
        return {"raw": {"success": True, "status": "queued"}, "refused": "target_would_reject", "resolved": None}
    gw.confirmed_write = refuse
    env = run(R._do_war_intent(gw, "make_peace", {"war_id": 301989948, "terms": "enforce_demands"}, ECHRAD,
                               {"id": ECHRAD}))
    ans = env["result"]["his_answer"]
    assert ans["accept_score"] == -62 and ans["missing"] == 63.0 and ans["answer"] == "no", ans
    chk = [p for a, p in gw.reads if a == "query_interaction_check"]
    assert chk and chk[-1]["war_id"] == 301989948 and chk[-1]["target_id"] == 73537


def test_8_CONTROL_an_unanswered_check_says_unread():
    out = run(R.peace_answer(Gw({"query_interaction_check": {"error": "unknown_action"}}), 1, "x", 2, 3))
    assert out["unread"].endswith("unknown_action") and "missing" not in out


# ── the budgets ─────────────────────────────────────────────────────────────────────────────────────────────

def test_budgets_listing_and_notes():
    ms = pytest.importorskip("mcp_server")
    out = [t.model_dump(mode="json", by_alias=True, exclude_none=True) for t in run(ms.mcp.list_tools())]
    out = [{"name": d["name"], "description": d.get("description", ""), "inputSchema": d["inputSchema"]}
           for d in out]
    assert len(out) == 13 and len(json.dumps(out)) <= 31_049 and len(json.dumps(out, indent=1)) <= 36_298
    doc = N.load()
    assert N.problems(doc) == []
    sizes = [len(json.dumps([N.render(n) for n in doc["notes"] if n["topic"] == t], ensure_ascii=False).encode())
             for t in doc["topics"]]
    assert sum(sizes) <= 7416 and max(len(n["text"].encode()) for n in doc["notes"]) <= 240
    assert len(json.dumps(N.turn_one_texts(doc), ensure_ascii=False).encode()) <= 1700
