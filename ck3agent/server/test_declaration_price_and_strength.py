"""Tool replies the six seated Munster runs showed were not what the game does.

1  committed strength: plan_war / cbs / declare_war compare a target with what your open wars leave
2  the declaration's price is the game's own (its cost block evaluated by the game), for every casus belli the
   reads offer; the bridge's mirror of the files is checked against the install
3  a refusal the game gives for the price names it, with when to retry
4  a tribal government buys men-at-arms with prestige (gate and listing)
5  no hint names a verb, intent, class, subject or domain that does not exist
6  query_maa_types / query_war_stance name the seat's ruler (one place: seat_read)
7  attackers_new: a ruler who attacked you stays a candidate however far he is
"""
from __future__ import annotations

import ast
import asyncio
import gzip
import json
import os
import re
from pathlib import Path

import pytest

import declare_gates as DG
import redesign as R
import threats as TH
import tools as T
import test_plan_war_claim_rows as PW
import test_ireland_walk_replies as T13
from test_ireland_walk_replies import Gw, cbs_by_target

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HEADER = ROOT / "bridge" / "include" / "script_templates.h"
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")
ME, FOE, ALLY, OTHER = 31762, 32287, 40001, 50002


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh():
    getattr(R, "_CB_PRICES", {}).clear()
    TH.reset()
    yield
    getattr(R, "_CB_PRICES", {}).clear()
    TH.reset()


def need_game():
    if not (GAME / "common" / "casus_belli_types").is_dir():
        pytest.skip(f"no game install at {GAME}")


# ── helpers: the bridge's encoding of a price (script_templates.h price_marks) ────────────────────────────

def marks_for(word: str, value: float) -> list:
    out = [f"{word}_NEG"] if value < 0 else []
    v = abs(value)
    whole = int(v)
    frac = int(round((v - whole) * 1024))
    out += [f"{word}_I{k}" for k in range(18) if whole >> k & 1]
    out += [f"{word}_F{k}" for k in range(10) if frac >> k & 1]
    return out + [f"{word}_SET"]


class PricedGw(Gw):
    """test_ireland_walk_replies's gateway whose cb_prices read answers `prices` {cb: (prestige, piety)} in request order."""

    def __init__(self, replies=None, prices=None, **kw):
        super().__init__(replies, **kw)
        self.prices = prices or {}

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if action == "script_action" and p.get("template") == "cb_prices":
            marks = ["READ"]
            for j, cb in enumerate(str(p.get("cbs")).split(",")):
                if cb in self.prices:
                    pr, pi = self.prices[cb]
                    marks += marks_for(f"C{j}P", pr) + marks_for(f"C{j}Q", pi)
            return {"success": True, "guard": "ran", "marks": marks, "token": "SACT_9"}
        return {"success": True}


# ── 2. the declaration's price is the game's ──────────────────────────────────────────────────────────────

def _header_rows():
    src = HEADER.read_text()
    body = src[src.index("inline constexpr CbPrice kCbPrices[] = {"):]
    body = body[:body.index("\n};")]
    rows = re.findall(r'\{"([a-z0-9_]+)", R"cb\((.*?)\)cb", R"cb\((.*?)\)cb", (true|false)\}', body)
    return {cb: (p, q, n == "true") for cb, p, q, n in rows}


def _strip(t):
    return re.sub(r"#[^\n]*", "", t)


def _block_at(t, i):
    d = 0
    for k in range(i, len(t)):
        if t[k] == "{":
            d += 1
        elif t[k] == "}":
            d -= 1
            if d == 0:
                return t[i:k + 1]


def _child(b, key):
    d, i = 0, 1
    while i < len(b) - 1:
        c = b[i]
        if c == "{":
            d += 1
        elif c == "}":
            d -= 1
        elif d == 0 and not (b[i - 1].isalnum() or b[i - 1] in "_:."):
            m = re.compile(key + r"\s*=\s*").match(b, i)
            if m:
                j = m.end()
                return _block_at(b, j) if b[j] == "{" else re.match(r"[A-Za-z0-9_:.@]+", b[j:]).group(0)
        i += 1
    return None


