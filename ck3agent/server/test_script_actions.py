"""Lane v4act: the script action layer and the standing policies, server side.

The bridge's `script_action` verb runs one of the game's own script effects,
composed by the BRIDGE from a fixed template (bridge/include/script_templates.h),
guarded by the game's own check. These rows hold the server to the contract:

  * the moved action kinds (gift, sway, council_task, declare_war) go out as
    `script_action` with a template NAME and ids/keys, and nothing a seat passes
    reaches the bridge as script;
  * the verdict is the game's: the guard's marker, the game's own error text
    (in blocked.detail), and the read-back. A script that ran with no change read
    back is `write_not_applied` / silent_no_op, never applied;
  * a character the script cannot address (no held title) goes the native way,
    and the reply says so;
  * set_policy is refused unless the game logged the policy mod enabled, and its
    applied means the game read the policy variable back; observe shows the
    policies.

The fakes answer in the EMITTER's keys: script_action / query_script_log
(bridge/src/action_handlers.cpp do_script_action, do_query_script_log),
query_council (query_handlers.cpp:5923 position_key, :5926 current_task),
query_schemes (scheme_engine.cpp:190 type, :508 target_id), get_active_wars
(as test_write_refusal_words' fake, do_get_active_wars).
"""
import ast
import asyncio
import re
from pathlib import Path

import pytest

import redesign as R

ME, FOE, COURTIER = 100, 200, 300
HEADER = Path(__file__).resolve().parents[2] / "bridge" / "include" / "script_templates.h"


@pytest.fixture(autouse=True)
def _instant(monkeypatch):
    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)


def ran(template, **kw):
    return {"success": True, "action": "script_action", "template": template, "token": "SACT_9",
            "script": "<composed by the bridge>", "engine_result": "Executing effect",
            "guard": "ran", "errors": [], "logs_found": True, "debug_log_offset": 10,
            "error_log_offset": 20, **kw}


class Gw:
    """A gateway whose reads answer from `state`, which a write may change."""

    def __init__(self, script_reply=None, state=None, on_write=None):
        self.script_reply = script_reply
        self.on_write = on_write
        self.state = {"gold": {ME: 100.0, FOE: 50.0, COURTIER: 5.0}, "schemes": [], "council": [
            {"position_key": "councillor_spymaster", "current_task": "task_disrupt_schemes",
             "holder_id": 555}], "wars": [], "mod_enabled": True, "policies": {}}
        self.state.update(state or {})
        self.writes, self.reads = [], []
        self.health = "ok"

    async def actor_gate(self):
        return None

    def game(self):
        return {"tick": 1, "date": "867.01.05", "paused": True}

    async def clock(self):
        return {"paused": True, "speed": 1, "date": "867.01.05", "tick": 1}

    async def read(self, action, params=None):
        params = params or {}
        self.reads.append((action, dict(params)))
        st = self.state
        if action == "query_character":
            cid = params.get("character_id") or ME
            return {"id": cid, "alive": True, "name": f"c{cid}", "gold": st["gold"].get(cid, 0.0),
                    "prestige": 300, "age": 30, "game_date": "867.01.05"}
        if action == "query_schemes":
            return {"character_id": params.get("character_id"), "schemes": list(st["schemes"])}
        if action == "query_council":
            return {"character_id": params.get("character_id"), "council": [dict(p) for p in st["council"]]}
        if action == "get_active_wars":
            return {"success": True, "action": "get_active_wars", "character_id": ME,
                    "wars": list(st["wars"]), "war_count": len(st["wars"])}
        if action == "query_cb_validity_strict":
            return {"results": []}
        if action == "query_script_log":
            if "character_id" in params:
                return {"action": "query_script_log", "success": True, "logs_found": True,
                        "character_id": params["character_id"], "mod_enabled": st["mod_enabled"],
                        "policies": {p: st["policies"].get(p, False) for p in R.POLICIES}}
            return {"action": "query_script_log", "success": True, "token": params.get("token"),
                    "guard": st.get("late_guard", "unseen"), "errors": []}
        return {}

    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        if action == "script_action":
            if self.on_write:
                self.on_write(self.state, params)
            r = self.script_reply
            return r(params) if callable(r) else r
        return {"success": True, "status": "queued", "intent_request_id": "si_1"}


