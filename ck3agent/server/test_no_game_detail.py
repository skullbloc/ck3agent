"""A refusal the game gives no reason for says so (cleanup 2026.11, L12 item 5).

The game's reason for a compound condition can be its header alone ("All of
these:"): the failing children render through their own tooltip keys and a
child without one renders nothing (FUN_0763e6b0; the imprison check and
imperial_policy_faction on the 867 save). The cleaners drop the bare header
(engine_text.h, redesign.engine_text), and every path that shows the game's
reason then says redesign.NO_GAME_DETAIL instead of an empty header or nothing.

inspect(pending): a rejected row always carries either the game's reason or that
sentence, also when the bridge sent no detail at all (w12-full-r2: designate_heir
11657 -> 14778 rejected, interaction_not_valid_for_target, detail None, and the
player read nothing).
"""
import asyncio

import factions
import redesign as R


class _GW:
    health = "ok"

    def __init__(self, results):
        self.results = results

    async def read(self, verb, params):
        return {"pending": []} if verb == "query_intents" else {"results": self.results}

    def game(self):
        return {"tick": 1}


def _completed(*rows):
    env = asyncio.run(R._inspect_pending(_GW(list(rows)), 16))
    return env["result"]["completed"]


def test_the_bare_header_is_no_reason():
    assert R.engine_text("All of these:") == ""
    assert R.engine_text("\x15indent_newline:0 \x15!\x15indent_newline:6 \x15X All of these:\x15!") == ""


def test_build_refusal_with_only_a_header_says_no_detail():
    assert R._build_refusal_summary({"engine_reasons": ["All of these:"]}) == \
        "the game refuses: " + R.NO_GAME_DETAIL
    # the control: a real reason is said, and no reasons at all is no claim
    assert R._build_refusal_summary({"engine_reasons": ["Missing 50 gold"]}) == \
        "the game refuses: Missing 50 gold"
    assert R._build_refusal_summary({}) == ""


def test_send_check_without_a_reason_says_no_detail():
    b = R._send_check_blocked({"sendable": False, "refused_by": "send_conditions_not_met"})
    assert b["summary"].endswith(": " + R.NO_GAME_DETAIL), b
    b = R._send_check_blocked({"sendable": False, "refused_by": "send_conditions_not_met",
                               "engine_reason": "Is not adult"})
    assert b["summary"].endswith(": Is not adult"), b


def test_faction_refusal_without_a_reason_says_no_detail():
    assert factions._refusal("engine_refused_create", "")["summary"] == R.NO_GAME_DETAIL
    assert factions._refusal("engine_refused_create", "X has a Ceremonial Monarch")["summary"] \
        == "X has a Ceremonial Monarch"


def test_a_rejected_row_with_no_detail_says_the_game_gives_none():
    rej = {"intent_id": "a", "action_name": "designate_heir", "status": "rejected",
           "rejection_reason": "interaction_not_valid_for_target",
           "actor_id": 11657, "target_id": 14778}
    assert _completed(rej)[0]["detail"] == R.NO_GAME_DETAIL
    assert _completed({**rej, "detail": None})[0]["detail"] == R.NO_GAME_DETAIL
    assert _completed({**rej, "detail": ""})[0]["detail"] == R.NO_GAME_DETAIL
    assert _completed({**rej, "detail": "All of these:"})[0]["detail"] == R.NO_GAME_DETAIL
    # the control: the game's own reason is said as words
    assert _completed({**rej, "detail": "Is not adult"})[0]["detail"] == "Is not adult"


def test_a_row_that_is_not_rejected_gains_no_detail_it_did_not_have():
    ok = {"intent_id": "b", "action_name": "gift", "status": "success"}
    assert "detail" not in _completed(ok)[0]
    assert _completed({**ok, "detail": "sent"})[0]["detail"] == "sent"
    assert _completed({**ok, "detail": "All of these:"})[0]["detail"] == R.NO_GAME_DETAIL
