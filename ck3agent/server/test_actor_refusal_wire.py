"""The bridge's typed no_actor refusal reaches the seat as the gate it means.

With bridge/include/actor_resolve.h, a seat (episode_seat) that holds
no ruler and asks query_character with character_id=0 is no longer answered about
the global primary (the first claimer's ruler); the dispatch refuses it and the
reply is the exact string below (bridge/test/test_actor_resolve.cpp row b11 pins
the bridge half on the same string). Before it, the same call returned ANOTHER
seat's ruler with ok=true, so unbound_actor_block returned None and the seat was
told it was that ruler.

The server half: that reply still classifies as no_ruler_bound (a declined read
with no error code), so a seat holding nothing is told to bind one, and the two
added keys change nothing the classifier reads.
"""
import redesign as R


def test_the_typed_refusal_is_no_ruler_bound():
    # bridge/test/test_actor_resolve.cpp b11, verbatim as a dict.
    blk = R.unbound_actor_block({"success": False, "action": "query_character",
                                 "actor_refused": "no_actor",
                                 "actor_rule": "seat_read_requires_actor"})
    assert blk is not None
    assert blk["gate"] == "no_ruler_bound", blk


def test_the_pre_fix_answer_was_another_rulers_profile():
    """Control that RUNS: the superseded reply (a profile of the global primary)
    is accepted as this seat's ruler, which is the hazard the bridge refuses."""
    assert R.unbound_actor_block({"id": 7001, "alive": True}) is None


# ---- A write naming its actor under a key its verb does not read ----
#: bridge/test/test_actor_resolve.cpp b13, verbatim (the dispatch's reply to
#: set_council_task {"character_id": <own ruler>} with no actor_id).
ACTOR_KEY_REPLY = {
    "success": False, "action": "set_council_task", "reason": "actor_key_not_read",
    "actor_refused": "actor_key_not_read", "actor_rule": "write_names_actor_under_unread_key",
    "actor_key_read": "actor_id", "actor_key_got": "character_id",
}


def test_the_unread_actor_key_refusal_is_a_typed_invalid_argument():
    blk = R.write_blocked(ACTOR_KEY_REPLY["reason"], raw=ACTOR_KEY_REPLY)
    assert blk["gate"] == "invalid_argument", blk
    assert blk.get("raw_reason") == "actor_key_not_read", blk


def test_the_unread_actor_key_refusal_is_not_unmapped_control():
    """Control that RUNS (non-discriminating: green on the base too): without the
    map row the same reply would be unmapped_refusal
    (the substring pass finds no code inside actor_key_not_read)."""
    table = dict(R.RAW_REASON_TO_GATE)
    try:
        R.RAW_REASON_TO_GATE.pop("actor_key_not_read", None)
        assert R.map_raw_reason("actor_key_not_read")["gate"] == "unmapped_refusal"
    finally:
        R.RAW_REASON_TO_GATE.clear()
        R.RAW_REASON_TO_GATE.update(table)
