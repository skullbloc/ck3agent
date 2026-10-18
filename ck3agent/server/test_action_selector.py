#!/usr/bin/env python3
"""
Standalone test suite for action_selector.py — pre-dispatch validation +
LLM-formatted action menu (ck-3hij9.3).

Runs with no dependencies (stdlib only). Exit 0 on success, 1 on any failure.

    python3 ck3agent/server/test_action_selector.py
"""

from __future__ import annotations

import sys
from pathlib import Path

# Allow direct execution from anywhere in the repo.
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from action_selector import (  # noqa: E402
    build_action_menu,
    format_action_menu,
    validate_act_params,
)


FAILURES: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    if ok:
        print(f"  PASS  {name}")
    else:
        FAILURES.append(f"{name} — {detail}")
        print(f"  FAIL  {name} — {detail}")


# ---------------------------------------------------------------------------
# validate_act_params
# ---------------------------------------------------------------------------

def test_rejects_missing_type() -> None:
    print("\n[validate] missing/unknown type")
    v = validate_act_params({})
    check("empty params rejected", not v["valid"] and v["reason"] == "missing_type",
          f"got {v}")
    check("retry hint names 'type'", "type" in v.get("retry_with", {}),
          f"retry_with={v.get('retry_with')}")

    v = validate_act_params({"type": "frobnicate"})
    check("unknown type rejected", not v["valid"] and v["reason"] == "unknown_type",
          f"got {v}")

    v = validate_act_params("not-a-dict")  # type: ignore[arg-type]
    check("non-dict rejected", not v["valid"] and v["reason"] == "params_not_object",
          f"got {v}")


def test_interaction_validation() -> None:
    print("\n[validate] interaction")

    v = validate_act_params({"type": "interaction"})
    check("missing interaction_type", not v["valid"]
          and v["reason"] == "missing_interaction_type", f"got {v}")

    v = validate_act_params({
        "type": "interaction",
        "interaction_type": "propose_marriage_interaction",
    })
    check("targeted interaction needs target_id", not v["valid"]
          and v["reason"] == "missing_target_id", f"got {v}")

    v = validate_act_params({
        "type": "interaction",
        "interaction_type": "propose_marriage_interaction",
        "target_id": 12345,
    })
    check("targeted with target ok", v["valid"], f"got {v}")

    v = validate_act_params({
        "type": "interaction",
        "interaction_type": "rest_interaction",
    })
    check("self interaction needs no target", v["valid"], f"got {v}")

    v = validate_act_params({
        "type": "interaction",
        "interaction_type": "send_gift_interaction",
        "actor_id": 999,
        "target_id": 999,
    })
    check("self-target rejected", not v["valid"]
          and v["reason"] == "self_target", f"got {v}")


def test_war_validation() -> None:
    print("\n[validate] war")

    v = validate_act_params({"type": "war"})
    check("war needs target", not v["valid"]
          and v["reason"] == "missing_target_id", f"got {v}")

    v = validate_act_params({"type": "war", "target_id": 100, "actor_id": 100})
    check("self war rejected", not v["valid"] and v["reason"] == "self_target",
          f"got {v}")

    v = validate_act_params({"type": "war", "target_id": 100})
    check("ai-path auto cb ok", v["valid"] and v["path"] == "ai", f"got {v}")

    v = validate_act_params({
        "type": "war", "target_id": 100, "cb_index": 11,
    })
    check("claim_cb without title rejected", not v["valid"]
          and v["reason"] == "missing_cb_title_id", f"got {v}")

    v = validate_act_params({
        "type": "war", "target_id": 100, "cb_index": 11, "cb_title_id": 9001,
    })
    check("claim_cb with title ok", v["valid"], f"got {v}")

    v = validate_act_params({
        "type": "war", "target_id": 100, "force_player_path": True,
    })
    check("force_player_path without CB rejected", not v["valid"]
          and v["reason"] == "missing_casus_belli", f"got {v}")

    v = validate_act_params({
        "type": "war", "target_id": 100,
        "force_player_path": True, "casus_belli": "claim_cb",
    })
    check("force_player_path with CB ok",
          v["valid"] and v["path"] == "force_player", f"got {v}")


