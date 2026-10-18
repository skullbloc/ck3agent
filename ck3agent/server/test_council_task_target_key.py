"""ck-z3kh — the council-task target must be sent under the key the BRIDGE reads.

`do_set_council_task` gates on `json_get_int(params_json, "target_title_id") <= 0`
and answers `target_required`. It never reads `title_id`. This surface used to
send `title_id`, so the target was dropped before it left the agent process and
NO agent-side spelling could work: in the deploy-43 confirming smoke, five seats
made twenty-five attempts and not one succeeded — 23 `target_required` and 2
`task_not_found` (the second pair being a lord's own nonsense-task control,
which established that the task name resolves and the failure comes after it).

WHY THIS IS A CONTRACT TEST AND NOT A ROUND TRIP. The wire key is only correct
relative to a consumer in another language, in another process, that this suite
cannot call. So the rows below assert the builder's output against the key read
out of `bridge/src/action_handlers.cpp` itself. Asserting the builder agrees
with a literal I also wrote here would have passed just as happily before the
fix (one definition, two readers: the bridge is the definition).

THE FIX THAT WOULD HAVE LOOKED RIGHT AND DONE NOTHING: the board's first
diagnosis was "the enumerator prints the wrong key", `target_id` where
`target_title_id` was wanted. Its own author retracted it. Changing the printed
key would have changed nothing, because the builder mapped BOTH spellings into
`title_id` — so a lane keyed on the printed string would have gone green over a
lever that was still dead. Hence the last row here, which asserts the printed
call's key is one the builder actually reads.
"""
import os
import re
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")

HANDLERS = os.path.join(os.path.dirname(__file__), "..", "..",
                        "bridge", "src", "action_handlers.cpp")


def _build(args, actor_id=35395):
    p = redesign._intent_to_act_params("council_task", args, actor_id)
    # Name the defect rather than raising KeyError out of every row below: an
    # absent key is exactly the pre-fix state, and it should read as a sentence.
    assert "target_title_id" in p, (
        "the wire carries no target_title_id at all — this is the ck-z3kh defect: "
        f"the builder sent {sorted(p)} and do_set_council_task gates on "
        "target_title_id <= 0")
    return p


def _handler_segment():
    """do_set_council_task's own text, bounded at the NEXT handler.

    A fixed-size window runs into the neighbouring function and credits it with
    reads it never makes — measured while sweeping this class, where a 9,000-byte
    window turned one real defect into four.
    """
    src = open(HANDLERS, encoding="utf-8", errors="replace").read()
    starts = [(m.start(), m.group(1))
              for m in re.finditer(r"\nbool (do_[a-z_0-9]+)\(", src)]
    for i, (pos, name) in enumerate(starts):
        if name == "do_set_council_task":
            end = starts[i + 1][0] if i + 1 < len(starts) else len(src)
            return src[pos:end]
    pytest.fail("do_set_council_task is not in action_handlers.cpp — this test "
                "cannot state which key the consumer reads, and a silent pass "
                "here is how the defect shipped")


def test_the_consumer_reads_target_title_id_and_gates_on_it():
    """The bridge is the definition. Read it, do not restate it."""
    seg = _handler_segment()
    assert 'json_get_int(params_json, "target_title_id")' in seg, (
        "do_set_council_task no longer reads target_title_id — the key this "
        "surface sends has moved and the rows below are now asserting the "
        "wrong contract")
    assert re.search(r"target_title_id\s*<=\s*0", seg), (
        "the target_required gate no longer keys on target_title_id <= 0")
    assert 'json_get_int(params_json, "title_id")' not in seg, (
        "the handler has started reading title_id too, so this test no longer "
        "discriminates the defect it exists for")


def test_the_exact_name_is_sent_on_the_wire():
    p = _build({"task": "task_fabricate_claim", "target_title_id": 227})
    assert p["target_title_id"] == 227
    assert "title_id" not in p, (
        "title_id is back on the wire; the handler does not read it")


def test_the_ENUMERATORS_OWN_KEY_also_reaches_the_wire():
    """`do(action_class="council_tasks")` prints `target_id` in its do_call rows.

    A printed call whose key the executor ignores is the defect the board spent a
    year on, so the row the tool hands back must work verbatim.
    """
    p = _build({"seat": "chancellor", "task": "task_fabricate_claim",
                "target_id": 227})
    assert p["target_title_id"] == 227


def test_the_exact_name_wins_when_both_are_given():
    p = _build({"task": "task_fabricate_claim",
                "target_title_id": 231, "target_id": 227})
    assert p["target_title_id"] == 231


def test_target_character_id_is_forwarded_not_dropped():
    seg = _handler_segment()
    assert 'json_get_int(params_json, "target_character_id")' in seg
    p = _build({"task": "task_fabricate_claim", "target_character_id": 6621})
    assert p["target_character_id"] == 6621


def test_a_missing_target_sends_zero_rather_than_omitting_the_key():
    """0 is what the handler's `<= 0` gate expects; an absent key reads as 0 too,
    but sending it explicitly keeps the refusal attributable to the AGENT's
    missing argument rather than to this builder dropping it again."""
    p = _build({"task": "task_increase_control"})
    assert p["target_title_id"] == 0


def test_the_printed_do_call_uses_a_key_the_builder_reads():
    """The enumerator and the executor, checked against each other.

    This is the row that would have caught the original defect from either side:
    the printed key must be one the builder consumes, whichever name it is.
    """
    src = open(os.path.join(os.path.dirname(__file__), "redesign.py"),
               encoding="utf-8").read()
    i = src.index('elif action_class == "council_tasks":')
    row = src[i:i + 1200]
    printed = set(re.findall(r'"(target_[a-z_]+|title_id)":', row))
    assert printed, "the council_tasks enumerator no longer prints a target key"
    for key in printed:
        p = _build({"task": "task_fabricate_claim", key: 227})
        assert p["target_title_id"] == 227, (
            f"the enumerator prints {key!r} in its do_call, and the builder does "
            f"not turn it into a target — a row the tool hands back that its own "
            f"executor ignores")
