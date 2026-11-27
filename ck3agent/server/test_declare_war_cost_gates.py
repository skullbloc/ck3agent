"""Why the first full model-driven seat on the Ireland task starved, each row replayed
from its recorded reply.

The fixture (fixtures/declare_war_gate_replies.json.gz) holds replies recorded on a live
1066 game (toolkit 2026.12.12) as Murchad, Count of Dublin (31506), played for 46 steps
of 73 days by a model-driven seat. Bridge replies the toolkit received are rebuilt from
the tool replies it gave; a value the recording does not hold is written into the test
as such, never into the fixture. Rules read from the game's own files are checked against
the install when one is present (CK3_GAME_DIR, or the default Steam path) and skipped
otherwise.
"""
import asyncio
import gzip
import json
import os
import re
import sys
from pathlib import Path

import pytest

import declare_gates as DG
import news
import redesign as R
import title_table as TT
from test_ireland_walk_replies import Gw, cbs_by_target
import test_script_actions as SA

HERE = Path(__file__).resolve().parent
LIVE = json.loads(gzip.open(HERE / "fixtures" / "declare_war_gate_replies.json.gz", "rt").read())
ME, OSSORY_LORD, ORIEL_LORD, C_OSSORY, C_ORIEL = 31506, 32287, 32422, 513, 479
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")


def run(c):
    return asyncio.run(c)


def size(x) -> int:
    return len(json.dumps(x))


def game_file(rel: str) -> str:
    p = GAME / rel
    if not p.is_file():
        pytest.skip(f"no game install at {GAME}")
    return p.read_text(encoding="utf-8-sig", errors="replace")


@pytest.fixture(autouse=True)
def _fresh(monkeypatch):
    news.reset()
    news._FABRICATING.clear()
    R.CB_COST_SEEN.clear()

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    yield
    news.reset()


def step(date):
    return next(s for s in LIVE["purse_by_step"] if s["date"] == date)


# ── 1: a declaration the game refuses for debt or prestige says so before it is sent ──

OSSORY_CBS = LIVE["cbs_32287_1069_04_22"]


def _purse_reply(gold, prestige, piety, income=3.0):
    return {"id": ME, "name": "Murchad", "alive": True, "age": 44, "liege_id": 0, "top_liege_id": ME,
            "gold": gold, "prestige": prestige, "piety": piety, "faith_key": "catholic",
            "income_breakdown": {"total_monthly": income}}


def test_1_the_recorded_refusals():
    """1069.04.22 and 07.04: cbs and plan_war offered claim_cb on Ossory, declare_war's guard
    refused it with no word; the war-validity read named the debt. 1075.07.04: the same
    for Oriel with prestige below zero (the war-validity read passed)."""
    assert LIVE["cbs_tool_reply_1069_04_22"]["best"] == "claim_cb"
    for d in ("1069_04_22", "1069_07_04", "1075_07_04"):
        b = LIVE["declare_war_reply_" + d]["reply"]["blocked"]
        assert (b["raw_reason"], b["detail"]) == ("can_declare_war_false", "the game gives no detail"), d
        assert "checked_before_sending" not in b
    a7 = next(c for c in LIVE["war_validity_1069_07_04"]["conditions"] if c["id"] == "A7")
    assert (a7["condition"], a7["pass"], a7["detail"]) == ("not_in_debt", False, "actor gold=-2.7")
    assert LIVE["war_validity_1075_07_04"]["valid"] is True
    assert step("1069.04.22")["gold"]["now"] == -9.69 and step("1069.07.04")["gold"]["now"] == -2.67
    assert step("1075.07.04")["prestige"]["now"] == -101.84
    # the bridge's cbs row priced the claim war at 0 prestige, "affordable"
    row = next(c for c in OSSORY_CBS["cbs"] if c["cb_key"] == "claim_cb")
    assert (row["valid"], row["cost_prestige"], row["affordable"]) == (True, 0, True)


def _cbs_gw(gold, prestige, piety, cbs=OSSORY_CBS):
    gw = Gw(cbs_by_target(cbs), date="1069.04.22")
    gw.replies["query_character"] = _purse_reply(gold, prestige, piety)
    return gw


