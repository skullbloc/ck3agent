"""Rows 4-6 of a recorded game.

  1  the AI's diarch blocked disinherit/vows unannounced; end_diarchy went the AI path   diarchy.py: the game's
     check reads the diarch (news under a line task), lever rows and refusals name the block, the debug-only
     interaction is refused with the game's reason and the real one (liege_dismiss_*, sent to its named target)
  2  call_ally refused at is_shown with no words and a marriage hint                    the causes the reads show
  3  a line task's observations carried conquest material                               task_goal.py, one source:
     CK3_GOAL_KIND; nothing changes under another goal (byte-for-byte on recorded Munster news)

No game. The recorded news blocks are the turn observations of three recorded games
(fixtures in a research note, extracted from the runs' turn prompts).
"""
from __future__ import annotations

import asyncio
import copy
import gzip
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import diarchy as D  # noqa: E402
import partition_forecast as PF  # noqa: E402
import redesign as R  # noqa: E402
import task_goal as TG  # noqa: E402

GEBHARD, BERTHOLD = 37151, 30331
WORDS = "You cannot take overt hostile actions against your Diarch"


def run(c):
    return asyncio.run(c)


def _check(rows):
    return {"success": True, "result": {"interaction": "x", "results": rows}}


# ── item 1 ────────────────────────────────────────────────────────────────────────────────────────────────────

def test_1a_the_debug_interaction_is_refused_with_the_games_reason_and_the_real_one():
    r = D.debug_refusal("end_diarchy_interaction")
    assert "debug_only = yes" in r["summary"] and "nothing was sent" in r["summary"]
    assert r["instead"]["interaction"] == "liege_dismiss_temporary_regency_interaction"
    assert D.debug_refusal("disinherit_interaction") is None


def test_1b_the_do_path_refuses_it_before_anything_is_sent():
    sent = []

    class GW:
        health = {}

        def game(self):
            return {"date": "1073.01.01"}

        async def read(self, a, p=None):
            sent.append(a)
            return {}

        async def write(self, a, p=None):
            sent.append(a)
            return {}
    env = run(R.do_impl(GW(), intent="interact", character_id=BERTHOLD,
                        args={"interaction": "end_diarchy_interaction", "target_id": GEBHARD}))
    b = env["blocked"]
    assert env["verdict"] == "rejected" and b["gate"] == "interaction_invalid" and "debug" in b["summary"]
    assert b["fix"]["args"]["args"]["interaction"] == "liege_dismiss_temporary_regency_interaction"
    assert "send_interaction" not in sent


def test_1c_the_regency_ends_are_sent_to_their_named_target():
    assert {D.DISMISS_TEMPORARY, D.DISMISS_ENTRENCHED} <= R.DIRECT_DISPATCH_INTERACTIONS
    src = (ROOT / "bridge" / "src" / "ai_eval_hook.cpp").read_text()
    assert f'"{D.DISMISS_TEMPORARY}",' in src and f'"{D.DISMISS_ENTRENCHED}",' in src


def test_1d_the_diarch_is_read_by_the_games_check():
    shown_no = {"target_id": 1, "sendable": False, "refused_by": "interaction_not_valid_for_target",
                "failed_check": "is_shown"}
    him = {"target_id": GEBHARD, "sendable": False, "refused_by": "interaction_not_valid_for_target",
           "failed_check": "is_valid_showing_failures_only"}
    got = D.diarch_of({D.DISMISS_TEMPORARY: _check([shown_no, him]), D.DISMISS_ENTRENCHED: _check([shown_no])})
    assert got["id"] == GEBHARD and got["kind"] == "temporary_regency" and got["can_end_now"] is False
    free = D.diarch_of({D.DISMISS_TEMPORARY: _check([dict(him, sendable=True)]), D.DISMISS_ENTRENCHED: _check([])})
    assert free["can_end_now"] is True
    none = D.diarch_of({D.DISMISS_TEMPORARY: _check([shown_no]), D.DISMISS_ENTRENCHED: _check([shown_no])})
    assert none == {"id": None}
    # nobody looked is not "no diarch": a dismiss check that did not answer
    assert D.diarch_of({D.DISMISS_TEMPORARY: {}, D.DISMISS_ENTRENCHED: _check([shown_no])}) is None
    # live d1073 (1073.01.01): a stage BEFORE is_shown (actor_or_recipient_unavailable) is not the diarch
    away = dict(shown_no, target_id=33029, failed_check="actor_or_recipient_unavailable")
    assert D.diarch_of({D.DISMISS_TEMPORARY: _check([away]), D.DISMISS_ENTRENCHED: _check([away])}) == {"id": None}
    # a regency no dismiss interaction shows for: the game's block words on his disinherit check find him
    words = D.diarch_of({D.DISMISS_TEMPORARY: _check([shown_no]), D.DISMISS_ENTRENCHED: _check([shown_no]),
                         D.WORDS_CHECK: _check([{"target_id": GEBHARD, "sendable": False, "engine_reason": WORDS}])})
    assert words["id"] == GEBHARD and words["kind"] == "not_read"


