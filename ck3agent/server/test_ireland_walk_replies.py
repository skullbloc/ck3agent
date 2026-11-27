"""What a live Ireland walk found, each item reproduced from the recorded
live reply.

The fixture (fixtures/ireland_walk_replies.json.gz) holds replies recorded on a live
1066 game (CK3 1.18.3.1, toolkit 2026.12.11) as Murchad, Count of Dublin (31506). Bridge
replies the toolkit received are rebuilt from the tool replies it gave (a cbs option is
the bridge's cb row renamed); every number below is from those recordings.
"""
import asyncio
import gzip
import json
import os
import re
from pathlib import Path

import pytest

import news
import redesign as R

HERE = Path(__file__).resolve().parent
LIVE = json.loads(gzip.open(HERE / "fixtures" / "ireland_walk_replies.json.gz", "rt").read())
ME, ATHLONE_LORD, ALLY, WAR = 31506, 29585, 37284, 16777231
C_DUBLIN, C_ATHLONE, C_LEINSTER, C_ULSTER = 463, 469, 508, 474
#: The target: no default reply over this many bytes on the recorded states.
REPLY_BUDGET = 8000


def run(c):
    return asyncio.run(c)


def size(env) -> int:
    return len(json.dumps(env))


class Gw:
    """A scripted gateway: `replies[action]` is a value or a callable of params."""

    def __init__(self, replies=None, date="1066.09.15"):
        self.replies = dict(replies or {})
        self.reads, self.writes = [], []
        self.health = "ok"
        self.date = date

    async def read(self, action, params=None):
        p = dict(params or {})
        self.reads.append((action, p))
        if action == "query_character" and action not in self.replies:
            return {"id": p.get("character_id") or ME, "name": "Murchad", "alive": True, "gold": 84.0,
                    "age": 41, "liege_id": 0, "top_liege_id": ME}
        r = self.replies.get(action, {})
        return r(p) if callable(r) else r

    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return {"success": True}

    async def actor_gate(self):
        return None

    async def delta_records_since_last(self):
        return []

    async def clock(self):
        return {"paused": True, "date": self.date}

    def game(self):
        return {"tick": 0, "date": self.date, "paused": True, "speed": 3}


def cbs_by_target(reply):
    """query_cbs_for_target answering `reply` for every target (the batched read unserved)."""
    def answer(p):
        return dict(reply, target_id=p.get("target_id"))
    return {"query_cbs_for_targets": {"error": "unknown_action"}, "query_cbs_for_target": answer}


# ── opinion's parameter names, and no ok:true reply carrying an error ─────

def test_G8_opinion_sends_the_names_the_bridge_reads():
    gw = Gw({"query_opinion": {"opinion": 12}})
    env = run(R.inspect_impl(gw, subject="opinion", character_id=ME, target_id=ATHLONE_LORD))
    (sent,) = [p for a, p in gw.reads if a == "query_opinion"]
    assert sent == {"actor_id": ME, "target_id": ATHLONE_LORD}, sent
    src = (HERE / ".." / ".." / "bridge" / "src" / "query_handlers.cpp").read_text(errors="replace")
    body = src[src.index("bool do_query_opinion(const char*"):][:600]
    assert 'json_get_int(params_json, "actor_id")' in body
    assert env["ok"] is True and env["result"]["opinion"] == 12, env


def test_G8_the_recorded_error_reply_is_a_refusal():
    """Live 1066.11.27: ok true with error "missing actor_id or target_id"."""
    gw = Gw({"query_opinion": LIVE["opinion_reply"]})
    env = run(R.inspect_impl(gw, subject="opinion", character_id=ME, target_id=ATHLONE_LORD))
    assert env["ok"] is False and env["blocked"]["gate"] in R.GATE_TABLE, env
    assert env["blocked"]["raw_reason"] == "missing actor_id or target_id", env["blocked"]


