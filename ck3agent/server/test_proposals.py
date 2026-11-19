"""The incoming-proposal queue, server side, against a STUB BRIDGE.

The stub (`Game` + `Gw`) speaks the contract of docs/design/incoming-proposals.md
("The contract between QMOD and QSRV") as written, not the bridge's code (QMOD
builds that in parallel): the four templates through `script_action`
(`proposal_seat`, `proposal_policy`, `proposal_list`, `proposal_answer`) with the
replies the contract's table names, and `query_script_log` with
`proposal_mod_enabled` (character mode) and `proposal_lines` (lines since an
offset, `next_offset`). The game behind it holds an offer only for a marked
recipient whose class policy is not `native`, answers `accept`/`decline`
classes by rule (a RESOLVED line by=rule), and scopes every template to the
actor's own offers (`not_found` otherwise). The other reads answer in their
emitters' keys: query_character `spouses`/`betrothed`/`liege_id`
(bridge/src/query_handlers.cpp:777, :813, :869), query_alliances `alliances[].ally_id`
(:10870ff), query_wars `primary_attacker`/`primary_defender` (:5175) read through
redesign._all_wars.

Every assumption about the not-yet-built bridge is a stub line here; the live
rows of the design (L1-) are what proves the real one.
"""
import asyncio
import json
import os
import re
import sys
from pathlib import Path

import pytest

import proposals as P
import redesign as R
import seat_lineage

ROOT = Path(__file__).resolve().parents[2]
KING, DUKE, SENDER, SON, BRIDE, ALLY, FOE = 100, 200, 300, 101, 301, 400, 500


class Game:
    """The stub game: characters, held offers (stories), markers, policies, lines."""

    def __init__(self, date="1066.01.10"):
        self.date = date
        self.mod_enabled = True
        self.chars = {cid: {"id": cid, "name": f"c{cid}", "alive": True, "age": 30, "spouses": [],
                            "betrothed": None, "liege_id": 0}
                      for cid in (KING, DUKE, SENDER, BRIDE, ALLY, FOE)}
        self.chars[SON] = dict(self.chars[KING], id=SON, name="son", age=12)
        self.markers, self.policy = set(), {}
        self.offers = {}            # id -> offer dict (state open|resolved|void)
        self.lines = []             # {"word","fields","date"}
        self.next_id = 1
        self.alliances = {}
        self.wars = []
        self.list_complete = True
        # The bridge's script_log_channel (action_handlers.cpp
        # emit_log_channel), on script_action and every query_script_log reply.
        self.channel = {"state": "open", "level": "info", "cap": 100000, "info_messages": 10,
                        "debug_messages": 500, "closed_since": None}
        self.answer_override = None
        self.primary = KING          # the ruler bound to the seat (a succession moves it)

    # -- the mod's capture, as the design specifies it ----------------------
    def send(self, kind, sender, recipient, **kw):
        cls = P.KIND_CLASS[kind]
        pol = self.policy.get((recipient, cls), P.DEFAULT_POLICY)
        if recipient not in self.markers or pol == "native":
            return None                                   # not captured: the native path
        pid = self.next_id
        self.next_id += 1
        self.offers[pid] = {"id": pid, "kind": kind, "class": cls, "state": "open", "actor_id": sender,
                            "recipient_id": recipient, "secondary_actor_id": kw.get("sa"),
                            "secondary_recipient_id": kw.get("sr"), "war_name": kw.get("war_name"),
                            "options": kw.get("options", []), "age_days_min": 0}
        self.lines.append({"word": P.WORD_HELD, "fields": [str(pid), kind, cls, str(sender), str(recipient)],
                           "date": self.date})
        if pol in ("accept", "decline"):               # the auto-answer event, a day later
            self._apply(pid, pol, "rule", f"class_policy:{cls}={pol}")
        return pid

    def _apply(self, pid, answer, by, rule):
        o = self.offers[pid]
        o["state"] = "resolved"
        if answer == "accept":
            if o["class"] == "marriage":
                a, b = o["secondary_recipient_id"], o["secondary_actor_id"]
                self.chars[a]["spouses"].append({"id": b, "alive": True})
            elif o["class"] == "alliance":
                self.alliances.setdefault(o["recipient_id"], set()).add(o["actor_id"])
            elif o["class"] == "vassalage":
                self.chars[o["recipient_id"]]["liege_id"] = o["actor_id"]
        self.lines.append({"word": P.WORD_RESOLVED, "fields": [str(pid), answer, by, rule or ""],
                           "date": self.date})

    def void(self, pid, reason):
        """The game voids an offer (the story's re-check, an answer's own check):
        the story keeps the reason and writes the VOID line."""
        o = self.offers[pid]
        o["state"], o["void_reason"] = "void", reason
        self.lines.append({"word": P.WORD_VOID, "fields": [str(pid), reason], "date": self.date})

    def end(self, pid):
        """The story ends on its next 10-day step after a void."""
        self.offers.pop(pid)

    # -- the four templates -------------------------------------------------
    def script(self, p):
        t, actor = p["template"], p.get("actor_id")
        base = {"success": True, "action": "script_action", "template": t, "token": f"SACT_{t}_{len(self.lines)}",
                "guard": "ran" if self.channel["state"] == "open" else "unseen", "errors": [],
                "debug_log_offset": 0, "error_log_offset": 0, "script_log_channel": dict(self.channel)}
        if t == P.T_SEAT:
            (self.markers.add if p["on"] else self.markers.discard)(actor)
            return base
        if t == P.T_POLICY:
            self.policy[(actor, p["class"])] = p["answer"]
            return base
        if t == P.T_LIST:
            mine = [dict(o) for o in self.offers.values() if o["recipient_id"] == actor and o["state"] == "open"]
            return dict(base, proposals=mine, list_complete=self.list_complete)
        if t == P.T_ANSWER:
            if self.answer_override:
                return dict(base, **self.answer_override)
            o = self.offers.get(p["proposal_id"])
            if o is None or o["recipient_id"] != actor:
                return dict(base, outcome="refused", outcome_reason="not_found")
            if o["state"] == "void":                  # An answer to a void offer names why
                return dict(base, outcome="refused", outcome_reason=o["void_reason"])
            if o["state"] != "open":
                return dict(base, outcome="refused", outcome_reason="already_resolved")
            ans = p["answer"]
            if ans == "native":
                if o["class"] not in P.NATIVE_SUPPORTED:
                    return dict(base, outcome="refused", outcome_reason="native_not_supported")
                self._apply(o["id"], "native", p["by"], None)
                return dict(base, outcome="handed_back")
            self._apply(o["id"], ans, p["by"], None)
            return dict(base, outcome="applied_accept" if ans == "accept" else "applied_decline")
        return {"success": False, "reason": "unknown_template"}