def test_1_cbs_moves_the_claim_war_to_blocked_now_with_what_is_missing():
    env = run(R.cbs_impl(_cbs_gw(-9.69, 429.59, 500.4), target_id=OSSORY_LORD, character_id=ME))
    res = env["result"]
    assert res["best"] is None and res["valid_options"] == [], res["valid_options"]
    (b,) = res["blocked_now"]
    assert b["cb"] == "claim_cb" and b["declare_args"]["cb_title_id"] == C_OSSORY
    (cond,) = b["blocked_by"]
    assert (cond["condition"], cond["missing"]) == ("in_debt", 9.69)
    assert res["invalid_by_gate"]["insufficient_gold"] == 1
    assert size(env) < 2400, size(env)


def test_1_CONTROL_cbs_with_gold_offers_it():
    env = run(R.cbs_impl(_cbs_gw(75.78, 434.19, 516.4), target_id=OSSORY_LORD, character_id=ME))
    assert env["result"]["best"] == "claim_cb" and "blocked_now" not in env["result"]
    assert "checked" not in env["result"]["valid_options"][0]


def _declare_gw(gold, prestige, piety, *, guard="refused"):
    class G(SA.Gw):
        async def read(self, action, params=None):
            if action == "query_character":
                self.reads.append((action, dict(params or {})))
                return _purse_reply(gold, prestige, piety)
            if action in ("query_cbs_for_targets", "query_cbs_for_target"):
                self.reads.append((action, dict(params or {})))
                return ({"error": "unknown_action"} if action == "query_cbs_for_targets"
                        else dict(OSSORY_CBS, target_id=params.get("target_id")))
            return await super().read(action, params)
    return G(SA.ran("declare_war", guard=guard))


DA = {"cb": "claim_cb", "cb_title_id": C_OSSORY, "claimant_id": ME, "target_id": OSSORY_LORD}


def test_1_declare_war_in_debt_is_refused_before_sending_with_the_amount():
    gw = _declare_gw(-9.69, 429.59, 500.4)
    env = run(R.declare_war_impl(gw, ME, declare_args=dict(DA), confirm_straddle=True))
    b = env["blocked"]
    assert env["verdict"] == "rejected" and b["gate"] == "insufficient_gold", b
    assert "9.69 gold short" in b["summary"] and b["raw_reason"] == "in_debt"
    assert b["fix"] == {"tool": "do", "args": {"action_class": "income_levers"}}
    assert SA.scripts_sent(gw) == [], "nothing sent"
    assert size(env) < 1800, size(env)


def test_1_declare_war_with_prestige_below_the_cost_is_refused_naming_the_cost():
    gw = _declare_gw(10.45, -101.84, 725.8)
    env = run(R.declare_war_impl(gw, ME, declare_args=dict(DA), confirm_straddle=True))
    b = env["blocked"]
    assert b["gate"] == "insufficient_prestige", b
    (m,) = b["measured"]["blocked_by"]
    assert (m["cost"], m["have"], m["missing"]) == (75.0, -101.84, 176.84)
    # The basis is the game's formula for this target (was "100 unpressed"), and the
    # measured line says what the formula does not explain
    assert "several times this price from causes not read" in m["measured"] and "price for THIS target" in m["cost_basis"]
    assert SA.scripts_sent(gw) == []


def test_1_CONTROL_the_game_refusing_alone_lists_what_was_checked():
    """Gold and prestige enough: the guard's refusal is the game's, and the reply now says
    what this server checked and what it cannot."""
    gw = _declare_gw(75.78, 434.19, 516.4)
    env = run(R.declare_war_impl(gw, ME, declare_args=dict(DA), confirm_straddle=True))
    b = env["blocked"]
    assert (b["gate"], b["raw_reason"]) == ("cb_invalid_for_target", "can_declare_war_false")
    chk = b["checked_before_sending"]
    assert chk["in_debt"]["ok"] and chk["prestige"]["ok"] and chk["truce"]["ok"], chk
    assert "valid_to_start" in b["not_checked"] and len(SA.scripts_sent(gw)) == 1


