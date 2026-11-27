"""The reads a ruler on the Ireland task needs to declare a claim war, each row replayed from
its recorded reply.

The fixture (fixtures/claim_war_replies.json.gz) holds whole tool replies recorded on a live
1066 game (a toolkit one release older than this tree) as the Dublin line, the
seat Donnchad (37528) from 1074. A value the recording does not hold is written into the
test as such, never into the fixture. Rules read from the game's own files are checked
against the install when one is present (CK3_GAME_DIR, or the default Steam path) and
skipped otherwise.
"""
import asyncio
import gzip
import json
import os
import re
from pathlib import Path

import pytest

import declare_gates as DG
import news
import redesign as R
import refusal_memo
import title_refs
from test_ireland_walk_replies import Gw

HERE = Path(__file__).resolve().parent
BRIDGE = HERE / ".." / ".." / "bridge"
LIVE = json.loads(gzip.open(HERE / "fixtures" / "claim_war_replies.json.gz", "rt").read())
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")
SEAT, CONCHOBAR, REALM_68530, ORIEL_LORD = 37528, 29585, 68530, 32422
C_ATHLONE, C_ORIEL, D_MEATH = 469, 479, 462


def run(c):
    return asyncio.run(c)


def reply(name):
    return LIVE[name]["reply"]["result"]


def game_file(rel: str) -> str:
    p = GAME / rel
    if not p.is_file():
        pytest.skip(f"no game install at {GAME}")
    return p.read_text(encoding="utf-8-sig", errors="replace")


def src(rel: str) -> str:
    return (BRIDGE / rel).read_text(errors="replace")


def _profiles():
    """query_character as recorded: Conchobar (29585) under 68530; the seat and 68530
    independent (the recording's inspect of 29585, and its cbs target_state for 68530)."""
    con = dict(reply("inspect_29585_1075_09_15")["result"])
    return {CONCHOBAR: con,
            REALM_68530: {"id": REALM_68530, "alive": True, "liege_id": 0, "top_liege_id": REALM_68530},
            SEAT: {"id": SEAT, "name": "Donnchad", "alive": True, "liege_id": 0, "top_liege_id": SEAT,
                   "gold": 34.0, "prestige": 50.0, "piety": 238.9}}


def _gw(**extra):
    prof = _profiles()
    gw = Gw(dict(extra), date="1075.09.15")
    gw.replies.setdefault("query_character", lambda p: prof.get(p.get("character_id")) or prof[SEAT])
    return gw


@pytest.fixture(autouse=True)
def _fresh(monkeypatch):
    news.reset()
    news._FABRICATING.clear()
    news._FAB_SEEN.clear()
    R.CB_COST_SEEN.clear()

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    yield
    news.reset()
    news._FABRICATING.clear()


# ── 1: a claim on a title held by someone's vassal reads as no claim ─────────────────────

def test_1_the_recorded_disagreement():
    """1075.09.15: the seat's own claim on c_athlone (holder Conchobar, 68530's vassal). The
    claim read listed it; cbs(68530) culled claim_cb claim_not_pressed; declare_war refused
    the same; the game's own validity read passed claim_cb against 68530."""
    (claim,) = reply("inspect_claims_1075_09_15")["result"]["claims"]
    assert (claim["title_id"], claim["holder_id"], claim["holder_liege_id"], claim["type"]) == \
        (C_ATHLONE, CONCHOBAR, REALM_68530, "unpressed")
    assert claim["block_reason"] == "target_not_independent"
    cbs = reply("cbs_68530_1075_09_15")["result"]
    assert cbs["invalid_by_gate"]["claim_not_pressed"] == 1 and cbs["valid_options"] == []
    assert reply("declare_469_vs_68530_1075_09_15")["blocked"]["raw_reason"] == "claim_not_pressed"
    iv = reply("interaction_validity_68530_1075_09_15")["result"]
    assert (iv["valid"], iv["inner_status"], iv["cb"]) == (True, "pass", "claim_cb")
    assert reply("declare_469_vs_29585_1076_04_22")["blocked"]["gate"] == "target_not_independent"


