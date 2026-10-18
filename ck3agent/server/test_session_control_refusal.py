"""ck-switch / Ruling Three — a refused control call must SAY it was refused.

The old branch inspected the bridge's reply for exactly one condition,
`cannot_act_as_player`, and never read `success`. So every other refusal — the
character already held by another seat, the watchlist at its cap, the character
dead — came back ok=True, verdict "applied", and an actor echo naming a
character the agent DOES NOT HOLD. An agent reading that believes it changed
seats. The ownership samples then disagree with the agent's own idea of itself,
and the episode's provenance has to reconcile two stories where there should be
one.

Three things are asserted here:

  1. every refusal returns ok=False with its OWN typed reason and NO actor echo;
  2. a reason outside the nine is `refusal_reason_unknown` — never the nearest
     member of the nine, because a code the client does not know is a fact about
     the client, not about the seat;
  3. `success` is three-valued and absence is not success: an old bridge that
     says nothing gets an explicit UNWITNESSED warning rather than a confident
     echo.

The nine come from the node seat's enumeration of deploy-27's source, not from a
grep: `ownership_divergence` is a ternary at dispatch_router.cpp:807-808 rather
than a string literal, so a literal search does not find it and a COUNT does not
notice it missing.
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")

THE_NINE = {
    "no_session", "invalid_character", "observe_mode", "owned_by_other",
    "watchlist_full", "character_not_found", "character_dead",
    "cannot_act_as_player", "ownership_divergence",
}


class _Gw:
    """Models the gw interface session_impl actually uses, and nothing else."""

    def __init__(self, write_reply):
        self._write_reply = write_reply
        self.health = "ok"
        self.reads = []

    async def write(self, action, params):
        self.last_write = (action, params)
        return self._write_reply

    async def read(self, action, params):
        self.reads.append(action)
        if action == "query_character":
            return {"character_id": params.get("character_id"), "name": "Someone"}
        return {}

    def game(self):
        return {"tick": 1, "date": "1066.09.15", "paused": True, "speed": 3}


def _control(reply, **kw):
    gw = _Gw(reply)
    out = asyncio.run(redesign.session_impl(gw, "control", character_id=30853, **kw))
    return gw, out


# ── 1. the reader, three-valued ──────────────────────────────────────────────

@pytest.mark.parametrize("reason", sorted(THE_NINE))
def test_every_one_of_the_nine_is_read_as_a_refusal(reason):
    assert redesign.control_refusal_reason(
        {"success": False, "reason": reason}) == reason


def test_the_player_gate_puts_its_code_under_ERROR_and_prose_under_reason():
    """reject_player_actor (dispatch_router.cpp:238) is the odd one on the wire:
    the CODE is under `error` and `reason` carries a sentence. Reading `reason`
    alone classifies this as unknown — which is exactly what piece 7's LEDGER
    consumer must do, because no ledger row has an `error` key at all. Two
    surfaces, two rules, on purpose."""
    reply = {"success": False, "error": "cannot_act_as_player",
             "reason": "the player character is target-only and cannot be controlled"}
    assert redesign.control_refusal_reason(reply) == "cannot_act_as_player"


def test_a_tenth_spelling_is_UNKNOWN_and_never_the_nearest_of_the_nine():
    assert redesign.control_refusal_reason(
        {"success": False, "reason": "watchlist_nearly_full"}) == "refusal_reason_unknown"
    assert redesign.control_refusal_reason(
        {"success": False}) == "refusal_reason_unknown"


def test_success_TRUE_is_the_only_witnessed_success():
    assert redesign.control_refusal_reason({"success": True}) is None
    # Absent is NOT success — but with no reason either, it is not a refusal we
    # can name, so the caller is told the outcome was unwitnessed instead.
    assert redesign.control_refusal_reason({}) is None
    # ...while a recognised code still refuses, `success` or no `success`.
    assert redesign.control_refusal_reason(
        {"reason": "owned_by_other"}) == "owned_by_other"


# ── 2. the branch ────────────────────────────────────────────────────────────

@pytest.mark.parametrize("reason", sorted(THE_NINE))
def test_a_refusal_returns_ok_FALSE_with_no_actor_echo(reason):
    gw, out = _control({"success": False, "reason": reason})
    assert out["ok"] is False, out
    assert out["blocked"]["gate"] == reason, out["blocked"]
    assert out["blocked"]["raw_reason"] == reason
    # THE POINT: no character is echoed, and the character was never even read.
    assert "result" not in out or not (out.get("result") or {}).get("controlled")
    assert "query_character" not in gw.reads


def test_the_refused_gate_is_never_the_UNKNOWN_bucket():
    """Without a GATE_TABLE entry, why_blocked falls back to `unknown`, whose
    severity is "blocked" and whose retryable is TRUE — so a seat refused
    because another agent HOLDS the character would be told to try again
    forever. Each of the nine carries its own answer to "should I retry"."""
    for reason in THE_NINE | {"refusal_reason_unknown"}:
        blocked = redesign.why_blocked(reason)
        assert blocked["gate"] == reason
        assert blocked["class"] != "unknown", reason
    # and the one that genuinely IS retryable is marked so, alone among them
    assert redesign.why_blocked("watchlist_full")["retryable"] is True
    for reason in THE_NINE - {"watchlist_full", "no_session"}:
        assert redesign.why_blocked(reason)["retryable"] is False, reason


def test_a_WITNESSED_success_applies_and_says_it_was_witnessed():
    gw, out = _control({"success": True, "controlled_primary": 30853})
    assert out.get("ok", True) is True
    assert out["verdict"] == "applied"
    assert out["result"]["control_witnessed"] is True
    assert out["result"]["controlled"]
    assert not out.get("warnings")


def test_an_UNWITNESSED_success_still_applies_but_warns_in_so_many_words():
    """An old bridge that reports no `success` may well have worked. "Probably"
    is not a witness, and a seat that believes it holds a character it does not
    is the failure this whole piece is about — so the echo is labelled as the
    REQUEST rather than silently presented as the outcome."""
    gw, out = _control({"status": "sent"})
    assert out["verdict"] == "applied"
    assert out["result"]["control_witnessed"] is False
    assert any("UNWITNESSED" in w for w in (out.get("warnings") or [])), out.get("warnings")


# ── 3. the enumeration itself ────────────────────────────────────────────────

def test_the_nine_are_exactly_nine_and_exactly_these():
    """Pinned by hand against the node seat's enumeration, because the source
    cannot be grepped for them: `ownership_divergence` is a ternary, not a
    literal. ck-760i's own list had nine entries too — and double-counted
    `observe_mode` (reachable from both gates under one spelling) while missing
    `ownership_divergence`, so a COUNT agreed while the SET did not."""
    assert redesign.CONTROL_REFUSAL_REASONS == THE_NINE
    assert len(redesign.CONTROL_REFUSAL_REASONS) == 9
    # every code an agent can be handed carries a sentence telling it what to do
    for reason in THE_NINE | {"refusal_reason_unknown"}:
        assert redesign.CONTROL_REFUSAL_SUMMARIES.get(reason), reason


# ── 4. the echo is what the seat HOLDS, not what it asked for ────────────────

class _SeatGw(_Gw):
    """A gateway whose get_bridge_state names what each seat holds, so the echo
    can be checked against a seat rather than against the request."""

    def __init__(self, write_reply, sessions, sid="me"):
        super().__init__(write_reply)
        self._sessions = sessions
        self.state = type("S", (), {"_client": type("C", (), {"session_id": sid})()})()

    async def read(self, action, params):
        self.reads.append(action)
        if action == "get_bridge_state":
            return {"controlled_primary": 11111, "sessions": self._sessions}
        if action == "query_character":
            return {"character_id": params.get("character_id"), "name": "Someone"}
        return {}


def test_the_echo_names_WHAT_THE_SEAT_HOLDS_when_it_differs_from_the_request():
    """THE 2,578 RETRIES. A call that is not refused, on a seat that ends up
    holding somebody else, used to echo the REQUESTED character: the agent read
    back the name it had just sent, saw it was not steering that character, and
    called again. The discrepancy has to be in the reply, not inferred three
    retries later."""
    gw = _SeatGw({"success": True, "controlled_primary": 99999}, sessions=[])
    out = asyncio.run(redesign.session_impl(gw, "control", character_id=30853))
    assert out["verdict"] == "applied"
    assert out["result"]["controlled"]["id"] == 99999
    assert out["result"]["requested_character_id"] == 30853
    assert out["result"]["controlled_is_held"] is True
    assert any("NOT the requested 30853" in w for w in out["warnings"]), out["warnings"]


def test_the_reply_is_the_first_authority_and_no_second_read_is_made():
    gw = _SeatGw({"success": True, "controlled_primary": 30853}, sessions=[])
    out = asyncio.run(redesign.session_impl(gw, "control", character_id=30853))
    assert out["result"]["controlled"]["id"] == 30853
    assert "get_bridge_state" not in gw.reads, (
        "the reply already named the held character; asking again is a second "
        "round trip for an answer already in hand")
    assert not out.get("warnings")


def test_a_reply_without_the_field_asks_THIS_SEAT_not_the_global_primary():
    """The fallback keys on session_id. `controlled_primary` at the top of
    get_bridge_state is the front of the claim order — the very value whoami was
    wrong to use — so reading it here would reproduce that defect one layer
    down."""
    gw = _SeatGw({"success": True},
                 sessions=[{"session_id": "other", "primary_character_id": 11111},
                           {"session_id": "me", "primary_character_id": 55555}])
    out = asyncio.run(redesign.session_impl(gw, "control", character_id=55555))
    assert "get_bridge_state" in gw.reads
    assert out["result"]["controlled"]["id"] == 55555
    assert out["result"]["controlled_is_held"] is True
    assert not out.get("warnings")


def test_an_UNDETERMINABLE_holding_is_labelled_as_the_request_not_presented_as_fact():
    gw = _SeatGw({"success": True}, sessions=[])          # no row for this seat
    out = asyncio.run(redesign.session_impl(gw, "control", character_id=30853))
    assert out["result"]["controlled_is_held"] is False
    assert out["result"]["controlled"]["id"] == 30853
    assert any("has not been confirmed against the seat" in w
               for w in out["warnings"]), out["warnings"]


# ── 5. release: the sibling branch, found by auditing every write call site ──

THE_FOUR = {"missing_character_id", "not_in_roster", "not_owned_by_session",
            "observe_mode"}

# ck-cwal added a FIFTH, and this file's exact-set assertion is what caught it
# — which is the assertion doing its job, not a reason to loosen it to `>=`.
# THE_FOUR stays spelled as the four because it names what the per-call-site
# audit found; the new member is listed apart so a later reader can still see
# which reasons that audit covered and which arrived by a different route.
THE_FIFTH = {"last_claim_of_control_session"}

#: Every reason a RELEASE can be refused for. The behavioural rows below are
#: parametrized over this, not over THE_FOUR: a fifth reason that never gets
#: driven through `session_impl` is a fifth reason nothing checks reports
#: refused-rather-than-applied.
RELEASE_REASONS = THE_FOUR | THE_FIFTH


@pytest.mark.parametrize("reason", sorted(RELEASE_REASONS))
def test_a_refused_RELEASE_says_so_instead_of_reporting_applied(reason):
    """THE SIBLING OF THE CONTROL DEFECT, one branch below it, and I fixed the
    first without looking at the second until a per-call-site audit of every
    bridge write in this module named it.

    The old branch discarded the reply outright — not even assigned — and
    answered verdict "applied" with `released: <id>` unconditionally. A refused
    release read exactly like a successful one, so the seat went on holding the
    character while the agent believed it had let go.

    It is load-bearing for the experiment and not only for the agent:
    `seat_vacated` invalidates an episode whose control seat was seated and
    later sat empty, so whether a seat really released is a question the
    evidence asks."""
    gw = _Gw({"success": False, "reason": reason})
    out = asyncio.run(redesign.session_impl(gw, "release", character_id=30853))
    assert out["ok"] is False, out
    assert out["blocked"]["gate"] == reason
    assert out["blocked"]["raw_reason"] == reason
    assert not (out.get("result") or {}).get("released")


def test_release_has_its_OWN_vocabulary_not_controls():
    """Only `observe_mode` is shared with control. A release is not a control
    call with the sign flipped, and reusing the nine here would claim the
    bridge can refuse a release for reasons it cannot.

    The set is pinned EXACTLY, so adding a member reds this row on purpose:
    the point is that each addition is a deliberate widening of what this
    client will accept from the bridge, reviewed once rather than absorbed.
    ck-cwal's `last_claim_of_control_session` is the first such widening.
    """
    assert redesign.RELEASE_REFUSAL_REASONS == RELEASE_REASONS
    assert RELEASE_REASONS & redesign.CONTROL_REFUSAL_REASONS == {"observe_mode"}
    assert THE_FIFTH & redesign.CONTROL_REFUSAL_REASONS == set()
    for reason in RELEASE_REASONS:
        assert redesign.RELEASE_REFUSAL_SUMMARIES.get(reason), reason
        assert redesign.why_blocked(reason)["class"] != "unknown", reason
        # THE FLAG THE AGENT ACTS ON, and the one that caught ck-cwal: a
        # summary can be perfect while the reason has no GATE_TABLE row, and
        # the fallback says retryable TRUE. Not one of the five is won by
        # trying again, so a True here is a loop, not a retry.
        assert redesign.why_blocked(reason)["retryable"] is False, reason


def test_a_fifth_release_spelling_is_UNFORESEEN_not_the_nearest_of_the_four():
    gw = _Gw({"success": False, "reason": "not_in_roster_yet"})
    out = asyncio.run(redesign.session_impl(gw, "release", character_id=30853))
    assert out["ok"] is False
    assert out["blocked"]["gate"] == "refusal_reason_unknown"
    assert out["blocked"]["raw_reason"] == "not_in_roster_yet"


def test_a_witnessed_release_applies_and_an_unwitnessed_one_warns():
    gw = _Gw({"success": True})
    out = asyncio.run(redesign.session_impl(gw, "release", character_id=30853))
    assert out["verdict"] == "applied"
    assert out["result"]["release_witnessed"] is True
    assert not out.get("warnings")

    gw2 = _Gw({"status": "sent"})                     # an old bridge: no `success`
    out2 = asyncio.run(redesign.session_impl(gw2, "release", character_id=30853))
    assert out2["verdict"] == "applied"
    assert out2["result"]["release_witnessed"] is False
    assert any("may still hold the character" in w for w in out2["warnings"])
