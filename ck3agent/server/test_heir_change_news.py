"""Two toolkit reads around a succession, driven by recorded game values.

succession_plan named Brian 36303 primary heir at 1082.03 (Dungal 56200 leaving with d_meath); the news at
1085.04.20 said only `heir_changed` Brian -> Dungal, three months before the ruler's death, and the record could not
tell whether Brian had died or the forecast had been wrong. The row now says why (one read of the last heir).

5 of 6 `inspect subject=title k_ireland` replies went over the 8 KB cap and lost explanations or the county
names to the cut. The seat's table now leaves the realm ruler's name and strength to `realms` and names each
next_call by a template, so the recorded kingdom table fits whole.
"""
import asyncio
import copy
import gzip
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import news as N  # noqa: E402
import reply_fit as RF  # noqa: E402
import title_table as TT  # noqa: E402

FIX = os.path.join(HERE, "fixtures", "munster_inspect_title.json.gz")


class _GW:
    def __init__(self, chars):
        self.chars, self.reads = chars, []

    async def read(self, action, params):
        self.reads.append((action, dict(params)))
        return self.chars.get(params.get("character_id"), {"error": "not_found"})


def _why(was, snap, chars):
    return asyncio.run(N.heir_change_why(_GW(chars), was, snap))


def test_3_the_heir_change_says_he_died():
    brian = {"id": 36303, "name": "Brian"}
    assert _why(brian, {"actor_id": 31762}, {36303: {"id": 36303, "alive": False}}) == {"why": "Brian (36303) died"}


def test_3_the_heir_change_says_he_is_now_the_ruler_without_a_read():
    gw = _GW({})
    got = asyncio.run(N.heir_change_why(gw, {"id": 56200, "name": "Dúngal"}, {"actor_id": 56200}))
    assert got == {"why": "Dúngal (56200) is now your ruler"} and gw.reads == []


def test_3_a_living_heir_passed_by_and_an_unread_one_are_said_apart():
    b = {"id": 36303, "name": "Brian"}
    assert "is alive and is no longer first" in _why(b, {"actor_id": 1}, {36303: {"alive": True}})["why"]
    assert _why(b, {"actor_id": 1}, {}) == {"why_unread": "query_character 36303 did not say whether he lives"}
    assert _why(None, {"actor_id": 1}, {}) == {}


def _recorded():
    return json.loads(gzip.open(FIX, "rt").read())["replies"]


def _full_table(step):
    """The recorded reply of `step` as the server built it before the cut: the whole rows and the explanations."""
    rep = next(r for r in _recorded() if r["step"] == step)["reply"]
    res = copy.deepcopy(rep["result"]["result"])
    res.pop("truncated", None)
    res["strength_unit"] = ("military_strength: the game's own soldier-strength figure for the ruler's realm (levies "
                            "plus men-at-arms), comparable between rows")
    res["next_call_rule"] = TT.NEXT_CALL_RULE
    return res


def _sent(res):
    env = {"ok": True, "tool": "inspect", "result": res, "blocked": None, "warnings": [], "errors": [],
           "actor": {"id": 31762, "name": "Murchad", "alive": True, "is_controlled": True, "is_player": False},
           "game": {"date": "1073.09.19", "paused": True, "speed": 0}, "bridge_health": "ok"}
    return RF.delivered(env, "inspect", {"subject": "title", "title_key": "k_ireland"})


def _as_1066(res):
    """The recorded 1073 table with the 1066 realm (11 counties not yours, each with its fabricate-claim call):
    the reply that went 10,189 bytes at turn 1."""
    t = copy.deepcopy(res)
    nc = next(r["next_call"] for r in t["counties"] if isinstance(r.get("next_call"), dict))
    for r in t["counties"]:
        if r["key"] not in ("c_thomond", "c_ennis", "c_ormond"):
            r["you_control"] = False
            r["next_call"] = json.loads(json.dumps(nc).replace(str(nc["args"]["args"]["target_title_id"]),
                                                               str(r["title_id"])))
    return t


def test_5_the_recorded_kingdom_table_fits_whole_for_the_seat(monkeypatch):
    """Turns 15 and 16 (1071, 1072: 8,221 and 8,234 bytes, the explanations cut) as built, and turn 18's table
    with the 1066 realm (the 10,189-byte case of turns 1-6, its county names cut). Inspect title is paged
    now, so the cut it had is read with pages off."""
    for res in (_full_table(15), _full_table(16), _as_1066(_full_table(18))):
        monkeypatch.setenv(RF.PAGES_ENV, "off")
        before = _sent(copy.deepcopy(res))
        monkeypatch.delenv(RF.PAGES_ENV)
        after = _sent(TT.seat_rows(res))
        assert "truncated" in before["result"]                     # the hazard: the cut took explanations/names
        assert "truncated" not in after["result"], after["result"].get("truncated")
        got = after["result"]
        assert len(got["counties"]) == 14 and all(r.get("name") and "holder_name" in r for r in got["counties"])
        assert got["next_call_rule"] == TT.NEXT_CALL_RULE and got["county_rows"] == TT.SEAT_ROWS_NOTE
        realms = {r["ruler_id"]: r for r in got["realms"]}
        for r in got["counties"]:
            assert "realm_ruler_name" not in r and r["realm_ruler_id"] in realms
            if "next_call" in r:
                tpl = json.dumps(got["next_call_templates"][r["next_call"]])
                assert '"<title_id>"' in tpl


def test_5_a_row_whose_realm_ruler_is_not_listed_keeps_his_name():
    res = {"counties": [{"title_id": 1, "key": "c_a", "realm_ruler_id": 9, "realm_ruler_name": "X",
                         "realm_ruler_strength": 5}], "realms": [{"ruler_id": 8}]}
    assert TT.seat_rows(res)["counties"][0]["realm_ruler_name"] == "X"


def test_5_CONTROL_other_readers_of_the_table_keep_whole_rows():
    """map_view and fabricate_targets call title_table directly; only inspect subject=title takes seat_rows."""
    src = open(os.path.join(HERE, "redesign.py"), encoding="utf-8").read()
    assert src.count("_tt.seat_rows(") == 1 and "**_tt.seat_rows(res)" in src
    assert "seat_rows" not in open(os.path.join(HERE, "map_view.py"), encoding="utf-8").read()
