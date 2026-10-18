"""ck-p9vt — observe_v2's reads are issued together, and the acceptance test is unchanged.

HOST ONLY. A counting fake gateway; no bridge, no game.

THE MEASURED COST. `observe_v2` p50 is 50-52 s (about 43 game-days) in every
set. It is the tool seats are told to call first and they route around it:
3,284 `query` calls against 582 `observe_v2`. Where the time was:

  * `_settled_dejure` issues one read then polls up to 6 more, so 1-7 bridge
    reads, and `observe_impl` called it TWICE IN SEQUENCE (k_ireland, then
    e_britannia) -- up to FOURTEEN reads before the main batch began.
  * `get_ai_personality` was a sixth span of its own, 160 lines below, on
    EVERY call. 93% of calls are depth="brief" (539 of 582 measured over
    d45-d50), so it was on the hot path every time.

WHAT IS NOT CLAIMED HERE. These rows prove the calls are ISSUED TOGETHER. They
say nothing about wall-clock: the bridge serves reads through the game thread
and whether concurrency helps is a live question, which deploy-51's first slot
answers. A latency number measured in an asyncio fixture would be a number
about asyncio.

  P1  both goal meters are issued in ONE read_many, not two
  P2  when both settle immediately the whole pair costs ONE span
  P3  a key that settles early is NOT asked again; only the unsettled one is
  P4  THE BEHAVIOUR CONTROL: the pair accepts exactly what the serial version
      accepted -- a positive total, and title_key_not_found as terminal
  P5  the poll stays bounded by max_polls
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

    async def test_P1_P2_both_meters_in_one_batch_when_settled(self):
        gw = _GW({"k_ireland": [{"total": 14, "held": 3}],
                  "e_britannia": [{"total": 93, "held": 40}]})
        out = await R._settled_dejure_pair(gw, 1, ("k_ireland", "e_britannia"))
        self.assertEqual(len(gw.batches), 1, "P2: one span for the settled pair")
        self.assertEqual([p.get("title_key") for _, p in gw.batches[0]],
                         ["k_ireland", "e_britannia"], "P1: both keys, one batch")
        self.assertEqual(gw.singles, [], "no single read escaped the batch")
        self.assertEqual([g["total"] for g in out], [14, 93], "values in key order")

    async def test_P3_only_the_unsettled_key_is_asked_again(self):
        # k_ireland settles at once; e_britannia is 0/0 for one round.
        gw = _GW({"k_ireland": [{"total": 14}],
                  "e_britannia": [{"total": 0}, {"total": 93}]})
        out = await R._settled_dejure_pair(gw, 1, ("k_ireland", "e_britannia"),
                                           interval_s=0)
        self.assertEqual(len(gw.batches), 2, "one re-poll round")
        self.assertEqual([p.get("title_key") for _, p in gw.batches[1]],
                         ["e_britannia"],
                         "P3: the settled key is NOT asked again -- the serial "
                         "version could not do this")
        self.assertEqual([g["total"] for g in out], [14, 93])

    async def test_P4_CONTROL_acceptance_matches_the_serial_version(self):
        """The pair must accept exactly what `_settled_dejure` accepts.

        This is the control that stops the batching changing the VALUE. A
        resolution error is terminal, a positive total is a real value, and a
        0/0 is retried -- preserved from re_observe_meter_0_0.md fix B.
        """
        for reply, expect_polls in (
            ({"error": "title_key_not_found"}, 1),   # terminal, no poll
            ({"total": 7}, 1),                       # real value, no poll
        ):
            gw = _GW({"k_ireland": [reply]})
            await R._settled_dejure_pair(gw, 1, ("k_ireland",), interval_s=0)
            self.assertEqual(len(gw.batches), expect_polls,
                             f"{reply} must not be polled again")
        # and the serial helper agrees on the same replies
        for reply in ({"error": "title_key_not_found"}, {"total": 7}):
            gw2 = _GW({"k_ireland": [reply]})

            async def _read(verb, params, _r=reply):
                gw2.singles.append((verb, params))
                return _r
            gw2.read = _read
            got = await R._settled_dejure(gw2, 1, "k_ireland", interval_s=0)
            self.assertEqual(got, reply, "the serial helper accepts the same")

    async def test_P5_the_poll_stays_bounded(self):
        never = [{"total": 0} for _ in range(50)]
        gw = _GW({"k_ireland": list(never)})
        await R._settled_dejure_pair(gw, 1, ("k_ireland",),
                                     max_polls=3, interval_s=0)
        self.assertLessEqual(len(gw.batches), 4,
                             "first read plus at most max_polls rounds")

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
