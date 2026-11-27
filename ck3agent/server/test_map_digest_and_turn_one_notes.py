"""The map digest, the turn-one notes, casus-belli labels from the game files, the activity pre-send
check, construction prices, map news, and the query arguments moved behind inspect(subject='map').

Recorded replies: fixtures/map_digest_replies.json.gz (built by a
research-tree script from two recorded games' calls files; each entry names its run and line). The map graph's text is
written in the bridge's format (map_graph.cpp) from the map study's geometry (ck3agent/lab_mapview/data/
ireland_geometry.json: read from the game's provinces.png and adjacencies.csv, and equal to the baked
ck3map.bin for the 14 Irish counties: a research-tree test).
"""
import asyncio
import gzip
import json
import re
from pathlib import Path

import pytest

import cb_labels as CBL
import map_view as MV
import news
import notes as N
import redesign as R
from test_ireland_walk_replies import Gw

HERE = Path(__file__).resolve().parent
REC = json.loads(gzip.open(HERE / "fixtures" / "map_digest_replies.json.gz", "rt").read())
FELIC = 55606
ME = 31506


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh():
    # Redesign's per-game-day cb_prices cache is module state; a test before this file (test_raised_army_declaration_rule's
    # declarations, the same seat, target and date) left it filled, so one test's write list read differently
    # alone and in the suite. Each test here starts and ends with it empty.
    R._CB_PRICES.clear()
    news.reset()
    MV.reset()
    yield
    R._CB_PRICES.clear()
    news.reset()
    MV.reset()


def rec(name):
    return json.loads(json.dumps(REC[name]["result"]))


TITLE = rec("title_k_ireland_1081_12_12")
STANCE = rec("war_stance_1081_10_29")
ARMIES = rec("armies_1081_10_29")
BARONIES = {486: ("b_donegal", "c_ailech"), "b_donegal": ("b_donegal", "c_ailech")}


@pytest.fixture
def recorded_title(monkeypatch):
    import title_table as TT

    async def tt(gw, **kw):
        t = json.loads(json.dumps(TITLE))
        for c in t["counties"]:           # as of 1081.10.29: Ailech was won 1081.12.10 (the map study's rollback)
            if c["key"] == "c_ailech":
                c.update(holder_id=32421, holder_name="Áed", realm_ruler_id=32421, realm_ruler_name="Áed",
                         realm_ruler_strength=695, you_control=False)
        t["realms"].append({"ruler_id": 32421, "ruler_name": "Áed", "military_strength": 695})
        return t, [], None
    monkeypatch.setattr(TT, "title_table", tt)

    async def dn(gw, me, targets):
        # the casus belli the cbs read gave against each realm ruler (a recorded game 1081.12.12's plan_war rows)
        out = {t: [] for t in targets}
        if 32421 in out:
            out[32421] = [{"cb": "individual_county_de_jure_cb", "valid": True, "dispatch_supported": True,
                           "cb_title_id": 485}]
        if 34580 in out:
            out[34580] = [{"cb": "make_tributary_cb", "valid": True, "dispatch_supported": True, "cb_title_id": 0}]
        if 36303 in out:
            out[36303] = [{"cb": "claim_cb", "valid": True, "dispatch_supported": True, "cb_title_id": 513,
                           "blocked_by": [{"gate": "insufficient_prestige"}]}]
        return out
    monkeypatch.setattr(R, "declarable_now", dn)


def digest(gw, **kw):
    return run(R.inspect_impl(gw, subject="map", character_id=FELIC, **kw))


# ── The digest ─────────────────────────────────────────────────────────


def test_1_recorded_the_county_read_answered_your_own_counties():
    r = REC["county_485_1081_07_01"]
    assert r["args"] == {"subject": "county", "title_id": 485}
    assert 485 not in [c["title_id"] for c in r["result"]["counties"]] and len(r["result"]["counties"]) == 5


# ── The query arguments moved behind the subject; the listing ──────────

