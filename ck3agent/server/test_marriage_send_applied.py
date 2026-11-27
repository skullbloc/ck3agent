"""A seat's marriage send.

The premise of this fix was "a marriage send is accepted by the tool and
never takes effect". The recorded runs say otherwise (re/findings/
re_marriage_send.md): every seat send the game let through applied (a recorded game
1078.02.03: sent to the King of Leinster for his courtier 45498, who sat as the
seat's councillor_spouse by 1079.06.03), and every other send was the game's own
refusal. Two tool defects made it look like "never":

  A. the do() reply had no readback for arrange_marriage_interaction, so a send
     that landed stayed `pending` forever and nobody saw it land;
  B. the candidate listing offered republics' rulers (mayors), whom the game
     bars an AI-run ruler from marrying: 3 of a recorded game's 4 sends.

These rows grade both fixes. Controls: each row names what the superseded tree
answers (no readback: pending; the listing passes the mayor through).
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

R = pytest.importorskip("redesign")
import tools  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE_SRC = os.path.join(HERE, "..", "..", "bridge", "src")

RULER, SON, BRIDE, OTHER_KING = 36303, 71230, 45498, 37528
QUEUED = {"success": True, "status": "queued", "action": "send_interaction",
          "interaction_type": "arrange_marriage_interaction", "method": "intent_dispatch",
          "intent_request_id": "si_36303_4156_46525"}


def run(coro):
    return asyncio.run(coro)


class MarriageGateway:
    """The drain says success; query_character of the secondary actor answers
    `before` until the dispatch verdict was read, then `after`."""

    def __init__(self, before, after, paused=True):
        self.before, self.after, self.paused = before, after, paused
        self.health = {"ok": True}
        self.drained = False
        self.writes = []

    async def actor_gate(self):
        return None

    async def clock(self):
        return {"paused": self.paused}

    async def read(self, action, params=None):
        p = params or {}
        if action == "query_dispatch_result":
            self.drained = True
            return {"status": "success", "dispatch_id": p.get("dispatch_id")}
        if action == "query_character":
            cid = int(p.get("character_id") or RULER)
            body = self.after if self.drained else self.before
            return dict({"id": cid, "alive": True, "name": "c"}, **(body if cid == SON else {}))
        return {}

    async def read_many(self, specs):
        return [{} for _ in specs]

    async def write(self, action, params=None):
        self.writes.append((action, params))
        if action == "script_action":
            return {"success": False, "action": "script_action",
                    "reason": "target_not_script_addressable", "detail": "target_id"}
        return dict(QUEUED)

    def game(self):
        return {"tick": 4156, "game_date": "1078.02.03"}


def _send(gw, monkeypatch, **extra):
    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    args = {"interaction": "arrange_marriage_interaction", "target_id": OTHER_KING,
            "secondary_actor_id": SON, "secondary_recipient_id": BRIDE}
    args.update(extra)
    return run(R.do_impl(gw, intent="interact", character_id=RULER, args=args))


UNMARRIED = {"spouses": [], "betrothed": None}
MARRIED = {"spouses": [{"id": BRIDE, "alive": True, "matrilineal": False}], "betrothed": None}
BETROTHED = {"spouses": [], "betrothed": {"id": BRIDE, "matrilineal": True}}


# ---------------------------------------------------------------- A: the readback
def test_A1_a_marriage_that_landed_is_APPLIED_with_the_tie_read_back(monkeypatch):
    """Superseded: LEVERS had readback None, so this exact send answered pending
    clock_advances whatever the spouse list said (a recorded game's applied send)."""
    gw = MarriageGateway(UNMARRIED, MARRIED)
    env = _send(gw, monkeypatch)
    assert env["verdict"] == "applied", env
    rb = env["result"]["readback"]
    assert {k: rb[k] for k in ("secondary_actor_id", "secondary_recipient_id", "tie", "matrilineal")} == {
        "secondary_actor_id": SON, "secondary_recipient_id": BRIDE, "tie": "married", "matrilineal": False}, rb
    # The alliance is a read beside the tie; this double answers no alliance read
    assert rb["alliance"]["read"] is False, rb


def test_A2_a_childs_betrothal_reads_back_as_betrothed(monkeypatch):
    gw = MarriageGateway(UNMARRIED, BETROTHED)
    env = _send(gw, monkeypatch)
    assert env["verdict"] == "applied" and env["result"]["readback"]["tie"] == "betrothed", env
    assert env["result"]["readback"]["matrilineal"] is True


def test_A3_a_betrothal_held_BEFORE_the_send_is_not_this_sends_effect(monkeypatch):
    """The pair was already betrothed: only a marriage would be new. Not applied;
    a paused game waits for the clock (pending clock_advances)."""
    gw = MarriageGateway(BETROTHED, BETROTHED, paused=True)
    env = _send(gw, monkeypatch)
    assert env["verdict"] == "pending", env
    assert env["pending"]["drains_when"] == "clock_advances", env["pending"]


def test_A4_not_yet_answered_is_pending_and_says_where_to_look(monkeypatch):
    gw = MarriageGateway(UNMARRIED, UNMARRIED, paused=True)
    env = _send(gw, monkeypatch)
    assert env["verdict"] == "pending" and env["result"]["sent"] is True, env
    p = env["pending"]
    assert p["drains_when"] == "clock_advances", p
    assert f"character_id={SON}" in p["expected_change"] and str(BRIDE) in p["expected_change"], p
    assert "never" not in p["why_pending"]


def test_A5_a_profile_without_the_family_fields_is_UNREAD_never_not_married(monkeypatch):
    gw = MarriageGateway({}, {}, paused=True)
    env = _send(gw, monkeypatch)
    assert env["ok"] is True and env["verdict"] == "pending", env
    assert env["pending"]["drains_when"] == "readback", env["pending"]


def test_A6_a_send_naming_no_pair_reads_nothing_back(monkeypatch):
    gw = MarriageGateway(UNMARRIED, MARRIED, paused=True)
    env = _send(gw, monkeypatch, secondary_actor_id=0, secondary_recipient_id=0, target_id=BRIDE)
    assert env["verdict"] == "pending", env


def test_A7_the_lever_row_names_the_readback_and_its_evidence():
    key, row = R.lever_of("interact", {"interaction_type": "arrange_marriage_interaction"})
    assert key == ("interact", "arrange_marriage_interaction")
    assert row["shows"] == "days" and "spouses" in row["readback"], row
    assert R.LEVER_READBACKS[key] == (R._marriage_before, R._marriage_landed)


# ---------------------------------------------------------------- B: the republic rule
LISTING = {"actor_id": RULER, "sort": "military", "require_in_range": True, "sex": "female",
           "candidates": [{"id": 900, "age": 30}]}


def _reader():
    people = {RULER: {"id": RULER, "name": "Brian", "is_female": False, "age": 30, "spouses": [],
                      "children": []},
              900: {"id": 900, "name": "Fedelm", "is_female": True, "age": 30, "spouses": []}}

    async def read(action, params):
        return people.get(int(params.get("character_id") or 0), {})
    return read


def test_B1_the_bridges_republic_count_is_a_not_listed_reason():
    out = run(R.marriage_rows(dict(LISTING, skipped_republics=3), _reader()))
    assert out["not_listed"][R.MARRIAGE_REPUBLIC_WHY] == 3, out["not_listed"]
    assert "skipped_republics" not in out and "republics" not in out


def test_B2_an_older_bridge_that_did_not_filter_says_so():
    """Superseded bridge reply (no skipped_republics): never a silent pass."""
    out = run(R.marriage_rows(dict(LISTING), _reader()))
    assert out["republics"] == R.MARRIAGE_REPUBLIC_UNFILTERED, out


def test_B3_a_filter_that_did_not_run_carries_the_bridges_reason():
    out = run(R.marriage_rows(dict(LISTING, republics_not_skipped="the actor's government was not read"),
                              _reader()))
    assert out["republics"] == "the actor's government was not read", out


def test_B4_both_readers_ask_the_bridge_to_skip_republics():
    action, fn = tools.QUERY_DOMAIN_MAP["marriage_candidates"]
    assert action == "query_marriage_candidates" and fn({})["skip_republics"] == 1
    action, fn = R._INSPECT_SUBJECTS["marriage"]
    assert action == "query_marriage_candidates" and fn({"cid": RULER})["skip_republics"] == 1


def _handler(name: str) -> str:
    src = open(os.path.join(BRIDGE_SRC, "query_handlers.cpp"), encoding="utf-8").read()
    body = src[src.index(f"bool {name}("):]
    return body[:body.index("\n}\n")]


def test_B5_the_bridge_scan_drops_exactly_the_flagged_government_for_a_non_republic_actor():
    body = _handler("do_query_marriage_candidates")
    assert '"skip_republics"' in body
    assert 'actor_government != "republic_government"' in body
    assert '== "republic_government"' in body
    # the skip sits before the row is counted, so `matched` is post-filter
    assert body.index("skipped_republics++") < body.index("matched++")
    assert '"republics_not_skipped"' in body


# ---------------------------------------------------------------- army action=raid
# The bridge reply shapes are the ones bridge/src/army_orders.cpp do_army_raid writes. Not run
# live (re/findings/re_raid_toggle.md names the proof owed).
import army_orders as A  # noqa: E402
from test_army_orders import BridgeGw, actor, gate, ON, ME  # noqa: E402


def test_R1_raid_is_an_order_action_refused_while_orders_are_off():
    assert "raid" in A.ARMY_ACTIONS and "raid" in A._ORDER_ACTIONS
    gw = BridgeGw({"success": True})
    env = run(A.do_army(gw, {"action": "raid", "army_id": 7, "on": True}, ME, actor(), environ={}))
    assert gate(env) == ("experimental_write_disabled", "army_orders_off") and gw.writes == []


def test_R2_raid_sends_the_state_asked_and_the_check():
    gw = BridgeGw(lambda a, p: {"success": True, "action": a, "army_id": 7, "on": bool(p["on"]),
                                "check": p["check"], "allowed": True, "applied": not p["check"],
                                "raiding_before": False, "raiding_after": not p["check"]})
    run(A.do_army(gw, {"action": "raid", "army_id": 7, "on": True}, ME, actor(), environ=ON))
    env = run(A.do_army(gw, {"action": "raid", "army_id": 7, "on": False, "check": True}, ME, actor(),
                        environ=ON))
    assert gw.writes == [("army_raid", {"actor_id": ME, "army_id": 7, "on": 1, "check": False}),
                         ("army_raid", {"actor_id": ME, "army_id": 7, "on": 0, "check": True})]
    assert env["result"]["checked_only"] is True


def test_R3_raid_without_a_boolean_on_is_refused_before_the_bridge():
    for bad in ({}, {"on": 1}, {"on": "yes"}):
        gw = BridgeGw({"success": True})
        env = run(A.do_army(gw, {"action": "raid", "army_id": 7, **bad}, ME, actor(), environ=ON))
        assert gate(env) == ("missing_required_argument", "raid_on_required") and gw.writes == [], bad


def test_R4_the_games_refusal_and_the_no_op_carry_their_reasons():
    gw = BridgeGw({"success": False, "action": "army_raid", "reason": "refused_by_game",
                   "game_reason": "Can only toggle raiding in own territory"})
    env = run(A.do_army(gw, {"action": "raid", "army_id": 7, "on": True}, ME, actor(), environ=ON))
    assert gate(env) == ("engine_refused", "refused_by_game") and "own territory" in env["blocked"]["summary"]
    gw = BridgeGw({"success": False, "action": "army_raid", "reason": "already_in_that_state",
                   "detail": "the army is already raiding; nothing was sent"})
    env = run(A.do_army(gw, {"action": "raid", "army_id": 7, "on": True}, ME, actor(), environ=ON))
    assert gate(env) == ("already_in_that_state", "already_in_that_state")


def test_R5_the_public_sentence_does_not_offer_the_unproven_raid():
    """Built, not proven live: do's sentence names only what ran live."""
    assert "raid" not in A.do_sentence(ON) and "raid" not in A.do_sentence({})


def test_R6_the_bridge_builds_the_toggle_as_the_army_window_does():
    src = open(os.path.join(BRIDGE_SRC, "army_orders.cpp"), encoding="utf-8").read()
    body = src[src.index("bool do_army_raid("):]
    body = body[:body.index("\n}\n")]
    for name in ("TOGGLE_RAIDING_CMD_CTOR", "TOGGLE_RAIDING_CMD_CANEXEC", "TOGGLE_RAIDING_CMD_EXECUTE",
                 "toggle_raiding_cmd::EXEC_THIS", "ARMY_IS_RAIDING", "raid_needs_toggle", "g_orders_on"):
        assert name in body, name
    # the toggle is sent only after the state check, and executed only when the game's check passed
    assert body.index("raid_needs_toggle") < body.index("ctor(cmd")
    assert "if (can && !check) { execute(" in body
    reg = open(os.path.join(BRIDGE_SRC, "action_registry.inc"), encoding="utf-8").read()
    assert 'BRIDGE_ACTION("army_raid", do_army_raid)' in reg
    snap = open(os.path.join(BRIDGE_SRC, "snapshot.cpp"), encoding="utf-8").read()
    assert '"army_raid",' in snap   # a game-thread action


# ---------------------------------------------------------------- follow-up: ai_veto what=war
# The bridge's record shape is do_query_ai_veto's (what "war", target_id, key the interaction's, priced no).
import seat_events as SE  # noqa: E402
from test_ai_veto import Veto, VGw  # noqa: E402
from test_ai_veto import actor as veto_actor  # noqa: E402


def test_W1_war_is_a_kind_the_seat_sets_on_its_own_not_in_spending():
    assert "war" in SE.AI_VETO_WHATS
    assert "war" not in SE.AI_VETO_PRESETS["spending"]
    assert "never dropped" in SE.AI_VETO_LEFT_OUT["war"]


def test_W2_set_war_sends_the_kind_to_the_bridge():
    gw = VGw(Veto())
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "war"}, 0, veto_actor()))
    assert env["ok"] is True, env
    assert [p["what"] for a, p in gw.writes if a == "set_ai_veto"] == ["war"]


