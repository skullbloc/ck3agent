"""What a read of a failed seated run found.
Every row here is built, not proven live; the
live proofs owed are listed in a research note

P1 a  every do intent refuses an argument it does not read (a recorded run: make_peace took end_war's `check` and
      surrendered a house-eradication war); make_peace has a real check that sends nothing.
P1 b  an event rule naming an option the event does not have is refused (a recorded run: 0302 -> 0301.a "applied").
"""
from __future__ import annotations

import ast
import asyncio
import json
import os
import pathlib
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

R = pytest.importorskip("redesign")
DIA = pytest.importorskip("do_intent_args")
SE = pytest.importorskip("seat_events")
FL = pytest.importorskip("faction_letters")

#: The game's files (CK3_GAME_DIR or the Steam default): the rows marked need_files read them.
need_files = pytest.mark.skipif(SE.game_dir() is None, reason="the game's files are not on this host")

ME, FOE, WAR = 36303, 34580, 268435540


def run(coro):
    return asyncio.run(coro)


class Gw:
    """A gateway that answers reads from `replies` and records every read and write."""

    def __init__(self, replies=None):
        self.replies = dict(replies or {})
        self.reads, self.writes = [], []
        self.health = "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        r = self.replies.get(action, {})
        return r(params) if callable(r) else r

    async def read_many(self, calls):
        return [await self.read(a, p) for a, p in calls]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return {"success": True}

    async def confirmed_write(self, action, params, **_kw):
        self.writes.append((action, dict(params or {})))
        return {"sent": False, "raw": {"success": False, "reason": "recipient_refuses"},
                "refused": "recipient_refuses"}

    def game(self):
        return {"tick": 1, "date": "1090.10.20", "paused": True}


# ── P1 a: the refusal, generated over every intent do_impl answers ─────────────────────────────────

ALL_INTENTS = sorted(set(R.INTENTS) | set(getattr(DIA, "ACCEPT_ONLY", {})))   # (superseded tree: collects, each row reds)


def test_1a_every_intent_do_impl_answers_has_an_accepted_list():
    acc = DIA.accepted()
    assert set(ALL_INTENTS) <= set(acc), sorted(set(ALL_INTENTS) - set(acc))
    assert "check" in acc["make_peace"] and "check" in acc["end_war"]


@pytest.mark.parametrize("intent", ALL_INTENTS)
def test_1a_every_intent_refuses_an_argument_it_does_not_read_before_any_read(intent):
    gw = Gw()
    acc = list(DIA.accepted()[intent])
    args = {k: None for k in acc[:1]}
    args["zz_not_an_argument"] = 1
    env = run(R.do_impl(gw, intent=intent, character_id=ME, args=args))
    assert env["verdict"] == "rejected" and env["blocked"]["gate"] == "unknown_argument", env
    assert env["blocked"]["fix"] == {"rejected_fields": ["zz_not_an_argument"], "accepted_fields": acc}
    assert "zz_not_an_argument" in env["blocked"]["summary"] and "Nothing was sent" in env["blocked"]["summary"]
    assert gw.reads == [] and gw.writes == []


@pytest.mark.parametrize("intent", ALL_INTENTS)
def test_1a_CONTROL_no_accepted_name_is_refused(intent):
    acc = DIA.accepted()[intent]
    assert DIA.unknown_args(intent, {k: None for k in acc}) == ([], list(acc))


