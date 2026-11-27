"""Ruler-change vetoes and mint rows, each reproduced from
a recorded reply.

The fixture (fixtures/ruler_change_veto_replies.json.gz, built)
holds whole tool replies and step news recorded in that run; `line` is the record's line in the evidence's
calls file. The "recorded" tests read what the run's toolkit answered (they hold with or without the
rules); the others drive the code and fail without the rules.
"""
import asyncio
import gzip
import json
from pathlib import Path

import pytest

import activities as A
import army_orders as AO
import declare_gates as DG
import news
import redesign as R
import seat_events as SE
from test_ireland_walk_replies import Gw
from test_activity_income_lever import COUNT, ScriptGw

HERE = Path(__file__).resolve().parent
LIVE = json.loads(gzip.open(HERE / "fixtures" / "ruler_change_veto_replies.json.gz", "rt").read())
MURCHAD, FELIC = 31506, 55606
ON = {"CK3_BRIDGE_ARMY_ORDERS": "1", "CK3_BRIDGE_ARMY_HOLD": "1"}


def run(c):
    return asyncio.run(c)


def _br():
    import buildings_read  # noqa: PLC0415 — the module: absent in a tree without the rule, so each row reds alone
    return buildings_read


def env_of(name):
    r = LIVE[name]["reply"]
    return r["result"] if isinstance(r, dict) and "tool_name" in r else r


def news_of(name):
    return LIVE[name]["news"]


@pytest.fixture(autouse=True)
def _fresh(monkeypatch):
    news.reset()
    for k in ON:
        monkeypatch.delenv(k, raising=False)

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    yield
    news.reset()


def snap(date, actor=MURCHAD, **kw):
    base = dict(actor_id=actor, date=date, chr_sect={"gold": 0.0, "prestige": 0.0, "piety": 0.0},
                wars={"wars": []}, claims={"claims": []}, council={"council": []}, heir=None,
                schemes={"schemes": []}, held={"held_titles": []}, council_tasks=[], laws={"active_laws": []},
                alliances={"alliances": []})
    base.update(kw)
    return news.snapshot(**base)


def veto_raw(me, vetoes, records=()):
    return {"success": True, "hook_installed": True, "rulers": [{"character_id": me, "vetoes": list(vetoes)}],
            "records": list(records)}


# ── a veto is the seat's, and follows it to the new ruler ─────────────────────

def test_1a_recorded_the_veto_was_the_old_rulers_and_the_law_passed_for_the_new_one():
    assert env_of("veto_set_1066_09_15")["result"]["vetoes"] == ["law"]
    rc = news_of("news_1079_12_31")["ruler_changed"]
    assert (rc["from"], rc["to"]) == (MURCHAD, FELIC) and "ai_veto_carried" not in rc
    (acted,) = news_of("news_1080_06_29")["ai_acted"]
    assert (acted["key"], acted["price"]) == ("crown_authority_1", {"prestige": 159.0})
    assert "no veto set" in acted["sent_by"]


class VetoGw(Gw):
    """A bridge veto table: set_ai_veto writes it, query_ai_veto reads it."""

    def __init__(self, table, **kw):
        super().__init__(**kw)
        self.table = table

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if action == "set_ai_veto":
            self.table.setdefault(p["character_id"], [])
            if p["what"] not in self.table[p["character_id"]]:
                self.table[p["character_id"]].append(p["what"])
        return {"success": True}

    async def read(self, action, params=None):
        p = dict(params or {})
        if action == "query_ai_veto":
            self.reads.append((action, p))
            me = p.get("character_id")
            return veto_raw(me, self.table.get(me, []))
        return await super().read(action, params)