def test_1_the_game_keeps_a_claim_war_on_a_vassals_title():
    """claim_cb's should_invalidate keeps the war while a target title's holder is the
    defender OR the defender is his liege or above: the rule the bridge reads with."""
    f = game_file("common/casus_belli_types/00_claim.txt")
    body = f[f.index("should_invalidate"):][:700]
    assert "this = scope:defender" in body and "target_is_liege_or_above = scope:defender" in body
    rule = src("include/claim_rule.h")
    assert "title_in_defender_realm" in rule and "claim_war_defender" in rule


def test_1_every_claim_walker_reads_the_realm_not_the_holder():
    q = src("src/query_handlers.cpp")
    for walker in ("static bool attacker_has_pressed_claim_on", "static uint32_t resolve_pressed_claim_title"):
        body = q[q.index(walker):][:1600]
        assert "claim_holder_in_realm(holder, defender_id)" in body, walker
        assert "if (holder == defender_id)" not in body, walker
    assert "claim_war_defender_for(c, holder_char_id)" in q          # query_claims
    assert "claim_war_defender_for(ac, holder_id)" in q              # the valid war targets


def test_1_a_vassal_holder_named_as_the_target_is_his_realms_ruler():
    tid, note = run(R.realm_war_target(_gw(), SEAT, CONCHOBAR))
    assert tid == REALM_68530 and note["code"] == "target_is_realm_ruler"
    assert note["given_target_id"] == CONCHOBAR and "top liege" in note["detail"]


def test_1_CONTROL_an_independent_target_and_one_in_your_own_realm_stay():
    gw = _gw()
    assert run(R.realm_war_target(gw, SEAT, REALM_68530)) == (REALM_68530, None)
    prof = _profiles()
    prof[SEAT] = dict(prof[SEAT], liege_id=REALM_68530, top_liege_id=REALM_68530)
    gw2 = Gw({})
    gw2.replies.setdefault("query_character", lambda p: prof[p["character_id"]])
    assert run(R.realm_war_target(gw2, SEAT, CONCHOBAR)) == (CONCHOBAR, None)    # same realm: game decides
    prof[CONCHOBAR] = dict(prof[CONCHOBAR], top_liege_id=0)                       # walk not finished
    prof[SEAT] = _profiles()[SEAT]
    assert run(R.realm_war_target(gw2, SEAT, CONCHOBAR)) == (CONCHOBAR, None)


def test_1_cbs_on_the_holder_reads_the_realms_ruler():
    seen = []

    def cbs(p):
        seen.append(p.get("target_id"))
        return {"cbs": [], "target_state": {"independent": True}}
    gw = _gw(query_cbs_for_target=cbs)
    env = run(R.cbs_impl(gw, target_id=CONCHOBAR, character_id=SEAT))
    assert seen and set(seen) == {REALM_68530}, seen
    assert env["result"]["target_id"] == REALM_68530
    assert any(w["code"] == "target_is_realm_ruler" for w in env["warnings"])


def test_1_declare_war_on_the_holder_declares_on_his_realms_ruler():
    """1076.04.22 the seat named Conchobar and was refused target_not_independent. Now the
    reads and the declaration are about 68530; the pre-check read here answers as the
    recorded (old) bridge did, so the call stops there, naming the target it used."""
    seen = []

    def strict(p):
        seen.append(p.get("target_id"))
        return {"results": [{"cb_name": "claim_cb", "target_mode": "claim", "outer_status": "pass",
                             "inner_status": "pass", "declarable": False, "cull_reason": "claim_not_pressed"}]}
    gw = _gw(get_active_wars={"wars": []}, query_cb_validity_strict=strict)
    env = run(R.declare_war_impl(gw, character_id=SEAT, target_id=CONCHOBAR, cb="claim_cb",
                                 cb_title_id=C_ATHLONE, claimant_id=SEAT))
    assert seen == [REALM_68530], seen
    assert any(w["code"] == "target_is_realm_ruler" for w in env["warnings"])