def _do_path_keys() -> dict:
    """{key: [function]}: every string key read off the do call's args on do_impl's path in redesign.py,
    following each call that passes the args on (the enumerate mode, action_class, is not an intent)."""
    src = (HERE / "redesign.py").read_text(encoding="utf-8")
    fns = {n.name: n for n in ast.walk(ast.parse(src)) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))}
    out: dict = {}
    seen: set = set()

    def keys(fn, var):
        for n in ast.walk(fn):
            k = None
            if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr in ("get", "pop") \
                    and isinstance(n.func.value, ast.Name) and n.func.value.id == var and n.args \
                    and isinstance(n.args[0], ast.Constant) and isinstance(n.args[0].value, str):
                k = n.args[0].value
            elif isinstance(n, ast.Subscript) and isinstance(n.value, ast.Name) and n.value.id == var \
                    and isinstance(n.slice, ast.Constant) and isinstance(n.slice.value, str):
                k = n.slice.value
            elif isinstance(n, ast.Compare) and isinstance(n.left, ast.Constant) and isinstance(n.left.value, str) \
                    and isinstance(n.comparators[0], ast.Name) and n.comparators[0].id == var:
                k = n.left.value
            if k is not None:
                yield k

    def walk(name, var):
        if (name, var) in seen or name not in fns or name == "_do_enumerate":
            return
        seen.add((name, var))
        fn = fns[name]
        for k in keys(fn, var):
            out.setdefault(k, []).append(name)
        for n in ast.walk(fn):
            if not isinstance(n, ast.Call):
                continue
            cn = n.func.id if isinstance(n.func, ast.Name) else getattr(n.func, "attr", None)
            if cn not in fns:
                continue
            params = [a.arg for a in fns[cn].args.args]
            for i, arg in enumerate(n.args):
                passed = (isinstance(arg, ast.Name) and arg.id == var) or (
                    isinstance(arg, ast.Call) and isinstance(arg.func, ast.Name) and arg.func.id == "dict"
                    and arg.args and isinstance(arg.args[0], (ast.Name, ast.BoolOp)))
                if passed and i < len(params):
                    walk(cn, params[i])
            for kw in n.keywords:
                if isinstance(kw.value, ast.Name) and kw.value.id == var and kw.arg in params:
                    walk(cn, kw.arg)

    walk("do_impl", "args")
    walk("_do_impl", "args")
    walk("_do_impl", "a")
    return out


def test_1a_census_every_key_the_do_path_reads_is_accepted_by_an_intent():
    """Fails OPEN: a key a guard reads that no intent accepts would be refused for every call that needs it."""
    union = {k for v in DIA.accepted().values() for k in v} | {"actor_id"}
    found = _do_path_keys()
    assert len(found) >= 20, found                  # the walk found the path (a broken walk finds nothing)
    assert "confirm_leaves_realm" in found and "check" in found
    missing = {k: v for k, v in found.items() if k not in union}
    assert not missing, missing


def test_1a_census_every_do_call_the_server_hands_out_carries_only_accepted_args():
    """The hints, next calls and fixes the server writes ({"intent": X, "args": {...}} literals) would be refused
    by the rule if one named a key the intent does not read."""
    acc, n, bad = DIA.accepted(), 0, []
    for p in sorted(HERE.glob("*.py")):
        if p.name.startswith("test_"):
            continue
        for node in ast.walk(ast.parse(p.read_text(encoding="utf-8"))):
            if not isinstance(node, ast.Dict):
                continue
            kv = {k.value: v for k, v in zip(node.keys, node.values) if isinstance(k, ast.Constant)}
            it, a = kv.get("intent"), kv.get("args")
            if isinstance(it, ast.Constant) and isinstance(a, ast.Dict) and it.value in acc:
                n += 1
                bad += [(p.name, node.lineno, k.value) for k in a.keys
                        if isinstance(k, ast.Constant) and k.value not in acc[it.value]]
    assert n >= 50 and bad == [], (n, bad)


def test_1a_the_listing_names_make_peace_check():
    assert "make_peace: args.{war_id, terms=white_peace|surrender|enforce_demands, check}" in DIA.table_text()


def _war_gw(cb, atk=FOE, dfn=ME):
    return Gw({"query_wars": {"wars": [{"war_id": WAR, "name": "Eradication", "primary_attacker": atk,
                                        "primary_defender": dfn, "attackers": [atk], "defenders": [dfn]}]},
               "get_active_wars": {"wars": [{"war_id": WAR, "attacker_id": atk, "defender_id": dfn,
                                             "cb_type": cb, "target_titles": []}]},
               "query_character": {"id": ME, "gold": -100.72, "prestige": 1098.51,
                                   "income_breakdown": {"total_monthly": 3.9}},
               "query_war_score_breakdown": {"total_warscore": 60, "total_warscore_source": "engine_cached"}})


def test_1a_S48_make_peace_check_sends_nothing_and_says_the_defeat_takes_everything():
    gw = _war_gw("eradicate_house_cb")
    env = run(R.do_impl(gw, intent="make_peace", character_id=ME,
                        args={"war_id": WAR, "terms": "surrender", "check": True}))
    assert [a for a, _ in gw.writes] == [], gw.writes
    res = env["result"]
    assert env.get("verdict") is None and res["check"] is True and res["sent"] is False
    assert res["side"] == "defender" and res["casus_belli"] == "eradicate_house_cb"
    assert "LOSE EVERY county and duchy" in res["takes_everything"] and "DIE" in res["takes_everything"]
    assert "LOSE EVERY" in res["gives_up"]["you"]
    assert res["your_war_score"] == -60 and res["proof"] == "built, not proven live"
    assert res["send"]["args"]["args"] == {"war_id": WAR, "terms": "surrender"}