def test_1a_a_ruler_change_sets_the_seats_vetoes_for_the_new_ruler_and_says_so():
    table = {MURCHAD: ["law"]}
    gw = VetoGw(table, date="1079.11.16")
    run(news.block(gw, 0, snap("1079.11.16", ai_veto=veto_raw(MURCHAD, ["law"])), offers_served=False,
                   baseline="turn"))
    n = run(news.block(gw, 0, snap("1079.12.31", actor=FELIC, ai_veto=veto_raw(FELIC, [])), offers_served=False,
                       baseline="turn"))
    c = n["ruler_changed"]["ai_veto_carried"]
    assert (c["from"], c["to"], c["vetoes"], c["carried"]) == (MURCHAD, FELIC, ["law"], ["law"])
    assert ("set_ai_veto", {"character_id": FELIC, "op": "set", "what": "law"}) in gw.writes
    assert table[FELIC] == ["law"]
    # the seat's own baseline reads the same row, and nothing is sent twice
    run(news.block(gw, 0, snap("1079.11.16", ai_veto=veto_raw(MURCHAD, ["law"])), offers_served=False))
    n2 = run(news.block(gw, 0, snap("1079.12.31", actor=FELIC, ai_veto=veto_raw(FELIC, ["law"])),
                        offers_served=False))
    assert n2["ruler_changed"]["ai_veto_carried"]["carried"] == ["law"]
    assert len([w for w in gw.writes if w[0] == "set_ai_veto"]) == 1


def test_1a_a_refused_carry_names_the_call_that_sets_it():
    class Refusing(VetoGw):
        async def write(self, action, params=None):
            self.writes.append((action, dict(params or {})))
            return {"success": False, "reason": "not_a_controlled_character"}
    gw = Refusing({MURCHAD: ["law", "create_title"]})
    run(news.block(gw, 0, snap("1079.11.16", ai_veto=veto_raw(MURCHAD, ["law", "create_title"])),
                   offers_served=False))
    n = run(news.block(gw, 0, snap("1079.12.31", actor=FELIC, ai_veto=veto_raw(FELIC, [])), offers_served=False))
    c = n["ruler_changed"]["ai_veto_carried"]
    assert c["carried"] == [] and c["not_carried"] == {"law": "not_a_controlled_character",
                                                      "create_title": "not_a_controlled_character"}
    assert c["next"] == news.ai_veto_call("law")


def test_1a_CONTROL_no_veto_held_nothing_is_set():
    gw = VetoGw({})
    run(news.block(gw, 0, snap("1079.11.16", ai_veto=veto_raw(MURCHAD, [])), offers_served=False))
    n = run(news.block(gw, 0, snap("1079.12.31", actor=FELIC, ai_veto=veto_raw(FELIC, [])), offers_served=False))
    assert "ai_veto_carried" not in n["ruler_changed"] and gw.writes == []


# ── the unset veto whose spend can come now ──────────────────────────────────

def test_1b_recorded_the_mint_came_with_no_row_before_it():
    (acted,) = news_of("news_1077_05_14")["ai_acted"]
    assert (acted["key"], acted["price"], acted["gold_at_attempt"]) == ("d_meath", {"gold": 212.5}, 4.81)
    assert "ai_veto_unset" not in news_of("news_1077_05_14")


def test_1b_a_count_at_his_domain_limit_with_no_title_veto_reads_the_row_with_the_call():
    news.note_creation_price(MURCHAD, "d_meath", "gold", 212.5, "1076.05.14")
    s = snap("1076.05.14", chr_sect={"gold": 4.81, "prestige": 353.27, "piety": 0.0, "ruler_tier": 2},
             ai_veto=veto_raw(MURCHAD, ["law"]), realm={"domain_size": 5, "domain_limit": 4})
    n = run(news.block(Gw(), 0, s, offers_served=False))
    row = n["ai_veto_unset"]["create_title"]
    assert row["next"] == {"tool": "do", "args": {"intent": "ai_veto", "args": {"op": "set", "what": "create_title"}}}
    assert row["price"]["gold"] == 212.5 and row["gold"] == 4.81 and row["prestige"] == 353.27
    assert row["domain"] == {"held": 5, "limit": 4}
    assert "law" not in n["ai_veto_unset"]
    # the per-turn form keeps it
    assert news.compact(n)["ai_veto_unset"]["create_title"]["next"] == row["next"]
    # AT the limit already: one more county and the check skips the gold
    s = snap("1076.05.14", chr_sect={"gold": 4.81, "prestige": 353.27, "piety": 0.0, "ruler_tier": 2},
             ai_veto=veto_raw(MURCHAD, ["law"]), realm={"domain_size": 4, "domain_limit": 4})
    assert news.veto_unset(s)["create_title"]["domain"] == {"held": 4, "limit": 4}