def test_1_query_lost_the_map_graph_scoping_arguments_and_the_listing_is_within_its_caps():
    ms = pytest.importorskip("mcp_server")
    tools = run(ms.mcp.list_tools())
    out = [t.model_dump(mode="json", by_alias=True, exclude_none=True) for t in tools]
    out = [{"name": d["name"], "description": d.get("description", ""), "inputSchema": d["inputSchema"]}
           for d in out]
    q = next(d for d in out if d["name"] == "query")["inputSchema"]["properties"]
    for gone in ("scope", "hops", "max_nodes", "target_county", "target_county_key", "counties"):
        assert gone not in q, gone
    ins = next(d for d in out if d["name"] == "inspect")
    assert "map" in ins["inputSchema"]["properties"]["subject"]["enum"]
    assert "map:" in ins["description"]
    assert len(out) == 13
    assert len(json.dumps(out)) <= 31_049 and len(json.dumps(out, indent=1)) <= 36_298


# ── map news ─────────────────────────────────────────────────────────


# ── The turn-one notes in the first observation ────────────────────────

def _snap(date):
    return news.snapshot(actor_id=ME, date=date, chr_sect={"gold": 84.0, "prestige": 600.0, "piety": 400.0},
                         wars={"wars": []}, claims={"claims": []}, council={"council": []}, heir=None,
                         schemes={"schemes": []}, held={"held_titles": []}, council_tasks=[],
                         laws={"active_laws": []}, alliances={"alliances": []})


def test_2_the_first_news_carries_the_turn_one_notes_and_the_levers_once(monkeypatch):
    monkeypatch.setattr(N, "SERVED", N.boot({}))
    first = run(news.block(Gw({}), 0, _snap("1066.09.15"), offers_served=False))
    t1 = first["turn_one_notes"]
    doc = N.derive(N.load(), None)        # the default set (CK3_NOTES_SET unset)
    assert t1["notes"] == [n["text"] for i in doc["turn_one"] for n in doc["notes"] if n["id"] == i]
    assert t1["version"] == N.version(doc)
    assert first["ai_spends"]["veto"]["args"]["intent"] == "ai_veto"
    assert first["ai_spends"]["claim_letters"]["args"]["intent"] == "event_rule"
    # with the notes on, the block keeps the calls: smaller than before (2,873 B live)
    assert len(json.dumps(first["ai_spends"], ensure_ascii=False).encode()) < 700
    assert len(json.dumps(first, ensure_ascii=False).encode()) < 3000
    second = run(news.block(Gw({}), 0, _snap("1066.10.15"), offers_served=False))
    assert "turn_one_notes" not in second
    # served once per server: the other baseline's first news (the seat's own observe after the
    # environment's turn read) does not carry a second copy
    other = run(news.block(Gw({}), 0, _snap("1066.09.15"), offers_served=False, baseline="turn"))
    assert other["first_observe"] is True and "turn_one_notes" not in other


def test_2_CONTROL_notes_off_no_turn_one_block_and_the_whole_ai_spends(monkeypatch):
    monkeypatch.setattr(N, "SERVED", N.boot({"CK3_NOTES": "off"}))
    first = run(news.block(Gw({}), 0, _snap("1066.09.15"), offers_served=False))
    assert "turn_one_notes" not in first
    assert first["ai_spends"]["title_creation"]["lever"]["no_veto"] == news.AI_CREATE_NO_VETO


def test_2_the_turn_one_notes_name_the_run_deciding_levers():
    doc = N.derive(N.load(), "ireland1066")      # the Ireland tasks' set (the pool whole)
    text = " ".join(N.turn_one_texts(doc))
    # In one test run the perk path took raised-armies' slot (the declare refusal itself now names do army disband,
    # declare_gates.ARMIES_RAISED_FIX_ON); that note stays in the claims topic (test_ruler_change_vetoes_and_mint_rows)
    # The veto row names create_title as a count's (a duke's AI mints his second duchy with gold)
    for want in ("do ai_veto what=spending", "mint_title args.title_key=k_ireland", "respond_demand refuse", "Declare on each claim",
                 "ai_veto perk", "ducal_conquest_perk", "diplomacy_foreign_affairs_focus", "marriage",
                 "350 a king", "end_turn pad"):
        assert want in text, want
    assert N.problems(doc) == [] and len(doc["notes"]) <= 35
    sizes = [len(json.dumps([N.render(n) for n in doc["notes"] if n["topic"] == t], ensure_ascii=False).encode())
             for t in doc["topics"]]
    assert sum(sizes) <= 7416                        # traded, not grown (the total)


