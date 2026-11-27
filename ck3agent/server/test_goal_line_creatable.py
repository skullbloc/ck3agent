"""Rows from the second seated Sweden 867 run, replayed from its recorded replies.

  1  the goal line's months: "no step read yet" at five steps   title_requirements(news_actor): the bound ruler's id
  2  the goal line gone when k_sweden could be created         goal_line.creatable_now
  3  inspect title and cbs cut, not paged (cbs 1,881 B unused)  reply_fit.PAGED_CALLS; fit's step 7 (partial_rows)
  4  the spending veto dropped the AI's creation of k_sweden    the goal title spared (bridge spare_title_id)
  5  prestige "unexplained -298.31" beside its own match        news._resource_change: a matched price is explained
  6  a claim row declarable while armies were raised            declare_gates.failing: armies_raised refuses

The recorded replies are in fixtures/sweden867_goal_line_replies.json (see its _doc). No game.
"""
from __future__ import annotations

import asyncio
import copy
import json
import os
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))

import declare_gates as DG  # noqa: E402
import news as N  # noqa: E402
import redesign as R  # noqa: E402
import reply_fit as F  # noqa: E402
import seat_events as SE  # noqa: E402

FX = json.loads((HERE / "fixtures" / "sweden867_goal_line_replies.json").read_text())
BJORN, K_SWEDEN = 12330, 2416


def run(c):
    return asyncio.run(c)


# ── 1. the months: the bound ruler's step, not character 0's ────────────────────────────────────────────────────

def _formable(monkeypatch, realised):
    import test_mint_price as TM
    monkeypatch.setattr(N, "_REALISED", dict(realised))
    gw = TM.Gateway(TM.price_reply(cost=150.0, have=45.5, engine_valid=False,
                                   reasons=["Björn does not have gold 150"]))
    env = run(R.do_impl(gw, action_class="formable_titles", character_id=0, candidates=[TM.KEY]))
    rows = env["result"]["available"] + env["result"]["unavailable"]
    return rows[0], TM.MURCHAD


def test_1_the_one_case_left_unread_is_no_step_read_yet(monkeypatch):
    """CONTROL: before any step (the first turn) nothing is known: the line says so, it is not made up."""
    row, _ = _formable(monkeypatch, {})
    price = next(q for q in row["requirements"] if q["requirement"] == "price")
    assert price["months_to_target"]["months"] is None and price["months_to_target"]["why"].startswith("no step")


def test_1_RED_the_base_looked_the_step_up_under_character_0(monkeypatch):
    """The recorded defect: the step is kept under the ruler's id; a lookup by 0 (the base) finds nothing."""
    import test_mint_price as TM
    monkeypatch.setattr(N, "_REALISED", {TM.MURCHAD: {"monthly": 4.38, "from": "0868.05.22", "to": "0868.11.18"}})
    assert N.realised_net(0) is None and R.months_to_target(104.5, N.realised_net(0))["months"] is None


# ── 2. the goal line when the title can be created now ─────────────────────────────────────────────────────────

ROW_T9 = {"title_key": "k_sweden", "do_call": {"intent": "mint_title", "args": {"title_key": "k_sweden"}},
          "blocked": None,
          "price_basis": {"at_succession": {"price": 250.0, "rises": 100.0,
                                            "why": "your own ducal_conquest_perk discount dies with you; create it "
                                                   "before your death to pay 150.0"}},
          "requirements": [{"requirement": "de_jure_counties", "have": 14, "need": 13, "of": 25, "met": True},
                           {"requirement": "price", "currency": "gold", "cost": 150.0, "have": 159.1, "met": True},
                           {"requirement": "game_check", "met": True, "reasons": []},
                           {"requirement": "tier_rule", "met": True, "rule": "met"}]}


# ── 3. inspect title and cbs are paged; a remaining cut uses the room it has ───────────────────────────────────

def _rebuilt(name: str) -> dict:
    """The recorded reply with each cut list grown back to its recorded length from its shown rows (the rows not
    shown were not recorded: each added row is a shown row with its key, ids and names marked as a copy)."""
    rec = copy.deepcopy(FX[name])
    res = rec["result"]
    for lst, n in rec["of"].items():
        rows = res[lst]
        src = [r for r in rows if isinstance(r, dict)]
        i = 0
        while len(rows) < n:
            r = copy.deepcopy(src[i % len(src)])
            for k in ("key", "cb", "name", "title_key"):
                if isinstance(r.get(k), str):
                    r[k] = f"{r[k]}_copy{len(rows)}"
            for k in ("title_id", "cb_title_id"):
                if isinstance(r.get(k), int):
                    r[k] = r[k] + 100000 + len(rows)
            rows.append(r)
            i += 1
    return res