def test_1b_an_unvetoed_law_the_ai_raises_reads_the_row_with_its_last_price():
    rec = {"seq": 1, "date": "1067.01.01", "character_id": MURCHAD, "what": "law", "dropped": True,
           "key": "crown_authority_1", "priced": "yes", "price": {"gold": 0, "prestige": 101.0, "piety": 0}}
    s = snap("1067.09.15", chr_sect={"gold": 84.0, "prestige": 300.0, "piety": 0.0, "ruler_tier": 2},
             ai_veto=veto_raw(MURCHAD, [], [rec]), laws={"active_laws": [{"name": "crown_authority_0"}]})
    n = run(news.block(Gw(), 0, s, offers_served=False))
    row = n["ai_veto_unset"]["law"]
    assert (row["law"], row["level"], row["max_level"]) == ("crown_authority_0", 0, 3)
    assert row["price"]["prestige"] == 101.0 and row["next"] == news.ai_veto_call("law")


def test_1b_CONTROL_vetoed_or_within_the_limit_no_row():
    s = snap("1076.05.14", chr_sect={"gold": 4.81, "prestige": 353.0, "piety": 0.0, "ruler_tier": 2},
             ai_veto=veto_raw(MURCHAD, ["law", "create_title"]), realm={"domain_size": 5, "domain_limit": 4},
             laws={"active_laws": [{"name": "crown_authority_0"}]})
    assert news.veto_unset(s) is None
    s = snap("1076.05.14", chr_sect={"gold": 4.81, "prestige": 353.0, "piety": 0.0, "ruler_tier": 2},
             ai_veto=veto_raw(MURCHAD, ["law"]), realm={"domain_size": 3, "domain_limit": 4})
    assert news.veto_unset(s) is None
    # the vetoes not read: no row, never a guess
    assert news.veto_unset(snap("1076.05.14", realm={"domain_size": 5, "domain_limit": 4})) is None


def test_1b_the_run_deciding_notes_are_in_the_topic_seats_opened_first():
    import notes as N
    doc = N.load(N.NOTES_FILE)
    assert N.problems(doc) == []
    first = list(doc["topics"])[0]
    assert first == "claims"     # the topic both seats that read notes opened on turn one (a recorded game)
    ids = [n["id"] for n in doc["notes"] if n["topic"] == first]
    for want in ("ai-spends-for-you", "claim-dies-with-ruler", "raised-armies-block-declares",
                 "ai-may-create-title"):
        assert want in ids, want
    text = {n["id"]: n["text"] for n in doc["notes"]}
    # create_title is vetoed by a count only (a duke's AI mints his second duchy with gold)
    # In one test series every seat vetoes create_title on turn one and mints its duchies and k_ireland itself (the AI
    # spent the ruler's gold on d_meath and k_ireland at its own time; the seat's own mint_title proven live)
    # The preset names them in one call (create_title in seat_events.AI_VETO_PRESETS["spending"])
    assert "do ai_veto what=spending" in text["ai-spends-for-you"]
    assert "for as long as they stand" in text["raised-armies-block-declares"]
    assert "cbs names the call" in text["raised-armies-block-declares"]    # No may_refuse field now


# ── The raised-armies refusal names the lever that is listed ───────────────────