def test_1_news_claim_row_names_the_gate_not_a_bare_cbs():
    gw = _cbs_gw(-9.69, 429.59, 500.4)
    nxt = run(news._claim_next(gw, ME, {"title_id": C_OSSORY, "holder_id": OSSORY_LORD}))
    assert nxt["tool"] == "cbs" and "9.69 gold short" in nxt["why"], nxt
    gw = _cbs_gw(75.78, 434.19, 516.4)
    nxt = run(news._claim_next(gw, ME, {"title_id": C_OSSORY, "holder_id": OSSORY_LORD}))
    assert nxt["tool"] == "declare_war"


def test_1_the_cost_table_is_the_games_file():
    txt = game_file("common/script_values/99_casus_belli_values.txt")
    body = txt[txt.index("claim_cb_prestige_cost = {"):]
    body = body[:body.index("desc = \"CB_CLAIM_COST_TARGET_TITLES\"")]
    found = {t: int(v) for t, v in re.findall(r"tier = tier_(\w+) \}\s*add = \{\s*value = (\d+)", body)}
    assert found == DG.CB_COST_TABLE["claim_cb"]["by_tier"], found
    pressed = re.findall(r"has_strong_claim_on = prev \} \}\s*add = -(\d+)", body)
    assert [int(p) for p in pressed] == [25, 50, 100, 200]          # 25% of each base
    # and the bridge's row says 0, the reason the table exists
    src = (HERE.parents[1] / "bridge" / "src" / "query_handlers.cpp").read_text(errors="replace")
    assert 'strstr(cb_name, "claim_cb")' in src[src.index("static CBCost cb_cost_for("):][:900]


# ── 2: the purse says what in the step moved it ─────────────────────────────────────

def test_2_every_recorded_change_was_unknown():
    causes = [r[k]["cause"] for r in LIVE["purse_by_step"] for k in ("gold", "prestige", "piety") if k in r]
    assert causes and all(str(c).startswith("unknown") for c in causes), set(causes)
    # four claims arrived; each step's gold fell 79-101 while income was ~5-7 a step
    drops = [(r["date"], r["gold"]["change"]) for r in LIVE["purse_by_step"]
             if "claims_gained" in r.get("events", {})]
    assert [d for d, _ in drops] == ["1067.11.27", "1069.04.22", "1070.09.15", "1074.11.27"]
    assert all(-101 < c < -79 for _, c in drops), drops


def _snap(date, gold, prestige, piety, *, claims=(), wars=None, council_skill=13, laws=None,
          income=2.9, titles=None):
    return news.snapshot(
        actor_id=ME, date=date,
        chr_sect={"gold": gold, "prestige": prestige, "piety": piety,
                  "income_breakdown": {"total_monthly": income}},
        wars={"wars": wars or []},
        claims={"claims": [{"title_id": t, "title_name": n, "type": "unpressed", "holder_id": 1}
                           for t, n in claims]},
        council={"council": [{"position_key": "councillor_court_chaplain", "holder_id": 56090,
                              "current_task": "task_religious_relations",
                              # Live: the learning rides `skills` (the lean `skill` is martial)
                              "skills": {"learning": council_skill}}]},
        heir=None, held={"held_titles": [{"title_id": t, "key": k} for t, k in (titles or [(463, "c_dublin")])]},
        laws={"active_laws": [{"name": x} for x in (laws or ["crown_authority_0"])]})


def test_2_a_claim_arriving_is_priced_with_the_chaplains_learning_and_income():
    """1067.09.15 -> 11.27: gold 113.9 -> 34.74 (-79.16) and the Athlone claim arrived."""
    a, b = step("1067.09.15"), step("1067.11.27")
    run(news.block(Gw({}), ME, _snap("1067.09.15", a["gold"]["now"], 410.9, 438.4), offers_served=False))
    n = run(news.block(Gw({}), ME, _snap("1067.11.27", b["gold"]["now"], 412.89, 444.8,
                                       claims=[(469, "Athlone")]), offers_served=False))
    g = n["purse"]["gold"]
    assert g["change"] == -79.16 and "cause" not in g, g
    (ev,) = g["in_this_step"]
    assert ev["what"] == "claim on Athlone arrived" and ev["expected"] == -81.0
    assert g["income"] == {"monthly": 2.9, "expected": 6.95}