def run(c):
    return asyncio.run(c)


def scripts_sent(gw):
    return [p for a, p in gw.writes if a == "script_action"]


# ── what crosses to the bridge ───────────────────────────────────────────────

def test_gift_goes_as_the_gift_template_with_ids_only_and_applies_on_the_purses():
    def pay(st, _p):
        st["gold"][ME] -= 30.0
        st["gold"][FOE] += 30.0
    gw = Gw(ran("gift"), on_write=pay)
    env = run(R.do_impl(gw, intent="interact", character_id=ME,
                        args={"interaction": "gift_interaction", "target_id": FOE,
                              "script": "add_gold = 100000"}))
    sent = scripts_sent(gw)
    assert sent == [{"actor_id": ME, "target_id": FOE, "template": "gift"}], sent
    assert env["verdict"] == "applied", env
    assert env["result"]["path"] == "script" and env["result"]["readback"]["amount"] == 30.0
    assert [a for a, _ in gw.writes] == ["script_action"], "no native send beside the script"


def test_no_key_a_seat_sends_reaches_the_bridge_as_script():
    """Whatever extra keys a seat puts in args, the bridge sees only the
    template's typed parameters."""
    gw = Gw(ran("sway"))
    run(R.do_impl(gw, intent="interact", character_id=ME,
                  args={"interaction": "sway_interaction", "target_id": FOE,
                        "effect": "x", "arg": "y", "command": "effect", "template": "raw"}))
    (p,) = scripts_sent(gw)
    assert set(p) == {"actor_id", "target_id", "template"} and p["template"] == "sway"


def test_the_templates_the_server_names_are_the_bridges():
    names = set(re.findall(r'\{"([a-z_]+)", Template::k', HEADER.read_text()))
    assert set(R.SCRIPT_ROUTES.values()) | {"declare_war", "set_policy", "clear_policy"} <= names
    pol = re.findall(r'\{"([a-z_]+)", "ck3agent_policy_[a-z_]+"\}', HEADER.read_text())
    assert tuple(pol) == R.POLICIES, "the server's policy names mirror script_templates.h kPolicies"


# ── the verdict is the game's ───────────────────────────────────────────────

def test_a_gift_the_game_ran_with_no_change_is_not_applied_never_ok():
    gw = Gw(ran("gift"))                                   # purses do not move
    env = run(R.do_impl(gw, intent="interact", character_id=ME,
                        args={"interaction": "gift_interaction", "target_id": FOE}))
    assert env["verdict"] == "rejected" and env["ok"] is False, env
    assert env["blocked"]["gate"] == "write_not_applied"
    assert env["blocked"]["raw_reason"] == "silent_no_op"


def test_the_games_error_text_is_the_refusal():
    words = ("run_interaction effect [ Interaction cannot be sent. Reason: "
             "\x16warning_icon! \x15X No target selected\x15!  ]")
    gw = Gw(ran("sway", errors=[words]))
    env = run(R.do_impl(gw, intent="interact", character_id=ME,
                        args={"interaction": "sway_interaction", "target_id": FOE}))
    assert env["verdict"] == "rejected"
    assert env["blocked"]["gate"] == "interaction_invalid"
    assert "No target selected" in env["blocked"]["detail"], env["blocked"]
    assert "No target selected" in env["blocked"]["summary"]