def _file_cost(cb):
    for f in sorted((GAME / "common" / "casus_belli_types").glob("*.txt")):
        t = _strip(f.read_text(encoding="utf-8-sig"))
        m = re.search(r"(?m)^" + cb + r"\s*=\s*\{", t)
        if m:
            cost = _child(_block_at(t, m.end() - 1), "cost")
            return [" ".join((_child(cost, k) or "").split()) for k in ("prestige", "piety")]
    return None


def test_2a_every_price_row_is_the_install_files_cost_block():
    need_game()
    rows = _header_rows()
    assert len(rows) == 14, sorted(rows)
    bad = {cb: (p, q) for cb, (p, q, _n) in rows.items() if _file_cost(cb) != [p, q]}
    assert not bad, bad


def test_2a_CONTROL_a_changed_row_is_seen():
    """The comparison is not vacuous: the vassalization row with 100 a county in place of 125 differs."""
    need_game()
    p, q, _n = _header_rows()["vassalization_cb"]
    assert "value = 125" in p and _file_cost("vassalization_cb") != [p.replace("value = 125", "value = 100"), q]


def test_2b_the_server_prices_the_set_the_bridge_prices():
    rows = _header_rows()
    assert set(DG.CB_PRICED_BY_GAME) == set(rows)
    assert set(DG.CB_PRICE_NEEDS_TITLE) == {cb for cb, r in rows.items() if r[2]}


def test_2d_the_encoding_decodes():
    for v in (125.0, 0.0, 93.75, 13.5, 375.0, 2500.25, 112.5):
        assert R.decode_price(marks_for("C0P", v), "C0P") == v
    assert R.decode_price(["C0P_I3"], "C0P") is None, "no SET mark: not evaluated, never 0"
    assert R.decode_price(marks_for("C0P", 1.0) + ["C0P_OVER"], "C0P") is None


def _vassalization_cbs():
    return {"cbs": [{"cb_key": "vassalization_cb", "valid": True, "target_mode": 0, "scope": "realm",
                     "family": "vassalization", "cost_prestige": 50, "cost_piety": 0}],
            "target_state": {"in_truce": False}}


def _me(prestige):
    return {"id": ME, "name": "Murchad", "alive": True, "gold": 84.0, "prestige": prestige, "piety": 300.0,
            "age": 41, "liege_id": 0, "top_liege_id": ME, "military_strength": 900,
            "monthly_change": {"prestige": 2.5, "piety": 1.0}}


def _cbs_gw(prestige, prices):
    gw = PricedGw({**cbs_by_target(_vassalization_cbs()), "get_active_wars": {"wars": []},
                   "query_alliances": {"own_military_strength": 900}},
                  prices=prices)
    gw.replies["query_character"] = lambda p: _me(prestige) if p.get("character_id") in (ME, 0, None) else \
        {"id": p.get("character_id"), "alive": True, "military_strength": 400}
    return gw


def test_2e_cbs_shows_the_games_price_and_gates_on_it():
    """A recorded game, 1080.07: two vassalization declarations refused at 50.07 prestige; the reads said 50."""
    gw = _cbs_gw(50.07, {"vassalization_cb": (375.0, 0.0)})
    env = run(R.cbs_impl(gw, target_id=FOE, character_id=ME))
    res = env["result"]
    sent = [p for a, p in gw.writes if a == "script_action"]
    assert sent and sent[0]["template"] == "cb_prices" and sent[0]["cbs"] == "vassalization_cb", sent
    (b,) = res["blocked_now"]
    (f,) = b["blocked_by"]
    assert f["gate"] == "insufficient_prestige" and f["cost"] == 375.0 and f["missing"] == 324.93, f
    assert f["retry_in_months"] == 130, f        # (375 - 50.07) / 2.5 a month