def test_decision_event_law_validation() -> None:
    print("\n[validate] decision / event / law")

    v = validate_act_params({"type": "decision"})
    check("decision needs decision_type", not v["valid"]
          and v["reason"] == "missing_decision_type", f"got {v}")
    v = validate_act_params({"type": "decision", "decision_type": "settle_dispute"})
    check("decision with name ok", v["valid"], f"got {v}")

    v = validate_act_params({"type": "event"})
    check("event needs event_id", not v["valid"]
          and v["reason"] == "missing_event_id", f"got {v}")
    v = validate_act_params({"type": "event", "event_id": "evt_x"})
    check("event needs instance_id", not v["valid"]
          and v["reason"] == "missing_instance_id", f"got {v}")
    v = validate_act_params({
        "type": "event", "event_id": "evt_x", "instance_id": 42,
    })
    check("event fully specified ok", v["valid"], f"got {v}")

    v = validate_act_params({"type": "law"})
    check("law needs law_name", not v["valid"]
          and v["reason"] == "missing_law_name", f"got {v}")
    v = validate_act_params({"type": "law", "law_name": "crown_authority_2"})
    check("law with name ok", v["valid"], f"got {v}")


def test_retry_with_completeness() -> None:
    """Every rejection should populate retry_with so the LLM has a recipe."""
    print("\n[validate] retry_with always populated on rejection")
    rejections = [
        {},
        {"type": "frobnicate"},
        {"type": "interaction"},
        {"type": "interaction", "interaction_type": "propose_marriage_interaction"},
        {"type": "war"},
        {"type": "war", "target_id": 1, "cb_index": 11},
        {"type": "war", "target_id": 1, "force_player_path": True},
        {"type": "decision"},
        {"type": "event"},
        {"type": "event", "event_id": "x"},
        {"type": "law"},
    ]
    for p in rejections:
        v = validate_act_params(p)
        check(f"rejection has retry_with for {p!r}",
              not v["valid"] and isinstance(v.get("retry_with"), dict)
              and len(v["retry_with"]) > 0, f"got {v}")
        check(f"rejection has rejected_fields for {p!r}",
              isinstance(v.get("rejected_fields"), list)
              and len(v["rejected_fields"]) > 0, f"got {v}")
        check(f"rejection has hint for {p!r}",
              isinstance(v.get("hint"), str) and len(v["hint"]) > 0, f"got {v}")


# ---------------------------------------------------------------------------
# build_action_menu
# ---------------------------------------------------------------------------

def test_menu_handles_empty_observation() -> None:
    print("\n[menu] empty observation")
    m = build_action_menu(None)
    check("empty menu has summary", "summary" in m, f"got {m}")
    check("interactions is empty list", m["interactions"] == [], f"got {m}")
    check("decisions is empty list", m["decisions"] == [], f"got {m}")
    check("wars is empty list", m["wars"] == [], f"got {m}")

    m = build_action_menu({})
    check("empty dict observation handled",
          m["interactions"] == [] and m["decisions"] == [], f"got {m}")


def test_menu_builds_interaction_entries() -> None:
    print("\n[menu] interactions")
    obs = {
        "available_interactions": [
            {"type": "propose_marriage_interaction", "target_id": 100,
             "target_name": "Duke X", "will_accept": True, "ai_score": 75},
            {"type": "send_gift_interaction", "target_id": 200,
             "target_name": "Count Y", "will_accept": False},
            {"type": "rest_interaction"},  # no target
            {"type": "", "target_id": 1},  # malformed — skipped
            "not-a-dict",  # malformed — skipped
        ],
    }
    m = build_action_menu(obs)
    check("interactions length", len(m["interactions"]) == 3,
          f"got {len(m['interactions'])}")
    # Targeted interactions sorted first
    check("targeted first", m["interactions"][0]["target_id"] != 0,
          f"got {m['interactions']}")
    # Each entry has a ready-to-dispatch act dict
    first = m["interactions"][0]
    check("entry has act dict", isinstance(first.get("act"), dict)
          and first["act"]["type"] == "interaction", f"got {first}")
    check("act dict mirrors target_id",
          first["act"]["target_id"] == first["target_id"], f"got {first}")
    # will_accept / ai_score surfaced (look up by name — sort order is impl detail)
    by_name = {e["interaction_type"]: e for e in m["interactions"]}
    marriage = by_name.get("propose_marriage_interaction") or {}
    gift = by_name.get("send_gift_interaction") or {}
    check("will_accept=True surfaced", marriage.get("will_accept") is True,
          f"got {marriage}")
    check("will_accept=False surfaced", gift.get("will_accept") is False,
          f"got {gift}")
    check("ai_score surfaced", marriage.get("ai_score") == 75, f"got {marriage}")