@pytest.mark.parametrize("subject", sorted(R._INSPECT_SUBJECTS))
def test_G8_sweep_every_inspect_subject_surfaces_a_handler_error(subject):
    gw = Gw({a: {"error": "handler_said_no"} for a, _ in R._INSPECT_SUBJECTS.values()})
    env = run(R.inspect_impl(gw, subject=subject, character_id=ME, title_key="k_ireland",
                             target_id=ATHLONE_LORD, title_id=C_ATHLONE, war_id=WAR))
    # a single-read subject is refused (blocked); the composed title table reports the
    # failed reads as errors[] (ok false either way)
    assert env["ok"] is False and ((env["blocked"] or {}).get("gate") or env["errors"]), (subject, env)


def test_G8_the_envelope_rule_and_its_control():
    bad = R.envelope("query", result={"error": "title_not_found", "title_id": 9})
    assert bad["ok"] is False and bad["blocked"]["raw_reason"] == "title_not_found"
    # control: success true beside an error field, and no error at all, stay readings
    assert R.envelope("query", result={"success": True, "error": ""})["ok"] is True
    assert R.envelope("query", result={"success": True, "error": "x"})["ok"] is True
    assert R.envelope("query", result={"value": 1})["ok"] is True


# ── acceptance runs the send's own checks; `interaction` is the same key ──

def _check_reply(**row):
    return {"success": True, "result": {"interaction": "negotiate_alliance_interaction", "actor_id": ME,
                                        "results": [dict({"target_id": ATHLONE_LORD}, **row)]}}


def test_G4_acceptance_and_the_send_agree_on_the_recorded_refusal():
    """Live: acceptance said valid + would_accept; the send was rejected
    interaction_not_valid_for_target (gate interaction_invalid). The acceptance read
    now runs the send's checks: when they refuse, it says so with the send's gate."""
    send_gate = LIVE["alliance_send"]["blocked"]["gate"]
    gw = Gw({"query_interaction_check": _check_reply(sendable=False,
                                                     refused_by="interaction_not_valid_for_target")})
    env = run(R.acceptance_impl(gw, "negotiate_alliance_interaction", ATHLONE_LORD, ME))
    res = env["result"]
    assert res["sendable"] is False and res["would_accept"] is False, res
    assert res["refused"]["gate"] == send_gate == "interaction_invalid", res
    assert [a for a, _ in gw.reads if a == "query_interaction_acceptance"] == []
    # the old read's verdict on the same pair, for the record: it said yes
    assert LIVE["acceptance_interaction_type_arg"]["valid"] is True
    assert LIVE["acceptance_interaction_type_arg"]["would_accept"] is True


def test_G4_a_sendable_yes_carries_the_do_call():
    gw = Gw({"query_interaction_check": _check_reply(sendable=True, answer="yes", accept_score=120.0)})
    res = run(R.acceptance_impl(gw, "sway_interaction", ATHLONE_LORD, ME))["result"]
    assert res["would_accept"] is True and res["do_call"]["args"]["args"]["interaction"] == "sway_interaction"


def test_G4_an_unchecked_interaction_says_what_was_not_checked():
    gw = Gw({"query_interaction_check": {"success": False, "reason": "interaction_not_checkable"},
             "query_interaction_acceptance": LIVE["acceptance_interaction_type_arg"]})
    res = run(R.acceptance_impl(gw, "some_interaction", ATHLONE_LORD, ME))["result"]
    assert res["not_checked"] == R.ACCEPTANCE_NOT_CHECKED and "valid" not in res, res
    ai = (HERE / ".." / ".." / "bridge" / "src" / "ai_eval_hook.cpp").read_text(errors="replace")
    assert "trivial \"return 1\" stub" in ai, "the slot-7 stub the statement cites"