class Gw:
    def __init__(self, game: Game):
        self.g = game
        self.reads, self.writes = [], []
        self.health = "ok"

    async def actor_gate(self):
        return None

    def game(self):
        return {"tick": 1, "date": self.g.date, "paused": True}

    async def read(self, action, params=None):
        params = dict(params or {})
        self.reads.append((action, params))
        g = self.g
        if action == "query_character":
            cid = params.get("character_id") or g.primary   # 0: the bound primary
            return dict(g.chars.get(cid) or {"error": "character_not_found"})
        if action == "query_script_log":
            if params.get("proposal_lines"):
                off = params.get("debug_log_offset", 0)
                return {"success": True, "lines": g.lines[off:], "next_offset": len(g.lines),
                        "script_log_channel": dict(g.channel)}
            if "character_id" in params:
                return {"success": True, "character_id": params["character_id"], "mod_enabled": True,
                        "policies": {}, "proposal_mod_enabled": g.mod_enabled}
            return {"success": True, "guard": "unseen"}
        if action == "query_alliances":
            return {"alliances": [{"ally_id": a} for a in g.alliances.get(params["character_id"], ())],
                    "forced_allies": []}
        if action == "query_wars":
            return {"wars": [dict(w) for w in g.wars]}
        return {}

    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def delta_records_since_last(self):
        return []

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        if action == "script_action":
            return self.g.script(dict(params or {}))
        return {"success": True}


def run(c):
    return asyncio.run(c)


def sent(gw, template=None):
    return [p for a, p in gw.writes if a == "script_action" and (template is None or p["template"] == template)]


@pytest.fixture(autouse=True)
def seat_env(monkeypatch, tmp_path):
    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    for k in (P.MOD_ENV, P.LEDGER_ENV, P.SESSION_ID_ENV, "CK3_EPISODE_HORIZON_START",
              "CK3_CYCLE_GAME_DAYS", "CK3_EPISODE_HORIZON_END"):
        monkeypatch.delenv(k, raising=False)
    P.reset()
    P.SYSTEM1[0] = P.system1_none
    yield
    P.reset()
    P.SYSTEM1[0] = P.system1_none


def declare(monkeypatch, tmp_path, sid="seat-K", *, cell=False):
    monkeypatch.setenv(P.MOD_ENV, "1")
    monkeypatch.setenv(P.LEDGER_ENV, str(tmp_path / f"{sid}{P.SINK_SUFFIX}"))
    monkeypatch.setenv(P.SESSION_ID_ENV, sid)
    if cell:
        monkeypatch.setenv("CK3_EPISODE_HORIZON_START", "1066.01.01")
        monkeypatch.setenv("CK3_CYCLE_GAME_DAYS", "360")
        monkeypatch.setenv("CK3_EPISODE_HORIZON_END", "1076.01.01")
    return tmp_path / f"{sid}{P.SINK_SUFFIX}"