def test_2e_CONTROL_a_bridge_that_gives_no_price_keeps_the_estimate_and_says_so():
    gw = _cbs_gw(50.07, {})
    env = run(R.cbs_impl(gw, target_id=FOE, character_id=ME))
    (o,) = env["result"]["valid_options"]
    assert o["cost_prestige"] == 50 and "blocked_now" not in env["result"]


def test_2f_the_declarations_price_shown_is_the_charge():
    """A 1066 game: the vassalization of Desmond (1 county) estimated 50 and charged 125."""
    gw = _cbs_gw(700.0, {"vassalization_cb": (125.0, 0.0)})
    env = run(R.cbs_impl(gw, target_id=FOE, character_id=ME))
    (o,) = env["result"]["valid_options"]
    assert o["cost_prestige"] == 125.0 and o["cost_source"] == R.CB_GAME_PRICE_SOURCE, o
    assert o["prestige_if"]["declare"] == -125.0 and o["affordable"] is True
    assert R.CB_COST_SEEN[(FOE, "vassalization_cb", 0)]["prestige"] == 125.0


# ── 3. the game's refusal for the price is named, with when to retry ───────────────────────────────────────

def test_3a_the_refused_branchs_price_reading_names_the_refusal():
    marks = ["PROBED", "WHY_PRESTIGE_BELOW_COST"] + marks_for("COSTP", 375.0) + marks_for("COSTQ", 0.0)
    says = R.declare_game_says(marks)
    (f,) = says["failed"]
    assert f["gate"] == "insufficient_prestige" and says["price"]["prestige"] == 375.0, says


def test_3a_CONTROL_a_price_held_narrows_the_unnamed_part_to_the_engine():
    marks = ["PROBED", "PROBE_BARE_OK"] + marks_for("COSTP", 125.0)
    assert R.declare_game_says(["PROBED"])["cb_part"] == R.CB_PART_CB, "no price read: the old words"
    says = R.declare_game_says(["PROBED"] + marks_for("COSTP", 125.0))
    assert says["cb_part"] == R.CB_PART_CB_PRICE_HELD and says["failed"] == []
    assert R.declare_game_says(marks)["cb_part"] == R.CB_PART_TITLE, "a title refusal is still the title's"
    says = R.declare_game_says(["PROBED", "WHY_RAISED_ARMIES"] + marks_for("COSTP", 125.0))
    assert says["cb_part"] == R.CB_PART_CB_PRICE_HELD, "another failure does not put the held price in doubt"


# ── 1. committed strength ─────────────────────────────────────────────────────────────────────────────────

def _war(war_id, attacker, defender, attackers, defenders):
    return {"war_id": war_id, "name": f"war {war_id}", "attacker_id": attacker, "defender_id": defender,
            "attackers": attackers, "defenders": defenders, "is_attacker": attacker == ME}


STR = {ME: 2000, FOE: 900, ALLY: 300, OTHER: 700}


def _strength_gw(wars, gw=None):
    gw = gw or Gw()
    gw.replies["get_active_wars"] = {"wars": wars}
    prev = gw.replies.get("query_character")

    def ch(p):
        c = p.get("character_id")
        base = (prev(p) if callable(prev) else {"id": c, "alive": True}) or {}
        return dict(base, military_strength=STR.get(c, base.get("military_strength")))
    gw.replies["query_character"] = ch
    return gw


def test_1a_an_open_offensive_war_commits_what_its_enemy_holds():
    gw = _strength_gw([_war(4, ME, FOE, [ME], [FOE, ALLY])])
    cs = run(R.committed_strength(gw, ME))
    assert cs["yours"] == 2000 and cs["committed"] == 1200 and cs["left"] == 800, cs
    assert cs["offensive_open"] == [4] and cs["wars"][0]["against_you"] == 1200
    words = R.offensive_open_words(cs)
    assert "you already lead 1 offensive war(s)" in words and "1200 is committed" in words and "800 is left" in words