def test_menu_builds_decision_entries() -> None:
    print("\n[menu] decisions")
    obs = {
        "available_decisions": [
            {"id": "settle_dispute", "description": "settle a feudal dispute"},
            {"name": "found_holy_order"},
            "press_button_decision",
        ],
    }
    m = build_action_menu(obs)
    check("decisions length", len(m["decisions"]) == 3,
          f"got {len(m['decisions'])}")
    check("dict-with-id parsed", m["decisions"][0]["decision_type"] == "settle_dispute",
          f"got {m['decisions'][0]}")
    check("dict-with-name parsed", m["decisions"][1]["decision_type"] == "found_holy_order",
          f"got {m['decisions'][1]}")
    check("bare-string parsed", m["decisions"][2]["decision_type"] == "press_button_decision",
          f"got {m['decisions'][2]}")
    check("description surfaced", m["decisions"][0].get("description") == "settle a feudal dispute",
          f"got {m['decisions'][0]}")


def test_menu_builds_war_targets() -> None:
    print("\n[menu] wars")
    obs = {
        "ruler": {
            "id": 1,
            "wars": [{"attacker_id": 1, "defender_id": 200}],
        },
        "nearby_rulers": [
            {"id": 100, "name": "Duke A", "opinion": 25, "military_strength": 5000},
            {"id": 200, "name": "King B"},   # already at war — excluded
            {"id": 1,   "name": "Self"},     # actor — excluded
            {"id": 300, "name": "Count C", "opinion": -50, "military_strength": 1200},
        ],
    }
    m = build_action_menu(obs)
    ids = [w["target_id"] for w in m["wars"]]
    check("excludes existing war target", 200 not in ids, f"ids={ids}")
    check("excludes self", 1 not in ids, f"ids={ids}")
    check("includes peaceful neighbors", set(ids) == {100, 300}, f"ids={ids}")
    war = m["wars"][0]
    check("war entry has ready act dict",
          war["act"]["type"] == "war"
          and war["act"]["target_id"] == war["target_id"]
          and war["act"]["cb_index"] == -1, f"got {war}")
    check("war entry includes hint", "discover" in war.get("hint", ""), f"got {war}")
    check("war entry surfaces opinion", war.get("opinion") == 25, f"got {war}")


def test_menu_truncation() -> None:
    print("\n[menu] truncation")
    obs = {
        "available_interactions": [
            {"type": f"act_{i}_interaction", "target_id": i + 1}
            for i in range(50)
        ],
    }
    m = build_action_menu(obs, limit_per_category=10)
    check("respects per-category limit", len(m["interactions"]) == 10,
          f"got {len(m['interactions'])}")
    check("truncation reported", m["truncated"].get("interactions") == 40,
          f"got {m['truncated']}")


def test_format_action_menu_smoke() -> None:
    print("\n[format] smoke render")
    obs = {
        "available_interactions": [
            {"type": "propose_marriage_interaction", "target_id": 100,
             "target_name": "Duke X", "will_accept": True, "ai_score": 75},
        ],
        "available_decisions": [{"id": "settle_dispute"}],
        "ruler": {"id": 1, "wars": []},
        "nearby_rulers": [
            {"id": 100, "name": "Duke X", "opinion": 25, "military_strength": 5000},
        ],
    }
    menu = build_action_menu(obs)
    rendered = format_action_menu(menu)
    check("render is non-empty string",
          isinstance(rendered, str) and len(rendered) > 0,
          f"len={len(rendered)}")
    check("render has Available Actions header",
          "Available Actions" in rendered, "header missing")
    check("render includes target name",
          "Duke X" in rendered, "Duke X missing")
    check("render mentions war pre-validation guidance",
          "war_validity" in rendered, "war_validity hint missing")


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main() -> int:
    print("=" * 60)
    print("action_selector test suite (ck-3hij9.3)")
    print("=" * 60)

    test_rejects_missing_type()
    test_interaction_validation()
    test_war_validation()
    test_decision_event_law_validation()
    test_retry_with_completeness()
    test_menu_handles_empty_observation()
    test_menu_builds_interaction_entries()
    test_menu_builds_decision_entries()
    test_menu_builds_war_targets()
    test_menu_truncation()
    test_format_action_menu_smoke()

    print("\n" + "=" * 60)
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} check(s)")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