def ledger(path):
    return [json.loads(x) for x in Path(path).read_text().splitlines() if x.strip()]


def observe(gw, cid=0):
    return run(R.observe_impl(gw, cid, "brief"))


def do(gw, intent, cid=KING, **args):
    return run(R.do_impl(gw, intent=intent, character_id=cid, args=args))


# ── not declared: nothing, and typed refusals ───────────────────────────────

def test_undeclared_observe_has_no_block_and_reads_nothing_of_the_mod():
    gw = Gw(Game())
    env = observe(gw, KING)
    assert "incoming_proposals" not in env["result"]
    assert not sent(gw) and not any(p.get("proposal_lines") for a, p in gw.reads)


def test_undeclared_both_intents_refuse_typed_and_send_nothing():
    gw = Gw(Game())
    a = do(gw, "answer_proposal", proposal_id=1, answer="accept")
    b = do(gw, "set_policy", policy="proposals", **{"class": "alliance"}, answer="decline")
    for env in (a, b):
        assert env["verdict"] == "rejected" and env["blocked"]["gate"] == P.G_NOT_DECLARED
    assert not sent(gw)


# ── the block ───────────────────────────────────────────────────────────────

def test_observe_marks_the_seat_lists_its_offer_and_writes_the_ledger(monkeypatch, tmp_path):
    led = declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)
    v = observe(gw)["result"]["incoming_proposals"]
    assert v["mod_enabled"] is True and v["seat_marked"] is True and v["offers"] == [] and v["unread"] is None
    assert KING in g.markers
    pid = g.send("arrange_marriage_interaction", SENDER, KING, sa=BRIDE, sr=SON, options=["matrilineal"])
    v = observe(gw)["result"]["incoming_proposals"]
    (o,) = v["offers"]
    assert o["id"] == pid and o["class"] == "marriage" and o["from"] == {"id": SENDER, "name": f"c{SENDER}"}
    assert o["offered"]["matrilineal"] is True and o["offered"]["betrothal"] is True
    assert o["offered"]["your_side"]["id"] == SON and o["arrived"] == "1066.01.10"
    assert o["default_at_lapse"] == "native" and "answer_proposal" in o["answer_with"]
    assert v["policies"]["marriage"] == {"answer": "ask_me", "source": "default", "set_at": None}
    kinds = [r["kind"] for r in ledger(led)]
    assert kinds[:2] == ["opened", "seat_marked"] and {"held", "shown", "listed"} <= set(kinds)
    assert {r["seat_session_id"] for r in ledger(led)} == {"seat-K"}
    # the marker is set once, not at every observe
    assert len(sent(gw, P.T_SEAT)) == 1


def test_an_unread_mod_check_and_an_absent_mod_are_unread_not_no_offers(monkeypatch, tmp_path):
    declare(monkeypatch, tmp_path)
    g = Game()
    g.mod_enabled = False
    v = observe(Gw(g))["result"]["incoming_proposals"]
    assert v["offers"] is None and v["unread"] == P.U_NOT_LOADED
    gw = Gw(g)

    async def no_reply(action, params=None):
        return {} if action == "query_script_log" else await Gw.read(gw, action, params)
    gw.read = no_reply
    v = observe(gw)["result"]["incoming_proposals"]
    assert v["offers"] is None and v["unread"].startswith(P.U_CHECK) and v["mod_enabled"] is None


def test_an_incomplete_list_is_unread_never_empty(monkeypatch, tmp_path):
    declare(monkeypatch, tmp_path)
    g = Game()
    g.list_complete = False
    v = observe(Gw(g))["result"]["incoming_proposals"]
    assert v["offers"] is None and v["unread"] == P.U_LIST_INCOMPLETE


# ── two seats: each lists and answers only its own ─────────────────────────