def test_G4_discover_takes_interaction_as_the_same_key(monkeypatch):
    """Live: discover(what='acceptance', interaction=...) answered interaction_not_found
    under ok true (the bridge got interaction_type '')."""
    ms = pytest.importorskip("mcp_server")
    gw = Gw({"query_interaction_check": _check_reply(sendable=True, answer="no")})

    async def now():
        return gw

    async def connected(_s, _t):
        return None
    state = ms.BridgeState()
    state.connected = True
    monkeypatch.setattr(ms, "_get_state", lambda: state)
    monkeypatch.setattr(ms, "_gateway_now", now)
    monkeypatch.setattr(ms, "_ensure_connected", connected)
    env = run(ms.discover(what="acceptance", interaction="negotiate_alliance_interaction",
                          target_id=ATHLONE_LORD, character_id=ME))
    (sent,) = [p for a, p in gw.reads if a == "query_interaction_check"]
    assert sent["interaction_type"] == "negotiate_alliance_interaction" and sent["actor_id"] == ME, sent
    assert env["result"]["answer"] == "no" and env["result"]["would_accept"] is False


# ── war_targets judged by the read cbs answers ────────────────────────────

def _wt_gw(cbs_reply):
    return Gw({"query_valid_war_targets": LIVE["valid_war_targets_1066_09_15"], **cbs_by_target(cbs_reply)})


def test_G2_no_war_target_says_declarable_when_cbs_refuses_it():
    """Live 1066.09.15: county_conquest_cb rows declarable true (not authoritative) while
    cbs and declare_war refused them engine_cb_validity_check_failed."""
    live_rows = LIVE["valid_war_targets_1066_09_15"]["targets"]
    assert any(r["declarable"] for r in live_rows)                 # the hazard, as recorded
    env = run(R.war_targets_impl(_wt_gw(LIVE["cbs_29585_1066_09_15"]), ME))
    rows = env["result"]["targets"]
    assert rows and not any(r["declarable"] is True for r in rows), rows
    athlone = next(r for r in rows if r["title_id"] == C_ATHLONE)
    assert athlone["blocked"]["gate"] == LIVE["declare_county_conquest_1066_09_15"]["gate"], athlone
    assert env["result"]["declarable_count"] == 0
    assert "declarable_authoritative" not in json.dumps(env)
    assert size(env) < REPLY_BUDGET, size(env)           # was 5,184 bytes live, every row "declarable"


def test_G2_a_row_cbs_accepts_carries_declare_args():
    reply = json.loads(json.dumps(LIVE["cbs_29585_1066_09_15"]))
    for c in reply["cbs"]:
        if c["cb_key"] == "county_conquest_cb":
            c.update(valid=True, requirements_unmet=[], cb_title_id=C_ATHLONE)
    env = run(R.war_targets_impl(_wt_gw(reply), ME))
    ok = [r for r in env["result"]["targets"] if r["declarable"] is True]
    assert ok and ok[0]["declare_args"]["cb"] == "county_conquest_cb", ok


def test_G2_an_unanswered_cbs_read_is_unread_not_declarable():
    gw = Gw({"query_valid_war_targets": LIVE["valid_war_targets_1066_09_15"],
             "query_cbs_for_targets": {"error": "unknown_action"}, "query_cbs_for_target": {}})
    rows = run(R.war_targets_impl(gw, ME))["result"]["targets"]
    assert rows and all(r["declarable"] is None and r["unread"] for r in rows)


# ── the fabricate list, and the counties a seat names ─────────────────────

IRELAND = [(C_DUBLIN, "c_dublin", ME, True), (C_ATHLONE, "c_athlone", ATHLONE_LORD, False),
           (C_ULSTER, "c_ulster", 31266, False), (C_LEINSTER, "c_leinster", 27052, False)]