def _env(tool: str, res: dict) -> dict:
    return {"ok": True, "tool": tool, "result": res, "blocked": None, "warnings": [], "errors": [],
            "actor": {"id": BJORN, "name": "Björn", "alive": True, "is_controlled": True, "is_player": False},
            "game": {"date": "0867.01.01", "paused": True, "speed": 0}, "bridge_health": "ok"}


def _all_pages(first: dict, tool: str, args: dict) -> list:
    pages = [first]
    while "more" in pages[-1]["result"] and pages[-1]["result"]["more"].get("next"):
        nxt = F.next_page()
        assert nxt.get("ok") is not False, nxt
        pages.append(F.delivered(nxt, "inspect", {"subject": "more"}))
    return pages


@pytest.mark.parametrize("name,tool,args,lst,idf", [
    ("inspect_title_t1", "inspect", {"subject": "title", "title_key": "k_sweden"}, "counties", "key"),
    ("cbs_t7", "cbs", {"target_id": 14128}, "valid_options", "cb")])
def test_3_the_reply_is_paged_and_every_row_is_on_a_page(monkeypatch, name, tool, args, lst, idf):
    monkeypatch.delenv(F.PAGES_ENV, raising=False)
    res = _rebuilt(name)
    env = _env(tool, res)
    assert F.size(env) > F.CAP                                        # over the cap, as recorded
    first = F.delivered(F.sent(copy.deepcopy(env), tool, args), tool, args)
    assert "truncated" not in first["result"] and first["result"]["more"]["page"].startswith("1 of ")
    pages = _all_pages(first, tool, args)
    got = [r[idf] for p in pages for r in p["result"].get(lst) or []]
    assert sorted(got) == sorted(r[idf] for r in res[lst]) and len(got) == len(res[lst])
    for p in pages:
        assert F.size(p) + F.wrapper_bytes(tool) <= F.CAP
    assert F.size(first) + F.wrapper_bytes(tool) > F.CAP - 2000        # the first page uses its room


def test_3_RED_the_base_paged_neither():
    for tool, args in (("inspect", {"subject": "title"}), ("cbs", {"target_id": 14128})):
        assert tool not in F.PAGED and F.paged_key(tool, args)          # the base's rule was `tool in PAGED`


def test_3_inspect_more_and_other_subjects_are_not_paged():
    assert F.paged_key("inspect", {"subject": "more"}) is None and F.paged_key("inspect", {"subject": "map"}) is None
    assert F.paged_key("observe", {"depth": "news"}) == "observe"


def test_3_a_cut_left_with_room_adds_the_next_row_fit_to_it(monkeypatch):
    """Pages off (the cut path): the recorded cbs cut at 6,311 bytes with 1,881 left; now the next row goes in, itself
    fit, so the reply uses the room it has."""
    monkeypatch.setenv(F.PAGES_ENV, "off")
    res = _rebuilt("cbs_t7")
    out = F.delivered(F.sent(_env("cbs", res), "cbs", {"target_id": 14128}), "cbs", {"target_id": 14128})
    t = out["result"]["truncated"]
    (row,) = [x for x in t["lists"] if x["list"] == "valid_options"]
    assert row["shown"] == 2 and row.get("last_row_cut") is True and row["of"] == 4
    part = out["result"]["valid_options"][1]
    assert part["cb"] == res["valid_options"][1]["cb"] and part["not_shown"] and part["valid"] is True
    assert F.CAP - 600 < F.size(out) + F.wrapper_bytes("cbs") <= F.CAP


# ── 4. the spending veto spares the goal title ────────────────────────────────────────────────────────────────

class VetoGw:
    def __init__(self, spare_back=True):
        self.writes, self.reads, self.spare_back, self.health = [], [], spare_back, "ok"
        self.table = {"vetoes": [], "decision_keys": [], "spare_title_id": 0}

    def game(self):
        return {"date": "0867.01.01", "paused": True}

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if p.get("op") == "set" and p["what"] not in self.table["vetoes"]:
            self.table["vetoes"].append(p["what"])
        if p.get("what") == "create_title" and self.spare_back:
            self.table["spare_title_id"] = int(p.get("spare_title_id") or 0)
        return {"success": True}

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_de_jure":
            return {"title": {"title_id": K_SWEDEN, "key": "k_sweden", "name": "Sweden"}}
        if action == "query_ai_veto":
            ruler = {"character_id": BJORN, **copy.deepcopy(self.table)}
            if not self.spare_back:
                ruler.pop("spare_title_id")
            return {"success": True, "rulers": [ruler], "records": []}
        return {}