def test_2_recorded_three_refusals_said_no_tool_disbands_with_army_listed():
    for k in ("declare_armies_1081_12_12", "declare_armies_1090_12_12", "declare_armies_1091_06_26"):
        b = env_of(k)["blocked"]
        assert b["gate"] == "armies_raised" and "no tool disbands them" in b["detail"], k
        assert b["fix"]["args"] == {"subject": "armies"}, k


def _armies(ids):
    return {"armies": [{"army_id": i, "commander_id": FELIC, "troop_count": 892} for i in ids]}


def test_2_with_army_orders_on_the_refusal_names_the_disband_call(monkeypatch):
    """Raised armies refuse before sending (the declaration's own gate, declare_gates.failing)."""
    from test_raised_army_declaration_rule import DA_MAYO, _declare_gw
    import test_raised_army_declaration_rule as T18
    monkeypatch.setenv("CK3_BRIDGE_ARMY_ORDERS", "1")
    gw = _declare_gw(_armies([257]), marks=["WHY_RAISED_ARMIES"])
    b = run(R.declare_war_impl(gw, 31506, declare_args=dict(DA_MAYO), confirm_straddle=True))["blocked"]
    assert b["gate"] == "armies_raised" and "no tool" not in b["summary"] and not T18.SA.scripts_sent(gw)
    assert "do army action=disband" in b["summary"]
    assert b["fix"] == {"tool": "do", "args": {"intent": "army", "args": {"action": "disband", "army_id": 257}}}


def test_2_CONTROL_with_the_switch_off_the_sentence_stays_true():
    from test_raised_army_declaration_rule import DA_MAYO, _declare_gw
    gw = _declare_gw(_armies([257]), marks=["WHY_RAISED_ARMIES"])
    b = run(R.declare_war_impl(gw, 31506, declare_args=dict(DA_MAYO), confirm_straddle=True))["blocked"]
    assert "no tool disbands them" in b["summary"] and b["fix"] == {"tool": "inspect", "args": {"subject": "armies"}}


def test_2_the_cbs_row_carries_the_disband_call_when_listed(monkeypatch):
    """The row's refusal (blocked_by armies_raised) carries the call that clears it."""
    opt = {"cb": "claim_cb", "valid": True, "cb_title_id": 501, "cost_prestige": 150.0}
    purse = {"gold": 129.26, "prestige": 152.6, "piety": 406.0,
             "armies": DG.armies_raised(_armies([173]))}
    DG.annotate([opt], purse, {"in_truce": False})
    (ar,) = [b for b in opt["blocked_by"] if b["condition"] == "armies_raised"]
    assert ar["next"] == {"tool": "inspect", "args": {"subject": "armies"}} and "may_refuse" not in opt
    monkeypatch.setenv("CK3_BRIDGE_ARMY_ORDERS", "1")
    DG.annotate([opt], purse, {"in_truce": False})
    (ar,) = [b for b in opt["blocked_by"] if b["condition"] == "armies_raised"]
    assert ar["next"] == DG.disband_call(173)


def test_2_sweep_no_text_says_no_tool_does_what_a_listed_lever_does():
    """Every string constant of the server's modules that says no tool disbands, moves or holds armies is
    the switched-off one, chosen only when army is not listed (armies_raised_fix, do_sentence)."""
    import importlib
    import re
    hits = []
    for f in sorted(HERE.glob("*.py")):
        if f.name.startswith(("test_", "conftest")):
            continue
        try:
            mod = importlib.import_module(f.stem)
        except Exception:                      # noqa: BLE001 — a module that needs a live process
            continue
        for k, v in vars(mod).items():
            if isinstance(v, str) and re.search(r"no tool (disbands|moves|holds)", v, re.I):
                hits.append(f"{f.stem}.{k}")
    assert hits == ["declare_gates.ARMIES_RAISED_FIX"], hits
    assert DG.armies_raised_fix([1], environ=ON)[0] == DG.ARMIES_RAISED_FIX_ON
    assert DG.armies_raised_fix([1], environ={})[0] == DG.ARMIES_RAISED_FIX
    assert AO.do_sentence({}).startswith("Armies: the game's AI raises and disbands them, no tool does")
    assert "no tool" not in AO.do_sentence(ON)


