"""
Action selector — LLM-facing menu + pre-dispatch validation for act() calls.

Two responsibilities (ck-3hij9.3):

1. build_action_menu(observation) — derive a structured menu of currently
   actionable options from an observation snapshot. The menu is intentionally
   flat and minimal — each entry carries exactly the fields needed to dispatch
   the action plus a short display hint.

2. validate_act_params(params) — pre-flight check the agent runs BEFORE
   sending an act() call to the bridge. Catches the common "I forgot
   target_id" / "cb_index without cb_title_id" mistakes locally so the LLM
   gets a structured rejection (with retry hints) instead of an opaque
   bridge timeout 15 seconds later. Pure logic, no IPC.

A validation pass here does NOT guarantee the bridge will accept the action —
trigger-slot evaluation still happens server-side. It guarantees only that
the shape is plausible. For full pre-validation use discover(what='war_validity')
or discover(what='interaction_validity') round-trips.
"""

from __future__ import annotations

from typing import Any


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

_ACT_TYPES = {"interaction", "war", "decision", "event", "law", "lifestyle", "council_task", "building",
              # write tools — validated bridge-side (their handlers return explicit reject reasons
              # like position_id_required / maa_type_required), so they pass the pre-flight gate.
              "appoint", "maa", "contract", "faction", "move_capital", "hire_mercenary", "faith",
              # ck-#130 culture: convert the actor's OWN culture (e.g. "english"/"anglo_saxon").
              # THE convert-own-culture lever — unblocks e_britannia (culture-gated can_create).
              # Routes to set_character_culture (engine SetCultureOnCharacter); self-validates bridge-side.
              "culture",
              # #96 raid lever: raise_armies (mobilize levies) + raid (toggle raised armies into
              # raiding for gold/prestige/piety). Wired in tools.py + dispatch_router.cpp; the
              # handlers self-validate (actor-must-be-AI, Norse/army-less no-op), so they pass here.
              "raise_armies", "raid",
              # #132 scheme: LEGITIMATELY start a hostile scheme (murder/fabricate_hook/
              # claim_throne) owned by an AI ruler against a target character. Routes to
              # scheme_action → CreateActiveScheme forward; the engine's own scheme-start
              # gates run inside that call (self-gating), and the handler self-validates
              # (actor-must-be-AI, target alive + not self), so it passes the pre-flight gate.
              "scheme",
              # #111 create_title: create a de-jure duchy/kingdom title the AI ruler controls
              # (the k_danelaw 2nd-duchy unlock). Engine-gated (CCreateTitleCommand CanExecute:
              # de-jure control + not-extant + gold cost); self-validates bridge-side.
              "create_title"}

# Interactions that target only the actor (no target_id required). The bridge
# accepts these with target_id=0 because the AI dispatch path special-cases
# self-scope interactions. List is conservative — adding entries is safe;
# removing them risks rejecting a valid call.
_SELF_INTERACTIONS = {
    "rest_interaction",
    "stop_scheme_interaction",
    "abdicate_interaction",
    "take_vow_interaction",
}

# CB indices that strictly require cb_title_id to dispatch successfully.
# claim_cb (11) and the de_jure family all need a specific title.
# See CLAUDE.md and re/findings/re_war.md.
_TITLE_REQUIRED_CB_INDICES = {11}  # claim_cb
_TITLE_REQUIRED_CB_NAME_PREFIXES = ("claim_", "de_jure_")