def test_1a_a_co_belligerent_carries_his_share_and_a_defence_is_not_offensive():
    gw = _strength_gw([_war(5, OTHER, ME, [OTHER], [ME, ALLY])])
    cs = run(R.committed_strength(gw, ME))
    assert cs["committed"] == 400 and cs["left"] == 1600 and cs["offensive_open"] == [], cs
    assert R.offensive_open_words(cs) is None


def test_1a_CONTROL_at_peace_nothing_is_committed():
    cs = run(R.committed_strength(_strength_gw([]), ME))
    assert cs["committed"] == 0 and cs["left"] == 2000 and cs["wars"] == []


def test_1a_a_side_not_read_counts_nothing_and_says_so():
    gw = _strength_gw([_war(6, ME, 99999, [ME], [99999])])
    cs = run(R.committed_strength(gw, ME))
    assert cs["committed"] == 0 and "unread" in cs["wars"][0], cs


def _plan_gw(wars):
    import test_mercenary_veto_vtable as _t31  # noqa: F401  (the de jure plan fixture shape)
    conquest = PW._venaissin_conquest()
    rc = conquest["duchies"][0]["remaining_counties"][0]
    rc.update(conquerable_now=True, block_reason="",
              declare={"casus_belli": "individual_county_de_jure_cb", "target_id": PW.LOTHAR,
                       "cb_title_id": PW.C_VENAISSIN})
    dj = {"cb_key": "individual_county_de_jure_cb", "family": "de_jure", "valid": True, "scope": "county",
          "cb_title_id": PW.C_VENAISSIN, "needs_cb_title_id": True, "target_mode": 1, "cost_prestige": 100,
          "cost_piety": 0, "affordable": True}
    gw = PW._gw(query_conquest_targets=conquest, query_cb_validity_strict=PW._strict("individual_county_de_jure_cb"),
                query_cbs_for_target=PW._cbs([dj]), query_dejure_progress={"held": 1, "total": 4},
                get_active_wars={"wars": wars},
                query_alliances=lambda p: {"own_military_strength": 2000 if p.get("character_id") == PW.CHARLES
                                           else 1000,
                                           "total_callable_strength": 2000 if p.get("character_id") == PW.CHARLES
                                           else 1000})
    st = {PW.CHARLES: 2000, PW.LOTHAR: 1000, FOE: 1500}

    def ch(p):
        return dict(PW._character(p), military_strength=st.get(p.get("character_id")))
    gw.replies["query_character"] = ch
    return gw


def test_1b_plan_war_compares_a_target_with_what_your_open_war_leaves():
    wars = [{"war_id": 7, "name": "the first war", "attacker_id": PW.CHARLES, "defender_id": FOE,
             "attackers": [PW.CHARLES], "defenders": [FOE], "is_attacker": True}]
    env = run(R.plan_war_impl(PW._gw() if False else _plan_gw(wars), character_id=PW.CHARLES, goal_scoped=False,
                              ally_free_only=False, exclude_straddling=False))
    res = env["result"]
    (row,) = res["targets"]
    assert row["strength"]["committed"] == 1500 and row["strength"]["mine_left"] == 500, row["strength"]
    assert row["strength"]["ratio"] == 2.0 and row["strength"]["ratio_left"] == 0.5
    assert row["score"] == 0.5, "ranked from what is left"
    assert "you already lead 1 offensive war(s)" in res["offensive_war_open"]
    assert res["committed_strength"]["left"] == 500


def test_1b_CONTROL_at_peace_plan_war_is_unchanged():
    env = run(R.plan_war_impl(_plan_gw([]), character_id=PW.CHARLES, goal_scoped=False,
                              ally_free_only=False, exclude_straddling=False))
    res = env["result"]
    (row,) = res["targets"]
    assert "committed" not in row["strength"] and row["score"] == 2.0
    assert "offensive_war_open" not in res and "committed_strength" not in res