def test_2_a_realm_law_passed_is_named_beside_a_prestige_drop():
    """1066.09.15 -> 11.27: prestige 600 -> 501.98 with no act of the seat. The start save
    holds crown_authority_0 (ai_will_do 1 at that level); whether the game's AI raised it
    is not recorded: this is what news says WHEN a law read shows it."""
    run(news.block(Gw({}), ME, _snap("1066.09.15", 84.0, 600.0, 400.0), offers_served=False))
    n = run(news.block(Gw({}), ME, _snap("1066.11.27", 89.29, 501.98, 406.4, laws=["crown_authority_1"]),
                       offers_served=False))
    p = n["purse"]["prestige"]
    assert p["in_this_step"][0]["what"] == "realm law passed: crown_authority_1", p
    assert "partly unknown" in p["cause"] and p["unexplained"] == -98.02


def test_2_CONTROL_nothing_seen_stays_unknown():
    run(news.block(Gw({}), ME, _snap("1066.09.15", 84.0, 600.0, 400.0), offers_served=False))
    n = run(news.block(Gw({}), ME, _snap("1066.11.27", 89.29, 501.98, 406.4), offers_served=False))
    assert n["purse"]["prestige"]["cause"].startswith("unknown")


# ── 3: appoint on the seat's own council ────────────────────────────────────────────

COUNCIL = LIVE["council_reply_1074_07_04"]


def test_3_the_recorded_refusal_was_the_ownership_gate_reading_the_courtier():
    for r in LIVE["appoint_replies_1072_11_27"]:
        assert r["reply"]["blocked"]["gate"] == "actor_not_claimed"
        assert 46877 in r["args"]["args"].values()
    # the bridge's gate falls back to params.character_id when actor_id is 0
    src = (HERE.parents[1] / "bridge" / "src" / "dispatch_router.cpp").read_text(errors="replace")
    assert 'if (actor_id <= 0) actor_id = json_get_int(params_json, "character_id");' in src
    steward = next(c for c in LIVE["observe_council_1072_11_27"] if c["seat"] == "councillor_steward")
    assert steward["vacant"] is True


def _council_gw(vacant_steward):
    rows = [dict(c, vacant=False) for c in COUNCIL]
    if vacant_steward:
        for c in rows:
            if c["position_key"] == "councillor_steward":
                c.update(holder_id=16777215, vacant=True)
    return Gw({"query_council": {"council": rows}, "appoint_councillor": {"success": True}})


def test_3_appoint_sends_the_actor_and_the_seats_position_id():
    gw = _council_gw(False)
    run(R.do_impl(gw, intent="appoint", character_id=0,
                  args={"seat": "councillor_steward", "character_id": 43295}))
    (sent,) = [p for a, p in gw.writes if a == "appoint_councillor"]
    assert (sent["actor_id"], sent["position_id"], sent["character_id"]) == (ME, 9485, 43295), sent


def test_3_a_vacant_seat_is_refused_before_sending_with_why():
    gw = _council_gw(True)
    env = run(R.do_impl(gw, intent="appoint", character_id=ME,
                        args={"seat": "steward", "character_id": 46877}))
    b = env["blocked"]
    assert b["raw_reason"] == "vacant_seat_fill_unsupported" and "game's AI fills" in b["summary"], b
    assert gw.writes == []


def test_3_the_explicit_actor_mirror_holds_for_appoint():
    src = (HERE.parents[1] / "bridge" / "src" / "action_handlers.cpp").read_text(errors="replace")
    body = src[src.index("bool do_appoint_councillor(const char* params_json"):][:1800]
    assert 'if (actor_raw <= 0) { short_error("actor_id_required"); return false; }' in body
    assert R.dispatch_actor_of(0, {"id": ME}) == ME     # Every generic intent, appoint included


# ── 4: the head of faith's claim ────────────────────────────────────────────────────

