"""Six things a live check of the toolkit found on the Ireland task, each
row replayed from its recorded reply.

The fixture (fixtures/tier_rule_claim_replies.json.gz) holds replies recorded on a live
1066 game (CK3 1.18.3.1, toolkit 2026.12.11) as Murchad, Count of Dublin (31506), driven
by a scripted policy. Bridge replies the toolkit received are rebuilt from the tool
replies it gave; a value the recording does not hold (a candidate's sex, the ruler's
children) is written into the test as such, never into the fixture.
"""
import asyncio
import gzip
import json
import sys
from pathlib import Path

import pytest

import news
import redesign as R
from test_ireland_walk_replies import LIVE as WALK, Gw, NewsGw, _title_gw, cbs_by_target

HERE = Path(__file__).resolve().parent
LIVE = json.loads(gzip.open(HERE / "fixtures" / "tier_rule_claim_replies.json.gz", "rt").read())
ME, ATHLONE_LORD, C_ATHLONE, B_ATHLONE, B_BIRR = 31506, 29585, 469, 470, 471
REPLY_BUDGET = 8000


def run(c):
    return asyncio.run(c)


def size(env) -> int:
    return len(json.dumps(env))


def reqs_of(row) -> dict:
    return {r["requirement"]: r for r in row["requirements"]}


# ── 1: the creation tier rule is one requirement, read off the game's lines ───

def _create_gw(inputs: dict) -> Gw:
    return Gw({"query_dejure_progress": lambda p: inputs[p["title_key"]]["query_dejure_progress"],
               "query_create_title_cost": lambda p: inputs[p["title_key"]]["query_create_title_cost"]})


def _formable(inputs, keys):
    env = run(R.do_impl(_create_gw(inputs), action_class="formable_titles", character_id=ME, candidates=keys))
    return env, {r["title_key"]: r for r in env["result"]["available"] + env["result"]["unavailable"]}


def test_1_the_recorded_reply_flattened_the_rule():
    """Live 1066.09.15: the game's lines came back as four requirements (and an empty
    header) beside tier_rule met null; a seat read that it needed a kingdom AND an empire."""
    (k,) = [r for r in LIVE["formable_reply_1066_09_15"]["unavailable"] if "k_ireland" in r["label"]]
    rec = reqs_of(k)
    assert rec["game_check"]["reasons"][:5] == ["", "Duchy Titles Required: 2 (currently: 0)",
                                                "Kingdom Titles Required: 1 (currently: 0)",
                                                "Empire Titles Required: 1 (currently: 0)",
                                                "Hegemony Titles Required: 1 (currently: 0)"]
    assert rec["tier_rule"]["met"] is None


def test_1_kingdom_tier_rule_is_one_row_for_a_count_with_the_bill():
    env, rows = _formable(LIVE["create_title_1066_09_15"], ["d_meath", "d_leinster", "k_ireland"])
    k = reqs_of(rows["k_ireland"])
    tr = k["tier_rule"]
    assert tr["met"] is False and tr["your_tier"].startswith("below duchy")
    assert tr["rule"] == "become a duke holding 2 duchy titles (or hold 1 title of kingdom tier or above)"
    assert tr["one_of"] == [{"tier": "duchy", "need": 2, "have": 0}, {"tier": "kingdom", "need": 1, "have": 0},
                            {"tier": "empire", "need": 1, "have": 0}, {"tier": "hegemony", "need": 1, "have": 0}]
    # the tier lines and their empty header left the flat list; the rest stayed
    assert k["game_check"]["reasons"] == ["You only have 1 of 8 required Counties",
                                          "Earl Murchad does not have gold 425"]
    # the bill: the two cheapest priced duchies of this reply plus the kingdom (live 212.50 x 2 + 425)
    assert [d["title_key"] for d in tr["duchies_priced"]] == ["d_meath", "d_leinster"]
    assert [d["cost"] for d in tr["duchies_priced"]] == [212.5, 212.5]
    assert tr["bill"] == {"currency": "gold", "cost": 850.0, "of": ["d_meath", "d_leinster", "k_ireland"]}
    # a duchy's own check carries no tier lines here, so it gets no tier row
    assert "tier_rule" not in reqs_of(rows["d_meath"])
    assert size(env) < REPLY_BUDGET, size(env)