def test_1c_cbs_and_declare_war_say_it_plainly():
    gw = _strength_gw([_war(4, ME, OTHER, [ME], [OTHER])], _cbs_gw(700.0, {"vassalization_cb": (125.0, 0.0)}))
    env = run(R.cbs_impl(gw, target_id=FOE, character_id=ME))
    res = env["result"]
    assert res["strength_verdict"]["committed"] == 700 and res["strength_verdict"]["left"] == 1300, res
    assert "you already lead 1 offensive war(s)" in res["offensive_war_open"]
    gw.replies["query_cb_validity_strict"] = {"results": [{"cb_name": "vassalization_cb", "target_mode": "none",
                                                           "outer_status": "pass", "inner_status": "pass",
                                                           "declarable": True}]}
    env = run(R.declare_war_impl(gw, character_id=ME, target_id=FOE, cb="vassalization_cb",
                                 confirm_straddle=True))
    w = [x for x in env.get("warnings") or [] if x.get("code") == "offensive_war_open"]
    assert w and "700 is committed" in w[0]["detail"], env.get("warnings")


# ── 4. a tribal government's men-at-arms price ─────────────────────────────────────────────────────────────

def test_4a_the_prestige_governments_and_multiplier_are_the_files():
    need_game()
    gov = (GAME / "common" / "governments" / "00_government_types.txt").read_text(encoding="utf-8-sig")
    gov = _strip(gov)
    want = set()
    for m in re.finditer(r"(?m)^([a-z_]+)\s*=\s*\{", gov):
        rules = _child(_block_at(gov, m.end() - 1), "government_rules") or ""
        if re.search(r"regiments_prestige_as_gold\s*=\s*yes", rules):
            want.add(m.group(1))
    h = (ROOT / "bridge" / "include" / "maa_currency.h").read_text()
    lst = re.search(r"kPrestigeGovernments\[\] = \{([^}]*)\}", h).group(1)
    assert set(re.findall(r'"([a-z_]+)"', lst)) == want == {"tribal_government"}
    d = (GAME / "common" / "defines" / "00_defines.txt").read_text(encoding="utf-8-sig")
    mult = float(re.search(r"MAA_PRESTIGE_BUY_COST_MULT\s*=\s*([0-9.]+)", d).group(1))
    assert float(re.search(r"kPrestigeBuyCostMult = ([0-9.]+);", h).group(1)) == mult == 2.0


def test_4b_the_listing_prices_a_tribal_ruler_in_prestige():
    mt = {"actor_id": ME, "gold": 300.0, "currency": "prestige", "government": "tribal_government",
          "prestige": 100.0, "price_basis": "your government buys regiments with prestige",
          "types": [{"maa_type": "bowmen", "recruit_cost": 55.0, "price": 110.0, "currency": "prestige",
                     "affordable": False, "upkeep_per_stack": 1.0, "fielded_regiments": 0}]}
    gw = Gw({"query_maa_types": mt})
    env = run(R.do_impl(gw, action_class="men_at_arms", character_id=ME))
    (row,) = env["result"]["unavailable"]
    assert row["label"] == "bowmen (recruit 110.0 prestige before modifiers)" and row["currency"] == "prestige"
    assert row["blocked"]["gate"] == "insufficient_prestige" and row["blocked"]["measured"]["have"] == 100.0
    (w,) = [w for w in env["warnings"] if w["code"] == "maa_price_in_prestige"]
    assert "before your own cost modifiers" in w["detail"]


def test_4b_a_refused_purchase_names_its_price():
    """a live check: the refusal read "insufficient_prestige" alone beside the bridge's cost and prestige."""
    b = R.write_blocked("insufficient_prestige", conf={"refused": "insufficient_prestige", "raw": {
        "reason": "insufficient_prestige", "maa_type": "cataphract", "cost": 520.0, "prestige": 519.0}})
    assert b["summary"] == "cataphract: the price is 520.0 prestige, you hold 519.0" and b["measured"]["have"] == 519.0