def test_2_turn_one_must_name_notes_and_stay_small():
    import copy
    d = copy.deepcopy(N.load())
    d["turn_one"] = d["turn_one"] + ["no-such-note"]
    assert any("not a note" in p for p in N.problems(d))
    d = copy.deepcopy(N.load())
    d["turn_one"] = [n["id"] for n in d["notes"]]
    assert any("turn_one renders" in p for p in N.problems(d))


def test_2_the_duchy_note_gives_both_facts_with_the_game_numbers():
    t = next(n["text"] for n in N.load()["notes"] if n["id"] == "ai-may-create-title")
    assert "+300 prestige" in t and "+25 legitimacy" in t and "x1.25" in t and "x2" in t


GAME = Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game"


@pytest.mark.skipif(not (GAME / "common/defines/00_defines.txt").is_file(), reason="needs the game's files")
def test_2_the_duchy_notes_numbers_are_the_game_files():
    d = (GAME / "common/defines/00_defines.txt").read_text(encoding="utf-8-sig")
    assert re.search(r"CREATE_TITLE_PRESTIGE_GAIN = \{ 0 0 0 300 ", d)
    assert re.search(r"CREATE_TITLE_LEGITIMACY_GAIN = \{ 0 0 0 25 ", d)
    v = (GAME / "common/script_values/99_casus_belli_values.txt").read_text(encoding="utf-8-sig")
    assert re.search(r"primary_title\.tier > scope:defender\.primary_title\.tier\s*\}\s*multiply = \{\s*value = 1\.25", v)


# ── Casus belli labels from the game files ─────────────────────────────

def test_3_recorded_ducal_conquest_was_labelled_a_duchy_war_with_no_reason():
    (o,) = [x for x in REC["cbs_ducal"]["result"]["options"] if x["cb"] == "ducal_conquest_cb"]
    assert o["scope"] == "duchy" and "de-jure duchy" in o["grants"] and "requires" not in o


def test_3_the_shaped_row_says_county_and_what_it_requires():
    raw = {"cbs": [{"cb_key": "ducal_conquest_cb", "valid": False, "scope": "duchy",
                    "grants": "the targeted de-jure duchy (de-jure-duchy variant of conquest)",
                    "requirements_unmet": ["in_truce"], "target_mode": 1}]}
    _, opts = R._shape_cb_options(raw, target_id=31266, purse={})
    (o,) = opts
    assert o["scope"] == "county" and "uncreated de jure duchy" in o["grants"]
    assert any("ducal_conquest_perk" in r for r in o["requires"]) and o["bridge_label"]["scope"] == "duchy"


def test_3_the_missing_perk_is_read_from_the_lifestyle_perks():
    perks = REC["lifestyle"]["result"]["perks"]
    row = run(CBL.ducal_conquest_row(Gw({"query_lifestyle": {"perks": perks}}), ME,
                                     [CBL.relabel({"cb": "ducal_conquest_cb", "valid": False})]))
    assert "not among your ruler's perks" in row["missing_now"][0]
    ok = run(CBL.ducal_conquest_row(Gw({"query_lifestyle": {"perks": perks + ["ducal_conquest_perk"]}}), ME,
                                    [CBL.relabel({"cb": "ducal_conquest_cb", "valid": False})]))
    assert "perk" not in ok["missing_now"][0].split(":")[0] or "not among" not in ok["missing_now"][0]
    assert run(CBL.ducal_conquest_row(Gw({}), ME, [{"cb": "ducal_conquest_cb", "valid": True}])) is None


def test_3_the_bridge_table_says_the_same_words():
    src = (HERE.parents[1] / "bridge" / "src" / "query_handlers.cpp").read_text()
    for cb, (scope, grants, _r, _s) in CBL.LABELS.items():
        m = re.search(r'\{"%s",\s*"([^"]*)",\s*"([^"]*)"\}' % re.escape(cb), src)
        assert m and (m.group(1), m.group(2)) == (scope, grants), cb


@pytest.mark.skipif(not (GAME / "common/casus_belli_types/00_conquest.txt").is_file(), reason="needs the game's files")
def test_3_ducal_conquest_in_the_game_file_targets_a_county_and_needs_the_perk():
    t = (GAME / "common/casus_belli_types/00_conquest.txt").read_text(encoding="utf-8-sig")
    body = t[t.index("ducal_conquest_cb = {"):t.index("ireland_laudabiliter_conquest_cb = {")]
    assert "has_perk = ducal_conquest_perk" in body and "target_titles = neighbor_land" in body
    assert re.search(r"scope:target = \{\s*is_landless_type_title = no\s*tier = tier_county", body)
    assert "is_title_created = no" in body