def test_1_the_numbers_come_from_the_lines():
    inputs = json.loads(json.dumps(LIVE["create_title_1068_09_15"]))
    rs = inputs["k_ireland"]["query_create_title_cost"]["engine_reasons"]
    rs[1] = "Duchy Titles Required: 3 (currently: 1)"
    rs[2] = "Kingdom Titles Required: 2 (currently: 0)"
    _env, rows = _formable(inputs, ["k_ireland"])
    tr = reqs_of(rows["k_ireland"])["tier_rule"]
    assert tr["rule"] == ("hold 3 duchy titles (you hold 1) (or hold 2 kingdom titles, or hold 1 empire "
                          "title, or hold 1 hegemony title)"), tr["rule"]
    assert tr["your_tier"] == "duchy" and tr["met"] is False
    assert "bill" in tr and tr["bill"]["cost"] is None      # no duchy priced in this reply
    rs[1] = "Duchy Titles Required: 2 (currently: 2)"
    _env, rows = _formable(inputs, ["k_ireland"])
    assert reqs_of(rows["k_ireland"])["tier_rule"]["met"] is True


def test_1_no_tier_lines_is_unknown_and_mint_carries_the_same_rows():
    inputs = json.loads(json.dumps(LIVE["create_title_1066_09_15"]))
    inputs["k_ireland"]["query_create_title_cost"]["engine_reasons"] = ["You only have 1 of 8 required Counties"]
    _env, rows = _formable(inputs, ["k_ireland"])
    tr = reqs_of(rows["k_ireland"])["tier_rule"]
    # The game's check lists only what it fails,
    # so a check that ran and listed no 'Titles Required' line passed the tier rule
    assert tr["met"] is True and tr["rule"] == R.TIER_RULE_PASSED
    gw = _create_gw(LIVE["create_title_1066_09_15"])
    mint = run(R.do_impl(gw, intent="mint_title", character_id=ME, args={"title_key": "k_ireland"}))
    got = {r["requirement"]: r for r in mint["blocked"]["measured"]["requirements"]}
    assert got["tier_rule"]["rule"].startswith("become a duke holding 2 duchy titles")


# ── 2: war_targets reads a claim the way cbs does ─────────────────────────────

def _after_claim_gw():
    return Gw({"query_valid_war_targets": LIVE["valid_war_targets_1067_11_27"],
               **cbs_by_target(WALK["cbs_29585_1067_11_27"])}, date="1067.11.27")


def test_2_the_recorded_war_targets_said_no_claim_while_cbs_offered_it():
    rec = LIVE["war_targets_reply_1067_11_27"]["targets"][0]
    assert rec["cb"] == "claim" and rec["declarable"] is False and "has no claim" in rec["blocked"]["summary"]
    assert LIVE["cbs_reply_1067_11_27"]["valid_options"][0]["cb"] == "claim_cb"


def test_2_the_claim_row_is_declarable_with_cbs_declare_args():
    env = run(R.war_targets_impl(_after_claim_gw(), ME))
    res = env["result"]
    claim = next(r for r in res["targets"] if r["cb"] == "claim_cb")
    want = LIVE["cbs_reply_1067_11_27"]["valid_options"][0]["declare_args"]
    assert claim["declarable"] is True and claim["declare_args"] == want, claim
    assert claim["title_id"] == C_ATHLONE and claim["title_key"] == "c_athlone"
    assert res["declarable_count"] == 1 and res["targets"][0] is claim
    # control: the conquest casus belli on the same county stays refused, as cbs refuses it
    conq = next(r for r in res["targets"] if r["cb"] == "county_conquest_cb" and r["title_id"] == C_ATHLONE)
    assert conq["declarable"] is False


def test_2_the_same_read_as_inspect_title_and_cbs():
    gw = _title_gw(claims=[{"title_id": C_ATHLONE, "type": "unpressed"}], cbs=WALK["cbs_29585_1067_11_27"])
    import title_table as TT    # A call several rows repeat is named by a template (title_table.seat_rows)
    res = run(R.inspect_impl(gw, subject="title", character_id=ME, title_key="k_ireland"))["result"]
    rows = {r["key"]: dict(r, next_call=TT.next_call_of(res, r)) for r in res["counties"]}
    wt = run(R.war_targets_impl(_after_claim_gw(), ME))["result"]["targets"][0]
    cbs = run(R.cbs_impl(Gw(cbs_by_target(WALK["cbs_29585_1067_11_27"])), target_id=ATHLONE_LORD,
                         character_id=ME))["result"]["valid_options"][0]["declare_args"]
    assert rows["c_athlone"]["next_call"]["args"]["declare_args"] == wt["declare_args"] == cbs