def test_two_seats_each_see_and_answer_only_their_own_offers(monkeypatch, tmp_path):
    g = Game()
    declare(monkeypatch, tmp_path, "seat-K")
    k = Gw(g)
    observe(k, KING)
    k_state = P._STATE[0]
    P.reset()
    declare(monkeypatch, tmp_path, "seat-D1")
    d = Gw(g)
    observe(d, DUKE)
    d_state = P._STATE[0]
    pk = g.send("negotiate_alliance_interaction", SENDER, KING)
    pd = g.send("negotiate_alliance_interaction", ALLY, DUKE)
    vd = observe(d, DUKE)["result"]["incoming_proposals"]
    assert [o["id"] for o in vd["offers"]] == [pd]
    # the duke's seat cannot answer the king's offer: refused before anything is sent
    env = do(d, "answer_proposal", cid=DUKE, proposal_id=pk, answer="accept")
    assert env["blocked"]["gate"] == P.G_NOT_YOURS and not sent(d, P.T_ANSWER)
    env = do(d, "answer_proposal", cid=DUKE, proposal_id=pd, answer="accept")
    assert env["verdict"] == "applied" and env["result"]["readback"] == {"ally_id": ALLY}
    P._STATE[:] = [k_state]
    vk = observe(k, KING)["result"]["incoming_proposals"]
    assert [o["id"] for o in vk["offers"]] == [pk]
    assert all(r["seat_session_id"] == "seat-D1" for r in ledger(d_state.path))
    assert pk not in {r.get("proposal_id") for r in ledger(d_state.path)}


def test_the_games_own_scope_refusal_is_not_your_proposal(monkeypatch, tmp_path):
    """The server's list and the game's disagree (a stale list): the template's
    not_found reaches the seat as not_your_proposal, with the game's reason."""
    declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)
    observe(gw, KING)
    pid = g.send("negotiate_alliance_interaction", SENDER, KING)
    observe(gw, KING)
    g.offers[pid]["recipient_id"] = DUKE
    env = do(gw, "answer_proposal", proposal_id=pid, answer="decline")
    assert env["blocked"]["gate"] == P.G_NOT_YOURS and env["blocked"]["raw_reason"] == "not_found"


# ── answers: verdicts with a read-back ─────────────────────────────────────

def _one(monkeypatch, tmp_path, kind, **kw):
    led = declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)
    observe(gw, KING)
    pid = g.send(kind, kw.pop("sender", SENDER), KING, **kw)
    observe(gw, KING)
    return g, gw, pid, led


def test_accept_marriage_applies_on_the_spouse_read_back(monkeypatch, tmp_path):
    g, gw, pid, led = _one(monkeypatch, tmp_path, "arrange_marriage_interaction", sa=BRIDE, sr=SON)
    env = do(gw, "answer_proposal", proposal_id=pid, answer="accept")
    assert env["verdict"] == "applied" and env["result"]["readback"] == {"married": [SON, BRIDE]}
    (call,) = sent(gw, P.T_ANSWER)
    assert call == {"template": "proposal_answer", "actor_id": KING, "proposal_id": pid,
                    "answer": "accept", "by": "seat"}
    row = [r for r in ledger(led) if r["kind"] == "answered" and r["source"] == "call"][0]
    assert row["by"] == "seat" and row["action_token"] and row["outcome"] == "applied_accept"


def test_an_accept_the_game_does_not_show_is_not_applied(monkeypatch, tmp_path):
    g, gw, pid, _ = _one(monkeypatch, tmp_path, "offer_vassalization_interaction")
    g.answer_override = {"outcome": "applied_accept"}             # said so, did nothing
    env = do(gw, "answer_proposal", proposal_id=pid, answer="accept")
    assert env["verdict"] == "rejected" and env["blocked"]["gate"] == "write_not_applied"


def test_decline_applies_on_the_list_and_the_resolved_line(monkeypatch, tmp_path):
    g, gw, pid, _ = _one(monkeypatch, tmp_path, "negotiate_alliance_interaction")
    env = do(gw, "answer_proposal", proposal_id=pid, answer="decline")
    assert env["verdict"] == "applied" and env["result"]["readback"]["gone_from_list"] is True


def test_native_is_pending_and_refused_for_war_classes_before_sending(monkeypatch, tmp_path):
    g, gw, pid, _ = _one(monkeypatch, tmp_path, "negotiate_alliance_interaction")
    env = do(gw, "answer_proposal", proposal_id=pid, answer="native")
    assert env["verdict"] == "pending" and env["pending"]["raw_reason"] == "handed_back"
    g.wars = [{"war_id": 7, "name": "w", "primary_attacker": SENDER, "primary_defender": FOE,
               "attackers": [SENDER], "defenders": [FOE]}]
    pid2 = g.send("call_ally_interaction", SENDER, KING, war_name="w")
    v = observe(gw, KING)["result"]["incoming_proposals"]
    (o,) = v["offers"]
    assert o["offered"] == {"war": {"war_id": 7, "name": "w"}, "side": "attacker"}
    assert o["default_at_lapse"] == "accept" and "'native'" not in o["answer_with"]
    n = len(sent(gw, P.T_ANSWER))
    env = do(gw, "answer_proposal", proposal_id=pid2, answer="native")
    assert env["blocked"]["gate"] == P.G_NATIVE and len(sent(gw, P.T_ANSWER)) == n