def _title_gw(claims=(), cbs=None):
    def de_jure(p):
        if p.get("title_key") == "k_ireland" or p.get("title_id") == 461:
            return {"title": {"title_id": 461, "key": "k_ireland", "name": "Ireland", "tier": 4},
                    "de_jure_children": [{"title_id": 900, "key": "d_meath", "tier": 3, "name": "Meath"}]}
        return {"title": {"title_id": 900, "key": "d_meath", "tier": 3},
                "de_jure_children": [{"title_id": t, "key": k, "tier": 2, "name": k[2:].title(),
                                      "holder_id": h} for t, k, h, _ in IRELAND]}
    replies = {
        "query_dejure_progress": {"character_id": ME, "held": 1, "total": 4, "fraction": 0.25,
                                  "counties": [{"county_id": t, "holder_id": h, "top_liege_id": h,
                                                "you_control": mine} for t, _, h, mine in IRELAND]},
        "query_de_jure": de_jure,
        "query_claims": {"claims": list(claims)},
        "query_realm": lambda p: {"realms": [{"character_id": x} for x in p.get("character_ids") or []]},
    }
    replies.update(cbs_by_target(cbs or {"cbs": []}))
    return Gw(replies)


def test_G1_the_bridge_list_was_not_the_rulers_and_says_so():
    """Live: the same 40 rows (223 ... 1637) for three counts, none in Ireland."""
    ids = LIVE["fabricate_row_titles"][str(ME)]
    assert len(ids) == 40 and not set(ids) & {t for t, *_ in IRELAND}
    # the bridge returns at most max_results rows and counts the rest (assignable_count)
    gw = Gw({"query_fabricate_targets": lambda p: {
        "success": True, "task_seat": "councillor_court_chaplain", "assignable_count": len(ids),
        "targets": [{"title_id": t, "title_key": f"c_{t}", "assignable": True}
                    for t in ids][:p["max_results"]]}})
    env = run(R.do_impl(gw, action_class="fabricate_targets", character_id=ME))
    codes = [w["code"] for w in env["warnings"]]
    assert "targets_not_ruler_specific" in codes, codes
    assert not any("warnings" in r for r in env["result"]["available"])
    assert size(env) < REPLY_BUDGET, size(env)       # was 12,479 bytes live
    assert [w["truncated"] for w in env["warnings"] if w["code"] == "truncated"] == [40 - R.FABRICATE_LIST_CAP]
    assert env["result"]["available"][0]["label"] == "Fabricate claim on c_223"


def test_G1_within_judges_the_named_counties():
    gw = _title_gw(claims=[{"title_id": C_ULSTER, "type": "unpressed"}])
    env = run(R.do_impl(gw, action_class="fabricate_targets", character_id=ME, args={"within": "k_ireland"}))
    res = env["result"]
    # The game's own check does not answer in this stub (a live 1066 game, reads it per county): the
    # counties are `unchecked`, never `available`
    assert res["available"] == [] and {r["title_id"] for r in res["unchecked"]} == {C_ATHLONE, C_LEINSTER}, res
    assert all(r["validity"] == "unknown" for r in res["unchecked"])
    (do_call,) = [r["do_call"] for r in res["unchecked"] if r["title_id"] == C_ATHLONE]
    assert do_call == {"tool": "do", "args": {"intent": "council_task", "args": {
        "seat": "court_chaplain", "task": "task_fabricate_claim", "target_title_id": C_ATHLONE}}}
    why = {r["title_id"]: r["blocked"] for r in res["unavailable"]}
    assert why[C_DUBLIN]["gate"] == why[C_ULSTER]["gate"] == "already_in_that_state"
    assert why[C_ULSTER]["fix"] == {"tool": "cbs", "args": {"target_id": 31266}}
    assert ("query_fabricate_targets", {"actor_id": ME, "title_id": C_ATHLONE}) in gw.reads
    assert "target_check_not_read" in [w["code"] for w in env["warnings"]]


# ── a sent call to arms is pending ────────────────────────────────────────

def test_G3_the_recorded_call_ally_was_sent_and_then_reported_a_refusal():
    """The record the fix answers (test_war_intents.py grades the code): the drain said
    success, and the reply said recipient_refuses; the ally joined a step later."""
    rec = LIVE["call_ally"]
    assert rec["result"]["intent_result"]["status"] == "success"
    assert rec["blocked"]["gate"] == "recipient_refuses"
    assert R.LEVERS[("call_ally", "")]["shows"] == "days"


# ── the stall verdict reads the sieges inspect wars reads ─────────────────