def test_4_the_bridge_sends_it_through_its_own_pipeline_with_the_title():
    sys.path.insert(0, str(HERE.parents[1] / "bridge" / "scripts"))
    import cpp_strip
    src = (HERE.parents[1] / "bridge" / "src" / "ai_eval_hook.cpp").read_text(errors="replace")
    code = cpp_strip.strip(src, keep_strings=True)
    i = code.index("direct_list[] = {", code.index("static bool is_direct_dispatch_interaction("))
    names = {body for _k, _s, _e, body in cpp_strip.literals(code[i:code.index("nullptr", i)])}
    assert "hof_ask_for_claim_interaction" in names
    # the title rides for every interaction on the list (apply_interaction_window_choices)
    body = code[code.index("bool apply_interaction_window_choices("):]
    assert "if (intent->title_id > 0)" in body[:4000]


def test_4_the_rules_are_the_interaction_files():
    txt = game_file("common/character_interactions/00_religious_interactions.txt")
    body = txt[txt.index("hof_ask_for_claim_interaction = {"):]
    body = body[:body.index("\n}\n")]
    assert "scope:actor.faith = scope:secondary_recipient.faith" in body           # who: same faith
    assert "scope:actor.faith.religious_head = {\n\t\t\t\tsave_scope_as = recipient" in body  # redirect
    assert "target_filter = secondary_recipient_domain_titles" in body
    vals = game_file("common/script_values/02_religion_values.txt")
    cv = vals[vals.index("claim_piety_value = {"):][:900]
    assert "multiply = 2" in cv and "value = scope:actor.sub_realm_size" in cv and "multiply = 10" in cv
    assert R.hof_claim_piety("county", 4) == 140 and R.hof_claim_piety("kingdom", 4) == 440


def test_4_the_row_says_whom_to_name_and_what_it_costs():
    gw = Gw({"list_interactions": {"interactions": [{"name": "hof_ask_for_claim_interaction"}]}})
    env = run(R.do_impl(gw, action_class="interactions", character_id=ME, candidates=["hof_ask"]))
    (row,) = env["result"]["available"]
    assert "unknown" not in row and "holder" in row["rules"]["send"] and "same" not in row["rules"]["send"]
    assert "shares YOUR faith" in row["rules"]["who"]
    assert size(env) < 1700, size(env)


def test_4_a_holder_of_another_faith_is_refused_before_sending():
    """Every Irish holder but Ormond's is Insular Celtic (the start save); Murchad is Catholic."""
    gw = Gw({"query_de_jure": {"title": {"title_id": C_OSSORY, "key": "c_ossory", "name": "Ossory"}}})
    gw.replies["query_character"] = lambda p: {
        "id": p["character_id"], "alive": True, "name": "x",
        "faith_key": "catholic" if p["character_id"] == ME else "insular_celtic"}
    env = run(R.do_impl(gw, intent="interact", character_id=ME,
                        args={"interaction": "hof_ask_for_claim_interaction", "target_id": OSSORY_LORD,
                              "title_id": "c_ossory"}))
    assert env["blocked"]["raw_reason"] == "faith_differs" and gw.writes == [], env["blocked"]


# ── 5: the creation bill prices the duchies the rule needs ──────────────────────────

def test_5_the_recorded_bill_was_unread_with_one_priced():
    c = LIVE["title_k_ireland_1073_02_08"]["creation"]
    assert "1 priced" in c["tier_rule"]["bill"]["unread"]


def test_5_creation_prices_both_duchies():
    rec = LIVE["title_k_ireland_1073_02_08"]
    costs = {"k_ireland": {"success": True, "engine_valid": False, "engine_reasons": [
                 "Duchy Titles Required: 2 (currently: 0)", "Kingdom Titles Required: 1 (currently: 0)",
                 "You only have 4 of 8 required Counties", "Earl Murchad does not have gold 425"],
                 "price": {"known": True, "currency": "gold", "cost": 425.0, "have": 24.29, "affordable": False}}}
    for d in ("d_leinster", "d_meath", "d_connacht", "d_ulster", "d_munster"):
        costs[d] = {"success": True, "engine_valid": False, "engine_reasons": [],
                    "price": {"known": True, "currency": "gold", "cost": 212.5, "have": 24.29, "affordable": False}}
    gw = Gw({"query_create_title_cost": lambda p: costs[p["title_key"]]}, date="1073.02.08")
    prog = {"held": 4, "total": 14, "fraction": 0.29}
    out = run(TT._creation(gw, ME, "k_ireland", prog, rec["duchies"], []))
    bill = out["tier_rule"]["bill"]
    assert bill == {"currency": "gold", "cost": 850.0, "of": ["d_leinster", "d_meath", "k_ireland"]}, bill
    assert out["nearest_unmet"]["step"] == "create d_leinster"
    # the same rows formable_titles prices when the seat names them
    assert [a for a, _ in gw.reads].count("query_create_title_cost") == 3


