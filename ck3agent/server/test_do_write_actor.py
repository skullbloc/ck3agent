"""The actor every generic-path `do` write carries to the bridge.

Live: from a save with 325 gold and 12 of 14
Irish counties, do(intent="mint_title", args={"title_key": "k_ireland"}) -- the call every reply
offers (formable_titles' do_call, the ai_vetoed row's next) -- was refused `actor_id_required`:
the server sent actor_id 0 for every intent but scheme and appoint, and do_create_title takes no
default. The same call naming character_id created the kingdom. The fix resolves the actor for
every intent in one place (redesign.dispatch_actor_of); these tests drive do_impl itself.
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

import redesign as R  # noqa: E402

ME = 31762

#: one call per generic-path intent (INTENT_TO_ACT less the war intents, which have their own
#: branch): arguments that pass the server's own checks, so the call reaches the bridge.
CALLS = {
    "mint_title": {"title_key": "k_ireland"},
    "take_decision": {"decision": "isolate_capital_decision"},
    "interact": {"interaction": "befriend_interaction", "target_id": 4242},
    "convert_faith": {"faith": "catholic"},
    "convert_culture": {"culture": "irish"},
    "set_doctrine": {"doctrine": "doctrine_pluralism_righteous"},
    "change_law": {"law": "crown_authority_1"},
    "council_task": {"task": "task_domestic_affairs"},
    "appoint": {"position_id": 2, "character_id": 4243},
    "lifestyle": {"focus": "stewardship_wealth_focus"},
    "set_education_focus": {"focus": "education_stewardship"},
    "build": {"building": "farm_estates_01", "location_id": 7},
    "men_at_arms": {"action": "recruit", "maa_type": "bowmen"},
    "hire_merc": {"company_id": 58},
    "move_capital": {"title_id": 7},
    "set_obligation": {"vassal_id": 4244, "obligation": "feudal_government_taxes", "level": 0},
    "scheme": {"action": "start", "scheme": "murder", "target_id": 4245},
}


class Gw:
    """Bound ruler ME. Reads answer what the pre-checks need; every write is recorded and
    refused by the bridge (so no follow-up read decides the test)."""

    def __init__(self, reply=None):
        self.writes = []
        self.reads = []
        self.health = {"ok": True}
        self.reply = reply or {"success": False, "reason": "stop_here"}

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return {"id": ME, "alive": True, "name": "Murchad", "gold": 400.0}
        if action == "query_dejure_progress":
            return {"held": 12, "total": 14, "fraction": 0.86}
        if action == "query_create_title_cost":
            return {"success": True, "action": "query_create_title_cost", "actor_id": ME, "title_id": 461,
                    "title_key": "k_ireland", "engine_valid": True, "engine_reasons": [],
                    "price": {"known": True, "currency": "gold", "cost": 325.0, "have": 400.0, "affordable": True}}
        return {}

    async def read_many(self, specs):
        return [{} for _ in specs]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return self.reply

    async def confirmed_write(self, action, params, *, readback, deadline_s=12.0):
        # as test_mint_price's Gateway: the write, then its readback
        raw = await self.write(action, params)
        return {"raw": raw, "resolved": await readback(raw), "refused": "", "sent": True,
                "drains_when": "next_pass", "game_date_at_submit": "1081.03.27", "intent_id": ""}

    def game(self):
        return {"tick": 1, "date": "1081.03.27", "game_date": "1081.03.27"}


def run(c):
    return asyncio.run(c)


def _actor_sent(params):
    # send_decision names its actor `character_id` (tools.resolve_act); every other write `actor_id`
    return params.get("actor_id") or params.get("character_id")


def _bridge_write(gw):
    sends = [w for w in gw.writes if w[0] not in ("script_action",) or w[1].get("template") not in ("activity",)]
    assert sends, f"nothing reached the bridge: reads {[r[0] for r in gw.reads]}"
    return sends[0]


def test_0_the_table_covers_every_generic_intent():
    assert set(CALLS) == set(R.INTENT_TO_ACT) - set(R.WAR_INTENTS)


@pytest.mark.parametrize("intent", sorted(CALLS))
def test_1_a_bare_call_sends_the_bound_ruler(intent):
    gw = Gw()
    run(R.do_impl(gw, intent=intent, args=dict(CALLS[intent])))
    action, params = _bridge_write(gw)
    assert _actor_sent(params) == ME, (intent, action, params)


@pytest.mark.parametrize("intent", sorted(CALLS))
def test_2_an_explicit_character_id_still_wins(intent):
    gw = Gw()
    run(R.do_impl(gw, intent=intent, character_id=4300, args=dict(CALLS[intent])))
    action, params = _bridge_write(gw)
    assert _actor_sent(params) == 4300, (intent, action, params)


def test_3_the_offered_mint_call_sends_create_title_with_the_ruler():
    gw = Gw()
    run(R.do_impl(gw, intent="mint_title", args={"title_key": "k_ireland"}))
    assert gw.writes[0] == ("create_title", {"actor_id": ME, "title_key": "k_ireland"})


def test_4_the_actor_id_required_refusal_says_what_to_send():
    gw = Gw({"success": False, "action": "create_title", "reason": "actor_id_required"})
    env = run(R.do_impl(gw, intent="move_capital", args={"title_id": 7}))
    b = env["blocked"]
    assert b["raw_reason"] == "actor_id_required"
    assert b["retry"] != "never" and b["retryable"] is True
    assert b["next"] == {"send_again_with": {"character_id": "your ruler's id (actor.id in this reply)"}}
    assert "enumerate" not in str(b["next"])


def test_4_CONTROL_another_missing_argument_keeps_its_answer():
    b = R.why_blocked("missing_required_argument", raw_reason="maa_type_required")
    assert b["retry"] == "never" and "send_again_with" not in str(b.get("next"))


# ── the census, read from the code: which bridge handler takes actor 0 how ─────────────────────

from pathlib import Path  # noqa: E402

BRIDGE = Path(__file__).resolve().parents[2] / "bridge" / "src"


def handler_actor_rule(bridge_action: str, _fn: str = "") -> str:
    """How the bridge handler of one action treats actor 0, read from its body (comments stripped):
    `refuses` (actor_id_required), `falls_back` (resolve_actor / the session's controlled ruler) or
    `unread` (neither found: reported, never guessed)."""
    sys.path.insert(0, str(BRIDGE.parent / "scripts"))
    import re
    import cpp_strip
    if _fn:
        fn = _fn
    else:
        reg = (BRIDGE / "action_registry.inc").read_text(encoding="utf-8")
        m = re.search(r'BRIDGE_ACTION\("%s",\s*(\w+)\)' % re.escape(bridge_action), reg)
        if not m:
            return "unread"
        fn = m.group(1)
    for f in sorted(BRIDGE.glob("*.cpp")):
        src = cpp_strip.strip(f.read_text(encoding="utf-8", errors="replace"), keep_strings=True)
        h = re.search(r"\bbool\s+%s\s*\([^)]*\)\s*\{" % fn, src)
        if not h:
            continue
        i = src.index("{", h.end() - 1)
        body = src[i:cpp_strip.match_close(src, i) + 1]
        if "actor_id_required" in body:
            return "refuses"
        if re.search(r"resolve_actor\(|get_controlled_character\(|controlled_character_reading\(|t\.actor", body):
            return "falls_back"
        hand = re.search(r"\breturn\s+(do_\w+)\s*\(\s*params_json", body)   # a handler that delegates
        return handler_actor_rule(bridge_action, hand.group(1)) if hand and hand.group(1) != fn else "unread"
    return "unread"


def census():
    import tools as T
    out = {}
    for intent, a in sorted(CALLS.items()):
        p = R._intent_to_act_params(intent, dict(a), 0)
        action, _ = T.resolve_act(p)
        out[intent] = (action, handler_actor_rule(action))
    return out


def test_5_the_census_finds_the_refusing_handlers_and_every_intent_is_sent_its_ruler():
    c = census()
    refusing = {i for i, (_, rule) in c.items() if rule == "refuses"}
    # the handlers that take no default (live: create_title refused the offered k_ireland call)
    assert {"mint_title", "move_capital", "set_obligation", "appoint", "scheme"} <= refusing, c
    # fail open: a census that classifies nothing is a broken reader, not a clean tree
    assert sum(rule != "unread" for _, rule in c.values()) >= 12, c
    # and whatever a handler does with 0, the server no longer sends 0 (test_1 drives each)
    assert R.dispatch_actor_of(0, {"id": ME}) == ME and R.dispatch_actor_of(4300, {"id": ME}) == 4300


class HeldGw(Gw):
    """The drain ran the creation: the bridge replies queued with the title id, and the title reads held by ME."""

    async def read(self, action, params=None):
        if action == "query_title_succession":
            self.reads.append((action, dict(params or {})))
            return {"action": "query_title_succession", "title_id": 461, "current_holder_id": ME}
        return await super().read(action, params)


def test_6_the_bare_mint_reads_back_the_ruler_it_was_sent_for():
    """Live: the bare call created k_ireland (gold 325.32 -> 0.32, history `created`,
    holder 31762) and the reply said rejected / write_unconfirmed: the readback compared the holder with the
    character_id argument, 0."""
    gw = HeldGw({"success": False, "action": "create_title", "actor_id": ME, "title_id": 461, "title_key": "k_ireland",
                 "status": "queued", "reason": "no_poll_on_game_thread", "drains_when": "next_pass"})
    env = run(R.do_impl(gw, intent="mint_title", args={"title_key": "k_ireland"}))
    assert env["verdict"] == "applied", env.get("blocked")
    assert env["result"]["readback"] == {"title_held": "k_ireland", "title_id": 461, "via": "title_holder"}


def test_6_CONTROL_another_holder_is_not_read_as_ours():
    class Other(HeldGw):
        async def read(self, action, params=None):
            if action == "query_title_succession":
                return {"action": "query_title_succession", "title_id": 461, "current_holder_id": 4300}
            return await super().read(action, params)
    gw = Other({"success": False, "action": "create_title", "actor_id": ME, "title_id": 461, "status": "queued",
                "reason": "no_poll_on_game_thread", "drains_when": "next_pass"})
    env = run(R.do_impl(gw, intent="mint_title", args={"title_key": "k_ireland"}))
    assert env["verdict"] != "applied"