# ── The activity pre-send check; a construction's price ────────────────

def _act_gw(marks):
    """test_raised_army_declaration_rule's declaration gateway (a recorded game's Mayo claim, every pre-send check passing) whose activity
    listing (the read-only `activity` template) answers `marks`."""
    from test_raised_army_declaration_rule import ARMIES_NONE, _declare_gw
    gw = _declare_gw(ARMIES_NONE, guard="ran")
    base = gw.write

    async def write(action, params=None):
        p = dict(params or {})
        if action == "script_action" and p.get("template") == "activity":
            gw.writes.append((action, p))
            return {"success": True, "template": "activity", "token": "SACT_1", "guard": "ran",
                    "marks": list(marks), "errors": []}
        return await base(action, params)
    gw.write = write
    return gw


def test_4_a_declaration_while_in_an_activity_is_refused_before_sending():
    from test_raised_army_declaration_rule import DA_MAYO
    gw = _act_gw(["IN_ACTIVITY", "RAN"])
    env = run(R.declare_war_impl(gw, 31506, declare_args=dict(DA_MAYO), confirm_straddle=True))
    b = env["blocked"]
    assert b["raw_reason"] == "involved_activity" and "when it ends is not read" in b["summary"], b
    # A guest's refusal names his way out (do host_activity activity=leave; a host's still the listing)
    assert b["fix"] == {"tool": "do", "args": {"intent": "host_activity", "args": {"activity": "leave"}}}
    sent = [p.get("template") for a, p in gw.writes if a == "script_action"]
    # no declaration sent: the activity check is the last write, and before it only the read-only price reads
    assert sent[-1] == "activity" and set(sent[:-1]) <= {"cb_prices"} and "declare_war" not in sent, sent
    assert "activity" not in R.DECLARE_NOT_CHECKED


def test_4_CONTROL_not_in_an_activity_the_declaration_is_sent():
    from test_raised_army_declaration_rule import DA_MAYO
    gw = _act_gw(["RAN"])
    run(R.declare_war_impl(gw, 31506, declare_args=dict(DA_MAYO), confirm_straddle=True))
    assert "declare_war" in [p.get("template") for a, p in gw.writes if a == "script_action"]


def test_4_a_started_construction_is_priced_and_its_ai_post_too():
    county = {"counties": [{"county_key": "c_dublin", "holdings": [
        {"barony_title_id": 464, "construction": {"building": "farm_estates_01", "started": "1082.03.01",
                                                  "ordered_by": ME}}]}]}
    gw = Gw({"query_county_details": county,
             "construction_check": {"success": True, "rows": [{"building": "farm_estates_01",
                                                                "price": {"gold": 132.4}}]}})
    built = run(news.constructions_started(gw, ME, "1082.01.01", "1082.06.01"))
    assert built[0]["price_estimate"] == -132.4 and built[0]["barony_title_id"] == 464
    assert ("construction_check", {"actor_id": ME, "location_id": 464, "buildings": ["farm_estates_01"]}) in gw.reads


def test_4_CONTROL_no_price_read_says_so():
    county = {"counties": [{"county_key": "c_dublin", "holdings": [
        {"barony_title_id": 464, "construction": {"building": "farm_estates_01", "started": "1082.03.01"}}]}]}
    built = run(news.constructions_started(Gw({"query_county_details": county}), ME, "1082.01.01", "1082.06.01"))
    assert "price_estimate" not in built[0] and "no price" in built[0]["price_unread"]


def test_4_the_unknown_tool_names_are_not_in_our_texts():
    """a recorded game called `call_ally_placeholder` and an unprefixed `inspect`: no server text, prompt or reply
    carried either (searched: the run's 271 replies, its rendered prompt and listing; here the sources)."""
    for p in list(HERE.glob("*.py")) + [HERE / "notes.json"]:
        if p.name.startswith("test_"):
            continue
        assert "call_ally_placeholder" not in p.read_text(), p.name