def validate_act_params(params: dict[str, Any]) -> dict[str, Any]:
    """Pre-flight validation for an act() tool call.

    Returns a dict with at minimum:
        valid: bool

    On valid=False also includes:
        reason: short machine token (e.g. 'missing_target_id')
        rejected_fields: list[str] of the input keys at fault
        hint: human-readable next step the LLM should take
        retry_with: dict of {field: placeholder_or_value} the LLM should fill

    The shape is stable so the LLM can pattern-match and self-correct without
    parsing prose.
    """
    if not isinstance(params, dict):
        return _reject(
            "params_not_object",
            ["<root>"],
            "act() params must be a JSON object.",
            {},
        )

    act_type = params.get("type")
    if not act_type:
        return _reject(
            "missing_type",
            ["type"],
            "act() requires type ∈ {interaction, war, decision, event, law}.",
            {"type": "<one of: interaction, war, decision, event, law>"},
        )
    if act_type not in _ACT_TYPES:
        return _reject(
            "unknown_type",
            ["type"],
            f"Unknown act type '{act_type}'. Use one of: "
            f"{', '.join(sorted(_ACT_TYPES))}.",
            {"type": "<one of: interaction, war, decision, event, law>"},
        )

    actor_id = _as_int(params.get("actor_id", 0))
    target_id = _as_int(params.get("target_id", 0))

    if act_type == "interaction":
        return _validate_interaction(params, actor_id, target_id)
    if act_type == "war":
        return _validate_war(params, actor_id, target_id)
    if act_type == "decision":
        return _validate_decision(params, actor_id)
    if act_type == "event":
        return _validate_event(params)
    if act_type == "law":
        return _validate_law(params, actor_id)
    if act_type == "lifestyle":
        return _validate_lifestyle(params)
    if act_type == "council_task":
        return _validate_council_task(params)
    if act_type == "building":
        return _validate_building(params)

    # Write tools (appoint/maa/contract/faction/move_capital/hire_mercenary/faith):
    # no dedicated pre-flight validator — they validate inside the bridge handler
    # (e.g. position_id_required, maa_type_required, company_not_found). Pass the
    # pre-flight gate so resolve_act can route them.
    if act_type in _ACT_TYPES:
        return {"valid": True}

    # Unreachable — _ACT_TYPES gates above.
    return _reject("internal_error", ["type"], "unhandled act type", {})


def _validate_building(params: dict) -> dict:
    building = (params.get("building") or "").strip()
    if not building:
        return _reject(
            "missing_building",
            ["building"],
            "type=building requires building (a building key, e.g. 'castle_03') "
            "and location_id (the holding's barony title id from "
            "query(domain='county_details') holdings[].barony_title_id).",
            {"building": "<building key>", "location_id": "<barony title id>"},
        )
    if not params.get("location_id"):
        return _reject(
            "missing_location_id", ["location_id"],
            "type=building requires location_id (the holding's barony title id).",
            {"location_id": "<barony title id>"},
        )
    return {"valid": True, "type": "building", "building": building,
            "location_id": params.get("location_id")}


def _validate_council_task(params: dict) -> dict:
    task = (params.get("task") or "").strip()
    if not task:
        return _reject(
            "missing_task",
            ["task"],
            "type=council_task requires task (a council-task key, e.g. "
            "'task_increase_control'). Call query(domain='council') to see the "
            "current tasks. Some tasks need target_title_id (a de jure title).",
            {"task": "<task key>"},
        )
    return {"valid": True, "type": "council_task", "task": task,
            "target_title_id": params.get("target_title_id", 0)}


def _validate_lifestyle(params: dict) -> dict:
    focus = (params.get("focus") or "").strip()
    if not focus:
        return _reject(
            "missing_focus",
            ["focus"],
            "type=lifestyle requires focus (a focus key, e.g. "
            "'stewardship_domain_focus'). Call query(domain='lifestyle') to see "
            "the current focus.",
            {"focus": "<focus key>"},
        )
    return {"valid": True, "type": "lifestyle", "focus": focus}


def _validate_interaction(params: dict, actor_id: int, target_id: int) -> dict:
    interaction_type = (params.get("interaction_type") or "").strip()
    if not interaction_type:
        return _reject(
            "missing_interaction_type",
            ["interaction_type"],
            "type=interaction requires interaction_type. Call "
            "discover(what='interactions') to enumerate the registry, or check "
            "observe()'s available_interactions list.",
            {"interaction_type": "<exact interaction name>"},
        )

    # Self-targeted interactions can omit target_id.
    needs_target = interaction_type not in _SELF_INTERACTIONS
    if needs_target and target_id == 0:
        return _reject(
            "missing_target_id",
            ["target_id"],
            f"Interaction '{interaction_type}' targets another character — "
            "pass target_id. Use observe() or query(domain='neighbors') to "
            "find candidates; verify with discover(what='acceptance').",
            {"target_id": "<character_id>"},
        )

    if target_id and actor_id and target_id == actor_id:
        return _reject(
            "self_target",
            ["target_id", "actor_id"],
            f"target_id ({target_id}) equals actor_id — interactions cannot "
            "target the actor. Pass target_id=0 for self-scope actions, "
            "or pick a different character.",
            {"target_id": "<character_id != actor_id>"},
        )

    return {"valid": True, "type": "interaction", "interaction_type": interaction_type}


