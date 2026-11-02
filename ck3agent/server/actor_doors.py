"""The doors that answer about the bound ruler (L14 FF1 m-20): ONE declared table.

A query domain or a discover subject asks about the bound ruler when its bridge
handler resolves an unnamed actor (character_id / actor_id 0 or absent) to the
controlled character: resolve_character, resolve_actor, get_controlled_character
or g_controlled_primary in the handler's chain (bridge/src). With no ruler bound
such a door has nobody to answer about, so the server answers the refusal every
actor-bearing tool answers (redesign.unbound_refusal). A door whose handler never
falls back to the bound ruler (a catalog, a feed, a read that needs its ids named)
is answered whatever is bound.

Until L14 FF1 the rule keyed on a PROXY: "the bridge params carry a character_id
key". discover(decisions) refused unbound because the server sends a character_id
its handler never reads (case_list_decisions ignores its params), while
discover(interactions), the same kind of catalog, answered.

Read by the server (redesign.unbound_refusal) and by the live scenario that grades
it (scripts/live867/scenarios/eval_a_surface.py). test_actor_doors.py checks the
table against the bridge source: every door the query and discover tools reach is
classified, a door classified as not reading the actor has no fallback in its
handler's chain, and an actor door either has one or names the function that does.
"""
from __future__ import annotations

#: The bridge-param keys that name the acting character.
ACTOR_PARAM_KEYS = ("character_id", "actor_id")

#: Bridge action -> the param keys that name ANOTHER subject. With one of them set
#: the handler answers about that subject and does not fall back to the ruler (a
#: title's de-jure, a culture's innovations, an explicit map scope).
ACTOR_DOORS: dict[str, tuple[str, ...]] = {
    "query_alliances": (),
    "query_armies": (),
    "query_call_to_arms": (),
    "query_character": (),
    "query_claims": (),
    "query_conquest_targets": (),
    "query_council": (),
    "query_county_details": (),
    "query_faction_threat": (),
    "query_factions": (),
    "query_hooks": (),
    "query_incoming_schemes": (),
    "query_available_law_changes": (),
    "query_realm_laws": (),
    "query_levy_readiness": (),
    "query_lifestyle": (),
    "query_marriage_candidates": (),
    "query_character_memories": (),
    "query_realm": (),
    "query_relations": (),
    "query_schemes": (),
    "query_succession": (),
    "query_vassal_loyalty": (),
    "query_wars": (),
    "query_de_jure": ("title_id",),
    "query_development_growth": ("title_id",),
    "query_election_state": ("title_id",),
    "query_county_neighbors": ("title_id",),
    "query_title_hierarchy": ("title_id",),
    "query_dynasty": ("dynasty_id",),
    "query_innovation_progress": ("culture_id",),
    "query_fascination_progress": ("culture_id",),
    "query_research_speed": ("culture_id",),
    "query_map_graph": ("target_county", "target_county_key", "counties"),
    "query_valid_war_targets": (),
    "pre_validate_war": (),
    "query_interaction_acceptance": (),
    "query_valid_cbs": (),
    "query_cb_validity_strict": (),
    "query_trigger_result": (),
    "construction_check": (),
    "list_decisions_annotated": (),
    "list_valid_decisions": (),
}

#: Actor doors whose fallback is not in the handler's own chain: the function
#: that holds it (the game-thread worker the handler submits to).
ACTOR_DOORS_VIA: dict[str, str] = {
    "list_decisions_annotated": "enumerate_decisions_annotated",
    "list_valid_decisions": "enumerate_decisions_annotated",
}

#: Doors whose handler falls back to the bound ruler and still answers without
#: one, and what it answers then. Not refused unbound.
ACTOR_OPTIONAL: dict[str, str] = {
    "query_council_tasks": "with no character it answers the council-task hook feed",
}

#: Doors the server composes from several reads, each about the bound ruler.
COMPOSED_ACTOR_DOORS = ("relative_power", "targets", "decision_why_blocked")


def about_the_bound_ruler(action: str, bridge_params: dict) -> bool:
    """True when this door, with these params, asks about the bound ruler: an
    actor door (ACTOR_DOORS or COMPOSED_ACTOR_DOORS) with no actor named and no
    other subject named."""
    if action not in ACTOR_DOORS and action not in COMPOSED_ACTOR_DOORS:
        return False
    p = bridge_params if isinstance(bridge_params, dict) else {}
    for k in ACTOR_PARAM_KEYS + ACTOR_DOORS.get(action, ()):
        v = p.get(k)
        if isinstance(v, bool):
            continue
        if isinstance(v, (int, float)) and v > 0:
            return False
        if isinstance(v, str) and v.strip() not in ("", "0"):
            return False
        if isinstance(v, (list, tuple)) and v:
            return False
    return True
