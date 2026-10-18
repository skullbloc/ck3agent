"""ck-s8vr — `since` is about the seat, and says what it withheld.

HOST ONLY. Pure shape over a gateway reply; no bridge, no game.

MEASURED over d49+d50: `delta` is 75.5 per cent of observe_v2's BYTES. Of its
5,640 entries, 14 (0.25%) mention the actor and 100% have empty names. Every
one is a Python-repr dict — "war: {'tick': 61, 'attacker_id': 30966, ...}" —
because `delta_since_last` falls back to `str(rec)` when a record carries no
`summary` or `name`, which the bridge's war and scheme records never do.

TWO FIXES, AND THE SECOND IS THE ONE THAT MATTERS. Read RECORDS so the thing
can be filtered at all; then SAY WHAT WAS DROPPED. ck-zgh4 put the second one
best: a filter that reports "4,312 withheld" states a fact, while one that
silently returns 14 asserts that 14 was all there was.

  S1  a record naming the seat's character id is MINE
  S2  a record naming other ids is withheld, and `withheld.count` says so
  S3  THE CONTROL THAT CARRIES THE BEAD: the withheld count is non-zero when
      entries were dropped. A filter that returns only `mine` with no count
      would pass a naive test and is the defect.
  S4  `by_kind` breaks the withheld down, so "quiet period" and "filtered"
      are distinguishable
  S5  THE CONTROL ON HONESTY: a record this reader CANNOT classify is counted
      as withheld AND as `unclassified` -- "I could not tell" is never "not
      about you"
  S6  an older gateway without the structured method still yields something,
      labelled as the legacy shape rather than silently empty
  S7  `delta` keeps its name and is now the seat's own entries

(C)1 widened "mine" from the character to the REALM. S1-S7 all passed
unchanged across that change -- the harness fed no vassal and no liege -- so:

  S8  THE DISCRIMINATING PAIR: one vassal war, run with and without the vassal
      list, must come out DIFFERENTLY. Asserting only that it lands in `mine`
      would also pass a filter that had stopped filtering.
  S9  a war naming the liege is mine, and `reach` records which key matched
  S10 THE CONTROL ON THE DEFAULT BUCKET: absent / independent / walk-failed /
      self are four states, not one "unreachable"
  S11 the withheld note describes the reach it actually has
"""
from __future__ import annotations

import asyncio
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import redesign as R                                             # noqa: E402

SEAT = 30853

#: Real d49 delta records, as the bridge emits them (before the f-string ate
#: their structure). The first names the seat; the rest do not.
MINE = {"kind": "war", "tick": 61, "war_id": 14,
        "attacker_id": 31808, "defender_id": SEAT,
        "cb_type": "invasion_cb"}
FOREIGN_1 = {"kind": "war", "tick": 50, "war_id": 13, "attacker_id": 31762,
             "defender_id": 31416, "cb_type": "individual_county_de_jure_cb"}
FOREIGN_2 = {"kind": "scheme", "tick": 42, "schemer_id": 29110,
             "victim_id": 35172}
UNCLASSIFIABLE = {"kind": "war", "tick": 7, "note": "no id-shaped field here"}


class _GW:
    def __init__(self, recs, *, structured=True, realm=None, chrx=None):
        self._recs = recs
        self._structured = structured
        # The ORIGINAL harness returned {} for query_realm and a character with
        # only an age, so no vassal and no liege key could ever appear. The S8+
        # rows below need those payloads to exist, and S8 needs to run the SAME
        # record both with and without them.
        self._realm = realm or {}
        self._chr = {"age": 40, **(chrx or {})}

    async def read_many(self, specs):
        out = []
        for v, _ in specs:
            if v == "query_character":
                out.append({"character": dict(self._chr)})
            elif v == "query_realm":
                out.append(dict(self._realm))
            else:
                out.append({})
        return out

    async def read(self, verb, params):
        return {}

    async def delta_since_last(self):
        return [f"{r.get('kind')}: {r}" for r in self._recs]

    def game(self):
        return {"speed": 2}

    @property
    def health(self):
        return {}


class _GWStructured(_GW):
    async def delta_records_since_last(self):
        return list(self._recs)


def _since(recs, *, structured=True, realm=None, chrx=None):
    async def go():
        gw = (_GWStructured if structured else _GW)(recs, realm=realm, chrx=chrx)
        oa, op = R._resolve_actor, R._settled_dejure_pair

        async def actor(_g, cid, _t):
            return ({"id": cid or SEAT}, None)

        async def pair(_g, _c, keys, **kw):
            return [{"total": 1, "held": 1, "title_key": k} for k in keys]
        R._resolve_actor, R._settled_dejure_pair = actor, pair
        try:
            env = await R.observe_impl(gw, character_id=SEAT, depth="brief")
        finally:
            R._resolve_actor, R._settled_dejure_pair = oa, op
        res = (env.get("result") or {}) if isinstance(env, dict) else {}
        return res.get("since"), env.get("delta")
    return asyncio.run(go())