def test_1a_make_peace_check_on_a_claim_war_prices_the_yield_and_names_no_takes_all():
    gw = _war_gw("claim_cb", atk=ME, dfn=FOE)
    env = run(R.do_impl(gw, intent="make_peace", character_id=ME,
                        args={"war_id": WAR, "terms": "surrender", "check": True}))
    res = env["result"]
    assert gw.writes == [] and "takes_everything" not in res
    assert "loses the claim" in res["gives_up"]["you"]


def test_1a_make_peace_check_on_an_unpriced_casus_belli_says_unknown_never_free():
    gw = _war_gw("some_unpriced_cb", atk=ME, dfn=FOE)
    res = run(R.do_impl(gw, intent="make_peace", character_id=ME,
                        args={"war_id": WAR, "terms": "surrender", "check": True}))["result"]
    assert res["gives_up"]["price"] == "unknown" and gw.writes == []


def test_1a_a_surrender_sent_on_an_eradication_war_warns_in_plain_words():
    gw = _war_gw("eradicate_house_cb")
    env = run(R.do_impl(gw, intent="make_peace", character_id=ME, args={"war_id": WAR, "terms": "surrender"}))
    codes = {w["code"]: w["detail"] for w in env["warnings"]}
    assert "THIS WAR'S DEFEAT TAKES EVERYTHING" in codes.get("defeat_takes_everything", ""), codes


def test_1a_S48_a_preview_word_make_peace_does_not_take_is_refused_not_run():
    gw = _war_gw("eradicate_house_cb")
    env = run(R.do_impl(gw, intent="make_peace", character_id=ME,
                        args={"war_id": WAR, "terms": "surrender", "dry_run": True}))
    assert env["blocked"]["gate"] == "unknown_argument" and gw.writes == [] and gw.reads == []


def test_1a_end_war_refuses_a_stray_script_argument():
    gw = Gw()
    env = run(R.do_impl(gw, intent="end_war", character_id=ME,
                        args={"enemy_id": FOE, "terms": "we_win", "script": "add_gold = 1"}))
    assert env["blocked"]["fix"]["rejected_fields"] == ["script"] and gw.writes == []


def test_1a_declare_args_takes_exactly_a_plan_war_rows_keys():
    assert tuple(R.declare_args_of({"cb": "claim_cb"}, 5)) == R.DECLARE_ARGS_KEYS
    gw = Gw()
    env = run(R.declare_war_impl(gw, character_id=ME, declare_args={"target_id": FOE, "cb": "claim_cb",
                                                                    "cb_title_id": 0, "claimant_id": None,
                                                                    "check": True}))
    assert env["blocked"]["gate"] == "unknown_argument" and env["blocked"]["fix"]["rejected_fields"] == ["check"]
    assert gw.reads == [] and gw.writes == []


# ── P1 b: an option the event does not have ─────────────────────────────────────────────────────────

EVENTS = """namespace = court_chaplain_task
court_chaplain_task.0301 = {
\toption = { name = court_chaplain_task.0301.a }
\toption = { name = court_chaplain_task.0301.b }
}
court_chaplain_task.0302 = {
\toption = { name = court_chaplain_task.0302.a }
\toption = { name = court_chaplain_task.0302.b }
\toption = { name = court_chaplain_task.0301.b }
}
stress_threshold.1731 = {
\toption = { name = { trigger = { always = yes } desc = stress_threshold.1731.a } }
\toption = { trigger = { always = yes } }
}
"""


@pytest.fixture
def game_files(tmp_path, monkeypatch):
    (tmp_path / "events").mkdir()
    (tmp_path / "events" / "intents.txt").write_text(EVENTS, encoding="utf-8")
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path))
    for cache in (SE._EVENT_CACHE, getattr(FL, "_FILES", {})):
        cache.clear()
    yield tmp_path
    for cache in (SE._EVENT_CACHE, getattr(FL, "_FILES", {})):
        cache.clear()