def test_1_news_claim_row_names_the_realms_ruler():
    seen = []

    def cbs(p):
        seen.append(p.get("target_id"))
        return {"cbs": [{"cb_key": "claim_cb", "valid": True, "target_mode": 6, "cb_title_id": C_ATHLONE,
                         "claimant_id": SEAT, "family": "claim"}], "target_state": {"in_truce": False}}
    gw = _gw(query_cbs_for_target=cbs, query_cbs_for_targets={"error": "unknown_action"})
    nxt = run(news._claim_next(gw, SEAT, {"title_id": C_ATHLONE, "holder_id": CONCHOBAR}))
    assert set(seen) == {REALM_68530} and nxt["tool"] == "declare_war"
    assert nxt["args"]["declare_args"]["target_id"] == REALM_68530
    # a bridge row that already names the war's target is used as given (one read)
    seen.clear()
    nxt = run(news._claim_next(gw, SEAT, {"title_id": C_ATHLONE, "holder_id": CONCHOBAR,
                                          "war_target_id": REALM_68530}))
    assert set(seen) == {REALM_68530}


def test_1_the_refusal_names_whom_to_pass():
    assert "top liege" in R.GATE_WHY["target_not_independent"]


def test_1_a_second_claim_in_one_realm_is_gated_by_the_same_casus_belli():
    """Live 1081.01.01 (a live check): c_ailech against 32422 went to the game
    unchecked ("the casus-belli read lists no claim_cb on this title"), because the cbs read
    names one claim title per defender (c_oriel, 479); prestige was -513.6."""
    prof = _profiles()
    prof[SEAT] = dict(prof[SEAT], prestige=-513.6, gold=4.26)
    prof[ORIEL_LORD] = {"id": ORIEL_LORD, "alive": True, "liege_id": 0, "top_liege_id": ORIEL_LORD}
    cbs = {"cbs": [{"cb_key": "claim_cb", "valid": True, "target_mode": 6, "cb_title_id": C_ORIEL, "scope": "county",
                    "claimant_id": SEAT, "family": "claim"}], "target_state": {"in_truce": False}}
    strict = {"results": [{"cb_name": "claim_cb", "target_mode": "claim", "outer_status": "pass",
                           "inner_status": "pass", "declarable": True}]}
    gw = Gw({"get_active_wars": {"wars": []}, "query_cb_validity_strict": strict,
             "query_cbs_for_targets": {"error": "unknown_action"}, "query_cbs_for_target": cbs})
    gw.replies.setdefault("query_character", lambda p: prof.get(p.get("character_id")) or prof[SEAT])
    env = run(R.declare_war_impl(gw, character_id=SEAT, target_id=ORIEL_LORD, cb="claim_cb",
                                 cb_title_id=485, claimant_id=SEAT))
    assert env["blocked"]["gate"] == "insufficient_prestige", env["blocked"]
    # The price read (cb_prices, read only) is not a send
    assert not [w for w in gw.writes if w[0] == "script_action" and w[1].get("template") != "cb_prices"]


# ── 2: a generated script never names a title script cannot resolve ─────────────────────

def test_2_the_recorded_script_named_a_dynamic_title():
    b = reply("declare_oriel_1078_11_27")
    script = b["result"]["raw"]["script"]
    assert "defender = title:x_script_846.holder" in script
    assert b["blocked"]["detail"] == "can_declare_war trigger [ defender was null ]"
    who = reply("inspect_32422_1078_11_27")["result"]
    assert (who["primary_title_key"], who["has_realm"]) == ("x_script_846", True)


def test_2_every_reference_goes_through_the_one_key_rule():
    """The script templates are the one composition site (no other source builds a
    `title:` reference); every title slot is checked by is_script_title_key, and the
    bridge picks a held title script resolves, refusing (target_not_script_addressable,
    the server's fallback to the native path) when there is none."""
    tpl = src("include/script_templates.h")
    assert "inline bool is_script_title_key(std::string_view k)" in tpl
    assert "auto ref = [](const std::string& k) { return is_script_title_key(k); };" in tpl
    assert not re.search(r"need\(s\.(actor_title|target_title|claimant_title|war_title|task_target_\w+)\)", tpl)
    ah = src("src/action_handlers.cpp")
    body = ah[ah.index("std::string primary_title_key_of(uint32_t cid"):][:2400]
    assert "is_script_title_key(key)" in body and "for (int32_t i = 0; i < cap; i++)" in body
    tk = ah[ah.index("std::string title_key_of(uint32_t tid"):][:900]
    assert "is_script_title_key(key)" in tk and "target_not_script_addressable" in tk
    assert "target_not_script_addressable" in R.SCRIPT_UNADDRESSABLE
    import sys
    sys.path.insert(0, str(BRIDGE / "scripts"))
    import cpp_strip
    hits = []
    for p in sorted((BRIDGE / "src").glob("*.cpp")) + sorted((BRIDGE / "include").glob("*.h")):
        code = cpp_strip.strip(p.read_text(errors="replace"), keep_strings=True)
        if '"title:' in code:
            hits.append(p.name)
    assert hits == ["script_templates.h"], hits