def _validate_war(params: dict, actor_id: int, target_id: int) -> dict:
    if target_id == 0:
        return _reject(
            "missing_target_id",
            ["target_id"],
            "type=war requires target_id (the defender). Use query("
            "domain='borders') or query(domain='neighbors') to find "
            "candidates, then discover(what='war_validity', target_id=N).",
            {"target_id": "<defender_character_id>"},
        )
    if actor_id and target_id == actor_id:
        return _reject(
            "self_target",
            ["target_id", "actor_id"],
            "Cannot declare war on yourself.",
            {"target_id": "<defender_character_id != actor_id>"},
        )

    force_player_path = bool(params.get("force_player_path"))
    casus_belli = (params.get("casus_belli") or "").strip()
    cb_index = _as_int(params.get("cb_index", -1))
    cb_title_id = _as_int(params.get("cb_title_id", 0))
    method = (params.get("method") or "").strip()

    # Both direct-declaration routes (steered / force_player_path) take the CB by
    # name and need cb_title_id for title-bound CBs. Without it the war is queued
    # then SILENTLY CULLED by the engine on the next tick — catch it here.
    if method == "steered" or force_player_path:
        route = "steered" if method == "steered" else "force_player"
        label = "method='steered'" if route == "steered" else "force_player_path=True"
        if not casus_belli:
            return _reject(
                "missing_casus_belli",
                ["casus_belli"],
                f"{label} declares a specific war and requires casus_belli (CB "
                "name string). Use discover(what='valid_cbs_strict', target_id=N).",
                {"casus_belli": "<cb_name e.g. 'claim_cb'>"},
            )
        if casus_belli.startswith(_TITLE_REQUIRED_CB_NAME_PREFIXES) and cb_title_id == 0:
            return _reject(
                "missing_cb_title_id",
                ["cb_title_id"],
                f"casus_belli='{casus_belli}' is title-bound and requires "
                "cb_title_id (from query(domain='claims')). Without it the war "
                "is queued then silently culled by the engine.",
                {"cb_title_id": "<title_id>"},
            )
        return {"valid": True, "type": "war", "path": route}

    # AI path. cb_index=-1 means auto-select. If the agent specifies a
    # title-bound CB by index, cb_title_id is mandatory.
    if cb_index in _TITLE_REQUIRED_CB_INDICES and cb_title_id == 0:
        return _reject(
            "missing_cb_title_id",
            ["cb_title_id"],
            f"cb_index={cb_index} (claim/de_jure family) requires "
            "cb_title_id. Pull from query(domain='claims', "
            "include_vassals=True).",
            {"cb_title_id": "<title_id>"},
        )

    # Casus belli by name (without force_player_path) is also valid — the
    # bridge accepts it as a hint. If neither cb_index nor casus_belli is
    # set, the AI path auto-selects.
    return {"valid": True, "type": "war", "path": "ai"}


def _validate_decision(params: dict, actor_id: int) -> dict:
    decision_type = (params.get("decision_type") or "").strip()
    if not decision_type:
        return _reject(
            "missing_decision_type",
            ["decision_type"],
            "type=decision requires decision_type. Call "
            "discover(what='decisions') to enumerate.",
            {"decision_type": "<exact decision name>"},
        )
    return {"valid": True, "type": "decision", "decision_type": decision_type}


def _validate_event(params: dict) -> dict:
    event_id = (params.get("event_id") or "").strip()
    instance_id = _as_int(params.get("instance_id", 0))
    if not event_id:
        return _reject(
            "missing_event_id",
            ["event_id"],
            "type=event requires event_id (string ID from the popup).",
            {"event_id": "<event_id>"},
        )
    if instance_id == 0:
        return _reject(
            "missing_instance_id",
            ["instance_id"],
            "type=event requires instance_id (the in-flight event instance "
            "ID, surfaced by the event popup).",
            {"instance_id": "<instance_id>"},
        )
    return {"valid": True, "type": "event", "event_id": event_id}


