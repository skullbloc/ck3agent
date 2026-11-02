"""ck-w4tq, cleanup 2026.11 — the war reaches the seat on the path it takes, and
the row says only what the reply read.

HOST ONLY. Pure shape over a reply; no bridge, no game.

THE HARM, in the agents' own words. Three kings and an earl reported a
stalemate for YEARS on the strength of `war_score: 0` while the Normans took
England:

  d46 king-england 01:43:18Z  "For 18 months I reported a 'dead-even
    stalemate' on the strength of war_score: 0 ... [it] never credited
    William's occupation."
  d49 earl-kent 16:36:33Z     "war_id 1 read 0-0, 'even' at every check."

WHY THEY COULD NOT SEE IT. 93% of observe calls are `brief` (539 of 582 over
d45-d50), and the war reached the brief path as ONE BOOLEAN. ck-w4tq put the
war rows on that path.

WHAT THIS FILE NOW GUARDS (cleanup 2026.11). Those rows came from
get_active_wars (query_handlers.cpp do_get_active_wars), which never reads a
war's breakdown, and the row filled the unread fields in: war_score_source
None, battle_score 0, ticking_score 0, sieges_in_progress 0. Because the
source was always absent, every war at a score of 0 carried "do not read 0 as
parity" on evidence nobody had read. A 0 that means "not read" is a
manufactured value. The row now carries the reply's keys and nothing else,
plus one sentence saying where the breakdown is read.

  W1  a war appears on the BRIEF path at all (a real reply)
  W2  each row carries only keys the reply carried (plus my_side, read from
      is_attacker, and the one breakdown sentence): nothing is filled in
  W3  a reply row with no score gives a row with no score, not a 0
  W4  no caveat is raised on absence: a real 0 "even" war carries none
  W5  the breakdown sentence names the read that has it
  W6  an unreadable list reports `count: None`, not 0 ("no wars")
  W7  the block states facts only: no recommended lever
  K   the fixture's keys and observe's pass-through set are the emitter's
      keys, parsed from do_get_active_wars (a new emitted key reds here)
"""
from __future__ import annotations

import asyncio
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import redesign as R                                             # noqa: E402

#: A real get_active_wars reply, verbatim: live867 run m-l12 gmisc r1,
#: results/l7_hook_state.json step 3 (game date 0867.03.14), the duke at war
#: (12037) defending both Sons of Lodbrok invasions.
REAL_REPLY_DUKE = {
    "request_id": "63116679-2d5d-4d7a-9cd5-822806fad7c8", "tick": 72,
    "game_date": "0867.03.14", "success": True, "action": "get_active_wars",
    "character_id": 12037,
    "wars": [
        {"war_id": 4, "name": "Sons of Loðbrók Invasion of Lothian",
         "attacker_id": 14859, "defender_id": 12037, "is_attacker": False,
         "war_score": -3, "favored_side": "defender"},
        {"war_id": 5, "name": "Sons of Loðbrók Invasion of Northumberland",
         "attacker_id": 12491, "defender_id": 12037, "is_attacker": False,
         "war_score": -3, "favored_side": "defender"},
    ],
    "war_count": 2,
}

#: The same run, the next reply (0867.03.14): a war at a real 0, "even".
REAL_REPLY_ZERO = {
    "request_id": "07762b71-1661-4b52-a3d2-b4540199329f", "tick": 72,
    "game_date": "0867.03.14", "success": True, "action": "get_active_wars",
    "character_id": 12618,
    "wars": [{"war_id": 0, "name": "Aghlabid Conquest of Sicily",
              "attacker_id": 12618, "defender_id": 13568, "is_attacker": True,
              "war_score": 0, "favored_side": "even"}],
    "war_count": 1,
}

#: A war the breakdown cache held nothing for, so the reply carries no score:
#: the same run, results/f1_declare_actor0.json, the readback row of a war
#: declared that tick.
REAL_ROW_NO_SCORE = {"war_id": 13,
                     "name": "Suðreyjer Conquest of the Earldom of Athlone",
                     "attacker_id": 14859, "defender_id": 12932, "is_attacker": True}

#: What observe adds to a row on top of the reply's keys.
_ADDED = {"my_side", "breakdown"}


def _emitted_war_keys() -> set:
    """The keys do_get_active_wars writes into each war object: those between
    its `wars` array open and close. Fails OPEN: a key added there appears."""
    src = (pathlib.Path(__file__).resolve().parents[2] / "bridge" / "src" /
           "query_handlers.cpp").read_text()
    body = src[src.index("bool do_get_active_wars("):src.index("bool do_query_wars(")]
    arr = body[body.index('r.key("wars"); r.begin_array();'):body.index('r.key("war_count")')]
    keys = set(re.findall(r'r\.key\("(\w+)"\)', arr)) - {"wars"}
    assert keys, "no per-war key found: the parse no longer matches the emitter"
    return keys