# ── 3: a title key anywhere a title is taken; no raw exception in a reply ───────────────

def test_3_the_recorded_raw_error():
    b = LIVE["declare_key_c_athlone_1075_09_15"]
    assert b["call"]["arguments"]["cb_title_id"] == "c_athlone"
    assert "invalid literal for int()" in b["reply"]["result"]["text"]


def test_3_the_repeat_guard_key_takes_a_title_key():
    """The raw error was refusal_memo.declare_key's int() of the key, before declare_war ran."""
    k = refusal_memo.declare_key(SEAT, {"target_id": REALM_68530, "cb": "claim_cb",
                                        "cb_title_id": "c_athlone", "claimant_id": SEAT})
    assert k == ("declare_war", SEAT, REALM_68530, "claim_cb", "c_athlone", SEAT)
    assert refusal_memo.declare_key(0, {"claimant_id": None})[-1] == 0


ms = pytest.importorskip("mcp_server")


def _payload(out):
    """The envelope a call_tool answered, from its text content (the structured copy of a
    dict return is the envelope itself, and the envelope has a `result` key of its own)."""
    blocks = out[0] if isinstance(out, tuple) else out
    for block in blocks:
        if getattr(block, "text", None):
            return json.loads(block.text)
    raise AssertionError(out)


@pytest.fixture
def connected(monkeypatch):
    gw = _gw(query_de_jure=lambda p: {"title": {"title_id": C_ATHLONE, "key": p.get("title_key"),
                                                "name": "Athlone"}},
             get_active_wars={"wars": []})
    state = ms.BridgeState()

    async def ok(_s, _t):
        return None

    async def now():
        return gw
    monkeypatch.setattr(ms, "_get_state", lambda: state)
    monkeypatch.setattr(ms, "_ensure_connected", ok)
    monkeypatch.setattr(ms, "_gateway_now", now)
    monkeypatch.setattr(ms, "_REFUSALS", refusal_memo.Memo())
    return gw


def test_3_the_recorded_call_reaches_the_tool_and_the_key_becomes_the_id(connected):
    args = dict(LIVE["declare_key_c_athlone_1075_09_15"]["call"]["arguments"])
    env = _payload(run(ms.mcp.call_tool("declare_war", args)))
    assert "Error executing" not in json.dumps(env)
    assert ("query_de_jure", {"title_key": "c_athlone"}) in connected.reads
    assert env["result"]["titles_named"][0]["title_id"] == C_ATHLONE, env


def _title_params():
    """Every registered tool's argument that takes a title, by name."""
    out = []
    for t in ms.registered_tool_definitions():
        for name, sch in (t["input_schema"].get("properties") or {}).items():
            if name in title_refs.TITLE_ARGS or name.endswith("title_id"):
                out.append((t["name"], name, sch))
    return out


def test_3_every_title_argument_takes_a_key_or_an_id():
    params = _title_params()
    assert {(t, n) for t, n, _ in params} >= {("declare_war", "cb_title_id"), ("query", "title_id"),
                                              ("inspect", "title_id"), ("discover", "title_id")}, params
    for tool, name, sch in params:
        kinds = {s.get("type") for s in sch.get("anyOf", [sch])}
        assert {"integer", "string"} <= kinds, (tool, name, sch)