def _validate_law(params: dict, actor_id: int) -> dict:
    law_name = (params.get("law_name") or "").strip()
    if not law_name:
        return _reject(
            "missing_law_name",
            ["law_name"],
            "type=law requires law_name. Call query(domain='law_options') "
            "to enumerate options for the current realm.",
            {"law_name": "<law name e.g. crown_authority_2>"},
        )
    return {"valid": True, "type": "law", "law_name": law_name}


def _reject(reason: str, fields: list[str], hint: str, retry_with: dict) -> dict:
    return {
        "valid": False,
        "reason": reason,
        "rejected_fields": fields,
        "hint": hint,
        "retry_with": retry_with,
    }


def _as_int(value: Any) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.strip().lstrip("-").isdigit():
        return int(value)
    return 0


# ---------------------------------------------------------------------------
# Menu construction
# ---------------------------------------------------------------------------

def build_action_menu(
    observation: dict[str, Any] | None,
    *,
    limit_per_category: int = 15,
) -> dict[str, Any]:
    """Build a structured action menu from an observation snapshot.

    Args:
        observation: bridge observation dict as returned by observe(). May
            contain available_interactions / available_decisions / ruler.wars /
            nearby_rulers. Missing or None yields an empty menu.
        limit_per_category: cap entries per category so prompt size stays
            bounded. Excess entries are flagged in the `truncated` field.

    Returns:
        Menu dict with keys:
          interactions: list of {act, interaction_type, target_id, target_name,
                                 hints}
          decisions:    list of {act, decision_type, description}
          wars:         list of {act, target_id, target_name, hint}
          summary:      one-line stats
          truncated:    dict of {category: dropped_count}

    Entries are ordered: targeted-first, then untargeted. Each entry's `act`
    key is a ready-to-dispatch param dict — the LLM can copy it into act().
    """
    obs = observation or {}
    ruler = obs.get("ruler") or {}

    interactions, int_dropped = _build_interactions_menu(
        obs.get("available_interactions"), limit_per_category
    )
    decisions, dec_dropped = _build_decisions_menu(
        obs.get("available_decisions"), limit_per_category
    )
    wars, war_dropped = _build_wars_menu(
        obs.get("nearby_rulers"), ruler, limit_per_category
    )

    truncated = {}
    if int_dropped:
        truncated["interactions"] = int_dropped
    if dec_dropped:
        truncated["decisions"] = dec_dropped
    if war_dropped:
        truncated["wars"] = war_dropped

    summary_parts = [
        f"{len(interactions)} interactions",
        f"{len(decisions)} decisions",
        f"{len(wars)} war targets",
    ]
    summary = ", ".join(summary_parts)

    return {
        "interactions": interactions,
        "decisions": decisions,
        "wars": wars,
        "summary": summary,
        "truncated": truncated,
    }


def _build_interactions_menu(
    raw: list | None, limit: int
) -> tuple[list[dict], int]:
    if not raw:
        return [], 0
    out: list[dict] = []
    for entry in raw:
        if not isinstance(entry, dict):
            continue
        name = entry.get("type") or entry.get("name") or ""
        if not name:
            continue
        target_id = entry.get("target_id") or 0
        item = {
            "interaction_type": name,
            "target_id": target_id,
            "act": {
                "type": "interaction",
                "interaction_type": name,
                "target_id": target_id,
            },
        }
        if entry.get("target_name"):
            item["target_name"] = entry["target_name"]
        if entry.get("will_accept") is not None:
            item["will_accept"] = bool(entry["will_accept"])
        if entry.get("ai_score") is not None:
            item["ai_score"] = entry["ai_score"]
        out.append(item)
    # Sort: targeted first (higher target_id first as a tie-breaker so the
    # ordering is deterministic for tests), then untargeted.
    out.sort(key=lambda e: (e["target_id"] == 0, -e["target_id"]))
    dropped = max(0, len(out) - limit)
    return out[:limit], dropped


