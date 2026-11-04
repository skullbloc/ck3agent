"""L18-B defects 3 (inspect war_score), 7 (the generate_claim path) and 8 (the
per-reply ETA warning), each against the pilot reply that showed it.

W1  inspect(subject="war_score") with no war_id answered every war in the game:
    budget0 e2/s3 (Bhoja 11329, 0873.10) "count": 394, 118,312 characters, over the
    CLI cap; the seat then jq'd out `.breakdowns[] | select(.war_id==...)`. Without
    war_id the reply is now the actor's own wars (get_active_wars).
W2  CONTROL: with war_id the one war is read, whoever fights it, and
    get_active_wars is not read.
G1  discover(scheme_targets, scheme="generate_claim") -> the pilot reply was
    {"gate":"target_ineligible","summary":"scheme_type_targets_a_title"} with no
    next call (23 times). The refusal now names the door that takes a claim on a
    title: do(action_class="fabricate_targets").
G2  the whipsaw (scale0 n8/s1 Cui, 0867.04.25-26): start_generate_claim_interaction
    with target_id == actor_id was told "Pass target_id=0 for self-scope actions";
    with target_id=0 it was told "targets another character — pass target_id".
    For an interaction that needs a recipient, following the self-target hint never
    lands on the missing-target refusal.
G3  a self-scope interaction is offered target_id=0 as a value (it was a
    placeholder string before; not a control: it reds on the base too).
T1  observe's standing carries the ETA's basis once and no reply carries the
    `war_duration_unavailable` warning (391 of 392 pilot observes did, at peace too).
"""

import asyncio
import inspect as _inspect
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
R = pytest.importorskip("redesign")
import action_selector as A  # noqa: E402

BHOJA = 11329


class GW:
    def __init__(self, replies):
        self.replies, self.reads, self.health = dict(replies), [], "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character" and action not in self.replies:
            return {"id": (params or {}).get("character_id"), "alive": True}
        r = self.replies.get(action, {})
        return r(params or {}) if callable(r) else r

    def game(self):
        return {"tick": 2480, "date": "0873.10.18", "paused": False}


def run(coro):
    return asyncio.run(coro)


def _bd(wid, total=0):
    # query_war_score_breakdown, war_id=0: {count, breakdowns[]} with the keys the
    # e2/s3 seat listed (.breakdowns[0] | keys)
    return {"war_id": wid, "total": total, "cached_tick": 2480, "battle": 0, "battle_atk": 0,
            "battle_def": 0, "imprisonment": 0, "imprisonment_atk": 0, "imprisonment_def": 0,
            "landless": 0, "landless_atk": 0, "landless_def": 0, "occupation": 0,
            "occupation_atk": 0, "occupation_def": 0, "ticking": 0, "ticking_atk": 0,
            "ticking_def": 0}


ALL_WARS = {"count": 394, "breakdowns": [_bd(28), _bd(117440524, -29), _bd(16777218)] + [
    _bd(50000000 + i) for i in range(391)]}


def test_W1_war_score_without_war_id_is_your_wars():
    gw = GW({"query_war_score_breakdown": ALL_WARS,
             "get_active_wars": {"wars": [{"war_id": 117440524, "attacker_id": 11329}]}})
    res = run(R.inspect_impl(gw, subject="war_score", character_id=BHOJA))["result"]
    assert [b["war_id"] for b in res["breakdowns"]] == [117440524], res
    assert res["count"] == 1 and "394" in res["scope"], res


def test_W2_CONTROL_a_war_id_reads_that_war_and_nothing_else():
    one = {"found": True, "war_id": 117440524, "total_warscore": -29}
    gw = GW({"query_war_score_breakdown": one})
    res = run(R.inspect_impl(gw, subject="war_score", character_id=BHOJA, war_id=117440524))["result"]
    assert res["total_warscore"] == -29
    assert "get_active_wars" not in [a for a, _ in gw.reads]


def test_G1_a_title_scheme_refusal_names_the_fabricate_door():
    gw = GW({"scheme_action": {"success": False, "reason": "scheme_type_targets_a_title"}})
    env = run(R.scheme_targets_impl(gw, "generate_claim", character_id=13191))
    b = env["blocked"]
    assert b["gate"] == "target_ineligible" and b["raw_reason"] == "scheme_type_targets_a_title"
    assert b["fix"] == {"tool": "do", "args": {"action_class": "fabricate_targets"}}, b


def _interact(target_id, actor_id=13191, name="start_generate_claim_interaction"):
    return A.validate_act_params({"type": "interaction", "interaction_type": name,
                                  "actor_id": actor_id, "target_id": target_id})


def test_G2_the_self_target_hint_never_leads_to_the_missing_target_refusal():
    first = _interact(13191)
    assert first["reason"] == "self_target"
    assert "target_id=0" not in first["hint"], first
    assert first["retry_with"]["target_id"] != 0, first


def test_G3_a_self_scope_interaction_is_offered_target_zero_as_a_value():
    r = _interact(13191, name="rest_interaction")
    assert r["reason"] == "self_target" and r["retry_with"]["target_id"] == 0, r