class ObserveSince(unittest.TestCase):

    def test_S1_a_record_naming_the_seat_is_mine(self):
        s, _ = _since([MINE])
        self.assertEqual(len(s["mine"]), 1)
        self.assertEqual(s["mine"][0]["war_id"], 14)
        self.assertEqual(s["withheld"]["count"], 0)

    def test_S2_S3_CONTROL_the_withheld_are_counted(self):
        s, _ = _since([MINE, FOREIGN_1, FOREIGN_2])
        self.assertEqual(len(s["mine"]), 1, "only the seat's entry is mine")
        self.assertEqual(s["withheld"]["count"], 2,
                         "a filter that returned only `mine` with no count "
                         "would assert that 1 was all there was")
        self.assertGreater(s["withheld"]["count"], 0)

    def test_S4_by_kind_distinguishes_quiet_from_filtered(self):
        s, _ = _since([FOREIGN_1, FOREIGN_1, FOREIGN_2])
        self.assertEqual(s["withheld"]["by_kind"], {"war": 2, "scheme": 1})
        quiet, _ = _since([])
        self.assertEqual(quiet["withheld"]["count"], 0)
        self.assertEqual(quiet["withheld"]["by_kind"], {},
                         "a quiet period and a filtered one must not look alike")

    def test_S5_CONTROL_unclassifiable_is_withheld_not_claimed_as_mine(self):
        s, _ = _since([UNCLASSIFIABLE])
        self.assertEqual(len(s["mine"]), 0,
                         "'I could not tell' is never 'not about you'")
        self.assertEqual(s["withheld"]["unclassified"], 1)
        self.assertEqual(s["withheld"]["count"], 1)

    def test_S6_an_older_gateway_still_yields_something_labelled(self):
        s, _ = _since([MINE, FOREIGN_1], structured=False)
        self.assertEqual(len(s["mine"]), 0)
        self.assertEqual(s["withheld"]["unclassified"], 2)
        self.assertEqual(s["withheld"]["by_kind"], {"legacy_string": 2},
                         "the legacy shape is named, not silently empty")

    def test_S7_delta_keeps_its_name_and_is_now_the_seats_own(self):
        s, delta = _since([MINE, FOREIGN_1, FOREIGN_2])
        self.assertEqual(delta, s["mine"])
        self.assertEqual(len(delta), 1)

    # ── ck-s8vr / (C)1: the widened reach ───────────────────────────────────
    # The six rows above all PASSED UNCHANGED after the filter was widened
    # from the character to the realm, because the harness fed no vassal and
    # no liege. A lane that cannot fail on the change is not evidence about it.

    VASSAL = 36559
    LIEGE = 255

    def test_S8_CONTROL_the_same_vassal_war_turns_on_the_vassal_list(self):
        """The discriminating pair: ONE record, one variable.

        Asserting only that a vassal war is `mine` would also pass if the
        filter had stopped filtering. So the identical record is run with and
        without the vassal list, and the verdict must DIFFER.
        """
        war = {"kind": "war", "tick": 70, "war_id": 21,
               "attacker_id": 31999, "defender_id": self.VASSAL}
        without, _ = _since([war])
        with_v, _ = _since([war], realm={"vassals": [{"id": self.VASSAL}]})
        self.assertEqual(len(without["mine"]), 0,
                         "no vassal list -> the war is foreign")
        self.assertEqual(without["withheld"]["count"], 1)
        self.assertEqual(len(with_v["mine"]), 1,
                         "a war on my vassal is my realm's war")
        self.assertEqual(with_v["withheld"]["count"], 0)
        self.assertIn("vassals(1)", with_v["matched_on"])

    def test_S9_a_war_naming_the_liege_is_mine_and_the_reach_says_so(self):
        war = {"kind": "war", "tick": 71, "war_id": 22,
               "attacker_id": 31999, "defender_id": self.LIEGE}
        s, _ = _since([war], chrx={"liege_id": self.LIEGE})
        self.assertEqual(len(s["mine"]), 1)
        self.assertIn("liege_id", s["matched_on"])
        self.assertEqual(s["reach"]["liege_id"], "matched")

    def test_S10_CONTROL_the_four_reach_states_are_not_one_bucket(self):
        """liege_id==0 is an ANSWER (independent); top_liege_id==0 is a WALK
        THAT FAILED (ck-6kvq.27, query_handlers.cpp:793). Reporting both as
        "unreachable" would tell every independent ruler its filter was blind.
        """
        absent, _ = _since([MINE])
        self.assertEqual(absent["reach"]["liege_id"], "absent_from_payload")

        zero, _ = _since([MINE], chrx={"liege_id": 0, "top_liege_id": 0})
        self.assertEqual(zero["reach"]["liege_id"], "independent_no_liege")
        self.assertEqual(zero["reach"]["top_liege_id"], "walk_failed",
                         "a failed walk is not an independent ruler")
        self.assertNotEqual(zero["reach"]["liege_id"],
                            zero["reach"]["top_liege_id"])

        me, _ = _since([MINE], chrx={"top_liege_id": SEAT})
        self.assertEqual(me["reach"]["top_liege_id"], "self_independent")
        self.assertNotIn("top_liege_id", me["matched_on"],
                         "walking to my own id adds no reach")

    def test_S11_the_withheld_note_tracks_the_reach_it_describes(self):
        """The note read "not naming this seat's character id" and went stale
        the moment the filter stopped being about the character id alone.
        """
        s, _ = _since([MINE], realm={"vassals": [{"id": self.VASSAL}]})
        note = s["withheld"]["note"]
        self.assertIn("matched_on", note)
        self.assertNotIn("this seat's character id", note)


if __name__ == "__main__":
    unittest.main()
