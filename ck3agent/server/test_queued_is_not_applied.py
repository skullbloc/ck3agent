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
three-verdict contract. `mint_title` already confirms by readback
and was left alone; `declare_war` has its own verdicts.
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

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        if action == "query_character":
            return {"id": 30853, "alive": True,
                                  "name": "seat"}
        return {}

    async def read_many(self, specs):
        return [{} for _ in specs]

    async def write(self, action, params=None):
        # Lane v4act: this write goes through the script layer first; these rows
        # grade the path it falls back to (a character with no title to name him by).
        if action == "script_action":
            return {"success": False, "action": "script_action", "reason": "target_not_script_addressable", "detail": "target_id"}
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
    assert p.get("intent_id") == "si_30853_658_x" and "poll_token" not in p, (
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


# ── L13 amendment 3: "later" is the NEXT PASS, and the server reads it ────────

class DrainGateway(Gateway):
    """The bridge drains the queued intent on the next pass, paused or running:
    query_dispatch_result answers `rows` in order (the last one repeats)."""

    def __init__(self, reply, rows, purses=None):
        super().__init__(reply)
        self.rows = list(rows)
        self.dispatch_reads = 0
        #: [(sender gold, recipient gold, game_date) before the drain is read,
        #:  (...) after it]; None: no gold field
        self.purses = list(purses or [])

    async def read(self, action, params=None):
        if action == "query_character" and self.purses:
            snd, rcp, date = self.purses[0] if self.dispatch_reads == 0 else self.purses[-1]
            cid = int((params or {}).get("character_id") or 30853)
            return {"game_date": date,
                    "id": cid, "alive": True, "name": "c",
                                  "gold": snd if cid == 30853 else rcp}
        if action == "query_dispatch_result":
            self.dispatch_reads += 1
            row = self.rows.pop(0) if len(self.rows) > 1 else self.rows[0]
            return dict(row, dispatch_id=(params or {}).get("dispatch_id"))
        return await super().read(action, params)


QUEUED = {"success": True, "status": "queued", "action": "send_interaction",
          "interaction_type": "gift_interaction", "method": "intent_dispatch",
          "intent_request_id": "si_30853_658_x"}


def _drain_do(rows, monkeypatch, interaction="befriend_interaction", purses=None):
    """A queued interact whose drain answers `rows`. befriend has no readback in
    LEVERS (treated as days); gift_interaction reads the sender's gold."""
    async def instant(_s=0):
        return None
    monkeypatch.setattr(redesign, "_SLEEP", instant)
    gw = DrainGateway(dict(QUEUED, interaction_type=interaction), rows, purses)
    env = asyncio.run(redesign.do_impl(gw, intent="interact", character_id=30853,
                                       args={"interaction_type": interaction,
                                             "target_id": 32643}))
    return gw, env


def test_a_queued_write_the_drain_RAN_is_SENT_and_pending_never_applied(monkeypatch):
    """FF2 ruling (witness R2): `applied` needs an effect the server read back.
    The drain's "success" says it was SENT; LEVERS has no readback for befriend,
    so it is pending until the clock advances, result.sent true, with what to
    check. The round-1 server answered applied on the bare dispatch success."""
    gw, env = _drain_do([{"status": "pending"}, {"status": "success"}], monkeypatch)
    assert _verdict(env) == "pending", env
    p = env["pending"]
    assert p["drains_when"] == "clock_advances" and p["intent_id"] == "si_30853_658_x", p
    assert "befriend_interaction" in p["expected_change"] and "32643" in p["expected_change"], p
    assert env["result"]["sent"] is True and env["result"]["drain"]["status"] == "success"
    assert gw.dispatch_reads == 2


def test_NOT_FOUND_is_not_a_refusal_the_drain_had_not_stored_it_yet(monkeypatch):
    """Witness R3: the drain marks an intent consumed while it runs it and stores
    it in the completed ring later in the same pass; a read in between answers
    not_found. Not found twice, then success: the write ran (sent, pending its
    effect). The round-1 server took not_found as terminal and answered rejected
    dispatch_not_found for a write that ran."""
    gw, env = _drain_do([{"status": "not_found"}, {"status": "not_found"},
                         {"status": "success"}], monkeypatch)
    assert _verdict(env) == "pending", env
    assert env["pending"]["drains_when"] == "clock_advances" and env["result"]["sent"] is True
    assert gw.dispatch_reads == 3


def test_NOT_FOUND_to_the_end_of_the_schedule_is_next_pass_not_rejected(monkeypatch):
    gw, env = _drain_do([{"status": "not_found"}], monkeypatch)
    assert _verdict(env) == "pending", env
    assert env["pending"]["drains_when"] == "next_pass", env["pending"]
    assert env["result"]["sent"] is False
    assert gw.dispatch_reads == 1 + len(redesign.DRAIN_SCHEDULE_S)


def test_a_queued_write_the_drain_REFUSED_is_rejected_with_its_reason(monkeypatch):
    gw, env = _drain_do([{"status": "rejected", "rejection_reason": "target_would_reject"}],
                        monkeypatch)
    assert _verdict(env) == "rejected", env
    assert env["blocked"]["raw_reason"] == "target_would_reject", env["blocked"]


def test_a_queued_write_whose_verdict_is_never_read_is_pending_NEXT_PASS(monkeypatch):
    gw, env = _drain_do([{"status": "pending"}], monkeypatch)
    assert _verdict(env) == "pending"
    p = env["pending"]
    assert p["drains_when"] == "next_pass" and p["intent_id"] == "si_30853_658_x", p
    assert gw.dispatch_reads == 1 + len(redesign.DRAIN_SCHEDULE_S)


# ── FF3 ruling (witness X2): a stage-1 lever is applied once its effect is read ─

D = "0867.03.01"


def test_a_GIFT_whose_exact_amount_moved_on_the_pass_is_APPLIED(monkeypatch):
    """gift_interaction is an "on_execute" lever: the drain ran it, and the
    sender's purse fell by exactly what the recipient's rose, with no game day
    between the readings: applied, with the amount. The do() tail takes no clock
    reading for it, so the answer is the same paused or running. FF2 answered
    pending clock_advances."""
    gw, env = _drain_do([{"status": "success"}], monkeypatch, interaction="gift_interaction",
                        purses=[(100.0, 10.0, D), (60.0, 50.0, D)])
    assert _verdict(env) == "applied", env
    rb = env["result"]["readback"]
    assert rb["amount"] == 40.0 and rb["gold_before"] == 100.0 and rb["gold_after"] == 60.0, rb


def test_a_GIFT_whose_gold_did_not_move_after_the_pass_is_UNCONFIRMED(monkeypatch):
    gw, env = _drain_do([{"status": "success"}], monkeypatch, interaction="gift_interaction",
                        purses=[(100.0, 10.0, D)])
    assert _verdict(env) == "rejected", env
    assert env["blocked"]["gate"] == "write_unconfirmed", env["blocked"]
    assert env["result"]["sent"] is True


def test_the_DRAIN_REFUSAL_outranks_a_readback_that_would_resolve(monkeypatch):
    """Witness r4 m1: settle_write ran the readback before it looked at the
    drain's refusal, so a refused gift whose purses moved anyway (another
    transfer the same pass) read applied. The refusal now comes first."""
    gw, env = _drain_do([{"status": "rejected", "rejection_reason": "target_would_reject"}],
                        monkeypatch, interaction="gift_interaction",
                        purses=[(100.0, 10.0, D), (60.0, 50.0, D)])
    assert _verdict(env) == "rejected", env
    assert env["blocked"]["raw_reason"] == "target_would_reject", env["blocked"]


def test_a_GIFT_reading_across_a_daily_tick_is_PENDING_READBACK_never_rejected(monkeypatch):
    """Witness r4 m1 and r5 G1 (FF5 ruling). Running, a daily tick between the
    readings pays income or takes an expense, so the transfer cannot be told
    from it: the reading is not made (never applied on a wrong amount), and a
    gift the drain RAN is never rejected for a reading the server could not
    make: pending, drains_when "readback", ok:true, result.sent, raw_reason
    write_unconfirmed. FF4 answered ok:false rejected write_unconfirmed, which the
    player could read as "not sent" and gift twice."""
    gw, env = _drain_do([{"status": "success"}], monkeypatch, interaction="gift_interaction",
                        purses=[(100.0, 10.0, D), (60.0 + 7.0, 50.0, "0867.03.02")])
    assert env["ok"] is True and _verdict(env) == "pending", env
    p = env["pending"]
    assert p["drains_when"] == "readback" and p["raw_reason"] == "write_unconfirmed", p
    assert "gift_interaction" in p["expected_change"], p
    assert env["result"]["sent"] is True


def test_a_GIFT_whose_clean_reading_shows_another_amount_is_not_applied(monkeypatch):
    """Both purses read on one game date and the recipient did not get what the
    sender paid: the reading was made and does not show the gift: rejected
    write_unconfirmed (the server read it), never applied."""
    gw, env = _drain_do([{"status": "success"}], monkeypatch, interaction="gift_interaction",
                        purses=[(100.0, 10.0, D), (55.0, 50.0, D)])
    assert _verdict(env) == "rejected", env
    assert env["blocked"]["gate"] == "write_unconfirmed", env["blocked"]


def test_EXECUTOR_RETURNED_NO_VERDICT_is_a_write_that_ran_not_a_refusal(monkeypatch):
    """Witness r5 n5: the drain records `rejected: executor_returned_no_verdict`
    when an executor ran and set no verdict (intent_queue.cpp finish). The write
    may have landed, so it is pending readback with result.sent, ok:true, never a
    refusal the player would answer by sending it again."""
    gw, env = _drain_do([{"status": "rejected", "rejection_reason": "executor_returned_no_verdict"}],
                        monkeypatch)
    assert env["ok"] is True and _verdict(env) == "pending", env
    assert env["pending"]["drains_when"] == "readback", env["pending"]
    assert env["result"]["sent"] is True


def test_a_CRASHED_drain_stays_a_refusal_and_says_the_effect_may_have_landed(monkeypatch):
    gw, env = _drain_do([{"status": "crashed", "rejection_reason": "game_thread_crash_during_drain"}],
                        monkeypatch)
    assert _verdict(env) == "rejected", env
    assert "MAY have landed" in env["blocked"]["summary"], env["blocked"]
    assert "before sending again" in env["blocked"]["summary"]


def test_every_do_tail_lever_with_a_readback_has_one():
    """LEVERS names the readback; LEVER_READBACKS implements it for the do()
    tail (the war intents and mint_title read back in their own paths)."""
    own = set(redesign.WAR_INTENTS) | {"mint_title", "faction", "scheme", "steer"}
    named = {k for k, r in redesign.LEVERS.items() if r.get("readback") and k[0] not in own}
    assert named == set(redesign.LEVER_READBACKS), (named, set(redesign.LEVER_READBACKS))
    for k, r in redesign.LEVERS.items():
        assert r["shows"] in ("on_execute", "days") and r.get("evidence"), k
        if r["shows"] == "on_execute":
            assert r.get("readback") and r.get("absent") in ("declined", "unconfirmed"), k