def test_peace_terms_are_read_from_the_seats_side(monkeypatch, tmp_path):
    declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)
    g.wars = [{"war_id": 9, "name": "v", "primary_attacker": KING, "primary_defender": FOE,
               "attackers": [KING], "defenders": [FOE]}]
    observe(gw, KING)
    g.send("end_war_attacker_victory_interaction", FOE, KING, war_name="v")
    (o,) = observe(gw, KING)["result"]["incoming_proposals"]["offers"]
    assert o["offered"]["terms"] == "you_win" and o["default_at_lapse"] == "decline"


@pytest.mark.parametrize("reason,gate", [("void_subject_dead", P.G_CLOSED), ("already_resolved", P.G_CLOSED),
                                         ("invalid_now", "interaction_invalid")])
def test_the_templates_refusals_are_typed(monkeypatch, tmp_path, reason, gate):
    g, gw, pid, led = _one(monkeypatch, tmp_path, "negotiate_alliance_interaction")
    g.answer_override = {"outcome": "refused", "outcome_reason": reason,
                         "errors": ["Interaction cannot be sent. Reason: x"] if reason == "invalid_now" else []}
    env = do(gw, "answer_proposal", proposal_id=pid, answer="accept")
    assert env["blocked"]["gate"] == gate and env["blocked"]["raw_reason"] == reason
    assert ledger(led)[-1]["kind"] == "answer_refused"


def test_bad_arguments_are_refused_before_anything(monkeypatch, tmp_path):
    declare(monkeypatch, tmp_path)
    gw = Gw(Game())
    for args in ({"proposal_id": 0, "answer": "accept"}, {"proposal_id": 3, "answer": "maybe"},
                 {"proposal_id": "3", "answer": "accept"}, {"proposal_id": True, "answer": "accept"}):
        env = do(gw, "answer_proposal", **args)
        assert env["blocked"]["raw_reason"] == P.R_BAD_ARG, args
    env = do(gw, "set_policy", policy="proposals", **{"class": "alliance"}, answer="sometimes")
    assert env["blocked"]["raw_reason"] == P.R_BAD_ARG
    env = do(gw, "set_policy", policy="proposals", **{"class": "tributary"}, answer="accept")
    assert env["blocked"]["gate"] == P.G_CLASS
    assert not sent(gw)


# ── policies, rules, the boundary ──────────────────────────────────────────

def test_a_rule_answers_without_the_seat_and_the_ledger_names_the_call_that_set_it(monkeypatch, tmp_path):
    led = declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)
    env = do(gw, "set_policy", policy="proposals", **{"class": "alliance"}, answer="decline")
    assert env["verdict"] == "applied" and g.policy[(KING, "alliance")] == "decline"
    assert env["result"]["proposal_policies"]["alliance"]["source"] == "set"
    pid = g.send("negotiate_alliance_interaction", SENDER, KING)
    v = observe(gw, KING)["result"]["incoming_proposals"]
    assert v["offers"] == []
    (a,) = v["answered_since_last_turn"]
    assert a == {"id": pid, "class": "alliance", "answer": "decline", "by": "rule",
                 "rule": "class_policy:alliance=decline", "date": "1066.01.10"}
    pol = [r for r in ledger(led) if r["kind"] == "policy_set"][0]
    row = [r for r in ledger(led) if r["kind"] == "answered" and r.get("by") == "rule"][0]
    assert row["rule_set_by_token"] == pol["action_token"] and row["rule"] == a["rule"]
    # shown once outside a cell
    assert observe(gw, KING)["result"]["incoming_proposals"]["answered_since_last_turn"] == []


def test_an_unread_mod_check_refuses_set_policy_as_a_read_failure(monkeypatch, tmp_path):
    declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)

    async def no_reply(action, params=None):
        return {} if action == "query_script_log" else await Gw.read(gw, action, params)
    gw.read = no_reply
    env = do(gw, "set_policy", policy="proposals", **{"class": "alliance"}, answer="decline")
    assert env["blocked"]["gate"] == "read_timeout" and env["blocked"]["raw_reason"] == P.R_CHECK_UNREAD
    g.mod_enabled = False
    env = do(Gw(g), "set_policy", policy="proposals", **{"class": "alliance"}, answer="decline")
    assert env["blocked"]["raw_reason"] == P.R_NOT_LOADED
    assert not g.policy