def _rule(gw, **a):
    return run(SE.do_event_rule(gw, dict(op="set", answer="option", **a), ME, {"id": ME}))


def test_1b_S16_an_option_of_another_event_is_refused_with_the_events_options(game_files):
    gw = Gw()
    env = _rule(gw, event="court_chaplain_task.0302", option="court_chaplain_task.0301.a")
    assert env["verdict"] == "rejected" and env["blocked"]["raw_reason"] == "option_not_in_event", env
    assert "1=court_chaplain_task.0302.b" in env["blocked"]["summary"]
    assert [a for a, _ in gw.writes] == []


def test_1b_an_index_past_the_last_option_is_refused(game_files):
    gw = Gw()
    env = _rule(gw, event="court_chaplain_task.0301", option=2)
    assert env["blocked"]["raw_reason"] == "option_not_in_event" and gw.writes == []


def test_1b_CONTROL_the_events_own_option_and_index_are_sent(game_files):
    for opt in ("court_chaplain_task.0302.b", 2, "court_chaplain_task.0301.b"):
        gw = Gw({"query_event_rules": {"success": True}})
        _rule(gw, event="court_chaplain_task.0302", option=opt)
        assert [a for a, _ in gw.writes] == ["set_event_rule"], (opt, gw.writes)


def test_1b_CONTROL_an_event_whose_file_does_not_name_every_option_is_not_refused_by_key(game_files):
    """The stress events' options are named by the game when they fire (stress_threshold.1731.e.journaller.gain,
    a recorded run's rule, applied): the file cannot say, so the key goes to the bridge."""
    gw = Gw({"query_event_rules": {"success": True}})
    _rule(gw, event="stress_threshold.1731", option="stress_threshold.1731.e.journaller.gain")
    assert [a for a, _ in gw.writes] == ["set_event_rule"]


# ── P2 ──────────────────────────────────────────────────────────────────────────────────────────────
NEWS = pytest.importorskip("news")
CBL = pytest.importorskip("cb_labels")


def test_2d_an_ai_event_pick_that_spent_gold_is_attributed_in_the_purse():
    """1093.08: stress_threshold.1731 answered profligate by the AI, gold 257.61 at the pick, 162.41 at the read."""
    prev = {"actor_id": FOE, "date": "1093.08.01", "gold": 250.0, "monthly_income": 7.3}
    snap = {"actor_id": FOE, "date": "1093.09.01", "gold": 162.41, "monthly_income": 7.3}
    pick = {"date": "1093.08.10", "event": "stress_threshold.1731", "applied": "stress_threshold.1731.c.profligate.gain",
            "rule": None, "gold_at_pick": 257.61,
            "next": {"tool": "do", "args": {"intent": "event_rule", "args": {"op": "set"}}}}
    g = NEWS._resource_change(prev, snap, "gold", {"events_answered": [pick]})
    row = next(r for r in g["in_this_step"] if "stress_threshold.1731" in r["what"])
    assert "by the game's AI" in row["what"] and "profligate" in row["what"]
    assert row["expected"] == round(162.41 - 257.61 - 7.3 * 22 / 30.44, 2) and row["next"] == pick["next"]
    assert "built, not proven live" in row["basis"]
    assert abs(g.get("unexplained", 0)) < 1 or g["unexplained"] > -10, g


def test_2d_CONTROL_a_pick_your_rule_took_or_a_claim_letter_adds_no_ai_row():
    prev = {"actor_id": FOE, "date": "1093.08.01", "gold": 250.0, "monthly_income": 7.3}
    snap = {"actor_id": FOE, "date": "1093.09.01", "gold": 162.41}
    ruled = {"date": "1093.08.10", "event": "stress_threshold.1731", "applied": "x", "rule": {"answer": "option"},
             "gold_at_pick": 257.61}
    letter = {"date": "1093.08.10", "event": "court_chaplain_task.0301", "applied": "x", "gold_at_pick": 257.61}
    assert NEWS.ai_pick_gold(prev, snap, [ruled, letter]) == []