# ── Army move takes a title id ─────────────────────────────────────────────────

def test_3_recorded_a_numeric_title_was_refused_as_missing():
    for k in ("army_move_485_1080_11_26", "army_move_486_1081_07_01"):
        b = env_of(k)["blocked"]
        assert (b["gate"], b["raw_reason"]) == ("missing_required_argument", "province_required"), k


class ArmyGw(Gw):
    async def read(self, action, params=None):
        p = dict(params or {})
        if action == "query_de_jure":
            self.reads.append((action, p))
            if p.get("title_id") == 485 or p.get("title_key") == "c_ailech":
                return {"title": {"title_id": 485, "key": "c_ailech", "name": "Ailech"}}
            return {"error": "title_not_found"}
        return await super().read(action, params)

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return {"success": True, "action": action, "after": "path_ends_at_target"}


@pytest.mark.parametrize("title", [485, "485", "c_ailech"])
def test_3_a_title_id_or_key_moves_the_army_and_is_named(title):
    gw = ArmyGw()
    env = run(AO.do_army(gw, {"action": "move", "army_id": 257, "title": title}, FELIC, {"id": FELIC}, environ=ON))
    assert env["ok"] is True, env
    assert gw.writes == [("army_move", {"actor_id": FELIC, "army_id": 257, "title": "c_ailech", "check": False})]
    assert env["result"]["titles_named"] == [{"arg": "title", "title_key": "c_ailech", "title_id": 485,
                                              "name": "Ailech"}]


def test_3_an_unknown_id_is_refused_and_nothing_is_sent():
    gw = ArmyGw()
    env = run(AO.do_army(gw, {"action": "move", "army_id": 257, "title": 99999}, FELIC, {"id": FELIC}, environ=ON))
    assert env["blocked"]["gate"] == "target_not_assignable" and gw.writes == []


# ── query(domain="buildings") ──────────────────────────────────────────────────

def test_4_recorded_the_reply_was_125588_characters_and_lost():
    r = LIVE["buildings_query_1093_12_17"]["reply"]
    assert isinstance(r, str) and "125,588 characters" in r and "exceeds maximum allowed tokens" in r


def _types(n=600):
    lines = ["castle", "curtain_walls", "pastures", "cereal_fields", "hunting_grounds", "farm_estates",
             "temple", "city", "guild_halls", "common_tradeport", "barracks", "military_camps"]
    rows = []
    for i in range(n):
        line = lines[i % len(lines)] if i < 96 else f"line{i}"
        rows.append({"name": f"{line}_{(i // len(lines)) % 8 + 1:02d}" if i < 96 else f"{line}_01",
                     "level": 1, "construction_days": 730, "description": "x" * 150})
    return {"building_types": rows}


def _county():
    return env_of("county_1083_01_06")["result"]


#: the construction validity's lines as the game wrote them (test_query_and_do_rows, read live)
_MISSING_66 = ("\x16warning_icon! \x15X Missing \x15TOOLTIP:GAME_CONCEPT,gold_i "
               "\x16gold_icon!\x15! 66\x15!\x15indent_newline:0")


def _check(p):
    rows = []
    for b in p["buildings"]:
        ok = b.startswith(("hunting_grounds", "barracks"))
        gold = b.startswith("farm_estates")
        rows.append({"building": b, "success": True, "buildable": ok, "price": {"gold": 100.0 if ok else 150.0},
                     "engine_reasons": [] if ok else [_MISSING_66] if gold else []})
    return {"success": True, "rows": rows}