# ── 3: marriage rows name a pair the fields allow ─────────────────────────────

#: NOT recorded (the walk did not read them): Murchad's three children and the
#: candidates' sexes, written here so the pairing can be graded.
KIDS = {36124: {"id": 36124, "name": "Son", "is_female": False, "age": 20, "spouses": [], "alive": True},
        37111: {"id": 37111, "name": "Daughter", "is_female": True, "age": 17, "spouses": [], "alive": True},
        37528: {"id": 37528, "name": "Boy", "is_female": False, "age": 10, "spouses": [], "alive": True}}
FEMALE = {33379}                      # Hedwig; the rest are taken as men


def _people(with_candidates=True):
    out = {ME: LIVE["character_31506_1066_09_15"], **KIDS}
    if with_candidates:
        for c in LIVE["marriage_bridge_1066_09_15"]["candidates"]:
            out[c["id"]] = {"id": c["id"], "name": c.get("name_key"), "is_female": c["id"] in FEMALE,
                            "age": c["age"], "spouses": [], "alive": True}
    return out


def _reader(people, log=None):
    async def read(action, p):
        if log is not None:
            log.append((action, p.get("character_id")))
        return people.get(p.get("character_id"), {}) if action == "query_character" else {}
    return read


def test_3_the_recorded_first_row_married_the_married_ruler_to_a_man():
    rec = LIVE["marriage_reply_1066_09_15"]
    first = rec["candidates"][0]
    assert first["name_key"] == "Svend" and first["do_call"]["args"]["secondary_actor_id"] == ME
    assert LIVE["character_31506_1066_09_15"]["spouse_id"] == 31658       # Murchad was married
    assert LIVE["marriage_check_30201"]["sendable"] is False
    assert len(json.dumps(rec)) > 16000


def test_3_rows_pair_an_unmarried_adult_of_the_other_sex_on_your_side():
    out = run(R.marriage_rows(LIVE["marriage_bridge_1066_09_15"], _reader(_people())))
    rows = out["candidates"]
    assert rows and len(rows) <= R.MARRIAGE_ROWS_SHOWN
    sides = {y["id"]: y for y in out["your_side"]}
    assert set(sides) == {36124, 37111}                    # not the married ruler, not the boy of 10
    for r in rows:
        a = r["do_call"]["args"]["args"]
        assert r["do_call"]["tool"] == "do" and r["do_call"]["args"]["intent"] == "interact"
        assert a["secondary_actor_id"] != ME and a["secondary_actor_id"] in sides
        assert sides[a["secondary_actor_id"]]["female"] is not r["female"]
        assert a["target_id"] == a["secondary_recipient_id"] == r["id"] and r["age"] >= 16
    assert rows[0]["id"] == 30201 and rows[0]["pair"]["id"] == 37111     # Svend, to the daughter
    assert all(r["pair"]["id"] == 37111 for r in rows)                    # every listed one a man here
    one = dict(LIVE["marriage_bridge_1066_09_15"],
               candidates=[c for c in LIVE["marriage_bridge_1066_09_15"]["candidates"] if c["id"] in FEMALE])
    (hedwig,) = run(R.marriage_rows(one, _reader(_people())))["candidates"]
    assert hedwig["name"] == "Hedwig" and hedwig["pair"]["id"] == 36124  # a woman, to the son
    # Philippe, 14, and (every bridge row is read now, not the first 16) one more under 16
    assert out["not_listed"]["under age"] == 2
    assert sum(out["not_listed"].values()) + len(rows) == 30              # every bridge row accounted
    assert out["check"] == R.MARRIAGE_CHECK_UNREAD   # This reader answers no check


def test_3_unread_sex_or_no_one_to_offer_lists_nothing():
    out = run(R.marriage_rows(LIVE["marriage_bridge_1066_09_15"], _reader(_people(with_candidates=False))))
    assert out["candidates"] == [] and out["not_listed"]["sex not read"] >= 1
    people = _people()
    for k in KIDS:
        people[k] = dict(people[k], spouses=[{"id": 1, "alive": True}])
    out = run(R.marriage_rows(LIVE["marriage_bridge_1066_09_15"], _reader(people)))
    assert out["candidates"] == [] and "you are married (spouse 31658)" in out["your_side_empty"]


