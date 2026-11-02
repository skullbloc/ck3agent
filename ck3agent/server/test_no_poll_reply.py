"""L14 S4 (audit 3(i)): a reply names the wait that happened.

do_call_war_create_core and do_create_title skip their poll on the game thread
(the drain runs the queued intent after the handler returns). They used to
answer intent_not_completed_within_5s / not_confirmed_within_12s there anyway,
a wait that never happened. On the game thread they now answer status
"queued", reason "no_poll_on_game_thread", drains_when "next_pass"; the timeout
words stay for call_war_create_core's off-thread path (declare_war's
force_player_path forwards to it from the IPC thread), where the poll does run.
create_title has no off-thread path (a game-thread action, no IPC-thread
caller): its poll and its not_confirmed_within_12s reply are deleted (L14 FB
m-5). The server reads the new reason as a verdict not yet read
(confirmation_pending), never a refusal."""
import asyncio
import re
from pathlib import Path

import pytest

import redesign as R

SRC = Path(__file__).resolve().parents[2] / "bridge" / "src" / "action_handlers.cpp"


def _body(sig: str) -> str:
    s = SRC.read_text(encoding="utf-8")
    i = s.index(sig)
    return s[i:s.index("\n}\n", i)]


@pytest.mark.parametrize("sig,timeout_words", [
    ("bool do_call_war_create_core(", "intent_not_completed_within_5s"),
])
def test_on_the_game_thread_the_reply_names_no_wait(sig, timeout_words):
    if not SRC.is_file():
        pytest.skip("no bridge sources in this layout")
    b = _body(sig)
    # The poll is skipped on the game thread ...
    assert re.search(r"for \(int poll = 0; poll < \d+ && !bridge::on_game_thread\(\); poll\+\+\)", b)
    # ... and the not-found branch says so before any timeout words.
    m = re.search(r"\} else if \(bridge::on_game_thread\(\)\) \{(.*?)\} else \{(.*?)\}", b, re.S)
    assert m, "the not-found branch does not distinguish the game thread"
    gt, off = m.group(1), m.group(2)
    assert '"no_poll_on_game_thread"' in gt and '"next_pass"' in gt and '"drains_when"' in gt, gt
    assert timeout_words not in gt
    assert timeout_words in off, "the off-thread path, where the poll ran, keeps its timeout words"


def test_create_title_answers_queued_and_never_polls():
    """m-5: the handler runs only on the game thread, so it neither polls nor
    says a timeout; it answers the queued words."""
    if not SRC.is_file():
        pytest.skip("no bridge sources in this layout")
    b = _body("bool do_create_title(")
    assert not re.search(r"\bint\s+poll\b", b) and "sleep_for" not in b and "completed_snapshot" not in b, b
    assert "not_confirmed_within_12s" not in b and "dispatched_pending" not in b
    for w in ('"queued"', '"no_poll_on_game_thread"', '"next_pass"', '"drains_when"'):
        assert w in b, w


def test_the_server_reads_no_poll_as_a_verdict_not_yet_read():
    g = R.map_raw_reason("no_poll_on_game_thread")
    assert g["gate"] == "confirmation_pending", g


def test_CONTROL_settle_write_keeps_a_queued_no_poll_reply_pending():
    """Not discriminating (green on 378222e6 too: status "queued" is already a
    pending status). A reply with no intent the drain lists (the read answers nothing) is a
    verdict never read: drains_when next_pass, not a refusal."""
    raw = {"success": False, "action": "create_title", "status": "queued",
           "reason": "no_poll_on_game_thread", "drains_when": "next_pass"}

    async def read(action, params=None):
        return {}

    async def clock():
        return {"paused": True}

    async def readback(_raw):
        return None

    conf = asyncio.run(R.settle_write(clock, read, raw, readback, game_date_at_submit="867.1.1",
                                      deadline_s=0.01, max_polls=0, poll_interval_s=0))
    assert conf["refused"] is None and conf["drains_when"] == "next_pass", conf