def test_2d_the_note_tells_the_seat_to_rule_stress_events_within_the_budgets():
    import notes as N
    doc = N.load()
    t1 = " ".join(N.turn_one_texts(doc))
    assert "event_rule on a stress event" in t1 and "Declare on each claim" in t1 and "not in debt" in t1
    ids = {n["id"] for n in doc["notes"]}
    assert "legitimacy-claim-price" not in ids and "chaplain-resets-after-claim" not in ids
    sizes = [len(json.dumps([N.render(n) for n in doc["notes"] if n["topic"] == t], ensure_ascii=False).encode())
             for t in doc["topics"]]
    assert sum(sizes) <= 7416 and N.problems(doc) == []


class BriefGw(Gw):
    def __init__(self, cbs_now, price_now, perks):
        super().__init__({"query_cbs_for_target": {"success": True, "cbs": cbs_now},
                          "query_create_title_cost": {"success": True, "engine_valid": True,
                                                      "price": {"known": True, "currency": "gold", "cost": price_now,
                                                                "have": 48.0}},
                          "query_lifestyle": {"lifestyles": [{"lifestyle": "diplomacy", "points": perks}],
                                              "perks": []}})


@need_files
def test_2e_the_first_news_after_a_ruler_change_briefs_cbs_price_perks_and_the_other_dynasty(monkeypatch):
    import notes as N
    monkeypatch.setattr(N, "SERVED", N.boot({}))
    R._CBS_SEEN.clear()
    R._CBS_SEEN[31762] = {29703: ("1068.01.01", ["claim_cb", "vassalization_cb"])}
    NEWS.note_creation_price(31762, "k_ireland", "gold", 325.0, "1068.01.01")
    gw = BriefGw([{"cb_key": "claim_cb", "valid": True}, {"cb_key": "vassalization_cb", "valid": False}], 425.0, 2)
    prev = {"actor_id": 31762, "dynasty_id": 100}
    snap = {"actor_id": 36303, "dynasty_id": 200}
    b = run(NEWS.ruler_change_briefing(gw, prev, snap, "k_ireland"))
    assert b["casus_belli"] == [{"target_id": 29703, "before": ["claim_cb", "vassalization_cb"], "read": "1068.01.01",
                                 "lost": ["vassalization_cb"], "gained": []}]
    assert b["price"]["was"] == 325.0 and b["price"]["now"] == 425.0 and "dies with him" in b["price"]["basis"]
    assert b["perk_points"] == {"diplomacy": 2} and "ducal_conquest_perk" in b["perk_note"]
    assert "not his blood" in b["another_dynasty"] and b["proof"] == "built, not proven live"
    assert ("query_cbs_for_target", {"attacker_id": 36303, "target_id": 29703, "timeout_ms": 6000}) in gw.reads


def test_2e_CONTROL_same_dynasty_and_no_cbs_read_say_so():
    R._CBS_SEEN.clear()
    b = run(NEWS.ruler_change_briefing(BriefGw([], 425.0, 0), {"actor_id": 1, "dynasty_id": 5},
                                       {"actor_id": 2, "dynasty_id": 5}, None))
    assert "another_dynasty" not in b and b["casus_belli"].startswith("unread") and "price" not in b


def test_2f_vassalization_refused_by_the_games_check_names_the_perk_and_what_is_unread():
    gw = Gw({"query_lifestyle": {"perks": ["thoughtful_perk"]}})
    opts = [{"cb": "vassalization_cb", "valid": False, "requirements_unmet": ["engine_cb_validity_check_failed"]},
            {"cb": "claim_cb", "valid": False, "requirements_unmet": ["engine_cb_validity_check_failed"]}]
    out = run(CBL.engine_failed_rows(gw, ME, opts))
    assert out["vassalization_cb"]["forced_vassalage_perk"] is False
    assert "conqueror" in out["vassalization_cb"]["else_unread"] and "no condition named" in out["others"]
    assert run(CBL.engine_failed_rows(gw, ME, [{"cb": "claim_cb", "valid": True}])) is None