def _war_rec(seq, dropped, target=29585):
    return {"seq": seq, "date": "1067.03.01", "character_id": 31506, "what": "war", "dropped": dropped,
            "target_id": target, "origin": "game", "key": "declare_war_interaction", "priced": "no",
            "unpriced_why": "war_cb_not_read", "gold": 80.0, "prestige": 200.0, "piety": 10.0}


def test_W3_a_dropped_war_is_one_news_row_per_target_with_the_target():
    out = SE.veto_news({}, {"ai_veto_records": [_war_rec(1, True), _war_rec(2, True), _war_rec(3, True, 32287)]})
    rows = out["ai_vetoed"]
    assert [(r["what"], r["target_id"], r["attempts"]) for r in rows] == [("war", 29585, 2), ("war", 32287, 1)]
    assert all("key" not in r for r in rows)


def test_W4_a_war_the_ai_declared_unvetoed_names_the_veto_call():
    out = SE.veto_news({}, {"ai_veto_records": [_war_rec(1, False)], "ai_vetoes": []})
    (a,) = out["ai_acted"]
    assert a["what"] == "war" and a["target_id"] == 29585 and "key" not in a
    assert a["veto"]["args"]["args"] == {"op": "set", "what": "war"}


def test_W5_the_bridge_marks_its_own_war_post_and_classifies_the_ai_war():
    src = open(os.path.join(BRIDGE_SRC, "intent_dispatch.cpp"), encoding="utf-8").read()
    i = src.index("war_post_decl(ai_ctrl, static_cast<int>(rng_seed), 1);")
    assert "bridge::OwnDispatchMark own_post;" in src[i - 600:i]
    veto = open(os.path.join(BRIDGE_SRC, "ai_veto.cpp"), encoding="utf-8").read()
    assert 'lookup_interaction("declare_war_interaction")' in veto
    assert "g_declare_war_itype.load(std::memory_order_acquire));" in veto