def test_1e_the_news_row_says_who_what_kind_and_what_it_blocks():
    now = {"id": GEBHARD, "kind": "temporary_regency", "can_end_now": False, "why_not": "travelling"}
    r = D.news_row({"id": None}, now)
    assert r["to"] == GEBHARD and r["kind"] == "temporary_regency" and "disinherit" in r["blocks"]
    assert "travels" in r["how_it_came"] and "next" not in r
    assert D.news_row({"id": None}, dict(now, can_end_now=True))["next"]["args"]["args"]["target_id"] == GEBHARD
    ended = D.news_row(now, {"id": None})
    assert ended["from"] == GEBHARD and ended["to"] is None and "no longer" in ended["ended"]
    assert D.news_row(now, dict(now)) is None and D.news_row(None, now) is None


def test_1f_the_read_asks_the_family_and_council_and_nothing_is_sent():
    asked = []

    class GW:
        async def read(self, a, p=None):
            asked.append((a, p))
            return _check([{"target_id": t, "sendable": False, "failed_check": "is_shown"} for t in p["candidates"]])
    got = run(D.read(GW(), BERTHOLD, {"children": [GEBHARD, 36857], "spouses": [{"character_id": 5}]},
                     [{"holder_id": 7}, {"holder_id": 8, "vacant": True}]))
    assert got == {"id": None}
    assert [a for a, _ in asked] == ["query_interaction_check"] * 3
    assert [p["interaction_type"] for _, p in asked][2] == "disinherit_interaction"
    assert asked[0][1]["candidates"] == [GEBHARD, 36857, 5, 7]


def test_1g_a_lever_row_refused_by_the_diarch_names_the_diarchy():
    gc = PF._game_check({"target_id": GEBHARD, "sendable": False, "refused_by": "interaction_not_valid_for_target",
                         "engine_reason": WORDS}, {})
    assert gc["refused"]["blocked_by"] == "diarchy" and str(GEBHARD) in gc["refused"]["why"]
    assert gc["refused"]["diarchy"]["end_call"]["args"]["args"]["interaction"] == D.DISMISS_TEMPORARY
    other = PF._game_check({"target_id": GEBHARD, "sendable": False, "refused_by": "send_cost_unaffordable"}, {})
    assert "blocked_by" not in other["refused"]                       # control


def test_1h_a_refused_send_with_the_words_names_it():
    env = {"verdict": "rejected", "blocked": {"gate": "interaction_invalid", "detail": WORDS,
                                              "raw_reason": "interaction_not_valid_for_target"}}
    R._explain_interaction_refusal(env, "take_vows_interaction", GEBHARD)
    assert env["blocked"]["diarchy"]["end_call"]["args"]["args"]["target_id"] == GEBHARD


# ── item 2 ────────────────────────────────────────────────────────────────────────────────────────────────────