@need_files
def test_2g_an_always_pay_letter_rule_below_the_letters_price_never_pays_into_debt():
    """The remedy: an always-pay letter rule is set as pay_if at the letter's price (the free option when
    the gold is short), so it cannot pay the letter from 9 gold; without the game's files (no free option to name) it
    is refused and nothing is set. Either way no rule that pays from a purse below the price is written."""
    gw = Gw({"query_council": {"council": [{"position_key": "councillor_court_chaplain", "holder_id": 9,
                                            "skills": {"learning": 12}}]},
             "query_character": {"gold": 9.0, "income_breakdown": {"total_monthly": 2.4}},
             "query_armies": {"armies": []}})
    for ev in ("court_chaplain_task.0301", "court_chaplain_task.0302"):
        env = run(SE.do_event_rule(gw, {"op": "set", "event": ev, "answer": "option", "option": ev + ".a"}, ME, None))
        if SE.game_dir() is None:
            assert env["verdict"] == "rejected" and env["blocked"]["raw_reason"] == "letter_floor_unread", env
        else:
            assert env["result"]["event_check"]["always_pay_floor"]["letter_costs"] == 84.0, env
    sent = [p for a, p in gw.writes if a == "set_event_rule"]
    assert all(p["answer"] == "pay_if" and p["amount"] >= 84 for p in sent), sent


def test_2h_a_dukes_mercenary_price_carries_his_rank_and_affordable_reads_the_upper_bound(monkeypatch):
    """S7 1068.12: company 157 listed 230.98, charged 254.0 to Brian, a duke (duke_modifier +10%)."""
    monkeypatch.setattr(R, "merc_price", lambda *_a, **_k: 230.98)
    gw = Gw({"query_hireable_mercenaries": {"companies": [{"company_id": 157, "in_range": True, "regiments": []}]},
             "query_character": {"id": ME, "gold": 233.0, "ruler_tier": 3,
                                 "income_breakdown": {"total_monthly": 2.17}},
             "query_realm": {"realm_total_counties": 5}, "query_maa_types": {"types": []}})
    rows, _far, note = run(R.mercenary_rows(gw, ME))
    r = rows[0]
    assert r["price_estimate"] == round(230.98 * 1.1, 2) and r["price_upper"] == round(230.98 * 1.25, 2)
    assert r["debt_after_hire"]["gold_after"] == round(233.0 - 230.98 * 1.25, 2)
    assert "duke +10%" in note and "price_upper" in note


def test_2i_the_price_row_basis_and_the_realised_net():
    pb = {"base": 500, "discount": 0.35, "sources": [{"source": "perk diplomat", "gold": -175.0}]}
    w = NEWS.price_basis_words(pb)
    assert w.startswith("base 500 less 35.0%") and "dies with him" in w
    assert NEWS.price_basis_words({"unread": "x"}) == "unread: x"
    prev = {"actor_id": 77, "date": "1072.01.01", "gold": 10.0, "monthly_income": 2.5}
    snap = {"actor_id": 77, "date": "1073.01.01", "gold": -14.0}
    NEWS._resource_change(prev, snap, "gold")
    assert NEWS.realised_net(77)["monthly"] == round(-24.0 / (365 / 30.44), 2)


def test_2j_plan_war_reads_the_claim_as_cbs_does():
    purse = {"claims": {501: "unpressed", 502: "pressed"}}
    assert R.claim_word_pressed(purse, 501, True) is False and R.claim_word_pressed(purse, 502, False) is True
    assert R.claim_word_pressed(purse, 503, True) is True          # not named by the claims read: the row's own


def test_2j_appoint_unchanged_names_the_seats_holder_and_the_candidates_seat():
    gw = Gw({"query_council": {"council": [
        {"position_key": "councillor_steward", "position_id": 7108, "holder_id": 15611},
        {"position_key": "councillor_marshal", "position_id": 7107, "holder_id": 36742}]}})
    env = {"blocked": {"raw_reason": "seat_holder_unchanged", "gate": "already_in_that_state"}}
    run(R._explain_seat_unchanged(gw, env, {"character_id": 36742, "position_id": 7108}, FOE))
    b = env["blocked"]
    assert b["gate"] == "engine_refused" and "15611" in b["summary"] and "councillor_marshal" in b["summary"]


def test_2j_control_refused_by_the_realm_guard_is_named():
    for code in ("realm_unresolved", "heir_outside_realm"):
        assert R.control_refusal_reason({"success": False, "reason": code}) == code
        assert R.CONTROL_REFUSAL_SUMMARIES[code]


def test_3_a_war_whose_defeat_takes_everything_says_so_where_it_starts():
    assert "LOSE EVERY" in NEWS._takes_all({"cb_type": "eradicate_house_cb", "is_attacker": False})["if_you_lose"]
    assert NEWS._takes_all({"cb_type": "eradicate_house_cb", "is_attacker": True}) == {}
    assert NEWS._takes_all({"cb_type": "claim_cb", "is_attacker": False}) == {}
