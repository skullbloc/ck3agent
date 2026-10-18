"""ck-zgh4 deploy-51 item (3) — `do` never says "applied" for a queued action.

THE BRIDGE WAS THE HONEST PARTY. `do_send_interaction`'s enqueue path replies
(action_handlers.cpp:531-535):

    success: true   status: "queued"   method: "intent_dispatch"

which states exactly what happened: accepted INTO THE QUEUE, not executed. The
server's dispatch tail checked `status in ("dispatched_pending",
"sent_no_confirmation")` — "queued" is in neither — so `success: true` fell
through to `verdict="applied"`. The toolchain designer measured the consequence:
449 deferred REJECTIONS against 203 successes, arriving after the seat had been
told its action applied.

TWO DEFECTS, and the second is the one that would have outlived a narrow fix:

  1. the pending set was incomplete — "queued" and "pending" both mean later
  2. `raw.get("success", True)` DEFAULTED TO TRUE, so a reply carrying no
     `success` key at all was treated as a success. The missing state fell into
     the good bucket, which is a claim and not a reading.

SCOPE, DISCLOSED: this is the evidenced `do` dispatch tail, NOT a surface-wide
three-verdict contract. `mint_title` and `muster` already confirm by readback
and were left alone; `declare_war` has its own verdicts.
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")


class Gateway:
    """Replies with exactly the payload under test."""

    def __init__(self, reply):
        self._reply = reply
        self.health = {"ok": True}

    async def multi_actor(self):
        return False

    async def read(self, action, params=None):
        if action == "query_character":
            return {"character": {"id": 30853, "alive": True, "is_player": False,
                                  "name": "seat"}}
        return {}

    async def read_many(self, specs):
        return [{} for _ in specs]

    async def write(self, action, params=None):
        return self._reply

    def game(self):
        return {"tick": 1, "game_date": "1066.09.15"}


def _do(reply, intent="interact", **a):
    gw = Gateway(reply)
    args = {"interaction_type": "gift_interaction", "target_id": 32643}
    args.update(a)
    return asyncio.run(redesign.do_impl(gw, intent=intent, character_id=30853,
                                        args=args))


def _verdict(env):
    return (env or {}).get("verdict")


def test_THE_REAL_BRIDGE_ENQUEUE_REPLY_is_PENDING_not_applied():
    """The exact payload action_handlers.cpp:531-535 emits."""
    env = _do({"success": True, "status": "queued", "action": "send_interaction",
               "interaction_type": "gift_interaction", "method": "intent_dispatch",
               "intent_request_id": "si_30853_658_x"})
    assert _verdict(env) == "pending", (
        f"a QUEUED interaction reported {_verdict(env)!r}; the bridge said "
        "status=queued and the seat must not be told it applied")
    p = (env or {}).get("pending") or {}
    assert p.get("poll_token") == "si_30853_658_x", (
        "the intent_request_id is the token a seat needs; request_id alone was "
        "empty on this reply shape")
    assert "queued" in str(p.get("why_pending", "")), "say WHY it is pending"


@pytest.mark.parametrize("status", ["queued", "pending", "dispatched_pending",
                                    "sent_no_confirmation"])
def test_EVERY_later_status_is_pending(status):
    env = _do({"success": True, "status": status})
    assert _verdict(env) == "pending", f"status={status!r} read as {_verdict(env)!r}"


def test_a_reply_with_NO_success_FIELD_is_pending_not_applied():
    """`raw.get("success", True)` made the absent state a success. An absent
    `success` is UNCONFIRMED, and unconfirmed is not applied."""
    env = _do({"action": "send_interaction"})
    assert _verdict(env) == "pending"
    assert "no `success` field" in str(((env or {}).get("pending") or {}).get("why_pending", ""))


def test_an_EXPLICIT_success_with_no_later_status_IS_still_applied():
    """The fix must not turn every write into a pending. A bridge that says
    success and does NOT say queued has executed it."""
    env = _do({"success": True, "action": "send_interaction"})
    assert _verdict(env) == "applied"


def test_an_EXPLICIT_failure_is_still_rejected():
    env = _do({"success": False, "reason": "target_required"})
    assert _verdict(env) == "rejected"
    assert (env or {}).get("ok") is False