class _GW:
    def __init__(self, wars):
        self._wars = wars

    async def read_many(self, specs):
        out = []
        for verb, _ in specs:
            if verb == "get_active_wars":
                out.append({"wars": self._wars})
            elif verb == "query_character":
                out.append({"age": 40, "gold": 10})
            else:
                out.append({})
        return out

    async def read(self, verb, params):
        return {}

    async def delta_records_since_last(self):
        return []

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
        orig_actor, orig_pair = R._resolve_actor, R._dejure_pair

        async def actor(_gw, cid, _tool):
            return ({"id": cid or 1}, None)

        async def pair(_gw, _cid, keys, **kw):
            return [{"total": 1, "held": 1, "title_key": k} for k in keys]
        R._resolve_actor, R._dejure_pair = actor, pair
        try:
            env = await R.observe_impl(gw, character_id=1, depth="brief")
        finally:
            R._resolve_actor, R._dejure_pair = orig_actor, orig_pair
        res = env.get("result") if isinstance(env, dict) else {}
        return (res or {}).get("wars")
    return asyncio.run(go())


class ObserveWarTruth(unittest.TestCase):

    def test_W1_a_war_appears_on_the_brief_path(self):
        b = _block(REAL_REPLY_DUKE["wars"])
        self.assertEqual(b["count"], 2)
        self.assertEqual([r["name"] for r in b["active"]],
                         [w["name"] for w in REAL_REPLY_DUKE["wars"]])

    def test_W2_a_row_carries_only_what_the_reply_carried(self):
        for reply in (REAL_REPLY_DUKE, REAL_REPLY_ZERO):
            for w, row in zip(reply["wars"], _block(reply["wars"])["active"]):
                self.assertEqual(set(row) - _ADDED, set(w) - {"is_attacker"}, row)
                for k in set(row) - _ADDED:
                    self.assertEqual(row[k], w[k], k)

    def test_W3_no_score_in_the_reply_is_no_score_in_the_row(self):
        row = _block([REAL_ROW_NO_SCORE])["active"][0]
        for k in ("war_score", "favored_side", "war_score_source",
                  "battle_score", "ticking_score", "sieges_in_progress"):
            self.assertNotIn(k, row, f"{k} was not read; a value there is made up")

    def test_W4_no_caveat_is_raised_on_absence(self):
        row = _block(REAL_REPLY_ZERO["wars"])["active"][0]
        self.assertEqual(row["war_score"], 0, "the reported 0 is the reply's 0")
        self.assertNotIn("score_is_uninformative", row)
        self.assertNotIn("score_caveat", row)

    def test_W5_the_row_says_where_the_breakdown_is_read(self):
        row = _block(REAL_REPLY_DUKE["wars"])["active"][0]
        self.assertIn('query(domain="wars")', row["breakdown"])
        self.assertIn("not read on this path", row["breakdown"])

    def test_W6_an_unreadable_list_says_so_rather_than_zero(self):
        b = _block(["not-a-dict"])
        self.assertIsNone(b["count"], "count 0 would read as `no wars`")
        self.assertIn("unreadable", b)

    def test_W7_the_block_states_facts_and_prescribes_nothing(self):
        row = _block(REAL_REPLY_DUKE["wars"])["active"][0]
        text = " ".join(str(v).lower() for v in row.values())
        for word in ("abandon", "should", "recommend", "white-peace", "lever"):
            self.assertNotIn(word, text,
                             f"{word!r} is advice; this block reports facts")


class ObserveWarRowKeys(unittest.TestCase):
    """K: one definition of a get_active_wars war row, the emitter's."""

    def test_the_pass_through_set_is_the_emitters_keys(self):
        self.assertEqual(set(R._ACTIVE_WAR_PASSTHROUGH) | {"is_attacker"},
                         _emitted_war_keys())

    def test_the_fixture_has_the_emitters_keys(self):
        """war_score_stale is emitted only when the cache holds an old entry; no
        captured reply has one. cb_type was added to the emitter by L15-D (NIT-3)
        after these replies were captured, and target_titles by L15 ff1 (m7).
        Those are the keys the fixtures lack."""
        seen = set()
        for w in REAL_REPLY_DUKE["wars"] + REAL_REPLY_ZERO["wars"] + [REAL_ROW_NO_SCORE]:
            seen |= set(w)
        self.assertEqual(seen | {"war_score_stale", "cb_type", "target_titles"},
                         _emitted_war_keys())

    def test_the_reply_names_each_wars_cb(self):
        """NIT-3: declare_war's idempotency matches target AND CB on these rows,
        and (L15 m7) the CB title against target_titles."""
        self.assertIn("cb_type", _emitted_war_keys())
        self.assertIn("target_titles", _emitted_war_keys())


class ObserveMySide(unittest.TestCase):
    """L12 witness: observe's wars.active[].my_side was null on every war,
    because get_active_wars emits is_attacker and no my_side/side key."""

    def test_the_reply_carries_is_attacker_and_no_side_key(self):
        keys = _emitted_war_keys()
        self.assertIn("is_attacker", keys)
        self.assertFalse({"my_side", "side"} & keys, keys)

    def test_my_side_is_read_from_is_attacker(self):
        self.assertEqual(_block(REAL_REPLY_DUKE["wars"])["active"][0]["my_side"], "defender")
        self.assertEqual(_block(REAL_REPLY_ZERO["wars"])["active"][0]["my_side"], "attacker")

    def test_control_a_row_that_does_not_say_is_not_guessed(self):
        row = {"war_id": 3, "name": "W", "war_score": 0}
        self.assertNotIn("my_side", _block([row])["active"][0])


if __name__ == "__main__":
    unittest.main()