def test_3_every_do_intent_title_argument_is_resolved():
    """do's args: every key an intent reads that names a title is one resolve_args turns
    from a key into an id (title_refs.TITLE_ARGS), from the intent table read off the code."""
    import do_intent_args as D
    table = D.intent_arg_table() if hasattr(D, "intent_arg_table") else None
    keys = set()
    if table is None:
        tree = __import__("ast").parse((HERE / "redesign.py").read_text())
        fn = next(n for n in tree.body if getattr(n, "name", "") == D.FUNC)
        keys = {k for k, _ in D._arg_keys(fn)}
    else:
        for row in table.values():
            keys |= set(row)
    titled = {k for k in keys if "title" in k and k not in ("title_key", "title", "titles", "title_keys")}
    assert titled and titled <= set(title_refs.TITLE_ARGS), titled - set(title_refs.TITLE_ARGS)


def test_3_a_tool_that_raises_answers_typed_without_the_text(connected, monkeypatch):
    async def boom(*a, **k):
        raise ValueError("invalid literal for int() with base 10: 'c_athlone'")
    monkeypatch.setattr(R, "declare_war_impl", boom)
    env = _payload(run(ms.mcp.call_tool("declare_war", {"target_id": REALM_68530, "cb": "claim_cb"})))
    assert env["ok"] is False and env["blocked"]["gate"] == "server_error"
    assert env["blocked"]["raw_reason"] == "ValueError"
    assert "invalid literal" not in json.dumps(env) and "Error executing" not in json.dumps(env)


def test_3_a_schema_refusal_names_the_field_without_pydantic_text(connected):
    env = _payload(run(ms.mcp.call_tool("declare_war", {"target_id": 1, "cb": "claim_cb",
                                                         "claimant_id": "the duke"})))
    assert env["blocked"]["gate"] == "invalid_argument"
    assert env["blocked"]["fix"]["rejected_fields"] == ["claimant_id"]
    assert "validation error" not in json.dumps(env)


# ── 4: refusals with no reason name what was and was not checked ───────────────────────

def _do_env(name):
    return json.loads(json.dumps(reply(name)))


def test_4_the_recorded_refusals_had_no_reason():
    for n in ("invite_to_court_1070_09_15", "hof_claim_1071_02_08", "declare_war_interaction_1075_11_27"):
        assert reply(n)["blocked"]["raw_reason"] == "ai_no_valid_target", n
        assert reply(n)["blocked"]["gate"] == "unmapped_refusal", n
    for n in ("grant_dublin_1073_04_22", "grant_dublin_1074_04_22"):
        assert reply(n)["blocked"]["detail"] == "the game gives no detail"
    for n in ("ducal_conquest_469_1078_09_15", "ducal_conquest_462_1078_09_15"):
        assert reply(n)["blocked"]["raw_reason"] == "can_declare_war_false"


def test_4_an_ai_path_refusal_says_the_target_was_not_used():
    for n, name in (("invite_to_court_1070_09_15", "invite_to_court_interaction"),
                    ("declare_war_interaction_1075_11_27", "declare_war_interaction")):
        env = _do_env(n)
        R._explain_interaction_refusal(env, name)
        b = env["blocked"]
        assert b["gate"] == "ai_path_no_target" and "target_id given was not used" in b["summary"], b
    env = _do_env("declare_war_interaction_1075_11_27")
    R._explain_interaction_refusal(env, "declare_war_interaction")
    assert env["blocked"]["fix"]["tool"] == "declare_war"
    # hof_ask_for_claim goes through the bridge's own pipeline at this tree: the
    # same code is not this gate there, and the bridge list says so
    assert "hof_ask_for_claim_interaction" in R.DIRECT_DISPATCH_INTERACTIONS
    assert '"hof_ask_for_claim_interaction",' in src("src/ai_eval_hook.cpp")


def test_4_a_grant_the_game_refused_lists_what_was_not_checked():
    env = _do_env("grant_dublin_1073_04_22")
    R._explain_interaction_refusal(env, "grant_titles_interaction")
    assert env["blocked"]["not_checked"] == R.GRANT_NOT_CHECKED and "checked_before_sending" in env["blocked"]


def test_4_the_grant_rules_are_the_interaction_file():
    f = game_file("common/character_interactions/00_grant_titles_interaction.txt")
    body = f[f.index("grant_titles_interaction = {"):][:4000]
    for rule in ("target_is_liege_or_above = scope:actor", "is_pool_guest_of = scope:actor",
                 "is_imprisoned = no", "has_trait = devoted", "has_trait = gallivanter", "count > 1"):
        assert rule in body, rule