def test_a_marker_the_logger_wrote_late_is_read_back():
    gw = Gw(ran("council_task", guard="unseen", seat="councillor_spymaster",
                task_used="task_find_secrets"),
            state={"late_guard": "refused"})
    env = run(R.do_impl(gw, intent="council_task", character_id=ME,
                        args={"task": "task_find_secrets", "target_character_id": FOE}))
    assert any(a == "query_script_log" and p.get("token") == "SACT_9" for a, p in gw.reads)
    assert env["verdict"] == "rejected" and env["blocked"]["gate"] == "engine_refused"
    assert env["blocked"]["raw_reason"] == "no_councillor_in_seat"
    assert env["result"]["marker_read_late"] is True


def test_a_council_task_applies_on_the_councils_read_back():
    def set_task(st, _p):
        st["council"][0]["current_task"] = "task_find_secrets"
    gw = Gw(ran("council_task", seat="councillor_spymaster", task_used="task_find_secrets"),
            on_write=set_task)
    env = run(R.do_impl(gw, intent="council_task", character_id=ME,
                        args={"task": "task_find_secrets", "target_character_id": FOE}))
    (p,) = scripts_sent(gw)
    assert p == {"actor_id": ME, "task": "task_find_secrets", "seat": "",
                 "target_character_id": FOE, "template": "council_task"}
    assert env["verdict"] == "applied", env
    assert env["result"]["readback"]["previous_task"] == "task_disrupt_schemes"


def test_no_answer_from_the_bridge_is_pending_not_rejected():
    gw = Gw({"status": "sent_no_confirmation"})
    env = run(R.do_impl(gw, intent="interact", character_id=ME,
                        args={"interaction": "sway_interaction", "target_id": FOE}))
    assert env["verdict"] == "pending", env
    assert env["pending"]["raw_reason"] == "sent_no_confirmation"


def test_declare_war_runs_start_war_under_can_declare_war():
    def war(st, _p):
        st["wars"].append({"war_id": 77, "attacker_id": ME, "defender_id": FOE, "cb_type": "claim_cb",
                           "target_titles": [1141], "name": "War"})
    gw = Gw(ran("declare_war"), on_write=war)
    env = run(R.declare_war_impl(gw, ME, target_id=FOE, cb="claim_cb", cb_title_id=1141,
                                 confirm_straddle=True))
    (p,) = scripts_sent(gw)
    assert p == {"actor_id": ME, "target_id": FOE, "cb": "claim_cb", "cb_title_id": 1141,
                 "claimant_id": ME, "template": "declare_war"}
    assert env["verdict"] == "applied" and env["result"]["war_id"] == 77, env
    assert env["result"]["path"] == "script"
    assert [a for a, _ in gw.writes] == ["script_action"]


def test_a_war_the_games_check_refuses_says_so_with_the_doors():
    gw = Gw(ran("declare_war", guard="refused"))
    env = run(R.declare_war_impl(gw, ME, target_id=FOE, cb="claim_cb", cb_title_id=1141,
                                 confirm_straddle=True))
    assert env["verdict"] == "rejected"
    assert env["blocked"]["gate"] == "cb_invalid_for_target"
    assert env["blocked"]["raw_reason"] == "can_declare_war_false"
    assert env["blocked"]["detail"] == R.NO_GAME_DETAIL      # the check gives no words
    assert env["blocked"]["fix"]["tool"] == "cbs"


def test_a_war_that_ran_and_is_not_listed_is_not_applied():
    gw = Gw(ran("declare_war"))
    env = run(R.declare_war_impl(gw, ME, target_id=FOE, cb="claim_cb", cb_title_id=1141,
                                 confirm_straddle=True))
    assert env["verdict"] == "rejected" and env["blocked"]["gate"] == "write_not_applied"


def test_an_unaddressable_character_goes_the_native_way_and_says_so():
    gw = Gw({"success": False, "action": "script_action", "reason": "target_not_script_addressable",
             "detail": "target_id"})
    env = run(R.do_impl(gw, intent="interact", character_id=ME,
                        args={"interaction": "sway_interaction", "target_id": COURTIER}))
    assert [a for a, _ in gw.writes] == ["script_action", "send_interaction"]
    assert any(w["code"] == "script_path_unavailable" for w in env["warnings"])