def test_4_with_a_county_the_reply_is_its_holdings_and_what_can_be_built_there_priced():
    assert len(json.dumps(_types())) > 120_000          # the whole database, as the run read it
    gw = Gw({"query_building_types": _types(), "query_county_details": _county(), "construction_check": _check})
    out, errs = run(_br().buildings_reply(gw, FELIC, 463))
    assert errs == []
    assert out["county"]["key"] == "c_dublin" and [h["barony_title_id"] for h in out["holdings"]] == \
        [464, 465, 466, 467, 468]
    assert out["holdings"][0]["built"] == ["castle_01", "curtain_walls_01", "cereal_fields_01", "pastures_01"]
    assert out["holdings"][3]["empty_slots"] == 3 and out["empty_slots"] == 4
    assert out["at"]["barony_title_id"] == 464
    (sent,) = [p for a, p in gw.reads if a == "construction_check"]
    assert "castle_02" in sent["buildings"] and "castle_01" not in sent["buildings"]
    assert all(r["do_call"]["args"]["args"]["location_id"] == 464 for r in out["can_build"])
    assert {r["building"] for r in out["can_build"]} >= {"hunting_grounds_01", "barracks_01"}
    # a live test run: a building refused for its gold alone is listed with the gold missing, not lost
    # among one refusal kind per amount
    (fe,) = [r for r in out["can_build_with_gold"] if r["building"] == "farm_estates_01"]
    assert fe["gold_missing"] == 66 and fe["do_call"]["args"]["args"]["location_id"] == 464
    assert out["refused"][0]["reason"] == _br().NO_REASON and out["refused"][0]["count"] > 0
    assert len(json.dumps(out)) < 8192, len(json.dumps(out))


def test_4_with_no_title_the_names_only_capped_and_counted():
    gw = Gw({"query_building_types": _types()})
    out, _ = run(_br().buildings_reply(gw, FELIC, 0))
    assert out["building_types"] == 600 and len(out["names"]) == _br().BUILDING_NAMES_SHOWN
    assert out["truncated"] == 600 - _br().BUILDING_NAMES_SHOWN and len(json.dumps(out)) < 8192
    out, _ = run(_br().buildings_reply(gw, FELIC, 0, "hunting"))
    assert out["matching"] == len(out["names"]) and all("hunting" in n for n in out["names"])


def test_4_a_title_not_of_your_domain_says_so():
    gw = Gw({"query_building_types": _types(), "query_county_details": _county()})
    out, _ = run(_br().buildings_reply(gw, FELIC, 501))
    assert out["holdings"] is None and "your own domain" in out["why"]


def test_4_every_query_reply_is_cut_to_the_cap_and_counted():
    import mcp_server as ms
    big = {"characters": [{"id": i, "name": "x" * 200} for i in range(200)], "total": 200}
    out = ms.fit_reply(big, ms.QUERY_REPLY_CAP)
    assert len(json.dumps(out)) <= ms.QUERY_REPLY_CAP
    t = out["truncated"]
    assert (t["list"], t["of"]) == ("characters", 200) and t["shown"] == len(out["characters"]) > 0
    small = {"characters": [{"id": 1}]}
    assert ms.fit_reply(small, ms.QUERY_REPLY_CAP) is small


# ── The activity gate names the seat and what clears it ────────────────────────

def test_5_recorded_the_gate_named_no_slot():
    b = env_of("feast_refused_1092_12_28")["blocked"]
    assert b["gate"] == "ai_potential_not_met" and "your capital has a building slot" in b["summary"]
    assert b["fix"]["args"]["args"]["location_id"] == "<your capital's barony>"


