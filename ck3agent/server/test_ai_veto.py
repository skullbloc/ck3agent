"""do(intent="ai_veto") and its news rows (seat_events.py, news.py).

The bridge replies are the shapes bridge/src/ai_veto.cpp do_set_ai_veto / do_query_ai_veto
write. The case: a recorded game — the game's AI created d_meath for Murchad (31506) at
29.6 gold against a price of 212.5; a recorded game — the AI passed crown_authority_1 three times
for the seat's ruler, about 502 prestige in all, reported without amounts.
"""
from __future__ import annotations

import asyncio

import pytest

import news
import redesign as R
import seat_events as SE
import tool_text
from test_ireland_walk_replies import Gw

ME = 31506
WHATS = ("create_title", "law", "mercenary", "construction")


def run(c):
    return asyncio.run(c)


def actor():
    return {"id": ME, "name": "Murchad"}


class Veto:
    """A bridge veto table: set_ai_veto writes it, query_ai_veto reads it."""

    def __init__(self, records=None, hook=True):
        self.vetoes, self.records, self.hook = [], list(records or []), hook

    def write(self, p):
        if not self.hook:
            return {"success": False, "action": "set_ai_veto", "reason": "ai_veto_hook_not_installed"}
        w = p["what"]
        self.vetoes = [x for x in self.vetoes if x != w] + ([w] if p.get("op") == "set" else [])
        return {"success": True, "action": "set_ai_veto", "op": p.get("op"), "what": w}

    def read(self, p):
        rulers = [{"character_id": ME, "vetoes": self.vetoes,
                   "dropped": {w: sum(1 for r in self.records if r["what"] == w and r["dropped"]) for w in WHATS},
                   "passed": {w: sum(1 for r in self.records if r["what"] == w and not r["dropped"]) for w in WHATS}}]
        return {"success": True, "action": "query_ai_veto", "hook_installed": self.hook, "disabled_by_env": False,
                "last_seq": len(self.records), "ring_cap": 128, "rulers": rulers, "records": self.records}


class VGw(Gw):
    def __init__(self, table, replies=None):
        super().__init__(dict(replies or {}, query_ai_veto=table.read))
        self.table = table

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        if action == "set_ai_veto":
            return self.table.write(dict(params or {}))
        return {"success": True}


def meath(seq, dropped=True, date="1074.11.11", gold=29.6):
    return {"seq": seq, "date": date, "character_id": ME, "what": "create_title", "dropped": dropped,
            "title_id": 900, "key": "d_meath", "gold": gold, "prestige": 150.0, "piety": 700.0, "priced": "yes",
            "price": {"gold": 212.5, "prestige": 0.0, "piety": 0.0, "currency": "gold"}}


def crown(seq, dropped=False, date="1066.12.01", prestige=300.0, cost=189.0):
    return {"seq": seq, "date": date, "character_id": ME, "what": "law", "dropped": dropped,
            "key": "crown_authority_1", "gold": 84.0, "prestige": prestige, "piety": 0.0, "priced": "yes",
            "price": {"gold": 0.0, "prestige": cost, "piety": 0.0, "currency": "gold"}}


# ── do(intent="ai_veto") ──────────────────────────────────────────────────────
def test_set_is_sent_and_read_back():
    t = Veto()
    gw = VGw(t)
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "create_title"}, 0, actor()))
    assert env["ok"] is True and env["verdict"] == "applied", env
    assert gw.writes == [("set_ai_veto", {"character_id": ME, "op": "set", "what": "create_title"})]
    assert env["result"]["vetoes"] == ["create_title"] and env["result"]["readback"]["read"] is True
    assert "mint_title" in env["result"]["applies_to"]


def test_clear_removes_it():
    t = Veto()
    gw = VGw(t)
    run(SE.do_ai_veto(gw, {"op": "set", "what": "law"}, 0, actor()))
    env = run(SE.do_ai_veto(gw, {"op": "clear", "what": "law"}, 0, actor()))
    assert env["verdict"] == "applied" and env["result"]["vetoes"] == []


def test_an_unknown_what_refuses_and_sends_nothing():
    gw = VGw(Veto())
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "siege"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "invalid_argument" and gw.writes == []