def _build_decisions_menu(
    raw: list | None, limit: int
) -> tuple[list[dict], int]:
    if not raw:
        return [], 0
    out: list[dict] = []
    for entry in raw:
        if isinstance(entry, dict):
            name = entry.get("id") or entry.get("name") or ""
            desc = entry.get("description", "")
        else:
            name = str(entry)
            desc = ""
        if not name:
            continue
        item = {
            "decision_type": name,
            "act": {"type": "decision", "decision_type": name},
        }
        if desc:
            item["description"] = desc
        out.append(item)
    dropped = max(0, len(out) - limit)
    return out[:limit], dropped


def _build_wars_menu(
    nearby: list | None, ruler: dict, limit: int
) -> tuple[list[dict], int]:
    """Nearby rulers we are NOT already at war with are war candidates."""
    if not nearby:
        return [], 0
    at_war_ids: set[int] = set()
    for w in ruler.get("wars", []) or []:
        if isinstance(w, dict):
            for key in ("attacker_id", "defender_id", "target_id"):
                v = w.get(key)
                if isinstance(v, int) and v != ruler.get("id"):
                    at_war_ids.add(v)
    actor_id = ruler.get("id") or 0
    out: list[dict] = []
    for r in nearby:
        if not isinstance(r, dict):
            continue
        rid = r.get("id")
        if not isinstance(rid, int) or rid in at_war_ids or rid == actor_id:
            continue
        item = {
            "target_id": rid,
            "target_name": r.get("name", f"Character {rid}"),
            "hint": (
                "Pre-validate with discover(what='war_validity', "
                f"target_id={rid}) before act(type='war', ...)."
            ),
            "act": {
                "type": "war",
                "target_id": rid,
                "cb_index": -1,
            },
        }
        if r.get("opinion") is not None:
            item["opinion"] = r["opinion"]
        if r.get("military_strength") is not None:
            item["military_strength"] = r["military_strength"]
        out.append(item)
    dropped = max(0, len(out) - limit)
    return out[:limit], dropped


# ---------------------------------------------------------------------------
# Pretty-print helper (for prompt inclusion / debug logs)
# ---------------------------------------------------------------------------

def format_action_menu(menu: dict[str, Any]) -> str:
    """Render a menu dict as a markdown block suitable for LLM prompts."""
    lines = ["## Available Actions", f"*{menu.get('summary', '')}*"]

    interactions = menu.get("interactions") or []
    if interactions:
        lines.append("\n### Interactions")
        for item in interactions:
            name = item["interaction_type"].replace("_interaction", "").replace("_", " ").title()
            target_id = item.get("target_id", 0)
            target_name = item.get("target_name", "")
            line = f"- **{name}**"
            if target_id:
                if target_name:
                    line += f" → {target_name} (id={target_id})"
                else:
                    line += f" → id={target_id}"
            extras = []
            if item.get("will_accept") is not None:
                extras.append(f"accept={'yes' if item['will_accept'] else 'no'}")
            if item.get("ai_score") is not None:
                extras.append(f"score={item['ai_score']}")
            if extras:
                line += f" ({', '.join(extras)})"
            lines.append(line)

    decisions = menu.get("decisions") or []
    if decisions:
        lines.append("\n### Decisions")
        for item in decisions:
            name = item["decision_type"].replace("_decision", "").replace("_", " ").title()
            line = f"- **{name}** (`{item['decision_type']}`)"
            desc = item.get("description")
            if desc:
                line += f" — {desc}"
            lines.append(line)

    wars = menu.get("wars") or []
    if wars:
        lines.append("\n### Potential War Targets")
        for item in wars:
            line = f"- **{item['target_name']}** (id={item['target_id']})"
            tags = []
            if item.get("opinion") is not None:
                tags.append(f"opinion={item['opinion']}")
            if item.get("military_strength") is not None:
                tags.append(f"mil={item['military_strength']}")
            if tags:
                line += " — " + ", ".join(tags)
            lines.append(line)
        lines.append(
            "*Always discover(what='war_validity', target_id=N) before "
            "act(type='war'). claim_cb (index 11) requires cb_title_id.*"
        )

    truncated = menu.get("truncated") or {}
    if truncated:
        lines.append("\n*Truncated:* " + ", ".join(
            f"{k}=+{v}" for k, v in truncated.items()
        ))

    return "\n".join(lines)
