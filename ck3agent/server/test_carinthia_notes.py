"""The carinthia1066 notes set: Lorraine's set from another seat, its two
start notes renamed, and one partition note that replaces the any-start one. No game."""
import json
import re

import pytest

import notes as N
from test_vassal_faction_news import REGION, set_bytes

DOC = N.load()
CAR = "carinthia1066"
#: this start's title and ruler keys: no other set may carry them, and this set carries no other start's
CAR_REGION = r"Carinthia|d_carinthia|Carniola|d_krain|Verona|Berthold|Markward|Z[äa]hringen"


def _served(name):
    return N.derive(DOC, name)


def test_the_set_is_valid_within_its_byte_limits():
    d = _served(CAR)
    assert N.problems(d) == []
    assert len(json.dumps(N.turn_one_texts(d), ensure_ascii=False).encode()) <= N.TURN_ONE_MAX
    assert set_bytes(d) <= N.SET_TOTAL_MAX == 7416
    assert all(len(n["text"].encode()) <= N.TEXT_MAX for n in d["notes"])
    assert N.version(d) not in (N.version(_served(None)), N.version(_served("lorraine1066")))


def test_it_is_lorraines_set_with_its_own_goal_notes_and_the_partition_note():
    s, lor = DOC["sets"][CAR], DOC["sets"]["lorraine1066"]
    assert s["about"] == "the holder of d_carinthia, 1066: keep the line in peace"
    assert s["drop"] == lor["drop"] + ["partition"]
    ren = {"lorraine-line-goal": "carinthia-line-goal", "lorraine-factions": "carinthia-factions"}
    ours = {n["id"]: n for n in s["add"]}
    for n in lor["add"]:
        mine = ours[ren.get(n["id"], n["id"])]
        assert {k: v for k, v in mine.items() if k not in ("id", "added")} == \
            {k: v for k, v in n.items() if k not in ("id", "added")}
    part = ours["carinthia-partition"]
    assert part["topic"] == "succession" and "confederate partition" in part["text"]
    assert "a duke has no duke vassals" in part["text"] and "Disinherit keeps it" in part["text"]
    assert [ren.get(i, i) for i in lor["turn_one"]] == [i for i in s["turn_one"] if i != "carinthia-partition"]


def test_turn_one_states_the_goal_the_vetoes_and_the_partition():
    t = N.turn_one_texts(_served(CAR))
    joined = " ".join(t)
    for words in ("passed at least once", "what=war", "what=faction", "confederate partition", "faction leave",
                  "Disinherit keeps it"):
        assert words in joined, words


@pytest.mark.parametrize("name", (None, "francia867", "sweden867", "lorraine1066", CAR))
def test_no_set_carries_another_start_s_title_or_ruler(name):
    d = _served(name)
    rendered = [N.render(n) for n in d["notes"]] + N.turn_one_texts(d)
    pats = dict(REGION, **{CAR: CAR_REGION})
    mine = "ireland" if name is None else name
    for other, pat in pats.items():
        if other == mine:
            continue
        bad = [t for t in rendered if re.search(pat, t)]
        assert not bad, (name, other, bad)


def test_no_other_set_in_the_file_carries_this_start_s_words():
    """Every set the file has (a later set included), and the default notes: none names Carinthia's titles or men."""
    for name in [None] + [n for n in DOC["sets"] if n != CAR]:
        d = _served(name)
        bad = [t for t in [N.render(n) for n in d["notes"]] + N.turn_one_texts(d) if re.search(CAR_REGION, t)]
        assert not bad, (name, bad)


def test_CONTROL_the_region_check_finds_a_planted_title():
    d = _served("lorraine1066")
    planted = dict(d, notes=d["notes"] + [dict(d["notes"][0], text="hold d_carinthia")])
    assert any(re.search(CAR_REGION, N.render(n)) for n in planted["notes"])