class _AllyGW:
    def __init__(self, strength=60, allied=True, house=(1, 2), truce=None):
        self.strength, self.allied, self.house, self.truce = strength, allied, house, truce
        R._truces.forget()                   # the truce map is cached per ruler and day

    def game(self):
        return {"date": "1084.06.01"}

    async def read(self, a, p=None):
        if a == "query_interaction_check":
            return _check([{"target_id": p["target_id"], "sendable": False,
                            "refused_by": "interaction_not_valid_for_target", "failed_check": "is_shown"}])
        if a == "query_alliances":
            return {"alliances": [{"ally_id": 37306}] if self.allied else []}
        if a == "query_character":
            me = p["character_id"] == BERTHOLD
            return {"id": p["character_id"], "is_ai": True, "liege_id": 36911,     # flat, as the bridge writes it
                    "house_id": self.house[0] if me else self.house[1],
                    "military_strength": 900 if me else self.strength}
        if a == "query_relations":
            rows = [{"target_id": 666, "source": "relation_slot", "end_date": self.truce}] if self.truce else []
            return {"truces": rows, "truces_on_him": []}
        return {}


WAR = {"war_id": 134217852, "attacker_id": 666, "defender_id": BERTHOLD, "attackers": [666],
       "defenders": [BERTHOLD]}


def test_2a_the_causes_read_are_named_and_only_those():
    why = run(R.call_ally_why(_AllyGW(strength=60), BERTHOLD, 37306, WAR["war_id"], WAR))
    causes = [c["cause"] for c in why["causes_read"]]
    assert causes == ["under_100_strength"] and "60" in why["causes_read"][0]["why"]
    why = run(R.call_ally_why(_AllyGW(strength=500, allied=False, house=(4, 4)), BERTHOLD, 37306, WAR["war_id"],
                              dict(WAR, defenders=[BERTHOLD, 37306])))
    assert [c["cause"] for c in why["causes_read"]] == ["not_an_ally", "house_member", "already_in_war"]


def test_2b_a_truce_with_the_enemy_leader_is_named_when_read():
    why = run(R.call_ally_why(_AllyGW(strength=500, truce="1086.01.01"), BERTHOLD, 37306, WAR["war_id"], WAR))
    assert [c["cause"] for c in why["causes_read"]] == ["truce_with_enemy"]


def test_2c_CONTROL_none_read_none_named():
    why = run(R.call_ally_why(_AllyGW(strength=500), BERTHOLD, 37306, WAR["war_id"], WAR))
    assert "causes_read" not in why


# ── item 3 ────────────────────────────────────────────────────────────────────────────────────────────────────


def test_3c_observe_names_no_kingdom_under_the_line_goal(monkeypatch):
    res = {"de_jure": {"primary_title": "d_carinthia", "next_title": {"title": "k_bavaria"}}}
    monkeypatch.setenv(TG.GOAL_KIND_ENV, "kingdom_held")
    assert TG.observe(copy.deepcopy(res)) == res
    monkeypatch.setenv(TG.GOAL_KIND_ENV, "line_kept_peacefully")
    assert TG.observe(copy.deepcopy(res)) == {"de_jure": {"primary_title": "d_carinthia"}}


def test_3d_the_diarch_read_runs_under_the_line_goal_only(monkeypatch):
    import news as N
    calls = []

    async def fake(gw, me, chr_sect, council):
        calls.append(me)
        return {"id": GEBHARD, "kind": "temporary_regency", "can_end_now": False} if len(calls) > 1 else {"id": None}
    monkeypatch.setattr(D, "read", fake)

    class GW:
        def game(self):
            return {"date": "1073.01.01"}

        async def read(self, a, p=None):
            return {}

        async def read_many(self, reqs):
            return [{} for _ in reqs]

    def snap(date):
        return N.snapshot(actor_id=BERTHOLD, date=date, chr_sect={"children": [GEBHARD]}, wars={}, claims={},
                          council=None, heir=None)
    for kind, want in (("kingdom_held", 0), ("line_kept_peacefully", 2)):
        monkeypatch.setenv(TG.GOAL_KIND_ENV, kind)
        calls.clear()
        N._LAST.clear()
        N._SHOWN.clear()
        run(N.block(GW(), BERTHOLD, snap("1072.12.01"), offers_served=False))
        out = run(N.block(GW(), BERTHOLD, snap("1073.01.01"), offers_served=False))
        assert len(calls) == want, kind
        assert ("diarch_changed" in out) is (want > 0), kind
    assert out["diarch_changed"]["to"] == GEBHARD


