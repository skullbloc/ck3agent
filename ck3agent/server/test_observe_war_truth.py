"""ck-w4tq — the war reaches the seat on the path it takes, and says when its score lies.

HOST ONLY. Pure shape over a reply; no bridge, no game.

THE HARM, in the agents' own words. Three kings and an earl reported a
stalemate for YEARS on the strength of `war_score: 0` while the Normans took
England:

  d46 king-england 01:43:18Z  "For 18 months I reported a 'dead-even
    stalemate' on the strength of war_score: 0 ... [it] never credited
    William's occupation."
  d45 king-england 15:03:27Z  "trusting a war_score and occupation_score
    frozen at zero -- while levy_own_domain_max fell 3,073 -> 2,718 -> 2,576."
  d49 earl-kent 16:36:33Z     "war_id 1 read 0-0, 'even' at every check."

WHY THEY COULD NOT SEE IT. `war_brief` and `_war_stance_view` are both inside
`depth == "full"`, and 93% of observe_v2 calls are `brief` -- 539 of 582
measured over d45-d50. The war reached the seat as ONE BOOLEAN on the path it
actually took. The information existed and was off the hot path.

  W1  a war appears on the BRIEF path at all
  W2  a zero score with sieges in progress is flagged, and the caveat NAMES
      the contrary evidence
  W3  THE CONTROL AGAINST MASSAGING: the reported `war_score` is still 0. The
      fix adds a caveat; it never edits the number.
  W4  THE CONTROL AGAINST CRYING WOLF: a zero score with NO contrary evidence
      is NOT flagged -- a flag on every zero would be noise and the seats would
      learn to ignore it
  W5  a non-zero score is never flagged
  W6  an unreadable list reports `count: None` and says so, rather than 0,
      which would read as "no wars"
  W7  the block states facts only: no recommended lever, no "abandon"
"""
from __future__ import annotations

import asyncio
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import redesign as R                                             # noqa: E402

#: The d49 war_id 1 entry, copied from a sealed transcript. The case the bead
#: exists for: 0-0 with the Normans taking England.
REAL_D49_WAR = {
    "war_id": 1, "name": "Norman Conquest of England", "side": "defender",
    "stance": "defending", "stance_source": "inferred_from_occupation",
    "will_siege": None, "war_score": 0, "occupation_score": 0,
    "defender_id": 30853, "attacker_id": 31808, "stall_suspect": False,
    "exit_lever": None, "sieges": [],
}


class _GW:
    def __init__(self, wars):
        self._wars = wars

    async def read_many(self, specs):
        out = []
        for verb, _ in specs:
            if verb == "get_active_wars":
                out.append({"wars": self._wars})
            elif verb == "query_character":
                out.append({"character": {"age": 40, "gold": 10}})
            else:
                out.append({})
        return out

    async def read(self, verb, params):
        return {}

    async def delta_since_last(self):
        return {}

    def game(self):
        return {"speed": 2}

    @property
    def health(self):
        return {}


def _block(wars):
    """Run the brief path and return its `wars` block."""
    async def go():
        gw = _GW(wars)
        # _resolve_actor and the de-jure poll are not what this row is about;
        # patch them to constants so the test exercises the war shape only.
        orig_actor, orig_pair = R._resolve_actor, R._settled_dejure_pair

        async def actor(_gw, cid, _tool):
            return ({"id": cid or 1}, None)

        async def pair(_gw, _cid, keys, **kw):
            return [{"total": 1, "held": 1, "title_key": k} for k in keys]
        R._resolve_actor, R._settled_dejure_pair = actor, pair
        try:
            env = await R.observe_impl(gw, character_id=1, depth="brief")
        finally:
            R._resolve_actor, R._settled_dejure_pair = orig_actor, orig_pair
        res = env.get("result") if isinstance(env, dict) else {}
        return (res or {}).get("wars")
    return asyncio.run(go())


class ObserveWarTruth(unittest.TestCase):

    def test_W1_a_war_appears_on_the_brief_path(self):
        b = _block([REAL_D49_WAR])
        self.assertIsNotNone(b, "the brief path must carry a `wars` block")
        self.assertEqual(b["count"], 1)
        self.assertEqual(b["active"][0]["name"], "Norman Conquest of England")

    def test_W2_a_zero_score_with_sieges_is_flagged_and_names_the_evidence(self):
        w = dict(REAL_D49_WAR, sieges=[{"county": "c_essex"}, {"county": "c_kent"}])
        row = _block([w])["active"][0]
        self.assertIs(row.get("score_is_uninformative"), True)
        self.assertIn("2 siege(s) in progress", row["score_caveat"])
        self.assertIn("Do not read 0 as parity", row["score_caveat"])

    def test_W3_CONTROL_the_number_is_never_massaged(self):
        w = dict(REAL_D49_WAR, sieges=[{"county": "c_essex"}], battle_score=40)
        row = _block([w])["active"][0]
        self.assertEqual(row["war_score"], 0,
                         "the reported score stays 0; the fix adds a caveat, "
                         "it does not edit the number")
        self.assertIn("battle_score 40", row["score_caveat"])

    def test_W4_CONTROL_a_bare_zero_is_not_flagged(self):
        # No sieges, no battle, no ticking, and a source IS present: there is
        # nothing contrary, so flagging would be crying wolf.
        w = dict(REAL_D49_WAR, war_score=0, war_score_source="engine",
                 battle_score=0, ticking_score=0, sieges=[])
        row = _block([w])["active"][0]
        self.assertNotIn("score_is_uninformative", row)
        self.assertNotIn("score_caveat", row)

    def test_W5_a_real_score_is_never_flagged(self):
        w = dict(REAL_D49_WAR, war_score=-35, war_score_source="engine",
                 sieges=[{"county": "c_essex"}])
        row = _block([w])["active"][0]
        self.assertNotIn("score_is_uninformative", row)
        self.assertEqual(row["war_score"], -35)

    def test_W6_an_unreadable_list_says_so_rather_than_zero(self):
        b = _block(["not-a-dict"])
        self.assertIsNone(b["count"], "count 0 would read as `no wars`")
        self.assertIn("unreadable", b)

    def test_W7_the_block_states_facts_and_prescribes_nothing(self):
        w = dict(REAL_D49_WAR, sieges=[{"county": "c_essex"}])
        row = _block([w])["active"][0]
        text = " ".join(str(v).lower() for v in row.values())
        for word in ("abandon", "should", "recommend", "white-peace", "lever"):
            self.assertNotIn(word, text,
                             f"{word!r} is advice; this block reports facts")


if __name__ == "__main__":
    unittest.main()