def test_T1_no_reply_carries_the_eta_warning_and_the_basis_is_on_the_field():
    src = _inspect.getsource(R)
    assert '"war_duration_unavailable"' not in src
    assert R.succession_eta_years(43) == 22 and R.succession_eta_years(None) is None
    assert R.succession_eta_years(70) == 0


# ── Q1/Q2: query(domain="conquest_targets") compact by default ────────────────
import json  # noqa: E402
import tools as T  # noqa: E402

CUI = 13191


def _own(cid):
    # scale0 n2/s1 line 117: a remaining county of Cui's partly-held d_yongxing
    return {"county_id": cid, "county_key": f"c_{cid}", "county_name": "Huazhou",
            "holder_id": 12284, "holder_name": "Jichu", "holder_top_liege_id": CUI,
            "holder_top_liege_independent": False, "is_independent": False, "is_kin": False,
            "is_allied": False, "is_truced": False, "already_at_war_opposing": False,
            "adjacent": True, "conquerable_now": False, "conquerable_now_source": "heuristic",
            "block_reason": "holder_not_independent"}


def _tengger(cid=14606, conquerable=True):
    # scale0 n2/s1 line 120: the first conquerable_now row, verbatim keys
    return {"county_id": cid, "county_key": "c_tengger", "county_name": "Tenggeli",
            "holder_id": 10744, "holder_name": "Yangchen", "holder_top_liege_id": 10744,
            "holder_top_liege_independent": True, "is_independent": True, "is_kin": False,
            "is_allied": False, "is_truced": False, "already_at_war_opposing": False,
            "adjacent": True, "conquerable_now": conquerable, "conquerable_now_source": "heuristic",
            "declare": {"type": "war", "method": "steered", "casus_belli": "county_conquest_cb",
                        "target_id": 10744, "cb_title_id": cid},
            "declare_options": [{"casus_belli": "county_conquest_cb", "scope": "county",
                                 "target_id": 10744, "cb_title_id": cid}]}


def _cui_reply():
    duchies = [{"duchy_id": 14324 + i, "duchy_key": f"d_{i}", "partially_held": True,
                "remaining_counties": [_own(20000 + 4 * i + j) for j in range(3)]} for i in range(60)]
    duchies += [{"duchy_id": 14602 + i, "duchy_key": f"d_x{i}", "expansion_target": True,
                 "remaining_counties": [_tengger(30000 + 2 * i, conquerable=(i % 3 == 0)),
                                        _tengger(30001 + 2 * i, conquerable=False)]}
                for i in range(34)]
    return {"action": "query_conquest_targets", "ruler_id": CUI, "duchy_count": 94,
            "expansion_count": 93, "toward_count": 0, "conquerable_count": 12, "target_count": 248,
            "note": "Conquest targets by de-jure duchy.", "duchies": duchies}


def test_Q1_the_compact_reply_keeps_what_seats_read_and_drops_their_own_realm():
    full = _cui_reply()
    out = T.compact_conquest_targets(full)
    rows = [rc for d in out["duchies"] for rc in d["remaining_counties"]]
    assert len(rows) == T.CONQUEST_TARGETS_DEFAULT_ROWS
    assert all(rc["holder_top_liege_id"] != CUI for rc in rows)
    assert sum(1 for rc in rows if rc["conquerable_now"]) == min(12, T.CONQUEST_TARGETS_DEFAULT_ROWS)  # conquerable first
    assert out["compact"]["own_realm_counties_dropped"] == 180
    assert out["compact"]["counties_omitted"] == 68 - T.CONQUEST_TARGETS_DEFAULT_ROWS
    # L20-B: the counts stay; the bridge's long note is under detail="full"
    for k in ("duchy_count", "expansion_count", "toward_count", "conquerable_count", "target_count"):
        assert out[k] == full[k], k
    assert set(rows[0]) >= {"county_id", "county_name", "holder_id", "holder_name",
                            "holder_top_liege_id", "conquerable_now_source", "declare_args"}
    assert len(json.dumps(out)) < len(json.dumps(full)) / 4


def test_Q3_an_engine_checked_row_keeps_the_games_cb_list():
    """README: each holder carries the game's own verdict (`conquerable_now`,
    `engine_valid_cbs`). The emitter writes engine_valid_cbs when the engine ran
    for the holder, with conquerable_now_source engine_county_conquest_cb
    (query_handlers.cpp do_query_conquest_targets)."""
    full = _cui_reply()
    rc = full["duchies"][60]["remaining_counties"][0]
    rc.update(conquerable_now_source="engine_county_conquest_cb",
              engine_valid_cbs=["county_conquest_cb", "claim_cb"])
    out = T.compact_conquest_targets(full, detail="full")     # L20-B: the list under detail="full"
    row = next(r for d in out["duchies"] for r in d["remaining_counties"]
               if r["county_id"] == rc["county_id"])
    assert row["engine_valid_cbs"] == ["county_conquest_cb", "claim_cb"], row


def test_Q2_CONTROL_max_results_lists_more():
    out = T.compact_conquest_targets(_cui_reply(), 100)
    assert out["compact"]["counties_listed"] == 68 and out["compact"]["counties_omitted"] == 0