def _spending(gw):
    return run(SE.do_ai_veto(gw, {"op": "set", "what": "spending"}, BJORN, {"id": BJORN}))


def test_4_the_preset_spares_the_goal_title_and_says_so(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_sweden")
    gw = VetoGw()
    env = _spending(gw)
    assert env["verdict"] == "applied", env
    ct = [p for a, p in gw.writes if p.get("what") == "create_title"]
    assert ct == [{"character_id": BJORN, "op": "set", "what": "create_title", "spare_title_id": K_SWEDEN}]
    assert all("spare_title_id" not in p for a, p in gw.writes if p.get("what") != "create_title")
    g = env["result"]["goal_title"]
    assert g["spared"] is True and g["title_id"] == K_SWEDEN and "never drops it" in g["why"]


def test_4_an_old_bridge_that_cannot_spare_it_is_said_not_hidden(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_sweden")
    env = _spending(VetoGw(spare_back=False))
    g = env["result"]["goal_title"]
    assert g["spared"] is False and "drops the AI's creation of it too" in g["why"]


def test_4_CONTROL_no_goal_title_no_spare_and_the_reply_as_before(monkeypatch):
    monkeypatch.delenv(SE.GOAL_TITLE_ENV, raising=False)
    gw = VetoGw()
    env = _spending(gw)
    assert all("spare_title_id" not in p for _a, p in gw.writes) and "goal_title" not in env["result"]
    assert not [a for a, _p in gw.reads if a == "query_de_jure"]


def test_4_the_direct_create_title_veto_spares_it_too(monkeypatch):
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_sweden")
    gw = VetoGw()
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "create_title"}, BJORN, {"id": BJORN}))
    assert gw.writes[-1][1]["spare_title_id"] == K_SWEDEN and env["result"]["goal_title"]["spared"] is True
    gw = VetoGw()
    run(SE.do_ai_veto(gw, {"op": "clear", "what": "create_title"}, BJORN, {"id": BJORN}))
    assert "spare_title_id" not in gw.writes[-1][1]


def _rec(seq, dropped, key="k_sweden"):
    return {"seq": seq, "character_id": BJORN, "what": "create_title", "key": key, "title_id": K_SWEDEN,
            "dropped": dropped, "origin": "game", "date": "0869.05.07", "gold": 152.73, "prestige": 1213.77,
            "price": {"gold": 150.0}, "priced": "yes"}


def test_4_the_news_names_the_goal_titles_drop_and_its_pass(monkeypatch):
    """The recorded drop (0869.05.07, 152.73 gold) as the news shows it now; and the same post let through."""
    monkeypatch.setenv(SE.GOAL_TITLE_ENV, "k_sweden")
    (row,) = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, True)]})["ai_vetoed"]
    assert row["goal_title"] is True and "your goal title" in row["note"] and row["key"] == "k_sweden"
    (acted,) = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, False)]})["ai_acted"]
    assert acted["goal_title"] is True and "never drops" in acted["sent_by"] and "veto" not in acted
    (other,) = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [_rec(1, True, "d_uppland")]})["ai_vetoed"]
    assert "goal_title" not in other


# ── 5. a matched price is not also unexplained ────────────────────────────────────────────────────────────────

def test_5_the_step_10_prestige_matches_the_decision_and_is_not_unexplained(monkeypatch):
    s = FX["prestige_step10"]
    monkeypatch.setattr(N, "_ACTS", {BJORN: [{"date": a["date"], "what": a["what"], "cost_known": True,
                                              "prestige": a["prestige"]} for a in s["your_acts"]]})
    prev = {"actor_id": BJORN, "date": s["from"], "prestige": s["was"], "monthly_prestige": s["monthly_prestige"]}
    snap = {"actor_id": BJORN, "date": s["to"], "prestige": s["now"]}
    out = N._resource_change(prev, snap, "prestige", {"ai_acted": s["ai_acted"]})
    # the AI's priced post is a priced row of the step (ai_acted_rows), subtracted whatever its amount
    assert [e["expected"] for e in out["in_this_step"] if "ai_acted" in e["what"]] == [-300.0]
    assert "unexplained" not in out and "cause" not in out, out