def test_a_bridge_refusal_before_the_game_is_typed():
    gw = Gw({"success": False, "action": "script_action", "reason": "task_not_found",
             "detail": "task_nope"})
    env = run(R.do_impl(gw, intent="council_task", character_id=ME, args={"task": "task_nope"}))
    assert env["verdict"] == "rejected" and [a for a, _ in gw.writes] == ["script_action"]


def test_peace_and_the_war_intents_stay_native():
    assert ("make_peace", "") not in R.SCRIPT_ROUTES
    assert not any(k[0] in R.WAR_INTENTS for k in R.SCRIPT_ROUTES)
    for kind in ("make_peace", "arrange_marriage_interaction", "declare_war_interaction"):
        assert kind in R.SCRIPT_NOT_MOVED


# ── standing policies ────────────────────────────────────────────────────────

def test_set_policy_is_refused_without_the_mod():
    gw = Gw(ran("set_policy"), state={"mod_enabled": False})
    env = run(R.do_impl(gw, intent="set_policy", character_id=ME,
                        args={"policy": "join_liege_wars", "on": True}))
    assert env["verdict"] == "rejected" and env["blocked"]["raw_reason"] == "policy_mod_not_loaded"
    assert scripts_sent(gw) == []


def test_set_policy_applies_when_the_game_reads_the_variable_back():
    def ledger(st, p):
        st["policies"][p["policy"]] = p["template"] == "set_policy"
    gw = Gw(ran("set_policy"), on_write=ledger)
    env = run(R.do_impl(gw, intent="set_policy", character_id=ME,
                        args={"policy": "stay_out_of_liege_factions", "on": True}))
    assert scripts_sent(gw) == [{"actor_id": ME, "policy": "stay_out_of_liege_factions",
                                 "template": "set_policy"}]
    assert env["verdict"] == "applied", env
    assert env["result"]["policies"]["policies"]["stay_out_of_liege_factions"] is True
    gw2 = Gw(ran("clear_policy", guard="refused"))
    env2 = run(R.do_impl(gw2, intent="set_policy", character_id=ME,
                         args={"policy": "stay_out_of_liege_factions", "on": False}))
    assert scripts_sent(gw2)[0]["template"] == "clear_policy"
    assert env2["verdict"] == "rejected" and env2["blocked"]["gate"] == "write_not_applied"


def test_set_policy_refuses_an_unknown_policy():
    gw = Gw(ran("set_policy"))
    env = run(R.do_impl(gw, intent="set_policy", character_id=ME, args={"policy": "conquer_all"}))
    assert env["blocked"]["gate"] == "missing_required_argument" and scripts_sent(gw) == []


def test_set_policy_is_an_intent_and_its_args_are_documented():
    import do_intent_args
    assert "set_policy" in R.INTENTS
    assert set(do_intent_args.extract_dedicated()["set_policy"]) >= {"policy", "on"}


def test_observe_carries_the_standing_policies():
    """observe_impl assigns result["standing_policies"] from policies_of (AST, not
    a text search: a comment naming it would not satisfy this)."""
    src = Path(R.__file__).read_text()
    fn = next(n for n in ast.walk(ast.parse(src))
              if isinstance(n, ast.AsyncFunctionDef) and n.name == "observe_impl")
    found = False
    for n in ast.walk(fn):
        if (isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Subscript)
                and isinstance(n.targets[0].slice, ast.Constant)
                and n.targets[0].slice.value == "standing_policies"):
            calls = [c for c in ast.walk(n.value) if isinstance(c, ast.Call)
                     and getattr(c.func, "id", None) == "policies_of"]
            found = bool(calls)
    assert found
    view = run(R.policies_of(Gw(None, state={"policies": {"join_liege_wars": True}}), ME))
    assert view["policies"] == {"join_liege_wars": True, "stay_out_of_liege_factions": False}
    assert view["mod_enabled"] is True and "set_policy" in view["set_with"]