def test_4b_CONTROL_a_bridge_that_sends_no_currency_says_gold_and_says_so():
    mt = {"actor_id": ME, "gold": 30.0, "types": [{"maa_type": "bowmen", "recruit_cost": 55.0,
                                                   "affordable": False, "upkeep_per_stack": 1.0}]}
    env = run(R.do_impl(Gw({"query_maa_types": mt}), action_class="men_at_arms", character_id=ME))
    (row,) = env["result"]["unavailable"]
    assert row["label"] == "bowmen (recruit 55.0 gold before modifiers)" and row["blocked"]["gate"] == "insufficient_gold"
    assert any(w["code"] == "maa_currency_not_sent" for w in env["warnings"])


def test_4c_the_bridge_gate_and_listing_read_the_currency():
    """The gate (do_maa_recruit) and the listing (do_query_maa_types) both decide by maa_currency.h; the gate
    refuses a government not read; the AI's purchase is priced the same way (ai_veto.cpp)."""
    src = (ROOT / "bridge" / "src" / "action_handlers.cpp").read_text()
    body = src[src.index("bool do_maa_recruit("):][:9000]
    assert "mc::of_government(gov)" in body and '"insufficient_prestige"' in body and '"maa_currency_unread"' in body
    q = (ROOT / "bridge" / "src" / "query_handlers.cpp").read_text()
    qb = q[q.index("bool do_query_maa_types("):][:7000]
    assert "mc::of_government(government)" in qb and "mc::price(cost, currency)" in qb
    assert "mc::of_government(read_government_key(lookup_character(r->ruler)))" in \
        (ROOT / "bridge" / "src" / "ai_veto.cpp").read_text()
    assert R.map_raw_reason("maa_currency_unread")["gate"] == "gate_introspection_unavailable"


# ── 5. every verb a hint names exists ──────────────────────────────────────────────────────────────────────

TOOLS = {"query", "discover", "steer", "pass_turn", "observe", "plan_war", "declare_war", "succession_plan",
         "set_phase", "do", "inspect", "cbs", "session"}
CALL = re.compile(r"\b([a-z_]+)\(\s*(intent|action_class|subject|domain|type)\s*=\s*\\?['\"]?([a-z_]+)")
KEYED = re.compile(r'"(tool|intent|action_class|subject|domain)"\s*:\s*"([a-zA-Z_]+)"')


def _vocab():
    return {"intent": set(R.INTENTS) | {R._ARMY_INTENT}, "action_class": set(T.ACTION_CLASSES),
            "subject": set(R.INSPECT_SUBJECTS), "domain": set(T.QUERY_DOMAINS), "tool": TOOLS}


def _hint_lines():
    """Every non-comment line of the bridge's sources and the server's (not tests)."""
    files = sorted((ROOT / "bridge" / "src").glob("*.cpp")) + sorted((ROOT / "bridge" / "include").glob("*.h")) + \
        sorted(p for p in HERE.glob("*.py") if not p.name.startswith("test_"))
    for f in files:
        cpp = f.suffix in (".cpp", ".h")
        for i, line in enumerate(f.read_text(errors="replace").splitlines(), 1):
            s = line.strip()
            if s.startswith("//" if cpp else "#"):
                continue
            yield f.name, i, line


def _bad_hints():
    v = _vocab()
    bad = []
    for name, i, line in _hint_lines():
        for m in CALL.finditer(line):
            tool, k, word = m.groups()
            if tool == "act" or (tool in TOOLS and k != "type" and word not in v[k]):
                bad.append(f"{name}:{i}: {m.group(0)}")
        for m in KEYED.finditer(line):
            k, word = m.groups()
            if word not in v[k] and not word.startswith("mcp__"):
                bad.append(f"{name}:{i}: {m.group(0)}")
        if re.search(r"\bact type=", line):
            bad.append(f"{name}:{i}: act type=")
    return bad


def test_5a_every_verb_a_hint_names_exists():
    assert not _bad_hints(), _bad_hints()