def _stance_gw(wars_reply, occ=0, native=None):
    w = wars_reply["wars"][0]
    active = {"wars": [{"war_id": w["war_id"], "name": w["name"], "attacker_id": w["primary_attacker"],
                        "defender_id": w["primary_defender"], "is_attacker": w["is_attacker"],
                        "war_score": w["war_score"]}]}
    return Gw({"get_active_wars": active, "query_wars": wars_reply,
               "query_war_score_breakdown": {"occupation_atk": occ, "occupation": occ},
               "query_war_stance": native or {}}, date="1068.02.08")


def test_G5_a_won_siege_is_not_a_stall():
    """Live 1068.02.08: war_stance said attacker_defensive, siege never started,
    stall_suspect true; inspect wars listed the won siege of 470; the war was won."""
    live = LIVE["war_stance_1068_02_08"]["wars"][0]
    assert live["stall_suspect"] is True and live["stance"] == "attacker_defensive"
    native = {"success": True, "record_walk_ran": True,
              "wars": [{"war_id": WAR, "stance": "attacker_defensive", "stance_known": True,
                        "will_siege": False, "is_attacker": True}]}
    R._WAR_STANCE_UNSERVED.clear()
    env = run(R.inspect_impl(_stance_gw(LIVE["wars_1068_02_08"], native=native), subject="war_stance",
                             character_id=ME))
    row = env["result"]["wars"][0]
    # This recorded row is an older bridge's, whose "progress 20" was another
    # siege's event timer: its progress is unread, no stall verdict either way, and no exit pushed
    assert row["stall_suspect"] is None and row["exit_lever"] is None, row
    assert row["sieges"][0]["holding_title_id"] == 470 and "status" not in row["sieges"][0]
    assert row["sieges"][0]["progress_unread"] == R.SIEGE_PROGRESS_MISREAD
    assert "siege_progress_unread" in [x["code"] for x in row["limitations"]]
    assert env["result"]["stalled"] == [] and not env["warnings"], env


def test_G5_no_siege_and_no_occupation_is_unknown_and_losing_is_a_stall():
    wars = json.loads(json.dumps(LIVE["wars_1068_02_08"]))
    wars["wars"][0]["sieges"] = []
    R._WAR_STANCE_UNSERVED.clear()
    row = run(R._war_stance_view(_stance_gw(wars), ME))["wars"][0]
    assert row["stall_suspect"] is None and "stall_unknown" in [x["code"] for x in row["limitations"]]
    wars["wars"][0]["war_score"] = -40
    R._WAR_STANCE_UNSERVED.clear()
    row = run(R._war_stance_view(_stance_gw(wars), ME))["wars"][0]
    assert row["stall_suspect"] is True and row["exit_lever"]["white_peace"]["args"]["war_id"] == WAR


# ── every creation requirement at once; observe's next title ────────

def _req_gw(held=1, total=14, price=(5000.0, 84.0)):
    return Gw({"query_dejure_progress": {"held": held, "total": total, "fraction": held / total},
               "query_create_title_cost": {"success": True, "engine_valid": False,
                                           "engine_reasons": ["Must hold more counties"],
                                           "price": {"known": True, "currency": "gold", "cost": price[0],
                                                     "have": price[1], "affordable": False}}})


def test_G6_a_kingdom_below_the_bar_shows_every_requirement():
    """Live: formable_titles and mint_title showed only the de-jure gate (1/8) for
    k_ireland, no price and nothing else."""
    env = run(R.do_impl(_req_gw(), action_class="formable_titles", character_id=ME, candidates=["k_ireland"]))
    (row,) = env["result"]["unavailable"]
    reqs = {r["requirement"]: r for r in row["requirements"]}
    assert reqs["de_jure_counties"] == {"requirement": "de_jure_counties", "have": 1, "need": 8, "of": 14,
                                        "met": False}     # More than half of 14 (the game's 8)
    assert reqs["price"]["cost"] == 5000.0 and reqs["price"]["met"] is False
    assert reqs["game_check"]["met"] is False and reqs["game_check"]["reasons"]
    # The game's check ran and listed no 'Titles Required' line (this fixture's one reason): the rule passed
    assert reqs["tier_rule"]["met"] is True and reqs["tier_rule"]["rule"] == R.TIER_RULE_PASSED
    mint = run(R.do_impl(_req_gw(), intent="mint_title", character_id=ME, args={"title_key": "k_ireland"}))
    assert mint["blocked"]["gate"] == "de_jure_control"
    assert [r["requirement"] for r in mint["blocked"]["measured"]["requirements"]] == list(reqs)