def _grant_gw(him):
    prof = {SEAT: {"id": SEAT, "liege_id": 0, "top_liege_id": SEAT}, 37529: dict({"id": 37529}, **him)}
    return Gw({"query_character": lambda p: prof[p["character_id"]]})


def test_4_a_grant_to_an_unlanded_prisoner_or_outside_the_realm_is_refused_before_sending():
    b = run(R._grant_guard(_grant_gw({"has_realm": False, "is_imprisoned": True, "imprisoner_id": CONCHOBAR}),
                           SEAT, 37529))
    assert b["gate"] == "target_imprisoned" and b["raw_reason"] == "grant_recipient_imprisoned"
    b = run(R._grant_guard(_grant_gw({"has_realm": True, "liege_id": REALM_68530, "top_liege_id": REALM_68530}),
                           SEAT, 37529))
    assert b["raw_reason"] == "grant_recipient_not_in_realm"
    b = run(R._grant_guard(_grant_gw({"has_realm": False, "traits": [{"name": "devoted"}]}), SEAT, 37529))
    assert b["raw_reason"] == "grant_recipient_trait"


def test_4_CONTROL_a_grant_to_a_free_courtier_or_a_vassal_is_sent():
    assert run(R._grant_guard(_grant_gw({"has_realm": False, "is_imprisoned": False}), SEAT, 37529)) is None
    assert run(R._grant_guard(_grant_gw({"has_realm": True, "liege_id": SEAT, "top_liege_id": SEAT}),
                              SEAT, 37529)) is None


def test_4_ducal_conquest_with_prestige_below_zero_is_refused_before_sending():
    """1078.09.15: prestige -540 (README); ducal_conquest_cb costs 50 or 100 times multipliers
    that never reach 0, so no price can be paid."""
    opt = {"cb": "ducal_conquest_cb", "valid": True, "dispatch_supported": True}
    bad, chk = DG.failing(opt, {"gold": 30.0, "prestige": -540.0, "piety": 300.0}, {"in_truce": False})
    (p,) = bad
    assert (p["condition"], p["gate"], p["have"]) == ("prestige", "insufficient_prestige", -540.0)
    bad, chk = DG.failing(opt, {"gold": 30.0, "prestige": 20.0, "piety": 300.0}, {"in_truce": False})
    assert bad == [] and chk["prestige"]["ok"] is None       # 20 may or may not pay it: the game decides
    f = game_file("common/casus_belli_types/00_conquest.txt")
    body = f[f.index("ducal_conquest_cb = {"):][:4500]
    assert "value = 50" in body and "value = 100" in body and "tier = tier_county" in body
    assert "ducal_conquest_cb" in R.CB_START_RULES


# ── 5: the fabrication across a succession ─────────────────────────────────────────────

def _tasks(prov):
    return [{"councillor_id": 56090, "task_name": news.FABRICATE_TASK, "target_province_id": prov}]


def test_5_a_fabrication_set_by_the_predecessor_is_the_heirs_with_its_title():
    """A recorded game, 1071.04.22 / 1074.04.22: after each succession the fabrication read target
    "unknown" (province 18, then 34, unchanged), and the seat set the same task again."""
    news.note_fabrication(37111, C_ATHLONE, "c_athlone", "court_chaplain", "1073.04.22")
    news.note_fabrication_province(37111, 34)
    fs = news.fabrication_status(SEAT, _tasks(34), "1074.04.22")
    assert fs["target"] == {"title_id": C_ATHLONE, "title_key": "c_athlone"}, fs
    assert fs["since"] == "1073.04.22" and fs["set_by"] == 37111
    assert "Whether the progress survived" in fs["succession_note"]
    assert news.fabrication_of(SEAT)["set_by"] == 37111 and news.fabrication_of(37111) is None


def test_5_CONTROL_another_province_is_not_adopted():
    news.note_fabrication(37111, C_ATHLONE, "c_athlone", "court_chaplain", "1073.04.22")
    news.note_fabrication_province(37111, 34)
    assert news.fabrication_status(SEAT, _tasks(18), "1074.04.22")["target"] == "unknown"