def test_5a_CONTROL_the_census_sees_the_raid_hint_and_the_vocabulary():
    v = _vocab()
    assert "appoint" in v["intent"] and "county" in v["domain"] and "raid" not in v["intent"]
    line = '"needs_prestige — raid (act type=raid) or win battles to fund"'
    assert re.search(r"\bact type=", line) and CALL.search("act(type='appoint', position_id=1)")
    assert CALL.search("query(domain='county_details')").group(3) not in v["domain"]


# ── 6. the two reads name the seat's ruler ─────────────────────────────────────────────────────────────────

def test_6a_inspect_war_stance_and_the_maa_listing_name_the_ruler():
    gw = Gw({"get_active_wars": {"wars": []}, "query_maa_types": {"types": []}})
    run(R.inspect_impl(gw, subject="war_stance"))
    run(R.do_impl(gw, action_class="men_at_arms"))
    ws = [p for a, p in gw.reads if a == "query_war_stance"]
    mt = [p for a, p in gw.reads if a == "query_maa_types"]
    ruler = T13.ME              # the gateway's bound ruler (query_character with id 0 answers him)
    assert mt == [{"actor_id": ruler}], mt
    assert all(p == {"character_id": ruler} for p in ws), ws
    assert [p for a, p in gw.reads if a == "get_active_wars"] == [{"character_id": ruler}]


def test_6b_no_other_call_reads_them_bare():
    """Every read of the two in the server goes through seat_read (an AST walk of each module)."""
    loose = []
    for f in sorted(HERE.glob("*.py")):
        if f.name.startswith("test_"):
            continue
        for n in ast.walk(ast.parse(f.read_text())):
            if isinstance(n, ast.Call) and n.args and isinstance(n.args[0], ast.Constant) \
                    and n.args[0].value in R.READS_NAMING_THE_RULER:
                fn = n.func.attr if isinstance(n.func, ast.Attribute) else getattr(n.func, "id", "")
                if fn != "read" or f.name != "redesign.py":
                    if fn != "seat_read":
                        loose.append((f.name, n.lineno, fn))
    # the one gw.read of each is seat_read's own
    src = (HERE / "redesign.py").read_text()
    body = src[src.index("async def seat_read("):][:900]
    assert "return await gw.read(action, dict(params, **{key: cid}))" in body
    assert not loose, loose


# ── 7. a ruler who attacked you stays a threat candidate ───────────────────────────────────────────────────

def test_7a_an_attacker_beyond_two_map_steps_is_a_candidate():
    """A recorded game: William attacked 1079.11 from beyond the ring and was named only at 1081.07."""
    ring = {"query_title_hierarchy": {"held_titles": [{"key": "c_desmond", "tier": 2}]},
            "query_map_graph": {"text": ""}}
    gw = Gw(dict(ring, get_active_wars={"wars": [_war(3, 31808, ME, [31808], [ME])]},
                 query_character=lambda p: {"id": p.get("character_id"), "top_liege_id": p.get("character_id")}))
    import map_view as MV
    orig = MV.parse_graph
    MV.parse_graph = lambda _t: {"c_desmond": {"holder": ME}, "c_ossory": {"holder": FOE}}
    gw.replies["query_map_graph"] = {"text": "x"}
    try:
        tops, why, _ = run(TH.candidates(gw, ME))
        assert why is None and 31808 in tops and FOE in tops, tops
        gw.replies["get_active_wars"] = {"wars": []}         # the war over: he stays one
        tops, _, _ = run(TH.candidates(gw, ME))
        assert 31808 in tops
    finally:
        MV.parse_graph = orig


def test_7a_CONTROL_a_war_you_lead_adds_nobody():
    gw = Gw({"query_title_hierarchy": {"held_titles": [{"key": "c_desmond", "tier": 2}]},
             "query_map_graph": {"text": "x"}, "get_active_wars": {"wars": [_war(3, ME, 31808, [ME], [31808])]}})
    import map_view as MV
    orig = MV.parse_graph
    MV.parse_graph = lambda _t: {"c_desmond": {"holder": ME}}
    try:
        tops, _, _ = run(TH.candidates(gw, ME))
        assert 31808 not in tops
    finally:
        MV.parse_graph = orig
