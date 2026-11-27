"""The notes set of the Munster task paid once (munster1066once): the Ireland set (ireland1066, the pool whole) with
the goal note in turn one.

The goal note states the task's reward in few words (counties, +10 once, play to the end date) and carries the
mint line of the Ireland set's pad note, which the set drops (the pad note without its mint line comes in its
place); turn one is the Ireland set's, the goal note where the pad note was. The set also drops the notes that
describe the Dublin start and are not true of Munster's ruler (NOT_MUNSTER: convert-faith-local reports Dublin's
ruler converting to Insular Celtic, the faith Munster's ruler holds at the 1066 start, as read live;
a culture change of Dublin's Norse land; prestige and purse figures read at Dublin). The limits are the served
set's: each note at most TEXT_MAX bytes, turn one at most TURN_ONE_MAX, the set at most SET_TOTAL_MAX. The
default set's version is pinned here (it is not this set's)."""
import json

import notes as N

SET = "munster1066once"
GOAL = "munster-once-goal"
#: the default set's notes scoped to the Dublin start (or to Murchad of Dublin's faith), dropped from this set
NOT_MUNSTER = ("convert-faith-local", "ai-converts-culture", "prestige-start-drain", "prestige-flow",
               "claims-eat-income")


def _doc():
    return N.load()


def test_the_set_is_served_within_every_limit():
    doc = _doc()
    assert N.problems(doc) == []
    d = N.derive(doc, SET)
    assert N.problems(d) == []
    assert N.set_bytes(d) <= N.SET_TOTAL_MAX
    assert len(json.dumps(N.turn_one_texts(d), ensure_ascii=False).encode()) <= N.TURN_ONE_MAX
    goal = next(n for n in d["notes"] if n["id"] == GOAL)
    assert len(goal["text"].encode()) <= N.TEXT_MAX and not N.PRIVATE_RE.search(json.dumps(goal))


def test_the_goal_note_states_the_reward():
    goal = next(n for n in N.derive(_doc(), SET)["notes"] if n["id"] == GOAL)["text"]
    # the reward in the words the prompt and the task's description use: the seat's ruler holds, both loss cases
    for words in ("+1/-1 per de jure Irish county won/lost", "+10 once", "when your ruler first holds k_ireland",
                  "(play on)", "-5 and the end if the line ends or passes to a ruler of another dynasty",
                  "do mint_title title_key=k_ireland"):
        assert words in goal, words


def test_turn_one_is_the_ireland_sets_with_the_goal_note_in_the_pad_notes_place():
    doc = _doc()
    want = [GOAL if x == "notes-pad-is-memory" else x for x in doc["sets"]["ireland1066"]["turn_one"]]
    assert doc["sets"][SET]["turn_one"] == want, "the Ireland turn one changed: re-take this set's from it"
    d = N.derive(doc, SET)
    ids = {n["id"] for n in d["notes"]}
    assert "notes-pad-is-memory" not in ids and {"notes-pad", GOAL} <= ids and not set(NOT_MUNSTER) & ids
    assert [n["id"] for n in d["notes"] if n["id"] not in ("notes-pad", GOAL)] == \
        [n["id"] for n in doc["notes"] if n["id"] not in ("notes-pad-is-memory", *NOT_MUNSTER)]
    # each dropped note is scoped to another 1066 seat, and none is in turn one
    by = {n["id"]: n for n in doc["notes"]}
    assert all("Dublin" in by[x]["scope"] and x not in want for x in NOT_MUNSTER)


def test_control_the_versions_are_pinned():
    doc = _doc()
    # each served figure checked against the game's files and scoped to its source: 618d4bbe488d (was cf8cfcaf887f);
    # the truce's other modifiers, the claim letter's struggle term before its floor, the crown-authority price by
    # realm size: e5842797c88a; the truce note scoped to most casus belli: b26b0c6df8b5
    assert N.version(N.derive(doc, SET)) == "b26b0c6df8b5"
    # the default set is general notes only (default_drop), its figures checked against the game's files:
    # f631a8a72e85 (was b92201c11c8e); the truce, claim letter and crown-authority wording: bce65ef7dcb5; the truce
    # note scoped to most casus belli: 440ecf864719
    assert N.version(N.derive(doc, None)) == "440ecf864719"
    assert N.version(N.derive(doc, SET)) != N.version(N.derive(doc, None))