def test_G7_next_title_is_the_first_unheld_de_jure_parent():
    hier = {"held_titles": [{"key": "c_dublin", "tier": 2, "de_jure_parent_key": "d_meath"},
                            {"key": "b_dublin", "tier": 1, "de_jure_parent_key": "c_dublin"}]}
    assert R.next_title_key({"primary_title_key": "c_dublin"}, hier) == "d_meath"
    hier["held_titles"].append({"key": "d_meath", "tier": 3, "de_jure_parent_key": "k_ireland"})
    assert R.next_title_key({"primary_title_key": "c_dublin"}, hier) == "k_ireland"
    blk = run(R.next_title_block(_req_gw(held=1, total=2, price=(212.5, 84.0)), ME,
                                 {"primary_title_key": "c_dublin"},
                                 {"held_titles": [{"key": "c_dublin", "de_jure_parent_key": "d_meath"}]}))
    assert blk["title"] == "d_meath" and blk["creatable_now"] is False, blk
    assert blk["first_gate"]["gate"] == "cannot_afford"
    live = LIVE["observe_brief_1066_09_15"]["de_jure"]
    assert live["can_mint_now"] is True and live["primary_title"]["title"] == "c_dublin"   # the record


# ── reply sizes on the recorded states ────────────────────────────────────

def test_G9_cbs_default_is_small_and_whole_on_the_valid_options():
    gw = Gw(cbs_by_target(LIVE["cbs_29585_1067_11_27"]))
    env = run(R.cbs_impl(gw, target_id=ATHLONE_LORD, character_id=ME))
    res = env["result"]
    assert size(env) < REPLY_BUDGET, size(env)           # was 37,933 bytes live
    assert [o["cb"] for o in res["valid_options"]] == ["claim_cb", "make_tributary_cb"]
    assert res["valid_options"][0]["declare_args"] == {"target_id": ATHLONE_LORD, "cb": "claim_cb",
                                                       "cb_title_id": C_ATHLONE, "claimant_id": ME}
    n = len(LIVE["cbs_29585_1067_11_27"]["cbs"])
    assert res["invalid_count"] == n - 2 == sum(res["invalid_by_gate"].values()), res["invalid_by_gate"]
    assert "options" not in res
    full = run(R.cbs_impl(gw, target_id=ATHLONE_LORD, character_id=ME, include_invalid=True))["result"]
    assert len(full["options"]) == n


def test_G9_interactions_are_capped_and_filtered():
    names = LIVE["interactions_names"]
    gw = Gw({"list_interactions": {"interactions": [{"name": n} for n in names]}})
    env = run(R.do_impl(gw, action_class="interactions", character_id=ME))
    assert size(env) < REPLY_BUDGET, size(env)           # was 82,120 bytes live
    (w,) = [w for w in env["warnings"] if w["code"] == "truncated"]
    assert w["truncated"] == len(names) - R.NAME_LIST_CAP
    some = run(R.do_impl(gw, action_class="interactions", character_id=ME, candidates=["alliance"]))
    got = [r["label"] for r in some["result"]["available"]]
    assert "negotiate_alliance_interaction" in got and all("alliance" in g for g in got)