def test_3_the_query_reply_is_under_the_budget(monkeypatch):
    ms = pytest.importorskip("mcp_server")
    state = ms.BridgeState()
    state.connected = True
    monkeypatch.setattr(ms, "_get_state", lambda: state)
    people = _people()

    async def qb(_s, action, params, timeout=15.0):
        if action == "query_marriage_candidates":
            return LIVE["marriage_bridge_1066_09_15"]
        if action == "query_character":
            return KIDS.get(params.get("character_id")) or people.get(params.get("character_id"), {})
        return {}

    async def now():
        return Gw()

    async def bound(*_a):
        return None
    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(ms, "_gateway_now", now)
    monkeypatch.setattr(R, "unbound_refusal", bound)
    env = run(ms.query(domain="marriage_candidates", character_id=ME))
    assert env["ok"] is True and env["result"]["candidates"], env
    assert size(env) < REPLY_BUDGET, size(env)           # was 17,015 bytes live


# ── 4: the head of faith's claim interaction says what is unknown ─────────────

def test_4_the_direct_dispatch_mirror_is_the_bridges_list():
    sys.path.insert(0, str(HERE.parents[1] / "bridge" / "scripts"))
    import cpp_strip                                     # the one C++ tokenizer
    src = (HERE / ".." / ".." / "bridge" / "src" / "ai_eval_hook.cpp").read_text(errors="replace")
    code = cpp_strip.strip(src, keep_strings=True)       # comments blanked, strings kept
    i = code.index("direct_list[] = {", code.index("static bool is_direct_dispatch_interaction("))
    names = {body for _k, _s, _e, body in cpp_strip.literals(code[i:code.index("nullptr", i)])}
    assert names == R.DIRECT_DISPATCH_INTERACTIONS
    # The head of faith's claim is on the bridge's list now (test_declare_war_cost_gates)
    assert "hof_ask_for_claim_interaction" in R.DIRECT_DISPATCH_INTERACTIONS


def test_4_the_listing_row_names_what_is_unknown():
    rec = LIVE["hof_interactions_reply"]["available"][0]
    assert set(rec) == {"label", "do_call"}                      # live: no target, no cost
    assert LIVE["hof_acceptance_29012"]["checked_by"].endswith("(interaction_not_checkable)")
    gw = Gw({"list_interactions": {"interactions": [{"name": "hof_ask_for_claim_interaction"},
                                                    {"name": "sway_interaction"}]}})
    env = run(R.do_impl(gw, action_class="interactions", character_id=ME, candidates=["hof_ask", "sway"]))
    rows = {r["label"]: r for r in env["result"]["available"]}
    # What was unknown is now the interaction file's rules (test_declare_war_cost_gates)
    u = rows["hof_ask_for_claim_interaction"]["rules"]
    assert "head of faith" in u["send"] and "args.title_id" in u["send"] and "piety" in u["cost"]
    assert "unknown" not in rows["sway_interaction"] and "rules" not in rows["sway_interaction"]


def test_4_a_title_passes_through_interact_and_is_flagged_where_not_applied(monkeypatch):
    monkeypatch.setattr(R, "DRAIN_SCHEDULE_S", (0.0,), raising=False)

    def gw():
        g = Gw({"query_de_jure": {"title": {"title_id": C_ATHLONE, "key": "c_athlone", "name": "Athlone"}}})
        return g
    g = gw()
    env = run(R.do_impl(g, intent="interact", character_id=ME,
                        args={"interaction": "hof_ask_for_claim_interaction", "target_id": 29012,
                              "title_id": "c_athlone"}))
    (sent,) = [p for a, p in g.writes if a == "send_interaction"]
    assert sent["title_id"] == C_ATHLONE and sent["target_id"] == 29012, sent
    # The bridge now sets the title for this interaction too
    assert "title_not_applied" not in [w["code"] for w in env["warnings"]], env["warnings"]
    g = gw()
    env = run(R.do_impl(g, intent="interact", character_id=ME,
                        args={"interaction": "befriend_x_interaction", "target_id": 29012,
                              "title_id": "c_athlone"}))
    assert "title_not_applied" in [w["code"] for w in env["warnings"]], env["warnings"]
    g = gw()
    env = run(R.do_impl(g, intent="interact", character_id=ME,
                        args={"interaction": "revoke_title_interaction", "target_id": 29012,
                              "title_id": "c_athlone"}))
    assert "title_not_applied" not in [w["code"] for w in env["warnings"]]


# ── 5: one shape for a ready call ─────────────────────────────────────────────