def test_5_setting_the_running_fabrication_again_is_not_sent():
    news.note_fabrication(37111, C_ATHLONE, "c_athlone", "court_chaplain", "1073.04.22")
    news.note_fabrication_province(37111, 34)
    gw = _gw(query_council_tasks={"character_id": SEAT, "seats": _tasks(34)})
    env = run(R.do_impl(gw, intent="council_task", character_id=SEAT,
                        args={"seat": "court_chaplain", "task": "task_fabricate_claim",
                              "target_title_id": C_ATHLONE}))
    assert env["ok"] is False and env["blocked"]["raw_reason"] == "fabrication_already_running", env
    assert gw.writes == []
    assert env["blocked"]["measured"]["fabrication"]["set_by"] == 37111


def test_5_CONTROL_another_target_is_sent():
    news.note_fabrication(SEAT, C_ATHLONE, "c_athlone", "court_chaplain", "1073.04.22")
    news.note_fabrication_province(SEAT, 34)
    gw = _gw(query_council_tasks={"character_id": SEAT, "seats": _tasks(34)})
    assert run(R._fabrication_already_running(gw, SEAT, C_ORIEL)) is None


def test_5_the_measured_line_is_a_rule_that_allows_no_claim():
    assert "never come" in news.FABRICATION_MEASURED and "not a measurement" in news.FABRICATION_MEASURED


# ── a scripted game of the Dublin seat (Murchad 31506, 1066-1071) ───────────────────────

def test_preflight_F1_the_same_shape_at_the_base():
    """1071.02.08: Athlone's holder under Connacht's 29703: on the holder target_not_independent
    (n63), on his liege claim_not_pressed (n65); the title table's next_call "no claim casus
    belli" (n66) while idle sent the seat to plan_war (n68)."""
    assert reply("preflight_n63")["blocked"]["gate"] == "target_not_independent"
    assert reply("preflight_n65")["blocked"]["raw_reason"] == "claim_not_pressed"
    ath = next(c for c in reply("preflight_n66")["result"]["counties"] if c["title_id"] == C_ATHLONE)
    assert (ath["realm_ruler_id"], ath["your_claim"], ath["next_call"]["gate"]) == \
        (29703, "unpressed", "cb_invalid_for_target")
    idle = reply("preflight_n68")["result"]["news"]["idle"]
    assert idle["claims_no_war"]["next"] == {"tool": "plan_war", "args": {}}


def test_preflight_F1_idle_sends_the_seat_to_the_claims_own_read():
    snap = {"actor_id": 31506, "date": "1071.02.08", "wars": {},
            "claims": {C_ATHLONE: {"title_id": C_ATHLONE, "holder_id": CONCHOBAR}}}
    assert news.idle(snap)["claims_no_war"]["next"] == {"tool": "cbs", "args": {"target_id": CONCHOBAR}}
    snap["claims"][C_ATHLONE]["war_target_id"] = 29703
    assert news.idle(snap)["claims_no_war"]["next"] == {"tool": "cbs", "args": {"target_id": 29703}}


def test_preflight_F3_both_recorded_shapes_reach_the_tool(connected):
    for n in ("preflight_n15", "preflight_n16"):
        assert "invalid literal for int()" in reply(n)["text"], n
        args = json.loads(json.dumps(LIVE[n]["call"]["arguments"]))
        env = _payload(run(ms.mcp.call_tool("declare_war", args)))
        assert "Error executing" not in json.dumps(env), n
        assert env["result"]["titles_named"][0]["title_key"] == "c_athlone", (n, env)


def _council_gw(writes_reply):
    council = reply("preflight_n9")["result"]
    gw = Gw({"appoint_councillor": writes_reply})
    gw.replies.setdefault("query_council", council)
    gw.write_reply = writes_reply

    async def write(action, params=None):
        gw.writes.append((action, dict(params or {})))
        return writes_reply
    gw.write = write
    return gw


def test_preflight_F2_a_courtier_off_the_council_is_refused_before_sending_naming_the_swap():
    """n11: courtier 36124 into the held steward seat: the bridge's write is the seat swap."""
    assert reply("preflight_n11")["blocked"]["gate"] == "council_seat_vacant"
    gw = _council_gw({"success": False, "reason": "vacant_seat_fill_unsupported"})
    a = {"seat": "steward", "character_id": 36124}
    b = run(R._appoint_seat(gw, a, 31506))
    assert b["raw_reason"] == "appointee_not_on_council" and "seat swap" in b["summary"]