def test_5_the_branch_that_holds_and_the_seat_are_named_with_the_read():
    gw = ScriptGw(dict(COUNT, query_realm={"capital_barony_id": 464}),
                  marks={"activity": ["BUILD_FOCUS", "BF_FREE_SLOT", "BF_SEAT_IS_CAPITAL", "RAN"]})
    env = run(R.do_impl(gw, action_class="activities", character_id=MURCHAD))
    feast = next(r for r in env["result"]["unavailable"] if r["activity"] == "feast")
    g = next(x for x in feast["gates"] if x["condition"] == "build_focus")
    assert "free building slot" in g["game_says"] and g["seat"]["barony_title_id"] == 464
    assert g["fix"]["args"] == {"domain": "buildings", "title_id": 464}
    assert "<your capital" not in json.dumps(feast)


def test_5_an_innovation_branch_says_what_level_clears_it_and_finds_the_seat_by_county():
    gw = ScriptGw(dict(COUNT, query_realm={"capital_barony_id": 465}, query_county_details=_county()),
                  marks={"activity": ["BUILD_FOCUS", "BF_LEDGER", "RAN"]})
    env = run(R.do_impl(gw, action_class="activities", character_id=MURCHAD))
    g = next(x for x in env["result"]["unavailable"][0]["gates"] if x["condition"] == "build_focus")
    assert "innovation_ledger" in g["game_says"] and "past 4" in g["game_says"]
    assert g["seat"]["barony_title_id"] == 464 and g["seat"]["county"] == "c_dublin"


def test_5_CONTROL_an_old_bridge_with_no_branch_mark_says_unknown():
    gw = ScriptGw(dict(COUNT, query_realm={"capital_barony_id": 464}),
                  marks={"activity": ["BUILD_FOCUS", "RAN"]})
    env = run(R.do_impl(gw, action_class="activities", character_id=MURCHAD))
    g = next(x for x in env["result"]["unavailable"][0]["gates"] if x["condition"] == "build_focus")
    assert "unknown" in g["game_says"]


def test_5_the_branches_are_the_files_words():
    src = (HERE / ".." / ".." / "bridge" / "include" / "script_templates.h").read_text()
    for w in A.BF_BRANCHES:
        assert f'"{w}"' in src, w
    game = Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game"
    f = game / "common" / "scripted_triggers" / "00_ai_value_triggers.txt"
    if not f.exists():
        pytest.skip("the game's files are not here")
    body = f.read_text()
    body = body[body.index("ai_should_focus_on_building_in_their_capital = {"):][:1400]
    for inn, lvl in (("innovation_ledger", 4), ("innovation_baliffs", 6), ("innovation_guilds", 9),
                     ("innovation_cranes", 12)):
        assert inn in body and f"combined_building_level <= {lvl}" in body
        assert f"has_innovation = {inn} }} scope:ck3a_bfp = {{ combined_building_level <= {lvl} }}" in src
    assert "capital_county.title_province.free_building_slots > 0" in body


# ── What a war can cost in prestige ────────────────────────────────────────────

def test_6_recorded_the_cbs_row_named_the_declare_price_only():
    (o,) = [x for x in env_of("cbs_breifne_1090_12_12")["result"]["valid_options"] if x["cb"] == "claim_cb"]
    assert o["cost_prestige"] == 150.0 and "prestige_if" not in o


def test_6_each_declarable_option_carries_the_prestige_on_each_outcome():
    opt = {"cb": "claim_cb", "valid": True, "cb_title_id": 501, "scope": "county"}
    purse = {"gold": 129.26, "prestige": 177.0, "piety": 406.0, "armies": {"raised": False, "men": 0, "armies": 0}}
    DG.annotate([opt], purse, {"in_truce": False}, title_keys={501: "c_breifne"})
    p = opt["prestige_if"]
    cost = opt["checked"]["prestige"]["cost"]
    assert p["have"] == 177.0 and p["declare"] == -cost and p["after_declare"] == round(177.0 - cost, 2)
    # The outcomes: claim_cb's are 5 / 10 x the war's prestige factor, which the engine sets and no read gives
    assert "-150 duchy" in p["each_ally_joining"] and "-5 x" in p["white_peace"] and "-10 x" in p["defeat"]
    assert "factor" in p["outcome_basis"]
    assert "below 0" in p["floor"] and p["monthly"] == "unread"
    assert len(json.dumps(p)) < 560          # the two factor rules in words, where numbers stood