def test_G9_decisions_are_names_capped_and_filtered(monkeypatch):
    ms = pytest.importorskip("mcp_server")
    state = ms.BridgeState()
    state.connected = True
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def qb(_s, action, params, timeout=15.0):
        return LIVE["decisions_reply"] if action == "list_decisions" else {}

    async def now():
        return Gw()
    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(ms, "_gateway_now", now)

    async def bound(*_a):
        return None
    monkeypatch.setattr(R, "unbound_refusal", bound)
    env = run(ms.discover(what="decisions", character_id=ME))
    assert size(env) < REPLY_BUDGET, size(env)           # was 28,022 bytes live
    assert env["result"]["count"] == 428 and env["result"]["truncated"] == 428 - R.NAME_LIST_CAP
    env = run(ms.discover(what="decisions", character_id=ME, decisions=["kingdom"]))
    assert "found_kingdom_decision" in env["result"]["decisions"]
    assert all("kingdom" in d for d in env["result"]["decisions"])


# ── news for what the walk saw ───────────────────────────────────────────

def _council(retasked: bool):
    """The court chaplain as live: task_fabricate_claim on 1066.09.15, the AI's
    task_religious_relations on 1067.11.27 once the claim had come."""
    return {"council": [{"position_key": "councillor_court_chaplain", "holder_id": 56090,
                         "current_task": "task_religious_relations" if retasked else "task_fabricate_claim"}]}


class NewsGw(Gw):
    def __init__(self):
        super().__init__(cbs_by_target(LIVE["cbs_29585_1067_11_27"]))
        self.claims = {"claims": []}
        self.retasked = False
        self.schemes = {"schemes": [{"scheme_id": 5, "type": "sway", "target_id": ATHLONE_LORD}]}
        self.held = {"held_titles": [{"title_id": C_DUBLIN, "key": "c_dublin", "tier": 2,
                                      "de_jure_parent_key": "d_meath"}]}
        self.wars = {"wars": [{"war_id": WAR, "name": "Claim on Athlone", "attacker_id": ME,
                               "defender_id": ATHLONE_LORD, "is_attacker": True}]}
        self.replies.update({
            "query_claims": lambda p: self.claims, "query_council": lambda p: _council(self.retasked),
            "query_schemes": lambda p: self.schemes, "query_title_hierarchy": lambda p: self.held,
            "get_active_wars": lambda p: self.wars,
            "query_realm": {"character_id": ME, "primary_title_key": "c_dublin", "vassals": [],
                            "held_title_count": 1},
            "query_war_history": {"wars": [{"war_id": WAR, "outcome": "attacker_victory",
                                            "end_date": "1068.7.20", "target_titles": [C_ATHLONE]}]},
        })


def test_G10_each_thing_the_walk_saw_is_news_once_with_its_next_call():
    news.reset()
    news._FABRICATING.clear()
    gw = NewsGw()
    run(R.observe_impl(gw, character_id=ME))
    news.note_fabrication(ME, C_ATHLONE, "c_athlone", "court_chaplain", "1066.09.15")
    # 1067.11.27: the claim arrived, the chaplain was re-tasked, the sway scheme was gone
    gw.date = "1067.11.27"
    gw.claims = LIVE["claims_1067_11_27"]
    gw.retasked = True
    assert [c["current_task"] for c in LIVE["council_1067_11_27"]["council"]
            if c["position_key"] == "councillor_court_chaplain"] == ["task_religious_relations"]
    gw.schemes = {"schemes": []}
    n = run(R.observe_impl(gw, character_id=ME))["result"]["news"]
    (claim,) = n["claims_gained"]
    assert claim["next"] == {"tool": "declare_war", "args": {"declare_args": {
        "target_id": ATHLONE_LORD, "cb": "claim_cb", "cb_title_id": C_ATHLONE, "claimant_id": ME}}}, claim
    (chap,) = [c for c in n["council_changed"] if c["seat"] == "councillor_court_chaplain"]
    assert chap["fabrication_left"]["title_key"] == "c_athlone"
    assert chap["next"] == {"tool": "do", "args": {"action_class": "fabricate_targets",
                                                   "args": {"within": "d_meath"}}}, chap
    (sch,) = n["schemes_ended"]
    assert sch["outcome"] is None and sch["next"]["args"] == {"subject": "opinion", "target_id": ATHLONE_LORD}
    # 1068.09.15: the war ended, Athlone came with it; Leinster came by inheritance
    gw.date = "1068.09.15"
    gw.wars = {"wars": []}
    gw.held = {"held_titles": gw.held["held_titles"] + [
        {"title_id": C_ATHLONE, "key": "c_athlone", "tier": 2}, {"title_id": C_LEINSTER, "key": "c_leinster",
                                                               "tier": 2}]}
    n = run(R.observe_impl(gw, character_id=ME))["result"]["news"]
    assert n["wars_ended"][0]["outcome"] == "attacker_victory" and "_titles" not in n["wars_ended"][0]
    got = {t["key"]: t for t in n["titles_gained"]}
    assert got["c_athlone"]["how"] == f"war {WAR} ended attacker_victory"
    # "unknown" with its reason, never null
    assert got["c_leinster"]["how"] == "unknown" and got["c_leinster"]["how_reason"] == "no_record_in_step"
    assert "claims_gained" in n and n["claims_gained"] == []      # once, not again