def test_preflight_F2_the_bridges_vacant_answer_on_a_held_seat_is_the_read_disagreeing():
    """n13: the swap with the chancellor 46877 (seated) was refused vacant_seat_fill_unsupported
    although query_council read the steward seat held by 43295."""
    assert reply("preflight_n13")["blocked"]["raw_reason"] == "vacant_seat_fill_unsupported"
    gw = _council_gw({"success": False, "action": "appoint_councillor", "reason": "vacant_seat_fill_unsupported"})
    env = run(R.do_impl(gw, intent="appoint", character_id=31506,
                        args={"seat": "steward", "character_id": 46877}))
    b = env["blocked"]
    assert b["gate"] == "bridge_read_disagrees" and "43295" in b["summary"], b
    assert b.get("next") != R.NOT_POSSIBLE and b["retryable"] is False


def test_preflight_item9_a_refused_head_of_faith_claim_is_not_suggested_again():
    snap = {"actor_id": 31506, "date": "1066.11.27", "piety": 600.0}
    # The claim is named only for a holder whose pre-check passed; `next` is the
    # head of faith's gold (income_levers), never the claim
    assert news.idle(snap)["piety"]["next"]["args"] == {"action_class": "income_levers"}
    news.note_refused_send(31506, news.HOF_CLAIM_SEND, "1066.09.15")
    p = news.idle(snap)["piety"]
    assert "hof_claim" not in p and p["hof_claim_refused"] == "1066.09.15"
    env = {"verdict": "rejected", "blocked": R.why_blocked("recipient_refuses", raw_reason="target_would_reject")}
    R._explain_interaction_refusal(env, R.HOF_CLAIM_INTERACTION)
    assert "head of faith" in env["blocked"]["whose_acceptance"]


# ── 6: one price for the claim war ─────────────────────────────────────────────────────

def test_6_the_bridges_free_claim_war_is_priced_by_the_declare_gate():
    """A recorded cbs row (1069.04.22, Ossory): claim_cb cost_prestige 0, affordable
    true. The shaped option now carries declare_gates' price and its verdict."""
    t16 = json.loads(gzip.open(HERE / "fixtures" / "declare_war_gate_replies.json.gz", "rt").read())
    raw = t16["cbs_32287_1069_04_22"]
    row = next(c for c in raw["cbs"] if c["cb_key"] == "claim_cb")
    assert (row["cost_prestige"], row["affordable"]) == (0, True)
    _v, opts = R._shape_cb_options(raw, target_id=32287, purse={"gold": 50.0, "prestige": 40.0, "piety": 9.0})
    o = next(x for x in opts if x["cb"] == "claim_cb")
    assert o["cost_prestige"] == 75 and o["affordable"] is False and "claim_cb_prestige_cost" in o["cost_source"]
    _v, opts = R._shape_cb_options(raw, target_id=32287, purse={"gold": 50.0, "prestige": 400.0, "piety": 9.0})
    o = next(x for x in opts if x["cb"] == "claim_cb")
    assert o["cost_prestige"] == 75 and o["affordable"] is True


# ── the surface and the fixture ─────────────────────────────────────────────────────────

def test_surface_thirteen_tools_and_the_dump_does_not_grow():
    tools = run(ms.mcp.list_tools())
    out = [t.model_dump(mode="json", by_alias=True, exclude_none=True) for t in tools]
    out = [{"name": d["name"], "description": d.get("description", ""), "inputSchema": d["inputSchema"]}
           for d in out]
    assert len(out) == 13
    # +268 for the shared notes' pointer in discover (test_ireland_walk_replies has it line by line)
    # +396 compact (+408 at indent=1) for do's two intents event_rule and gold_reserve: their
    # enum entries (+32), their rows in the generated args table (+165) and one description sentence (+199)
    assert len(json.dumps(out)) <= 31_049, len(json.dumps(out))      # the recorded reply's size


def test_fixture_is_shippable():
    s = json.dumps({k: v for k, v in LIVE.items() if not k.startswith("source")})
    for bad in ("/home", "/mnt", "api_key", "token\": \"sk"):
        assert bad not in s, bad
