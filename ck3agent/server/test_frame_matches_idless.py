"""defect: `pipeline_correlation.frame_matches` took an id-less frame as a reply.

Read live (2026-11-19): the first read after a seat's server bound its character
came back as THIS frame, the bind's drain record broadcast to every session:
`caller_request_id` set, no `request_id`, type "result". The classifier's last
rule ("no request_id and not a named push -> match") handed it to the waiting
caller as its answer. The bridge says what such a frame is (ipc_server.cpp,
bridge_init.cpp the drain record): a RECORD, keyed `caller_request_id` precisely
so that no reader pops a future on it. And a pipeline `result` frame for a call
that sent an id always echoes that id; one without it is someone else's.
"""
import pipeline_correlation as pc

#: the frame read live, verbatim (a harness's backend test holds the same FOREIGN_BIND_RESULT)
FOREIGN_BIND_RESULT = {"caller_request_id": "4b175fe1-6f41-4276-adf1-19abe55260b9", "tick": 0,
                       "game_date": "1066.09.15", "flush_id": 0, "action": "session_control",
                       "verb": "set_controlled_character", "success": True, "type": "result"}


def test_a_broadcast_drain_record_is_never_a_reply():
    assert pc.frame_matches(FOREIGN_BIND_RESULT, "my-request") == "skip"


def test_a_drain_record_naming_my_own_id_is_still_a_record_not_the_reply():
    rec = dict(FOREIGN_BIND_RESULT, caller_request_id="my-request")
    assert pc.frame_matches(rec, "my-request") == "skip"


def test_an_idless_result_frame_is_not_the_answer_to_a_call_that_sent_an_id():
    frame = {k: v for k, v in FOREIGN_BIND_RESULT.items() if k != "caller_request_id"}
    assert pc.frame_matches(frame, "my-request") == "skip"


def test_CONTROL_the_reply_and_the_legacy_idless_reply_still_match():
    """The rows the fix must not break: my id matches; a legacy-mode reply (no id,
    no type) to a caller that sent none, or sent one the path did not echo, is
    still taken (the module's documented tolerance)."""
    assert pc.frame_matches({"request_id": "my-request", "type": "result", "gold": 1}, "my-request") == "match"
    assert pc.frame_matches({"gold": 1, "success": True}, "my-request") == "match"
    assert pc.frame_matches({"gold": 1, "success": True}, None) == "match"
    assert pc.frame_matches({"type": "ping"}, None) == "skip"


def test_a_drain_record_is_not_the_reply_of_a_caller_that_sent_no_id():
    """Isolates the caller_request_id rule: the result-type rule cannot see this one
    (the caller sent no id), so each rule has a row only it reds."""
    assert pc.frame_matches(FOREIGN_BIND_RESULT, None) == "skip"
    assert pc.frame_matches({k: v for k, v in FOREIGN_BIND_RESULT.items() if k != "type"}, "my-request") == "skip"