def test_the_boundary_hands_an_ignored_offer_its_default_even_in_a_new_process(monkeypatch, tmp_path):
    led = declare(monkeypatch, tmp_path, cell=True)
    g = Game()
    gw = Gw(g)
    observe(gw, KING)
    pid = g.send("negotiate_alliance_interaction", SENDER, KING)
    v = observe(gw, KING)["result"]["incoming_proposals"]
    assert v["turn"] == 0 and [o["id"] for o in v["offers"]] == [pid]
    P.reset()                                          # same turn, a new process: still shown,
    gw1 = Gw(g)                                        # not handed back, marker not re-sent
    assert [o["id"] for o in observe(gw1, KING)["result"]["incoming_proposals"]["offers"]] == [pid]
    assert not sent(gw1, P.T_ANSWER) and not sent(gw1, P.T_SEAT)
    P.reset()                                          # the next turn is a new server process
    g.date = "1067.01.10"
    gw2 = Gw(g)
    v = observe(gw2, KING)["result"]["incoming_proposals"]
    (call,) = sent(gw2, P.T_ANSWER)
    assert call["by"] == "boundary" and call["answer"] == "native" and call["proposal_id"] == pid
    assert v["offers"] == [] and v["turn"] == 1
    assert [a["by"] for a in v["answered_since_last_turn"]] == ["boundary"]
    # 365 days after it was set: older than MARKER_REFRESH_DAYS, so refreshed (set, not moved)
    assert sent(gw2, P.T_SEAT) == [{"template": "proposal_seat", "actor_id": KING, "on": True}]
    rows = [r for r in ledger(led) if r["kind"] == "handed_back"]
    assert {r["source"] for r in rows} == {"call", "line"} and all(r["by"] == "boundary" for r in rows)


def test_a_new_process_can_answer_what_the_last_one_listed(monkeypatch, tmp_path):
    g, gw, pid, _ = _one(monkeypatch, tmp_path, "negotiate_alliance_interaction")
    P.reset()
    env = do(Gw(g), "answer_proposal", proposal_id=pid, answer="accept")
    assert env["verdict"] == "applied"


def test_after_a_succession_the_marker_moves_to_the_heir(monkeypatch, tmp_path):
    led = declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)
    observe(gw)                                        # the bound primary
    assert g.markers == {KING}

    g.primary = SON                                    # the king died; the seat follows his line
    env = observe(gw)
    assert env["actor"]["id"] == SON
    assert g.markers == {SON}
    rows = [r for r in ledger(led) if r["kind"] in ("seat_marked", "seat_unmarked")]
    assert [(r["kind"], r["actor_id"]) for r in rows] == [("seat_marked", KING), ("seat_unmarked", KING),
                                                          ("seat_marked", SON)]


def test_system1_answers_through_the_same_template_by_classifier(monkeypatch, tmp_path):
    declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)
    observe(gw, KING)
    g.send("negotiate_alliance_interaction", SENDER, KING)
    P.SYSTEM1[0] = lambda offer, pol: ("decline", "model:test-1", 0.9)
    v = observe(gw, KING)["result"]["incoming_proposals"]
    (call,) = sent(gw, P.T_ANSWER)
    assert call["by"] == "classifier" and v["offers"] == []


# ── the surface and the mirrors ────────────────────────────────────────────

def test_the_intents_and_their_arguments_are_read_from_the_code():
    import do_intent_args
    assert "answer_proposal" in R.INTENTS
    t = do_intent_args.extract_dedicated()
    assert t["answer_proposal"] == ["proposal_id", "answer"]
    assert {"policy", "class", "answer", "on"} <= set(t["set_policy"])
    for g in (P.G_NOT_DECLARED, P.G_NOT_YOURS, P.G_CLOSED, P.G_NATIVE, P.G_CLASS):
        assert g in R.GATE_TABLE


def test_the_classes_mirror_the_generators_first_set_and_the_design_table():
    sys.path.insert(0, str(ROOT / "scripts"))
    import proposal_overrides as PO
    assert {k: v[0] for k, v in PO.FIRST_SET.items()} == P.KIND_CLASS
    assert {k: v[1] for k, v in PO.FIRST_SET.items()} == P.FAMILY
    doc = (ROOT / "docs" / "design" / "incoming-proposals.md").read_text(encoding="utf-8")
    table = doc.split("<!-- first-set:begin -->")[1].split("<!-- first-set:end -->")[0]
    rows = re.findall(r"^\| `(\w+)` \| `(\w+)` \| `(\w+)` \|", table, re.M)
    assert {k: c for c, k, _f in rows} == P.KIND_CLASS
    assert P.SESSION_ID_ENV == seat_lineage.SESSION_ID_ENV
    # the lapse defaults the design states (Timing)
    assert {c: s.lapse_default for c, s in P.CLASS_SPECS.items()} == {
        "marriage": "native", "alliance": "native", "vassalage": "native",
        "call_to_war": "accept", "peace": "decline"}


# ── Answers to VOID offers are typed by the void's reason ─────────