#: the ready-call fields, written here too (the control below runs on any tree);
#: test_5_the_envelope_rule_and_its_control checks the server's set is this one
CALL_KEYS = {"do_call", "next_call", "next", "fix", "check_first", "enumerate"}
TOOLS = {"query", "discover", "steer", "pass_turn", "observe", "plan_war", "declare_war", "succession_plan",
         "set_phase", "do", "inspect", "cbs", "session"}


def old_call_shapes(obj, path="$") -> list:
    """Every ready-call field (CALL_KEYS) holding a call in another shape than
    {tool, args}: do's own arguments ({intent|action_class, ...}) or {<tool>: args}."""
    bad = []
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k in CALL_KEYS and isinstance(v, dict) and "tool" not in v and (
                    "intent" in v or "action_class" in v or (len(v) == 1 and set(v) <= TOOLS)):
                bad.append(f"{path}.{k}")
            if k in CALL_KEYS and isinstance(v, dict) and "tool" in v:
                assert v["tool"] in TOOLS and isinstance(v.get("args"), dict), (path, k, v)
            bad += old_call_shapes(v, f"{path}.{k}")
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            bad += old_call_shapes(v, f"{path}[{i}]")
    return bad


def test_5_the_recorded_replies_carry_the_old_shapes():
    """The control the walk below needs: on the recorded replies it finds the shapes
    the scripted driver broke on (title rows {do: ...}, rows' do_call {intent, args})."""
    found = {name: old_call_shapes(rep) for name, rep in LIVE["ready_call_replies"].items()}
    assert any(p.endswith(".next_call") for p in found["inspect_title_k_ireland_1066_09_15"])
    assert any(p.endswith(".next_call") for p in found["inspect_title_k_ireland_1067_11_27"])
    for name in ("formable_titles_1066_09_15", "marriage_candidates_1066_09_15", "income_levers_1066_09_15",
                 "fabricate_within_k_ireland_1066_09_15", "fabricate_list_1066_09_15", "interactions_1066_09_15"):
        assert found[name], name
    assert found["observe_1067_11_27"] == []                      # news was already {tool, args}


def test_5_every_reply_rebuilt_from_the_recorded_inputs_has_one_shape():
    replies = {}
    gw = _title_gw(claims=[{"title_id": C_ATHLONE, "type": "unpressed"}], cbs=WALK["cbs_29585_1067_11_27"])
    replies["inspect_title"] = run(R.inspect_impl(gw, subject="title", character_id=ME, title_key="k_ireland"))
    replies["fabricate_within"] = run(R.do_impl(_title_gw(), action_class="fabricate_targets", character_id=ME,
                                                args={"within": "k_ireland"}))
    ids = WALK["fabricate_row_titles"][str(ME)]
    replies["fabricate_list"] = run(R.do_impl(Gw({"query_fabricate_targets": {
        "success": True, "assignable_count": len(ids),
        "targets": [{"title_id": t, "title_key": f"c_{t}", "assignable": True} for t in ids]}}),
        action_class="fabricate_targets", character_id=ME))
    replies["formable"], _ = _formable(LIVE["create_title_1066_09_15"], ["d_meath", "d_leinster", "k_ireland"])
    replies["mint"] = run(R.do_impl(_create_gw(LIVE["create_title_1066_09_15"]), intent="mint_title",
                                    character_id=ME, args={"title_key": "k_ireland"}))
    replies["income"] = run(R.do_impl(Gw(), action_class="income_levers", character_id=ME))
    replies["interactions"] = run(R.do_impl(Gw({"list_interactions": {"interactions": [
        {"name": n} for n in WALK["interactions_names"]]}}), action_class="interactions", character_id=ME))
    replies["marriage"] = R.envelope("query", result=run(R.marriage_rows(LIVE["marriage_bridge_1066_09_15"],
                                                                         _reader(_people()))))
    replies["war_targets"] = run(R.war_targets_impl(_after_claim_gw(), ME))
    replies["cbs"] = run(R.cbs_impl(Gw(cbs_by_target(WALK["cbs_29585_1067_11_27"])), target_id=ATHLONE_LORD,
                                    character_id=ME))
    news.reset()
    news._FABRICATING.clear()
    ngw = NewsGw()
    run(R.observe_impl(ngw, character_id=ME))
    news.note_fabrication(ME, C_ATHLONE, "c_athlone", "court_chaplain", "1066.09.15")
    ngw.date, ngw.claims, ngw.retasked = "1067.11.27", WALK["claims_1067_11_27"], True
    replies["observe_news"] = run(R.observe_impl(ngw, character_id=ME))
    for name, env in replies.items():
        assert old_call_shapes(env) == [], (name, old_call_shapes(env))
    # the walk saw each kind of ready call, in the one shape
    import title_table as TT    # A call several rows repeat is named by a template (title_table.seat_rows)
    it = replies["inspect_title"]["result"]
    t = {r["key"]: dict(r, next_call=TT.next_call_of(it, r)) for r in it["counties"]}
    assert t["c_athlone"]["next_call"]["tool"] == "declare_war"
    assert t["c_ulster"]["next_call"] == {"tool": "do", "args": {"intent": "council_task", "args": {
        "seat": R.FABRICATE_CLAIM_SEAT, "task": "task_fabricate_claim", "target_title_id": 474}}}
    assert replies["formable"]["result"]["unavailable"][0]["do_call"]["tool"] == "do"
    assert replies["income"]["result"]["available"][0]["do_call"]["args"]["intent"] == "build"
    assert replies["observe_news"]["result"]["news"]["claims_gained"][0]["next"]["tool"] == "declare_war"