def test_5_what_is_left_after_a_match_stays_unexplained(monkeypatch):
    """CONTROL: the match takes its own price only. With a larger income (the match's tolerance is the step's income)
    a 40 left after the 300 stays unexplained."""
    s = FX["prestige_step10"]
    monkeypatch.setattr(N, "_ACTS", {BJORN: [{"date": s["from"], "what": "mint_title k_sweden", "cost_known": True,
                                              "prestige": 400.0}]})
    days = N._ordinal_of(s["to"]) - N._ordinal_of(s["from"])
    prev = {"actor_id": BJORN, "date": s["from"], "prestige": s["was"], "monthly_prestige": 30.44}
    snap = {"actor_id": BJORN, "date": s["to"], "prestige": round(s["was"] + 400.0 + days - 260.0, 2)}
    out = N._resource_change(prev, snap, "prestige", {"ai_acted": s["ai_acted"]})
    assert [e["expected"] for e in out["in_this_step"] if "ai_acted" in e["what"]] == [-300.0]
    assert abs(out["unexplained"] - 40.0) < 0.02, out


# ── 6. raised armies: the claim row and the declaration read one check ────────────────────────────────────────

def test_6_the_recorded_cbs_option_with_armies_raised_is_not_declarable():
    """Turn 7's claim_cb row read valid with may_refuse armies 1,741; the same purse now refuses it in blocked_by."""
    opt = copy.deepcopy(FX["cbs_t7"]["result"]["valid_options"][0])
    for k in ("may_refuse", "blocked_by", "checked", "prestige_if"):
        opt.pop(k, None)
    purse = {"gold": 160.0, "prestige": 1335.7, "piety": 500.0,
             "armies": {"raised": True, "armies": 1, "men": 1741, "army_ids": [136]}}
    DG.annotate([opt], purse, {"in_truce": False})
    assert not R.cb_option_declarable(opt) and opt["blocked_by"][0]["condition"] == "armies_raised"
    purse["armies"] = {"raised": False, "armies": 0, "men": 0, "army_ids": []}
    DG.annotate([opt], purse, {"in_truce": False})
    assert R.cb_option_declarable(opt) and "blocked_by" not in opt                 # CONTROL


def test_3_live_the_counties_start_on_the_first_page(monkeypatch):
    """Live 0867.01.01 (runs/s867): the 25 counties went whole to page 2 and page 1 was sent at 5,467 bytes. Now the
    list starts on page 1 where the room is and goes on to page 2, every row once."""
    monkeypatch.delenv(F.PAGES_ENV, raising=False)
    res = copy.deepcopy(FX["inspect_title_live_0867"]["result"])
    args = {"subject": "title", "title_key": "k_sweden"}
    first = F.delivered(F.sent(_env("inspect", copy.deepcopy(res)), "inspect", args), "inspect", args)
    assert first["result"].get("counties"), "no county on page 1"
    assert F.size(first) + F.wrapper_bytes("inspect") > F.CAP - 1200
    pages = _all_pages(first, "inspect", args)
    keys = [r["key"] for p in pages for r in p["result"].get("counties") or []]
    assert keys == [r["key"] for r in res["counties"]]
    # RED: the layout without the split (the base pager) puts no county on page 1
    monkeypatch.setattr(F, "PAGE_SPLIT", ())
    base = F.delivered(F.sent(_env("inspect", copy.deepcopy(res)), "inspect", args), "inspect", args)
    assert not base["result"].get("counties") and F.size(base) < 6000


def test_3_live_the_summary_the_bench_reads_stays_on_the_first_page(monkeypatch):
    """Live (0869.01.01): with the counties split onto page 1 the summary went to page 2, and a harness's
    de-jure metric (inspect title summary.held, Ck3Backend._target_reads) read unreadable: the episode was
    truncated at its first step. The summary and the title id lead page 1."""
    monkeypatch.delenv(F.PAGES_ENV, raising=False)
    res = copy.deepcopy(FX["inspect_title_live_0867"]["result"])
    args = {"subject": "title", "title_key": "k_sweden", "character_id": BJORN}
    first = F.delivered(F.sent(_env("inspect", copy.deepcopy(res)), "inspect", args), "inspect", args)
    assert "more" in first["result"]
    assert first["result"]["summary"] == res["summary"] and first["result"]["title_id"] == res["title_id"]
    assert isinstance(first["result"]["summary"].get("held"), int)
    monkeypatch.setitem(F.PAGE_ORDER, "inspect:title", [k for k in F.PAGE_ORDER["inspect:title"] if k != "summary"])
    bad = F.delivered(F.sent(_env("inspect", copy.deepcopy(res)), "inspect", args), "inspect", args)
    assert "summary" not in bad["result"]                                   # RED: the order that failed live