# ── inspect title rows carry next_call from the same read ───

def test_cold1_title_rows_carry_next_call_from_the_cbs_read():
    gw = _title_gw(claims=[{"title_id": C_ATHLONE, "type": "unpressed"}],
                   cbs=LIVE["cbs_29585_1067_11_27"])
    res = run(R.inspect_impl(gw, subject="title", character_id=ME, title_key="k_ireland"))["result"]
    import title_table as TT    # A call several rows repeat is named by a template (title_table.seat_rows)
    rows = {r["key"]: dict(r, next_call=TT.next_call_of(res, r)) if "next_call" in r else r for r in res["counties"]}
    assert "next_call" not in rows["c_dublin"]
    assert rows["c_athlone"]["next_call"] == {"tool": "declare_war", "args": {"declare_args": {
        "target_id": ATHLONE_LORD, "cb": "claim_cb", "cb_title_id": C_ATHLONE, "claimant_id": ME}}}
    assert rows["c_ulster"]["next_call"]["args"]["args"]["target_title_id"] == C_ULSTER
    assert res["summary"]["held"] == 1 and res["summary"]["total"] == 4   # a harness's metric keys
    assert "realm_ruler_id" in res["next_call_rule"]


# ── end_war says what each term needs ───────────────────────

def test_cold3_end_war_thresholds_mirror_the_guard():
    h = (HERE / ".." / ".." / "bridge" / "include" / "script_templates.h").read_text()
    assert f"kWeWinScore = {R.END_WAR_WE_WIN_SCORE};" in h
    assert f"kWhitePeaceEnemyBelow = {R.END_WAR_WHITE_ENEMY_BELOW};" in h
    live = LIVE["end_war_check_1067_11_27"]
    assert "we_win" not in live["reading"]["open_terms"] and "term_needs" not in live


# ── the surface ───────────────────────────────────────────────────────────────

def test_surface_thirteen_tools_and_the_dump_does_not_grow():
    ms = pytest.importorskip("mcp_server")
    tools = run(ms.mcp.list_tools())
    out = [t.model_dump(mode="json", by_alias=True, exclude_none=True) for t in tools]
    out = [{"name": d["name"], "description": d.get("description", ""), "inputSchema": d["inputSchema"]}
           for d in out]
    assert len(out) == 13
    # 35,531 until the shared notes (notes.py): +359 at indent=1 (+268 compact) for discover's
    # `notes` in what's enum (+9), its `topic` parameter with the topics' enum (+186) and one
    # description sentence (+73). With CK3_NOTES=off the dump is the pre-notes one, byte for
    # byte (test_notes.py checks the schema loses both).
    # +396 compact (+408 at indent=1) for do's two intents event_rule and gold_reserve: their
    # enum entries (+32), their rows in the generated args table (+165) and one description sentence (+199)
    assert len(json.dumps(out, indent=1)) <= 36_298, len(json.dumps(out, indent=1))