def _void_reply(env, reason):
    assert env["verdict"] == "rejected", env
    assert env["blocked"]["raw_reason"] == reason
    assert env["blocked"]["gate"] == P._OUTCOME_GATES[reason][0]
    assert env["result"]["void"]["reason"] == reason and env["result"]["void"]["by"] == "void"


def test_an_offer_the_game_voided_since_the_list_is_refused_typed_and_nothing_is_sent(monkeypatch, tmp_path):
    g, gw, pid, led = _one(monkeypatch, tmp_path, "arrange_marriage_interaction", sa=BRIDE, sr=SON)
    g.void(pid, "void_subject_dead")                     # after the seat's list, before its answer
    n = len(sent(gw, P.T_ANSWER))
    env = do(gw, "answer_proposal", proposal_id=pid, answer="accept")
    _void_reply(env, "void_subject_dead")
    assert env["result"]["sent"] is False and len(sent(gw, P.T_ANSWER)) == n
    assert env["result"]["void"]["date"] == g.date
    row = ledger(led)[-1]
    assert row["kind"] == "answer_refused" and row["outcome_reason"] == "void_subject_dead" and row["source"] == "server"


def test_a_void_the_list_already_dropped_is_void_not_someone_elses(monkeypatch, tmp_path):
    g, gw, pid, _ = _one(monkeypatch, tmp_path, "arrange_marriage_interaction", sa=BRIDE, sr=SON)
    g.void(pid, "void_sender_dead")
    observe(gw, KING)                                   # the VOID line folded, the list without it
    env = do(gw, "answer_proposal", proposal_id=pid, answer="decline")
    _void_reply(env, "void_sender_dead")
    assert env["blocked"]["gate"] != P.G_NOT_YOURS
    P.reset()                                           # a new process re-reads its ledger
    env = do(Gw(g), "answer_proposal", proposal_id=pid, answer="decline")
    _void_reply(env, "void_sender_dead")


@pytest.mark.parametrize("reason", ["void_subject_dead", "void_recipient_dead", "invalid_now", "void_reason_unread"])
def test_the_mods_void_outcome_is_typed_by_its_reason(monkeypatch, tmp_path, reason):
    g, gw, pid, led = _one(monkeypatch, tmp_path, "negotiate_alliance_interaction")
    g.answer_override = {"outcome": "refused", "outcome_reason": reason,
                         "errors": ["Interaction cannot be sent. Reason: x"] if reason == "invalid_now" else []}
    env = do(gw, "answer_proposal", proposal_id=pid, answer="accept")
    _void_reply(env, reason)
    assert env["result"]["sent"] is True
    assert ledger(led)[-1]["kind"] == "answer_refused" and ledger(led)[-1]["outcome_reason"] == reason


def test_an_answer_the_mod_voids_on_its_own_check_is_never_applied(monkeypatch, tmp_path):
    """The mod's check (the interaction's own conditions) voids the offer as it is
    answered: the reply is the typed void, a second answer is refused the same
    way without a call."""
    g, gw, pid, _ = _one(monkeypatch, tmp_path, "arrange_marriage_interaction", sa=BRIDE, sr=SON)
    real = g.script

    def answer_voids(p):
        if p["template"] == P.T_ANSWER and p["proposal_id"] == pid and g.offers[pid]["state"] == "open":
            g.void(pid, "invalid_now")
        return real(p)
    g.script = answer_voids
    env = do(gw, "answer_proposal", proposal_id=pid, answer="accept")
    _void_reply(env, "invalid_now")
    assert g.chars[SON]["spouses"] == []
    n = len(sent(gw, P.T_ANSWER))
    env = do(gw, "answer_proposal", proposal_id=pid, answer="accept")
    _void_reply(env, "invalid_now")
    assert len(sent(gw, P.T_ANSWER)) == n


def test_a_story_gone_after_its_void_is_void_not_not_found(monkeypatch, tmp_path):
    g, gw, pid, _ = _one(monkeypatch, tmp_path, "arrange_marriage_interaction", sa=BRIDE, sr=SON)
    g.end(pid)                      # voided and ended; its VOID line reaches the log after the call
    hidden = {"word": P.WORD_VOID, "fields": [str(pid), "void_war_ended"], "date": g.date}
    real = g.script

    def write_line_then_answer(p):
        r = real(p)
        if p["template"] == P.T_ANSWER:
            g.lines.append(hidden)
        return r
    g.script = write_line_then_answer
    env = do(gw, "answer_proposal", proposal_id=pid, answer="accept")
    _void_reply(env, "void_war_ended")
    assert env["result"]["sent"] is True


