"""ck-p9vt — observe_v2's reads are issued together, and the acceptance test is unchanged.

HOST ONLY. A counting fake gateway; no bridge, no game.

THE MEASURED COST. `observe_v2` p50 is 50-52 s (about 43 game-days) in every
set. It is the tool seats are told to call first and they route around it:
3,284 `query` calls against 582 `observe_v2`. Where the time was:

  * `_settled_dejure` issued one read then polled up to 6 more, so 1-7 bridge
    reads, and `observe_impl` called it TWICE IN SEQUENCE (k_ireland, then
    e_britannia) -- up to FOURTEEN reads before the main batch began. (L13
    deleted the poll: `_dejure_pair` is one batch.)
  * `get_ai_personality` was a sixth span of its own, 160 lines below, on
    EVERY call. 93% of calls are depth="brief" (539 of 582 measured over
    d45-d50), so it was on the hot path every time.

WHAT IS NOT CLAIMED HERE. These rows prove the calls are ISSUED TOGETHER. They
say nothing about wall-clock: the bridge serves reads through the game thread
and whether concurrency helps is a live question, which deploy-51's first slot
answers. A latency number measured in an asyncio fixture would be a number
about asyncio.

  P1  both goal meters are issued in ONE read_many, not two
  P2  the whole pair costs ONE span
  P3  a 0/0 reply is reported as read, never re-polled (L13)
  P6  get_ai_personality is in the batch, so `brief` issues no span for it
"""
from __future__ import annotations

import asyncio
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import redesign as R                                             # noqa: E402


class _GW:
    """Counts read_many batches and single reads, and what each asked for."""

    def __init__(self, answers):
        self.answers = answers          # title_key -> list of replies, popped
        self.batches: list[list] = []
        self.singles: list[tuple] = []

    async def read_many(self, specs):
        self.batches.append([s for s in specs])
        out = []
        for verb, params in specs:
            key = params.get("title_key")
            seq = self.answers.get(key)
            out.append(seq.pop(0) if seq else {"total": 1})
        return out

    async def read(self, verb, params):
        self.singles.append((verb, params))
        return {}


class ObserveSpans(unittest.IsolatedAsyncioTestCase):

    async def test_P1_P2_both_meters_in_one_batch(self):
        gw = _GW({"k_ireland": [{"total": 14, "held": 3}],
                  "e_britannia": [{"total": 93, "held": 40}]})
        out = await R._dejure_pair(gw, 1, ("k_ireland", "e_britannia"))
        self.assertEqual(len(gw.batches), 1, "P2: one span for the pair")
        self.assertEqual([p.get("title_key") for _, p in gw.batches[0]],
                         ["k_ireland", "e_britannia"], "P1: both keys, one batch")
        self.assertEqual(gw.singles, [], "no single read escaped the batch")
        self.assertEqual([g["total"] for g in out], [14, 93], "values in key order")

    async def test_P3_a_zero_tree_is_reported_not_re_polled(self):
        """L13: a 0/0 reply is the bridge's answer, read on the game thread
        between ticks and already settled by the bridge's own read; the client
        does not poll it again (the re-poll for re_observe_meter_0_0.md's
        IPC-thread race is deleted)."""
        gw = _GW({"k_ireland": [{"total": 14}],
                  "e_britannia": [{"total": 0}, {"total": 93}]})
        out = await R._dejure_pair(gw, 1, ("k_ireland", "e_britannia"))
        self.assertEqual(len(gw.batches), 1, "no re-poll round")
        self.assertEqual([g["total"] for g in out], [14, 0])
        self.assertFalse(hasattr(R, "_settled_dejure"), "the serial poll is deleted")

    def test_P6_get_ai_personality_is_in_the_batch(self):
        src = (pathlib.Path(__file__).resolve().parent / "redesign.py").read_text(
            encoding="utf-8", errors="replace")
        i = src.index("async def observe_impl")
        body = src[i:i + 9000]
        batch_start = body.index("read_many([")
        batch_end = body.index("])", batch_start)
        in_batch = "get_ai_personality" in body[batch_start:batch_end]
        # and it must no longer have a span of its own on the brief path
        own_read = 'await gw.read("get_ai_personality"' in body
        self.assertTrue(in_batch, "P6: get_ai_personality joins the batch")
        self.assertFalse(own_read, "P6: and has no span of its own")


if __name__ == "__main__":
    unittest.main()