# ── The 1093 gold fall ─────────────────────────────────────────────────────────

def test_7_recorded_the_letter_was_paid_and_read_as_not_paid():
    n = news_of("news_1093_12_17")
    g = n["purse"]["gold"]
    assert g["unexplained"] == -212.9 and g["mercenaries_hired_for_you"][0]["company_id"] == 536
    assert "mercenaries_expected" not in g               # 536 was named in 1091.12: not counted again
    (letter,) = [e for e in n["events_answered"] if e["event"] == news.CLAIM_LETTER]
    assert letter["applied"] == "court_chaplain_task.0301.a" and letter["covered"] is True
    assert "not by your chaplain's letter" in g["in_this_step"][0]["what"]


def test_7_a_paid_letter_in_the_bridges_record_is_not_declined():
    raw = {"options": ["court_chaplain_task.0301.a", "court_chaplain_task.0301.b"],
           "applied": {"index": 0, "key": "court_chaplain_task.0301.a"}}
    assert news._declined(raw) is False
    assert news._declined(dict(raw, applied={"index": 1, "key": "court_chaplain_task.0301.b"})) is True
    assert news._declined({"options": raw["options"], "applied": "court_chaplain_task.0301.a"}) is False


def test_7_an_ai_hire_reads_as_acted_with_its_price_and_the_veto():
    rec = {"seq": 5, "date": "1093.08.02", "character_id": FELIC, "what": "mercenary", "dropped": False,
           "company_id": 536, "gold": 230.4, "prestige": -55.0, "piety": 0, "priced": "no",
           "unpriced_why": "no_price_read"}
    prev = {"ai_veto_records": []}
    out = SE.veto_news(prev, {"ai_veto_records": [rec]})
    (row,) = out["ai_acted"]
    assert row["what"] == "mercenary" and row["company_id"] == 536 and row["gold_at_attempt"] == 230.4
    assert row["veto"] == news.ai_veto_call("mercenary")
    assert "does not read a hire's price" in row["price_unread"]


def test_7_the_hire_recorded_in_the_step_is_priced_though_the_company_was_named_before():
    merc = {"companies": [{"company_id": 536, "hired_by": FELIC, "label": "Mercenary company from Lanarkshire",
                           "troops": 835, "regiments": []}]}
    gw = Gw({"query_hireable_mercenaries": merc, "query_realm": {"realm_total_counties": 7},
             "query_maa_types": {"types": []}, "query_county_details": {"counties": []}})
    news._MERC_SEEN[FELIC] = {536}
    rec = {"seq": 5, "date": "1093.08.02", "character_id": FELIC, "what": "mercenary", "dropped": False,
           "company_id": 536, "gold": 230.4, "prestige": -55.0, "piety": 0, "priced": "no"}
    run(news.block(gw, 0, snap("1093.06.26", actor=FELIC, chr_sect={"gold": 208.99, "prestige": -59.7, "piety": 0,
                                                         "income_breakdown": {"total_monthly": 10.25}},
                               ai_veto=veto_raw(FELIC, ["law"])), offers_served=False))
    n = run(news.block(gw, 0, snap("1093.12.17", actor=FELIC, chr_sect={"gold": 55.77, "prestige": -39.9, "piety": 0},
                                   ai_veto=veto_raw(FELIC, ["law"], [rec])), offers_served=False))
    g = n["purse"]["gold"]
    assert g["hired_by_the_ai_in_this_step"] == [536] and g["mercenary_veto"] == news.ai_veto_call("mercenary")
    assert "mercenaries_expected" in g
    (acted,) = n["ai_acted"]
    assert acted["company_id"] == 536 and acted["veto"]["args"]["args"]["what"] == "mercenary"