def test_the_servers_outcome_reasons_are_the_bridges():
    h = (ROOT / "bridge" / "include" / "script_templates.h").read_text(encoding="utf-8")

    def arr(name):
        return re.findall(r'"([^"]*)"', re.search(name + r"\[\] = \{(.*?)\};", h, re.S).group(1))
    assert set(P.OUTCOME_REASONS) == set(arr("kProposalOutcomeReasons"))
    assert set(P.VOID_REASONS) == set(arr("kProposalVoid")[1:]) | {"void_reason_unread"}
    assert set(P._OUTCOME_GATES) == set(P.OUTCOME_REASONS)


# ── The answer's own call row does not blind the read-back ────

def _held_before_the_seat_was_known(monkeypatch, tmp_path):
    """Live qf2 (2026-11-15): offers 4 and 5 to seat 37241 were HELD while
    this server had marked only other seats (the scenario marked 37241 through
    the template first), so their HELD lines were read and left out; the seat's
    observe then LISTED both. The answer's own call row took the id off the last
    list, and the RESOLVED line that followed was dropped as "not an offer to
    this seat": decline came back rejected / silent_no_op, the native hand-back's
    RESOLVED never reached the ledger."""
    led = declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)
    g.markers.add(KING)                                   # marked outside this server
    pid = g.send("arrange_marriage_interaction", SENDER, KING, sa=BRIDE, sr=SON)
    observe(gw, DUKE)                                     # reads (and leaves out) KING's HELD line
    view = observe(gw, KING)["result"]["incoming_proposals"]
    assert [o["id"] for o in view["offers"]] == [pid]
    assert pid not in P.state().held                      # the premise: the HELD line was not kept
    return g, gw, pid, led


def test_a_decline_of_an_offer_listed_but_never_held_here_is_applied(monkeypatch, tmp_path):
    g, gw, pid, led = _held_before_the_seat_was_known(monkeypatch, tmp_path)
    env = do(gw, "answer_proposal", proposal_id=pid, answer="decline")
    assert env["verdict"] == "applied", env.get("blocked")
    assert env["result"]["readback"]["gone_from_list"] is True
    rows = [r for r in ledger(led) if r.get("proposal_id") == pid and r.get("source") == "line"]
    assert [(r["kind"], r["answer"], r["by"], r["actor_id"], r["class"]) for r in rows] == \
        [("answered", "decline", "seat", KING, "marriage")]


def test_a_hand_back_of_an_offer_listed_but_never_held_here_reaches_the_ledger(monkeypatch, tmp_path):
    g, gw, pid, led = _held_before_the_seat_was_known(monkeypatch, tmp_path)
    env = do(gw, "answer_proposal", proposal_id=pid, answer="native")
    assert env["verdict"] == "pending"
    observe(gw, KING)                                     # the next read folds the RESOLVED line
    rows = [r for r in ledger(led) if r.get("proposal_id") == pid and r.get("source") == "line"]
    assert [(r["kind"], r["answer"], r["actor_id"]) for r in rows] == [("handed_back", "native", KING)]


def test_a_line_about_an_offer_this_server_never_saw_is_still_left_out(monkeypatch, tmp_path):
    """The other seat's offers stay out of this seat's ledger (the rule the fix keeps)."""
    led = declare(monkeypatch, tmp_path)
    g = Game()
    gw = Gw(g)
    observe(gw, KING)
    g.markers.add(DUKE)
    pid = g.send("arrange_marriage_interaction", SENDER, DUKE, sa=BRIDE, sr=SON)
    g._apply(pid, "decline", "rule", "class_policy:marriage=decline")
    observe(gw, KING)
    assert not [r for r in ledger(led) if r.get("proposal_id") == pid]


# ── A closed script log channel is said, never read as no offers ──

CLOSED = {"state": "closed", "level": "info", "cap": 100000, "info_messages": 100000,
          "debug_messages": 100000, "closed_since": "1088.01.27"}


def test_lines_from_a_closed_channel_are_unread_and_the_offset_holds():
    g = Game()
    g.lines = [{"word": P.WORD_HELD, "fields": ["1", "k", "c", "2", "3"], "date": "1088.1.1"}]
    g.channel = dict(CLOSED)
    st = P.SeatState()
    why = run(P.read_lines(Gw(g), st, {KING}))
    assert why == "script_log_unreadable: channel closed since 1088.01.27", why
    assert st.offset == 0 and not st.held


def test_a_list_cut_short_by_a_closed_channel_says_so_not_incomplete():
    g = Game()
    g.list_complete = False
    g.channel = dict(CLOSED)
    items, why, _ = run(P.list_offers(Gw(g), KING))
    g2 = Game()
    g2.list_complete = False
    items2, why2, _ = run(P.list_offers(Gw(g2), KING))
    assert items is None and why.startswith(P.U_LOG_UNREADABLE) and "closed since 1088.01.27" in why
    assert items2 is None and why2 == P.U_LIST_INCOMPLETE, "an open channel's short list stays incomplete"