def test_set_without_what_refuses():
    gw = VGw(Veto())
    env = run(SE.do_ai_veto(gw, {"op": "set"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "missing_required_argument" and gw.writes == []


def test_list_shows_the_vetoes_and_the_attempts():
    t = Veto([meath(1), meath(2, date="1075.01.02")])
    t.vetoes = ["create_title"]
    env = run(SE.do_ai_veto(VGw(t), {}, 0, actor()))
    r = env["result"]
    assert r["op"] == "list" and r["vetoes"] == ["create_title"] and r["dropped"]["create_title"] == 2
    assert r["attempts"][-1]["key"] == "d_meath" and r["attempts"][-1]["price"] == {"gold": 212.5}


def test_the_bridge_refusal_names_its_gate():
    gw = VGw(Veto(hook=False))
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "law"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["raw_reason"] == "ai_veto_hook_not_installed"


def test_the_intent_is_routed_and_listed():
    assert "ai_veto" in R.INTENTS
    t = Veto()
    env = run(R.do_impl(VGw(t), intent="ai_veto", args={"op": "set", "what": "create_title"}, character_id=ME))
    assert env["verdict"] == "applied", env
    assert "ai_veto" in tool_text.DO


# ── news ──────────────────────────────────────────────────────────────────────
def snap(date, records, vetoes=(), **kw):
    veto = Veto(records)
    veto.vetoes = list(vetoes)
    base = dict(actor_id=ME, date=date, chr_sect={"gold": 0.0, "prestige": 0.0, "piety": 0.0},
                wars={"wars": []}, claims={"claims": []}, council={"council": []}, heir=None,
                schemes={"schemes": []}, held={"held_titles": []}, council_tasks=[], laws={"active_laws": []},
                alliances={"alliances": []}, ai_veto=veto.read({}))
    base.update(kw)
    return news.snapshot(**base)


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    yield
    news.reset()


def two(a, b):
    run(news.block(Gw({}), 0, a, offers_served=False))
    return run(news.block(Gw({}), 0, b, offers_served=False))


def test_a_vetoed_creation_is_in_the_news_with_title_price_gold_and_the_mint_call():
    out = two(snap("1074.06.01", []), snap("1075.02.09", [meath(1), meath(2, date="1075.01.02", gold=31.0)],
                                           vetoes=["create_title"]))
    (row,) = out["ai_vetoed"]
    assert row["what"] == "create_title" and row["key"] == "d_meath" and row["attempts"] == 2
    assert row["price"] == {"gold": 212.5} and row["gold_at_attempt"] == 31.0
    assert row["first"] == "1074.11.11" and row["last"] == "1075.01.02"
    assert row["next"] == {"tool": "do", "args": {"intent": "mint_title", "args": {"title_key": "d_meath"}}}


def test_only_new_attempts_are_news():
    a = snap("1075.02.09", [meath(1)], vetoes=["create_title"])
    b = snap("1076.02.09", [meath(1)], vetoes=["create_title"])
    out = two(a, b)
    assert "ai_vetoed" not in out


def test_a_law_the_ai_passed_carries_its_price_and_the_veto_lever():
    a = snap("1066.09.15", [], chr_sect={"gold": 84.0, "prestige": 300.0, "piety": 0.0})
    b = snap("1067.09.15", [crown(1)], chr_sect={"gold": 84.0, "prestige": 111.0, "piety": 0.0},
             laws={"active_laws": [{"name": "crown_authority_1"}]})
    out = two(a, b)
    (row,) = [e for e in out["purse"]["prestige"]["in_this_step"] if "crown_authority_1" in e["what"]]
    assert row["decided_by"].startswith("the game's AI") and row["expected"] == -189.0
    assert row["price"] == {"prestige": 189.0} and "not read" not in row["basis"]
    assert row["lever"]["args"] == {"intent": "ai_veto", "args": {"op": "set", "what": "law"}}
    (sent,) = out["ai_acted"]
    assert sent["what"] == "law" and sent["key"] == "crown_authority_1" and sent["price"] == {"prestige": 189.0}


def test_a_vetoed_law_is_news_and_not_a_passed_law():
    out = two(snap("1066.09.15", []), snap("1067.09.15", [crown(1, dropped=True)], vetoes=["law"]))
    (row,) = out["ai_vetoed"]
    assert row["what"] == "law" and row["key"] == "crown_authority_1" and row["price"] == {"prestige": 189.0}
    assert "next" in row and row["next"]["args"]["intent"] == "change_law"
    assert "ai_acted" not in out


def test_the_first_news_names_ai_veto_as_the_lever_for_titles_and_laws(monkeypatch):
    # the whole block, as served with the notes off (with them on, test_map_digest_and_turn_one_notes checks the calls)
    import notes as _notes
    monkeypatch.setattr(_notes, "SERVED", None)
    out = run(news.block(Gw({}), 0, snap("1066.09.15", []), offers_served=False))
    ai = out["ai_spends"]
    assert ai["title_creation"]["lever"]["veto"]["args"] == {"intent": "ai_veto",
                                                            "args": {"op": "set", "what": "create_title"}}
    assert ai["laws"]["lever"]["args"] == {"intent": "ai_veto", "args": {"op": "set", "what": "law"}}
    # The veto is THE lever for the AI's hires and buildings (gold_reserve did not stop a hire)
    assert ai["buildings_and_mercenaries"]["lever"]["args"]["intent"] == "ai_veto"


def test_news_reads_query_ai_veto():
    gw = Gw({"query_ai_veto": Veto([meath(1)]).read})
    got = run(SE.news_reads(gw, ME))
    assert len(got) == 3 and got[2]["records"][0]["key"] == "d_meath"


def test_a_law_with_no_recorded_post_is_not_claimed_for_the_veto():
    """a veto test run: top_liege_redirected_to_border_wars_law passed with no CAddLawCommand of the ruler's."""
    a = snap("1067.09.15", [], chr_sect={"gold": 0.0, "prestige": 700.0, "piety": 0.0})
    b = snap("1068.09.15", [meath(1, dropped=False)], chr_sect={"gold": 0.0, "prestige": 690.0, "piety": 0.0},
             laws={"active_laws": [{"name": "top_liege_redirected_to_border_wars_law"}]})
    out = two(a, b)
    (row,) = [e for e in out["purse"]["prestige"]["in_this_step"] if "border_wars" in e["what"]]
    assert "lever" not in row and "another way" in row["decided_by"]