def test_5_the_envelope_rule_and_its_control():
    assert R.READY_CALL_KEYS == CALL_KEYS
    env = R.envelope("do", result={"rows": [{"do_call": {"intent": "x", "args": {"a": 1}}},
                                            {"next": {"tool": "cbs", "args": {"target_id": 1}}},
                                            {"fix": {"rejected_fields": ["a"]}}]})
    rows = env["result"]["rows"]
    assert rows[0]["do_call"] == {"tool": "do", "args": {"intent": "x", "args": {"a": 1}}}
    assert rows[1]["next"] == {"tool": "cbs", "args": {"target_id": 1}}   # already the shape: unchanged
    assert rows[2]["fix"] == {"rejected_fields": ["a"]}                    # not a call: unchanged
    again = R.envelope("do", result=env["result"])
    assert again["result"] == env["result"]                                # idempotent


# ── 6: a barony gained with its county takes the county's how ─────────────────

def test_6_the_recorded_barony_read_how_null():
    rows = {t["key"]: t for t in LIVE["news_1068_09_15"]["titles_gained"]}
    assert rows["c_athlone"]["how"].endswith("attacker_victory") and rows["b_athlone"]["how"] is None
    (birr,) = LIVE["news_1068_11_27"]["titles_gained"]
    assert birr["key"] == "b_birr" and birr["how"] is None


def test_6_a_barony_gained_with_its_county_takes_the_county_how():
    news.reset()
    news._FABRICATING.clear()
    gw = NewsGw()
    run(R.observe_impl(gw, character_id=ME))
    gw.date, gw.wars = "1068.09.15", {"wars": []}
    base = gw.held["held_titles"]
    gw.held = {"held_titles": base + [
        {"title_id": C_ATHLONE, "key": "c_athlone", "tier": 2, "de_jure_parent_key": "d_meath"},
        {"title_id": B_ATHLONE, "key": "b_athlone", "tier": 1, "de_jure_parent_key": "c_athlone"}]}
    n = run(R.observe_impl(gw, character_id=ME))["result"]["news"]
    got = {t["key"]: t for t in n["titles_gained"]}
    assert got["b_athlone"]["how"] == got["c_athlone"]["how"] == "war 16777231 ended attacker_victory"
    assert got["b_athlone"]["with_county"] == "c_athlone" and "how_unread" not in got["b_athlone"]
    # b_birr, de jure under Athlone, came a step later: not "with" the county (no with_county), and (pin moved
    # deliberately: b_slemish / b_armagh in another run) says it is inside Athlone with Athlone's recorded cause
    gw.date = "1068.11.27"
    gw.held = {"held_titles": gw.held["held_titles"] + [
        {"title_id": B_BIRR, "key": "b_birr", "tier": 1, "de_jure_parent_key": "c_athlone"}]}
    n = run(R.observe_impl(gw, character_id=ME))["result"]["news"]
    (birr,) = n["titles_gained"]
    assert "with_county" not in birr and birr["inside_county"] == "c_athlone"
    assert birr["how"] == "inside c_athlone, which you gained on or before 1068.09.15: war 16777231 ended " \
                          "attacker_victory"
    assert birr["barony_source"].startswith("the county's recorded cause")


# ── the surface ───────────────────────────────────────────────────────────────

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
    assert len(json.dumps(out)) <= 31_065, len(json.dumps(out))      # the recorded reply's size