# ── 6: the idle block ──────────────────────────────────────────────────────────────

def test_6_the_recorded_run_had_bare_turns():
    assert len(LIVE["bare_turns"]) == 19


def test_6_idle_states_facts_with_their_enumerators_and_stays_small():
    s = news.snapshot(actor_id=ME, date="1072.11.27", chr_sect={"gold": 1.0, "prestige": -127.0, "piety": 640.6},
                      wars={"wars": []}, claims={"claims": [{"title_id": 501, "holder_id": 34580}]},
                      council={"council": [{"position_key": "councillor_steward", "holder_id": 16777215,
                                            "vacant": True, "current_task": "task_collect_taxes"},
                                           {"position_key": "councillor_spouse", "holder_id": 16777215,
                                            "vacant": True}]},
                      heir=None, council_tasks=[{"task_name": "task_religious_relations"}],
                      alliances={"alliances": []})
    i = news.idle(s)
    assert set(i) == {"vacant_seats", "no_fabrication", "claims_no_war", "no_alliance", "piety"}, i
    assert i["vacant_seats"]["seats"] == ["steward"]
    assert all(set(v["next"]) == {"tool", "args"} for v in i.values())
    # every fact at once; one fact is ~100. claims_no_war's next names the claim's own
    # read (cbs on its war target), 13 bytes more than the bare plan_war it replaced
    assert size(i) <= 514, size(i)


def test_6_CONTROL_nothing_idle_is_no_block():
    s = news.snapshot(actor_id=ME, date="1070.02.08", chr_sect={"gold": 70.0, "prestige": 300.0, "piety": 50.0},
                      wars={"wars": [{"war_id": 1, "attacker_id": ME, "defender_id": 2, "is_attacker": True}]},
                      claims={"claims": []}, council={"council": [{"position_key": "councillor_steward",
                                                                   "holder_id": 7, "current_task": "x"}]},
                      heir=None, council_tasks=[{"task_name": "task_fabricate_claim"}],
                      alliances={"alliances": [{"ally_id": 9}]})
    assert news.idle(s) is None


# ── the surface and the fixture ─────────────────────────────────────────────────────

def test_surface_thirteen_tools_and_the_dump_does_not_grow():
    ms = pytest.importorskip("mcp_server")
    tools = run(ms.mcp.list_tools())
    out = [t.model_dump(mode="json", by_alias=True, exclude_none=True) for t in tools]
    out = [{"name": d["name"], "description": d.get("description", ""), "inputSchema": d["inputSchema"]}
           for d in out]
    assert len(out) == 13
    # +268 for the shared notes' pointer in discover (test_ireland_walk_replies has it line by line)
    # +396 compact (+408 at indent=1) for do's two intents event_rule and gold_reserve: their
    # enum entries (+32), their rows in the generated args table (+165) and one description sentence (+199)
    assert len(json.dumps(out)) <= 31_049, len(json.dumps(out))      # the recorded reply's size
    by = {d["name"]: d["description"] for d in out}
    assert "blocked_now" in by["cbs"] and "idle" in by["observe"] and "debt" in by["declare_war"]


def test_fixture_is_shippable():
    s = json.dumps(LIVE)
    assert LIVE["_source"].startswith("recorded replies from a live 1066 game")
    for bad in ("/home", "/mnt", "pilot", "lane"):
        assert bad not in s, bad


def test_6_the_news_block_carries_idle_and_compact_keeps_it():
    run(news.block(Gw({}), ME, _snap("1067.02.08", 94.9, 402.97, 412.8), offers_served=False))
    n = run(news.block(Gw({}), ME, _snap("1067.04.22", 101.82, 405.94, 422.4), offers_served=False))
    assert n["idle"]["piety"]["have"] == 422 and news.compact(n)["idle"] == n["idle"]
