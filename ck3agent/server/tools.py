"""
CK3 Agent tool dispatch — 11 consolidated MCP tools replacing the original 56.

Consolidation at MCP layer only. All bridge actions stay unchanged.
This module maps consolidated tool calls to existing bridge action strings.

Entry points:
  - get_tool_definitions() -> list[dict]   Anthropic API tool-use schema (agent.py)
  - resolve_observe_sections / resolve_query / resolve_discover / resolve_act /
    resolve_intent / resolve_game / resolve_control — per-tool dispatch helpers
    used by both agent.py and mcp_server.py.
"""


# ---------------------------------------------------------------------------
# Query domain → bridge action mapping
# ---------------------------------------------------------------------------

# Maps query domain string to (bridge_action, param_builder_fn)
# param_builder_fn takes the tool input dict and returns bridge params dict
QUERY_DOMAIN_MAP = {
    # character: + house_id/dynasty_id (numeric) + kin flags relative to the controlled
    # ruler — is_kin, same_house, same_dynasty, relation (self|child|parent|spouse|
    # sibling|same_house|same_dynasty|unrelated). MURDER GATE: only fire
    # act(type=scheme, action=start, scheme=murder, target_id=N) when is_kin==false
    # (kinslayer trait → catastrophic stress). (#is_kin)
    "character":            ("query_character",            lambda p: {"character_id": p.get("character_id", 0)}),
    # ck-leg: character browser — scan CHARACTER_DB (≈103k chars) with filter+sort,
    # bounded top-N. filter ∈ all|alive|dead|ruler|independent|ai_controlled;
    # sort ∈ military|gold|realm_size|tier|age|<skill>. The first real way to find
    # strong independent rulers (e.g. marriage-alliance prospects) by name+stats.
    "characters":           ("search_characters",          lambda p: {"filter": p.get("filter") or "alive", "sort": p.get("sort") or "military", "sort_order": p.get("sort_order") or "desc", "max_results": p.get("max_results") or 50, **({"verbose": p["verbose"]} if "verbose" in p else {}), **({"court_of": p["court_of"]} if p.get("court_of") else {})}),
    # §1a (re_heir_education_RE.md): per-child/heir education-focus read. education_focus
    # + education_focus_skill are REAL; guardian_id/_martial null (§1b DEEP-RE).
    "heir_education":        ("query_heir_education",        lambda p: {"character_id": p.get("character_id", 0)}),
    # ck-leg: marriage-market browser — landed unmarried rulers in diplomatic range
    # of character_id, top-N by `sort` (military default), annotated age/distance.
    # The precise "who can I marry" surface (vs the generic `characters` browser).
    # Pass require_in_range=0 (via max_results/sort params path) to browse globally.
    "marriage_candidates":  ("query_marriage_candidates",  lambda p: {"character_id": p.get("character_id", 0), "sort": p.get("sort") or "military", "max_results": p.get("max_results") or 30, **({"require_in_range": p["require_in_range"]} if "require_in_range" in p else {})}),
    "opinion":              ("query_opinion",              lambda p: {"actor_id": p.get("character_id", 0), "target_id": p.get("target_id", 0)}),
    "realm":                ("query_realm",                lambda p: {"character_id": p.get("character_id", 0), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    "wars":                 ("query_wars",                 lambda p: {"character_id": p.get("character_id", 0), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    # L6: unpaused-safe minimal active-wars brief (bridge-internal, served
    # inline even while UNPAUSED). Returns per-war war_id/name/attacker_id/
    # defender_id/is_attacker (+ war_score/favored_side when the breakdown cache
    # has it). Use this when the game is running and the rich `wars` domain is
    # pause-gated out.
    "active_wars":          ("get_active_wars",            lambda p: {"character_id": p.get("character_id", 0)}),
    # #120: a specific war_id (gen-tagged ref OR bare slot idx) returns the FULL
    # attacker-perspective breakdown — total_warscore (-100..100) + cache_status +
    # the components battle_score/ticking_warscore (always live) and
    # occupation_score/imprisonment_score/landless_score (engine-cached). A
    # high battle_score with occupation_score 0 and cache_status="cached" means
    # battles won but war-goal counties NOT sieged → siege-stall, intervene.
    # cache_status="live_only" means occupation just hasn't been computed yet.
    "war_score":            ("query_war_score_breakdown",  lambda p: {"war_id": p.get("war_id", 0)}),
    "war_declarations":     ("query_war_declarations",     lambda p: {"limit": p.get("limit", 64), "since_tick": p.get("since_tick", 0)}),
    "scheme_inits":         ("query_scheme_inits",         lambda p: {"limit": p.get("limit", 64), "since_tick": p.get("since_tick", 0), **({"scheme_type": p["scheme_type"]} if p.get("scheme_type") else {})}),
    "council_tasks":        ("query_council_tasks",        lambda p: {"limit": p.get("limit", 64), "since_tick": p.get("since_tick", 0)}),
    "factions":             ("query_factions",             lambda p: {"character_id": p.get("character_id", 0)}),
    "faction_threat":       ("query_faction_threat",       lambda p: {"character_id": p.get("character_id", 0)}),
    # Per-vassal loyalty/opinion roster for the liege's direct vassals — lowest-opinion
    # first, with count_disloyal / count_in_faction summary. The "who's about to revolt"
    # surface; pairs with faction_threat. (handler do_query_vassal_loyalty)
    "vassal_loyalty":       ("query_vassal_loyalty",       lambda p: {"character_id": p.get("character_id", 0)}),
    # hooks held BY this character (strong/weak) — a strong hook locks a vassal OUT of
    # factions (faction-eligibility, playbook OBSERVE #8) and powers forced interactions.
    "hooks":                ("query_hooks",                lambda p: {"character_id": p.get("character_id", 0)}),
    "borders":              ("query_borders",              lambda p: {"character_id": p.get("character_id", 0)}),
    # #110/#117: conquest targeting toward the de-jure ladder (2nd duchy -> kingdom
    # -> e_britannia). Returns candidate duchies: partially-held (foothold-completion)
    # FIRST, then OUTWARD EXPANSION targets (expansion_target=true) — duchies de-jure
    # under a British kingdom (de_jure_british=true, de_jure_kingdom_key=k_england/
    # k_scotland/k_wales/k_ireland) or adjacent to the realm — so a King-tier ruler
    # holding complete duchies still SEES the next British duchies to conquer. Each
    # duchy carries remaining_counties[] with holder + is_kin/is_allied/is_independent
    # flags, holder_top_liege_id (the INDEPENDENT top-liege), and conquerable_now
    # (engine-verified county_conquest_cb for top candidates; heuristic otherwise) plus
    # an embedded `declare` block. The targeting half of the directional-conquest lever
    # (#74) — pair with act(type=war, method=steered, casus_belli=county_conquest_cb).
    # Optional: include_expansion (default true), max_engine_checks, engine_timeout_ms.
    "conquest_targets":     ("query_conquest_targets",     lambda p: {"character_id": p.get("character_id", 0), **({"include_expansion": p["include_expansion"]} if "include_expansion" in p else {}), **({"max_engine_checks": p["max_engine_checks"]} if "max_engine_checks" in p else {}), **({"engine_timeout_ms": p["engine_timeout_ms"]} if "engine_timeout_ms" in p else {})}),
    "claims":               ("query_claims",               lambda p: {"character_id": p.get("character_id", 0), "include_vassals": p.get("include_vassals", False), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    # Owned schemes — each entry: scheme_id, owner_id/owner_name, target_id/target_name
    # (char targets), type (raw key) + scheme_type_label (friendly), success_chance,
    # progress/max_progress/progress_pct, phase (building|ready, derived), agents,
    # is_frozen, discovery_timer/exposure_timer + is_discovered, eta_days (advisory,
    # only when growth>0), start_date. (#scheme_legibility)
    "schemes":              ("query_schemes",              lambda p: {"character_id": p.get("character_id", 0)}),
    # Plots TARGETING me (murder/abduct/fabricate_hook/...) — the survival surface.
    # Each entry: schemer_id/schemer_name (the threat), type + scheme_type_label,
    # danger (lethal|high|moderate|low) + dangerous bool, success_chance,
    # progress/max_progress/progress_pct, is_discovered, eta_days (advisory). Top-level
    # dangerous_count flags lethal/takeover plots. (#scheme_legibility, re_incoming_schemes.md)
    "incoming_schemes":     ("query_incoming_schemes",     lambda p: {"character_id": p.get("character_id", 0)}),
    "succession":           ("query_succession",           lambda p: {"character_id": p.get("character_id", 0)}),
    "lifestyle":            ("query_lifestyle",            lambda p: {"character_id": p.get("character_id", 0)}),
    "armies":               ("query_armies",               lambda p: {"character_id": p.get("character_id", 0), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    "county":               ("query_county_details",       lambda p: {"character_id": p.get("character_id", 0)}),
    "development":          ("query_development_growth",   lambda p: {"title_id": p.get("title_id", 0), "character_id": p.get("character_id", 0)}),
    "buildings":            ("query_building_types",       lambda p: {"category": p.get("category", 0)}),
    # lane-query item 4: title_id=0 resolves the character (character_id, 0 =
    # the controlled ruler): its capital county, plus bordering_realms[].
    "neighbors":            ("query_county_neighbors",     lambda p: {"title_id": p.get("title_id", 0),
                                                                       "character_id": p.get("character_id", 0)}),
    "adjacency":            ("query_province_adjacency",   lambda p: {"title_id": p.get("title_id", 0)}),
    # map_graph: the baked map-graph window (ck3map.bin via ck3mapgen). Static topology
    # + de-jure + terrain, live overlay (holder/faith/culture/dev/control), serialized to
    # a sigil text block in data.text. scope=realm|target|explicit; BFS to `hops`, capped
    # by max_nodes; detail=brief drops distances + the live overlay for wide overviews.
    # Self-disables ({"error":"map_artifact_missing"}) when the artifact is absent.
    "map_graph":            ("query_map_graph",            lambda p: {
        "scope": p.get("scope", "realm"),
        "character_id": p.get("character_id", 0),
        "hops": p.get("hops", 2),
        "max_nodes": p.get("max_nodes", 60),
        **({"target_county": p["target_county"]} if p.get("target_county") else {}),
        **({"target_county_key": p["target_county_key"]} if p.get("target_county_key") else {}),
        **({"counties": p["counties"]} if p.get("counties") else {}),
        **({"detail": p["detail"]} if p.get("detail") else {}),
    }),
    "titles":               ("query_title_hierarchy",      lambda p: {"title_id": p.get("title_id", 0), "character_id": p.get("character_id", 0)}),
    "de_jure":              ("query_de_jure",              lambda p: {"title_id": p.get("title_id", 0), "character_id": p.get("character_id", 0), **({"claimant_id": p["claimant_id"]} if p.get("claimant_id") else {})}),
    # the de-jure county counter — query_dejure_progress
    # is the documented "ONLY progress metric" but had NO legacy-query entry (reachable
    # only via v2 observe/inspect), so CLAUDE.md/MEMORY references to query(domain=...)
    # for the meter were dead. Wire it onto the legacy path for parity (additive; the v2
    # surface is unchanged). ck-zgh4 deploy-51 REMOVED the e_britannia default; pass
    # title_id / title_key to read any title's meter, verbose=1 for the per-county array.
    "dejure":               ("query_dejure_progress",      lambda p: {"character_id": p.get("character_id", 0), **({"title_id": p["title_id"]} if p.get("title_id") else {}), **({"title_key": p["title_key"]} if p.get("title_key") else {}), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    "goal":                 ("query_dejure_progress",      lambda p: {"character_id": p.get("character_id", 0), **({"title_id": p["title_id"]} if p.get("title_id") else {}), **({"title_key": p["title_key"]} if p.get("title_key") else {}), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    "council":              ("query_council",              lambda p: {"character_id": p.get("character_id", 0), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    # Read-only enumeration of a character's MEMORIES (CCharacterMemory pool):
    # per-memory ref_id/index/generation, owner_id, primary participant (id+name),
    # participant_count, and creation/end dates (raw + formatted "YYYY.MM.DD").
    # The memory TYPE NAME and per-participant {char,role} decode are not yet
    # resolvable on this build (type=null) — see re_character_memories.md open
    # questions. character_id defaults to controlled.
    "memories":             ("query_character_memories",   lambda p: {"character_id": p.get("character_id", 0)}),
    "election":             ("query_election_state",       lambda p: {"title_id": p.get("title_id", 0), "character_id": p.get("character_id", 0)}),
    "laws":                 ("query_realm_laws",           lambda p: {"character_id": p.get("character_id", 0)}),
    "law_groups":           ("query_law_groups",           lambda p: {}),
    "law_options":          ("query_available_law_changes", lambda p: {"character_id": p.get("character_id", 0)}),
    "relations":            ("query_relations",            lambda p: {"character_id": p.get("character_id", 0)}),
    # ck-leg: who holds claims on a title (pressed/unpressed, alive, is_ruler) —
    # the "who do I back as claimant" surface for claim wars. Pair with
    # discover(what="war_targets") and query(domain="claims").
    "claimants":            ("query_title_claimants",      lambda p: {"title_id": p.get("title_id", 0), **({"actor_id": p["character_id"]} if p.get("character_id") else {})}),
    # history/legibility: chronological HOLDER history of a title (who held it when,
    # grant/succession/destroyed events) — "who used to hold this title" + recent
    # realm changes. Pure native read. Params: title_id, since_date "Y.M.D", limit, order.
    "title_history":        ("query_title_history",        lambda p: {"title_id": p.get("title_id", 0), **({"since_date": p["since_date"]} if p.get("since_date") else {}), **({"limit": p["limit"]} if p.get("limit") else {}), **({"order": p["order"]} if p.get("order") else {})}),
    # plotting: title-centric succession — current holder + succession LAW + the
    # ORDERED heir line (who inherits next, and after) + designated heir; for elective
    # titles call domain="election" for vote tallies. Params: title_id.
    "title_succession":     ("query_title_succession",     lambda p: {"title_id": p.get("title_id", 0)}),
    # ck-leg: per-modifier breakdown of why target's opinion of actor is what it
    # is (decaying modifiers, start/end dates). character_id = actor, target_id = subject.
    "opinion_breakdown":    ("query_opinion_breakdown",    lambda p: {"actor_id": p.get("character_id", 0), "target_id": p.get("target_id", 0)}),
    # ck-xoj6x: 'available_cbs' deprecated — strictly inferior to
    # discover(what="valid_cbs_strict"). The bridge handler do_query_available_cbs
    # remains for legacy scripts (war_dispatch_lab, bridge_test_suite) and the
    # inline-safety regression suite; it just isn't exposed at the MCP layer.
    "traits":               ("query_trait_db",             lambda p: {}),
    "trait_compatibility":  ("query_trait_compatibility",  lambda p: {"trait_id": p.get("trait_id", 0)}),
    # dynasty: + legacies[] {perk_key, track, rank} (every unlocked legacy perk by name)
    # and tracks{} (track -> max rank owned) alongside prestige/level/head. (#dynasty_house)
    "dynasty":              ("query_dynasty",              lambda p: {"dynasty_id": p.get("dynasty_id", 0), "character_id": p.get("character_id", 0)}),
    # innovation_progress: + era_summary[] {era_index, era_name, known, total,
    # is_fascination_era} (the feudalize gate), per-row era_name + category
    # (known|researching|available), fascination_name/_era, current_era. (#innovations)
    "innovation_progress":  ("query_innovation_progress",  lambda p: {"culture_id": p.get("culture_id", 0), "character_id": p.get("character_id", 0)}),
    # fascination_progress: fascination = PRIMARY (+0x6F0) by NAME, bonus_fascination
    # (+0x6D8), eligible_fascinations[] {name,era,db_index,progress_pct}, culture_key,
    # current_era. (#fascination)
    "fascination_progress": ("query_fascination_progress", lambda p: {"culture_id": p.get("culture_id", 0), "character_id": p.get("character_id", 0)}),
    "research_speed":       ("query_research_speed",       lambda p: {"culture_id": p.get("culture_id", 0), "character_id": p.get("character_id", 0)}),
    "levy_readiness":       ("query_levy_readiness",       lambda p: {"character_id": p.get("character_id", 0)}),
    "partition_preview":    ("query_partition_preview",     lambda p: {"character_id": p.get("character_id", 0)}),
    # succession_preview: the per-title partition MAP — for each held title, which heir
    # gets it and under which succession law (partition vs single-heir), plus whether the
    # NEXT succession SCATTERS titles (multiple heirs land titles) or passes to one heir.
    # Pure crash-hardened READ (safe_read/is_mapped, no engine reentry/alloc). The
    # at-a-glance "does my realm survive the trough whole?" surface the agent checks each
    # reign so it can drive toward single-heir BEFORE the trough (ck-succprev).
    "succession_preview":   ("query_title_succession_map",   lambda p: {"character_id": p.get("character_id", 0)}),
    "succession_simulation": ("query_succession_simulation", lambda p: {"character_id": p.get("character_id", 0)}),
    "diplomatic_range":     ("query_diplomatic_range",     lambda p: {"character_id_a": p.get("character_id", 0), "character_id_b": p.get("target_id", 0)}),
    # ck-leg: real alliance handler (own/ally military strength + why each
    # alliance exists). Previously aliased to query_relations, which hid the
    # actual strength data the agent needs to judge whether allies are useful.
    "alliances":            ("query_alliances",            lambda p: {"character_id": p.get("character_id", 0)}),
    "call_to_arms":         ("query_call_to_arms",         lambda p: {"character_id": p.get("character_id", 0)}),
    "event_probabilities":  ("query_event_probabilities",  lambda p: {}),
    "debug_flags":          ("query_debug_flags",          lambda p: {}),
}

# event_log is a synthetic aggregation domain (handled specially in mcp_server.query,
# not via QUERY_DOMAIN_MAP) — it merges the war/scheme/council since_tick feeds.
QUERY_DOMAINS = sorted(QUERY_DOMAIN_MAP.keys()) + ["event_log", "relative_power"]

# Observe section → bridge action mapping
OBSERVE_SECTIONS = {
    "character":    ("query_character",          lambda p: {}),
    "tick":         ("get_tick_info",            lambda p: {}),
    "personality":  ("get_ai_personality",       lambda p: {"character_id": p.get("character_id", 0)}),
    "evaluations":  ("get_ai_evaluations",       lambda p: {"character_id": p.get("character_id", 0)}),
    "subsystems":   ("get_character_subsystems", lambda p: {"character_id": p.get("character_id", 0)}),
}

OBSERVE_DEFAULTS = ["character", "tick", "personality", "evaluations"]


# ---------------------------------------------------------------------------
# Resolve helpers — convert consolidated tool params to bridge actions
# ---------------------------------------------------------------------------

def resolve_query(domain: str, params: dict) -> tuple[str, dict] | None:
    """Resolve a query domain to (bridge_action, bridge_params). Returns None for unknown domain."""
    entry = QUERY_DOMAIN_MAP.get(domain)
    if entry is None:
        return None
    action, param_fn = entry
    return action, param_fn(params)


def resolve_observe_sections(include: list[str] | None, params: dict) -> list[tuple[str, str, dict]]:
    """Resolve observe sections to [(section_name, bridge_action, bridge_params), ...]."""
    sections = include if include else OBSERVE_DEFAULTS
    result = []
    for section in sections:
        entry = OBSERVE_SECTIONS.get(section)
        if entry:
            action, param_fn = entry
            result.append((section, action, param_fn(params)))
    return result


def resolve_act(params: dict) -> tuple[str, dict] | None:
    """Resolve an act tool call to (bridge_action, bridge_params)."""
    act_type = params.get("type")
    actor_id = params.get("actor_id", 0)

    if act_type == "interaction":
        return "send_interaction", {
            "interaction_type": params.get("interaction_type", ""),
            "target_id": params.get("target_id", 0),
            "actor_id": actor_id,
            "title_id": params.get("title_id", 0),
            "secondary_actor_id": params.get("secondary_actor_id", 0),
            "secondary_recipient_id": params.get("secondary_recipient_id", 0),
        }
    elif act_type == "war":
        casus_belli = params.get("casus_belli")
        # STEERED declaration (method='steered'): the bridge makes the actor
        # declare a war on EXACTLY the chosen target / CB / title / claimant via
        # WAR_CREATE_CORE direct — deterministic, independent of the AI evaluator,
        # named, no crash (re_steered_war_SOLVED_20260611). Requires casus_belli
        # (CB script name, from discover(what='valid_cbs_strict')) and cb_title_id;
        # claimant_id defaults to the actor. claim_cb needs a PRESSED claim or the
        # engine culls the war on the next tick.
        if params.get("method") == "steered":
            if not casus_belli:
                raise ValueError(
                    "act(type='war', method='steered') requires casus_belli (CB name)")
            return "call_war_create_core", {
                "actor_id": actor_id,
                "target_id": params.get("target_id", 0),
                "casus_belli": casus_belli,
                "cb_title_id": params.get("cb_title_id", 0),
                # default claimant to the actor (the common claim_cb "press my own
                # claim" case); 0 would map to 0xFFFFFFFF=none → claim_cb culled.
                "claimant_id": params.get("claimant_id") or actor_id,
            }

        # Default: AI-pipeline declaration (target/CB are hints; AI may decline).
        result = {
            "target_id": params.get("target_id", 0),
            "actor_id": actor_id,
            "cb_index": params.get("cb_index", -1),
            "cb_title_id": params.get("cb_title_id", 0),
            "claimant_id": params.get("claimant_id", 0),
        }
        if params.get("skip_pre_validate"):
            result["skip_pre_validate"] = True
        # ck-j3vb0: force_player_path bypasses the AI war pipeline and
        # routes through WAR_CREATE_CORE direct. Required when set: the
        # CB name string (casus_belli). Use discover(what='valid_cbs_strict')
        # to enumerate valid CB names for an (actor, target) pair.
        if params.get("force_player_path"):
            result["force_player_path"] = True
        if casus_belli:
            result["casus_belli"] = casus_belli
        return "declare_war", result
    elif act_type == "decision":
        return "send_decision", {
            "decision_type": params.get("decision_type", ""),
            "character_id": actor_id,
        }
    elif act_type == "event":
        return "respond_event", {
            "option": params.get("option", 0),
            "event_id": params.get("event_id", ""),
            "instance_id": params.get("instance_id", 0),
        }
    elif act_type == "law":
        # Per-TITLE law setter (e.g. a succession law on a specific CLandedTitle)
        # when law_scope="title": routes to the native add_title_law console
        # handler (applies real costs + effects). Default (no law_scope) keeps
        # the realm-law path unchanged.
        if params.get("law_scope") == "title":
            return "set_title_law", {
                "actor_id": actor_id,
                "title_id": params.get("title_id", 0),
                "law_name": params.get("law_name", ""),
            }
        # On refusal the bridge returns a SPECIFIC blocker (#109): data.reason is one
        # of at_war / cooldown_active / insufficient_prestige / missing_prerequisite /
        # not_eligible_yet / already_active / not_a_ruler, with data.failures[] carrying
        # the raw engine loc-keys. These pass through the envelope unchanged — read
        # data.reason to plan around the gate rather than retrying blindly.
        return "change_realm_law", {
            "law_name": params.get("law_name", ""),
            "actor_id": actor_id,
        }
    elif act_type == "faith":
        # Faith mutation on the ACTOR. Two modes:
        #   - convert (params has "faith" or "faith_id"): CONVERT the actor's OWN
        #     faith to the named faith (e.g. faith="catholic"). Routes to the
        #     engine setter SetFaithOnCharacter (SET_FAITH) — a real conversion
        #     with on_faith_conversion + recalc side effects, NOT a memory poke.
        #     This is THE convert-own-faith lever (e.g. Norse k_danelaw → Catholic
        #     to stop being holy-warred). Voluntary self-conversion is auto_accept
        #     in CK3; honor the in-game is_shown checks yourself (must differ from
        #     current faith; landed). Pass faith by KEY (faith="catholic") or
        #     faith_id (int). Must be UNPAUSED.
        #   - doctrine (default): swap/add one doctrine/tenet on the actor's
        #     CURRENT faith by key (e.g. "doctrine_theocracy"). Native
        #     FAITH_ADD_DOCTRINE (validated + group-limit-checked). ADD-only (v1).
        if params.get("faith") or params.get("faith_id"):
            args = {"actor_id": actor_id}
            if params.get("faith"):
                args["faith"] = params.get("faith")
            if params.get("faith_id"):
                args["faith_id"] = params.get("faith_id")
            return "set_character_faith", args
        return "set_doctrine", {
            "actor_id": actor_id,
            "doctrine": params.get("doctrine", ""),
        }
    elif act_type == "culture":
        # CONVERT the actor's OWN culture to the named culture (e.g.
        # culture="english" or "anglo_saxon"). Routes to the engine setter
        # SetCultureOnCharacter (SET_CULTURE) — a real conversion with full
        # culture/heritage/innovation recalc, NOT a memory poke. THE
        # convert-own-culture lever: e_britannia's can_create is CULTURE-GATED
        # (needs heritage_west_germanic: english/anglo_saxon — or goidelic/
        # brythonic). A NORSE (north_germanic) Halfdan-line ruler can form
        # e_britannia by NEITHER path, so convert culture to "english"/
        # "anglo_saxon" FIRST (after holding English land). Pass culture by
        # script KEY (culture="english") or culture_id (int). Must be UNPAUSED.
        # Returns previous_culture/current_culture for verification.
        args = {"actor_id": actor_id}
        if params.get("culture"):
            args["culture"] = params.get("culture")
        if params.get("culture_id"):
            args["culture_id"] = params.get("culture_id")
        return "set_character_culture", args
    elif act_type == "lifestyle":
        # ck-leg: set the actor's lifestyle focus by name (e.g.
        # "stewardship_domain_focus"). Resolved native SET_FOCUS_APPLY call.
        return "set_lifestyle_focus", {
            "actor_id": actor_id,
            "focus": params.get("focus", ""),
        }
    elif act_type == "set_education_focus":
        # §2 (re_heir_education_RE.md): set a CHILD heir's childhood education focus
        # by name (education_martial / education_diplomacy / education_stewardship /
        # education_intrigue / education_learning). Type-checked alias over the
        # crash-hardened SET_FOCUS_APPLY; childhood and adult focus share one slot, so
        # the readback detects success on a child. EXPERIMENTAL until one child smoke;
        # verify via query(domain="heir_education", character_id=<child>).education_focus.
        return "set_education_focus", {
            "actor_id": actor_id,
            "focus": params.get("focus", ""),
        }
    elif act_type == "building":
        # Construct a building in a holding by building key (e.g. "castle_03").
        # CAddBuildingEffect path (do_construct_building); applies instantly.
        # location_id = the holding's barony title id (from
        # query(domain="county_details") holdings[].barony_title_id).
        # #133 build_gate: REFUSES with {success:false, reason:"refused_not_buildable",
        # detail:"not_owner_or_realm"} when the actor does not own/control the holding
        # (pure-memory ownership pre-gate). Deeper terrain/innovation gating is a
        # documented follow-up (engine conjunction, pending live crash-validation).
        return "construct_building", {
            "actor_id": actor_id,
            "building": params.get("building", ""),
            "location_id": params.get("location_id", 0),
            "execute": True,
        }
    elif act_type == "council_task":
        # Assign a council task to the actor's councillor by task key (e.g.
        # "task_increase_control"). Resolved native CHANGE_COUNCIL_TASK call —
        # finds the matching council position and dispatches the change command.
        # Some tasks need a de jure title target (target_title_id). The command
        # applies on a game tick, so success=dispatched; poll query(council) to
        # confirm the task transitioned.
        out = {
            "actor_id": actor_id,
            "task": params.get("task", ""),
            "target_title_id": params.get("target_title_id", 0),
        }
        # lane-query item 1: forward an explicit seat so the bridge can REFUSE
        # a seat that cannot take this task (it used to be dropped here, and
        # the task silently landed on its own seat).
        if params.get("seat"):
            out["seat"] = str(params.get("seat"))
        return "set_council_task", out
    elif act_type == "maa":
        # Men-at-arms control (the doomstack engine). action ∈ recruit|station|unstation.
        #   recruit  — raise a regiment of maa_type (from query(domain="armies")/unlocked MaA)
        #   station  — set a regiment's origin to a bonus holding (regiment_id + location_id)
        #   unstation— clear a regiment's origin (regiment_id)
        # regiment_id/location_id from query(domain="levy_readiness").
        maa_action = params.get("action", "recruit")
        if maa_action == "station":
            return "maa_station", {
                "actor_id": actor_id,
                "regiment_id": params.get("regiment_id", 0),
                "location_id": params.get("location_id", 0) or params.get("title_id", 0),
            }
        elif maa_action == "unstation":
            return "maa_unstation", {
                "actor_id": actor_id,
                "regiment_id": params.get("regiment_id", 0),
            }
        return "maa_recruit", {
            "actor_id": actor_id,
            "maa_type": params.get("maa_type", ""),
        }
    elif act_type == "hire_mercenary":
        # Hire a mercenary company (sudden military power). company_id from
        # discover(what="hireable_mercenaries"). Actor must be an AI ruler.
        return "hire_mercenary", {
            "actor_id": actor_id,
            "company_id": params.get("company_id", 0),
            "contract_len": params.get("contract_len", 0),
        }
    elif act_type == "raise_armies":
        # A8.2: mobilize a steered AI ruler's levies (guarded g_PlayerCharacterID
        # global-swap → RaiseAllArmiesCore → restore; crash/hang-safe, live-validated
        # 2026-06-21). Game-thread-deferred: dispatches an intent that executes on the
        # next on_tick (needs UNPAUSED). May respond status:"dispatched_pending" while
        # the intent completes — confirm via the completed list (query_intents). actor
        # must be an AI ruler (rejects the player char).
        return "raise_armies", {
            "actor_id": actor_id,
        }
    elif act_type == "raid":
        # #96: toggle a steered Norse/tribal ruler's RAISED armies into RAIDING
        # mode — the historical resource engine (raise → raid → loot for gold +
        # prestige + piety, NO casus belli needed; the lever past the King-tier
        # ceiling). Guarded g_PlayerCharacterID global-swap → toggle-raiding
        # dispatch → restore (crash/hang-safe). VALIDATOR-GATED: a non-Norse or
        # army-less ruler no-ops. RAISE ARMIES FIRST (act type="raise_armies"),
        # then raid. Game-thread-deferred (needs UNPAUSED); may respond
        # status:"dispatched_pending" — confirm via query(domain="armies")
        # (raiding flag) or query_intents. actor must be an AI ruler.
        return "raise_raiders", {
            "actor_id": actor_id,
        }
    # REMOVED: order_siege / move_army act-type mapping deleted per design
    # (agent does not drive army micro).
    elif act_type == "create_title":
        # #111: create a de-jure DUCHY or KINGDOM title the AI ruler already
        # controls (the standard "Create Title" action — cheap gold, gated on
        # controlling the title's de-jure counties + the title not already
        # existing). Routes through the engine's CCreateTitleCommand CanExecute
        # gate then GAME_COMMAND_DISPATCH — legitimate (not a poke). THE unlock
        # for the k_danelaw win-line: create a 2nd k_england duchy title (e.g.
        # "d_northumbria") whose counties you fully control, then take
        # negotiate_the_danelaw_decision. Pass either title_id OR title_key
        # (the duchy/kingdom key from query(domain="conquest_targets") /
        # query(domain="de_jure")). Game-thread-deferred (needs UNPAUSED); may
        # respond status:"dispatched_pending" — confirm via query(domain="realm")
        # / query(domain="titles") that the ruler now HOLDS the title. An
        # ineligible ruler (doesn't control the de-jure counties / can't afford)
        # is refused by the engine gate. actor must be an AI ruler.
        out = {"actor_id": actor_id}
        tid = params.get("title_id", 0)
        if tid:
            out["title_id"] = tid
        tkey = params.get("title_key") or params.get("title")
        if tkey:
            out["title_key"] = tkey
        return "create_title", out
    elif act_type == "appoint":
        # Seat a courtier in a council position (L29 — fill the vacant Court Chaplain
        # seat so fabrication can run). position_id from query(domain="council").
        return "appoint_councillor", {
            "actor_id": actor_id,
            "position_id": params.get("position_id", 0),
            "character_id": params.get("character_id", 0) or params.get("target_id", 0),
            "replace": 1 if params.get("replace") else 0,
        }
    elif act_type == "contract":
        # Set a vassal's contract obligation level by name (vassal management —
        # raise taxes/levies, or grant leniency to head off a faction). Direct
        # guarded memory write. obligation = type key (e.g. feudal_government_taxes).
        return "set_obligation_level", {
            "actor_id": actor_id,
            "vassal_id": params.get("vassal_id", 0) or params.get("target_id", 0),
            "obligation": params.get("obligation", ""),
            "level": params.get("level", 0),
        }
    elif act_type == "move_capital":
        # Relocate the realm capital to a county title (succession/economy play —
        # capital duchy is the engine). Native CAPITAL_RELOCATE + validator.
        return "move_capital", {
            "actor_id": actor_id,
            "title_id": params.get("title_id", 0),
        }
    elif act_type == "break_alliance":
        # GAP 3 (re_toolgap_break_alliance.md, 2026-06-28) — the FINAL bridge impl LANDED a
        # real break: a DIRECT command (break_alliance_cmd), NOT the scripted-effect Execute
        # path, so the #73 scope-frame blocker is moot. The legacy 11-tool act() path does NOT
        # route the new command (resolve_act has no break_alliance bridge marshalling); the
        # LIVE surface is the 8-tool redesign: do(intent="break_alliance", character_id=<actor>,
        # args={"target_id": <ally>}). It is bridge-gated (default-OFF + observer-save smoke
        # pending): arm via session(action="set_experimental", flag="break_alliance"). Refuse
        # here with a CORRECT pointer (not the stale "engine gap / no interaction / pre-#73"
        # message that taught the agent the capability was impossible).
        raise ValueError(
            "break_alliance is not routed through the legacy act() surface. The break lever "
            "now EXISTS (bridge break_alliance_cmd) — use the redesign surface: "
            "do(intent='break_alliance', character_id=<actor>, args={'target_id': <ally>}). "
            "It is bridge-gated (default-OFF + observer-save smoke pending): arm via "
            "session(action='set_experimental', flag='break_alliance', enabled=true), and run "
            "the first dissolve on a DISPOSABLE observer save with confirm_observer_save=true. "
            "See re/findings/re_toolgap_break_alliance.md and re_break_alliance_FINAL.md."
        )
    elif act_type == "decline_call":
        # NON-FUNCTIONAL by ENGINE DESIGN (re_break_alliance_audit.md, 2026-06-27).
        # `call_ally_interaction` is the CALLER's offer; firing it as the recipient is
        # the wrong direction. There is no recipient-fireable decline interaction — the
        # ally-call accept/decline is decided engine-internally (auto_accept/ai_accept).
        # Short-circuit with a clear refusal rather than the misleading
        # `interaction_not_found` retry-bait.
        raise ValueError(
            "decline_call is not supported in this build (genuine engine gap): the ally-call "
            "accept/decline is engine-internal — no recipient-fireable decline interaction "
            "exists. Mitigation = break the alliance at the source to stop FUTURE drags: "
            "do(intent='break_alliance', character_id=<actor>, args={'target_id': <ally>}) "
            "(observer-save-gated; see re_toolgap_break_alliance.md), plus PREVENTION "
            "(don't hold liability alliances; never sit in an ally-offensive war across a "
            "succession)."
        )
    elif act_type == "faction":
        # Create/join a political faction against a liege — the vassal-breaks-free
        # lever (independence_faction/liberty_faction) or claimant_faction. Routed
        # through GET_OR_CREATE_AND_JOIN on the game thread (intent + crash-net).
        # target_id = the liege you're rebelling against. "leave" not yet supported.
        return "faction_action", {
            "actor_id": actor_id,
            "target_id": params.get("target_id", 0),
            "faction_type": params.get("faction_type", "independence_faction"),
            "faction_action": params.get("faction_action", "create"),
            "title_id": params.get("title_id", 0),
            "claimant_id": params.get("claimant_id", 0),
        }
    elif act_type == "scheme":
        # #132 — LEGITIMATELY start a hostile/secret scheme (murder / fabricate_hook /
        # claim_throne / …) owned by the AI ruler against a target character. Routed
        # through the engine's own CreateActiveScheme on the game thread (intent +
        # crash-net), which runs the scheme type's is_shown/is_valid + target-validity
        # + alive + cooldown gates internally — an invalid murder (dead/imprisoned-by-us/
        # cooldown target) is refused by the engine, not forced. actor_id = the
        # schemer (must be AI), target_id = the victim, scheme = type key (e.g.
        # "murder"). THE destabilize-rival lever: kill a partition-succession rival
        # king → his realm shatters; murder his strong heir → regency. Run at PEACE
        # (−50 success if at war with the target). Poll query(domain="schemes",
        # character_id=<schemer>) to confirm + watch progress/success_chance.
        # KIN GATE (#is_kin): before a MURDER scheme, query(domain="character",
        # character_id=<target>) and SKIP if is_kin==true (kinslayer trait → stress).
        #
        # #scheme_cancel: action="cancel" abandons the owner's OWN scheme by scheme_id
        # (from query(domain="schemes", character_id=<owner>).schemes[].scheme_id) via
        # the engine's RemoveActiveScheme. Returns {success, action:"cancel", scheme_id,
        # removed:true} (or not_scheme_owner / scheme_not_found / already_invalidated).
        # action defaults to "start" — existing callers unchanged.
        action = params.get("action", "start")
        if action == "cancel":
            return "scheme_action", {
                "actor_id": actor_id,
                "action": "cancel",
                "scheme_id": params.get("scheme_id", 0),
            }
        return "scheme_action", {
            "actor_id": actor_id,
            "action": "start",
            "target_id": params.get("target_id", 0),
            "scheme": params.get("scheme", params.get("scheme_type", "murder")),
        }
    return None


# ---------------------------------------------------------------------------
# Decision why-blocked clause-checker (the SAFE legibility surface)
# ---------------------------------------------------------------------------
# The engine's per-clause decision eval (discover what="decisions_annotated")
# HANGS the whole game (the engine masks SIGALRM in the trigger-eval critical
# section — re/findings/re_decision_eval_hang.md / memory decision-eval-freezes-game),
# so it is disabled by default. The agent therefore could never SEE which
# is_valid clause of negotiate_the_danelaw was failing — attempt-9 spent ~15
# game-years guessing the unmet "2 completely-controlled English duchies"
# clause (re/findings/re_shield_robustness.md, the (D) LEGIBILITY co-factor).
#
# This checker derives the SHIELD-decision gates from CHEAP STRUCTURAL READS
# (held titles + tier, prestige_level, government, at-war, primary tier, per-duchy
# de-jure control) — the SAME approach observe_v2's `mint_requirements` uses — WITHOUT
# the hanging engine eval. It is additive, crash-safe, and works PAUSED (pure
# reads). It does not REPLACE the engine verdict (act(type=decision) still routes
# through AI_TRY_DECISION_CORE); it tells the agent WHICH clause to fix first.

# title-key constant: the kingdom whose de-jure duchies the danelaw gate counts.
_DANELAW_KINGDOM_KEY = "k_england"

# Per-decision static gate definitions (game-file verified — see
# re/findings/re_shield_robustness.md "The exact gate" + britannia-campaign.md STAGE A).
# Each clause carries a human label, the game requirement, and a fix hint. The
# checker fills pass/fail + measured values from structural reads.
_SHIELD_DECISION_GATES = {
    "negotiate_the_danelaw_decision": {
        "kingdom": "k_danelaw",
        "prestige_level_min": 2,   # medium_prestige_level = 1000 prestige
        "needs_era_tribal": True,
        "needs_duchies": 2,        # 2 completely-controlled k_england duchy TITLES
    },
    "found_kingdom_decision": {
        "kingdom": None,           # mints a CUSTOM kingdom — no specific de-jure parent
        "prestige_level_min": 3,   # high_prestige_level = 2000 prestige
        "needs_era_tribal": False, # era-INDEPENDENT salvage (Fallback A)
        "needs_duchies": 3,        # 3 held duchies OR sub_realm_size >= 30
        "gold_cost": 300, "piety_cost": 200, "prestige_cost": 500,
    },
}

SHIELD_DECISIONS = tuple(_SHIELD_DECISION_GATES.keys())


def _clause(key: str, label: str, ok: bool, *, requirement: str,
            measured=None, fix: str = "") -> dict:
    """One decision-gate clause verdict (pass/fail + the why)."""
    c = {"clause": key, "label": label, "pass": bool(ok), "requirement": requirement}
    if measured is not None:
        c["measured"] = measured
    if not ok and fix:
        c["fix"] = fix
    return c


def shield_decision_why_blocked(decision: str, *, realm: dict, character: dict,
                                duchy_control: dict | None = None) -> dict:
    """Compute per-clause pass/fail for a SHIELD decision from structural reads.

    Inputs are raw bridge query results (any may be {} / None — degrades safely):
      realm        — query_realm     (primary_title_tier, gold, titles[], etc.)
      character    — query_character  (prestige, prestige_level, government,
                                       culture_era, is_at_war, imprisoned, age)
      duchy_control— optional pre-computed {complete_duchies, candidate_duchies}
                     for the kingdom (each a list of {title_id,key,held,total}).
                     When absent the duchy clause is reported UNMEASURED (not
                     a false fail) so the agent knows to enumerate it.

    Returns {decision, gate, clauses[], blocking[], all_structural_pass, note}.
    NEVER raises — every read is defensive. This is a HINT surface; the engine's
    AI_TRY_DECISION_CORE remains the authority.
    """
    gate = _SHIELD_DECISION_GATES.get(decision)
    if gate is None:
        return {"decision": decision, "supported": False,
                "note": "no structural gate model — supported: "
                        + ", ".join(SHIELD_DECISIONS)}

    realm = realm if isinstance(realm, dict) else {}
    character = character if isinstance(character, dict) else {}
    # query_character may nest under "character".
    if "character" in character and isinstance(character["character"], dict):
        character = character["character"]

    def _num(d, *keys, default=0):
        for k in keys:
            v = d.get(k)
            if isinstance(v, (int, float)):
                return v
        return default

    clauses: list[dict] = []

    # G1/G-tier: highest held title must be <= DUKE (tier 3). A king/emperor is
    # ineligible (forming the kingdom is the TERMINAL act, not a precondition).
    prim_tier = _num(realm, "primary_title_tier", "tier", default=0)
    clauses.append(_clause(
        "highest_title_le_duke", "highest held title <= duke (tier 3)",
        prim_tier <= 3 and prim_tier > 0,
        requirement="primary_title_tier <= 3 (already a KING -> ineligible)",
        measured={"primary_title_tier": prim_tier},
        fix="you already hold a kingdom; this shield is done"))

    # Independent — top_liege == self. query_realm exposes is_independent / liege.
    indep = realm.get("is_independent")
    if indep is None:
        liege = _num(realm, "liege_id", "top_liege_id", default=0)
        indep = (liege == 0) if liege is not None else None
    clauses.append(_clause(
        "independent", "independent (top_liege = self)",
        bool(indep) if indep is not None else True,
        requirement="top_liege == self",
        measured={"is_independent": indep},
        fix="you are a vassal — only an independent ruler can form a kingdom"))

    # G7: at peace.
    at_war = bool(character.get("is_at_war") or realm.get("is_at_war"))
    clauses.append(_clause(
        "at_peace", "at peace (is_at_war = no)", not at_war,
        requirement="is_at_war == false",
        measured={"is_at_war": at_war},
        fix="go to peace first: steer DeclareWar off + end/white-peace active wars"))

    # not imprisoned + adult (showing-failure gates).
    imprisoned = bool(character.get("imprisoned") or character.get("is_imprisoned"))
    clauses.append(_clause(
        "not_imprisoned", "not imprisoned", not imprisoned,
        requirement="imprisoned == false",
        measured={"imprisoned": imprisoned}))

    # G4: prestige_level.
    plvl = _num(character, "prestige_level", default=None)
    prestige = _num(character, "prestige", default=None)
    need_plvl = gate["prestige_level_min"]
    plvl_ok = (plvl is None) or (plvl >= need_plvl)
    clauses.append(_clause(
        "prestige_level", f"prestige_level >= {need_plvl}",
        plvl_ok,
        requirement=f"prestige_level >= {need_plvl} "
                    f"({'1000' if need_plvl == 2 else '2000'} prestige)",
        measured={"prestige_level": plvl, "prestige": prestige},
        fix="bank prestige (GHA + create_title + conquests pump it); do not "
            "stress-break (a break dumps prestige)"))

    # G8: tribal era (one-way timer) — only for negotiate_the_danelaw.
    if gate["needs_era_tribal"]:
        gov = str(character.get("government") or "").lower()
        era = str(character.get("culture_era") or character.get("era") or "").lower()
        # Tribal gov keeps the culture in the tribal era; feudal/clan-advanced loses it.
        # Treat explicit early_medieval-or-later era OR non-tribal gov as a fail; an
        # unknown era + tribal gov is a PASS (gov is the durable signal).
        era_advanced = bool(era) and ("early_medieval" in era or "high_medieval" in era
                                      or "late_medieval" in era)
        tribal = ("tribal" in gov) if gov else None
        era_ok = (tribal is not False) and (not era_advanced)
        clauses.append(_clause(
            "tribal_era", "culture still in TRIBAL era (one-way timer)",
            era_ok,
            requirement="government == tribal AND culture era < early_medieval",
            measured={"government": gov or None, "culture_era": era or None},
            fix="the tribal-era timer is CLOSING/CLOSED — if feudal/era-advanced "
                "use found_kingdom_decision (Fallback A) instead"))

    # G5: 2 (or 3) completely-controlled k_england duchy TITLES.
    need_duchies = gate["needs_duchies"]
    if duchy_control is not None and isinstance(duchy_control, dict):
        complete = duchy_control.get("complete_duchies") or []
        candidates = duchy_control.get("candidate_duchies") or []
        n_complete = len(complete)
        duchy_ok = n_complete >= need_duchies
        clauses.append(_clause(
            "controlled_duchies",
            f"control {need_duchies} {gate['kingdom'] or 'held'} duchy titles "
            "(every de-jure county, vassal control counts)",
            duchy_ok,
            requirement=f"count >= {need_duchies} completely-controlled duchy "
                        f"titles de-jure-above = {gate['kingdom'] or '(any)'}",
            measured={"complete": n_complete, "need": need_duchies,
                      "complete_duchies": complete,
                      "incomplete_candidates": candidates},
            fix="THE usual blocker: complete one more duchy. A duchy you control by "
                "VASSAL satisfies this gate but to MINT its TITLE create_title needs "
                "a de-jure county in PERSONAL demesne. Re-conquer any lost duchy "
                "county FAST (a mid-build loss breaks this gate)."))
    else:
        clauses.append(_clause(
            "controlled_duchies",
            f"control {need_duchies} duchy titles (UNMEASURED)",
            True,
            requirement=f"count >= {need_duchies} completely-controlled duchy titles",
            measured={"need": need_duchies, "note": "duchy control not enumerated "
                      "in this call — pass duchy_control or read query(domain='titles') "
                      "+ per-duchy query(domain='de_jure')"}))

    blocking = [c["clause"] for c in clauses if not c["pass"]]
    all_pass = not blocking
    note = ("all STRUCTURAL clauses pass — if act(type=decision) still returns "
            "vtable_validation_failed, the unmeasured duchy-control clause or a "
            "negotiation rejection (british_isles.1011) is the cause; re-fire next "
            "peace window") if all_pass else (
            "fix the BLOCKING clause(s) before firing the decision; do not spam-retry")
    return {
        "decision": decision,
        "kingdom": gate.get("kingdom"),
        "gate": "negotiate_the_danelaw / found_kingdom shield gate (structural model)",
        "clauses": clauses,
        "blocking": blocking,
        "all_structural_pass": all_pass,
        "is_structural_hint": True,
        "note": note,
        "doc": "re/findings/re_shield_robustness.md; "
               "ck3agent/knowledge/evolving-lessons.md AL-11",
    }


def resolve_discover(params: dict) -> tuple[str, dict] | None:
    """Resolve a discover tool call to (bridge_action, bridge_params)."""
    what = params.get("what")
    if what == "interactions":
        return "list_interactions", {}
    elif what == "decisions":
        # LIGHT list (name+hash) — fast, works PAUSED. For validity/requirements use
        # "decisions_annotated" or "valid_decisions" (those run the engine eval on the
        # game thread and need the game UNPAUSED).
        return "list_decisions", {
            "character_id": params.get("character_id", params.get("actor_id", 0)),
        }
    elif what == "decisions_annotated":
        # ANNOTATED for SPECIFIC decisions: pass decisions=[key names and/or name-hashes]
        # (get the names from what="decisions"). Each returns shown/valid/cost_ok/available
        # + failure requirement loc-keys + cost + the character's current gold/piety/prestige
        # (have-vs-need). Bounded → fast + safe. Deferred game-thread eval — game UNPAUSED.
        # No decisions list → falls back to the light name+hash list. character_id optional.
        return "list_decisions_annotated", {
            "character_id": params.get("character_id", params.get("actor_id", 0)),
            "decisions": params.get("decisions", []),
        }
    elif what == "valid_decisions":
        # Same bounded annotated eval over the passed decisions=[...], returning ONLY the
        # available==true ones (with cost + current values). Deferred game-thread eval —
        # game UNPAUSED. No decisions list → light name+hash list.
        return "list_valid_decisions", {
            "character_id": params.get("character_id", params.get("actor_id", 0)),
            "decisions": params.get("decisions", []),
        }
    elif what == "acceptance":
        return "query_interaction_acceptance", {
            "interaction_type": params.get("interaction_type", ""),
            "target_id": params.get("target_id", 0),
            "actor_id": params.get("actor_id", 0),
        }
    elif what == "valid_cbs":
        return "query_valid_cbs", {
            "actor_id": params.get("actor_id", 0),
            "target_id": params.get("target_id", 0),
        }
    elif what == "valid_cbs_strict":
        # ck-5qpg: per-CB outer/inner verdict surface with reason strings.
        # Mirrors AI_CB_EVALUATE_ALL Stage 1 (bilateral) + Stage 2 (group flags)
        # outer filter; per-CB outer triggers (allowed_for_character /
        # allowed_against_character / valid_to_start) deferred to ck-39e1.
        return "query_cb_validity_strict", {
            "actor_id": params.get("actor_id", 0),
            "target_id": params.get("target_id", 0),
            "include_ineligible": params.get("include_ineligible", True),
            # audit B3: the strict CB worker needs the game thread (unpaused) and
            # ~6s for a full sweep (98 CBs); the old 2s default returned
            # cb_query_timeout. Do not call concurrently — single-flight worker
            # returns cb_query_busy on overlap.
            "timeout_ms": params.get("timeout_ms", 6000),
        }
    elif what == "trigger_result":
        return "query_trigger_result", {
            "interaction_type": params.get("interaction_type", ""),
            "trigger_slot": params.get("trigger_slot", -1),
            "actor_id": params.get("actor_id", 0),
            "target_id": params.get("target_id", 0),
        }
    elif what == "war_validity":
        return "pre_validate_war", {
            "actor_id": params.get("actor_id", 0),
            "target_id": params.get("target_id", 0),
        }
    elif what == "construction_check":
        # Engine can-build check (ADD_CONSTRUCTION_VALIDITY, no mutation): is
        # `building` constructible by `actor_id` at `location_id` (a barony/county
        # landed-title id) in `slot`? Returns {buildable, building_index}.
        return "construction_check", {
            "actor_id": params.get("actor_id", 0),
            "building": params.get("building", ""),
            "location_id": params.get("location_id", 0),
            "slot": params.get("slot", 0),
        }
    elif what == "fabricate_targets":
        # Which counties are eligible fabricate-claim targets for actor_id. The
        # bridge runs the task's data-driven target triggers per county AND the full
        # CanAssign (councillor-side) check, annotating each with `assignable`
        # (true ⇒ set_council_task WILL apply + stick) + an `assignable_count`.
        # Read-only. Feed an `assignable:true` title_id into
        # set_council_task(task='task_fabricate_claim', target_title_id=...).
        return "query_fabricate_targets", {
            "actor_id": params.get("actor_id", 0),
            "task": params.get("task") or "task_fabricate_claim",
            "max_results": params.get("max_results", 0) or 40,
        }
    elif what == "joinable_factions":
        # Which factions the actor can create/join against its liege (the
        # vassal-breaks-free lever). Returns candidate faction types + a hint;
        # attempt via act(type="faction"). Param: actor_id (or character_id).
        return "query_joinable_factions", {
            "character_id": params.get("actor_id", 0) or params.get("character_id", 0),
        }
    elif what == "hireable_mercenaries":
        # Enumerate hireable mercenary companies (company_id list). Feed a chosen
        # company_id into act(type="hire_mercenary", company_id=...).
        # lane-query item 2: rows carry home county, captain, troops,
        # regiments and (with an actor) distance; nearest first, capped.
        return "query_hireable_mercenaries", {
            "actor_id": params.get("actor_id", 0) or params.get("character_id", 0),
            "max_results": params.get("max_results", 0) or 25,
        }
    elif what == "war_targets":
        # ck-leg: enumerate every (target, title, CB) the actor can declare war
        # on right now — the "who can I attack and with what" surface. Each entry
        # carries cb_type, pressed (claim already pressed?), tier, title. Feed a
        # chosen entry straight into act(type="war", method="steered", ...).
        return "query_valid_war_targets", {
            "actor_id": params.get("actor_id", 0),
        }
    elif what == "imprisonment":
        # ck-leg: can the actor imprison / banish / execute the target, and on
        # what grounds (the reason modifiers + has_imprisonment_reason). The
        # legibility gate before act(type="interaction", imprison_interaction).
        return "query_imprisonment_reasons", {
            "actor_id": params.get("actor_id", 0),
            "target_id": params.get("target_id", 0),
        }
    elif what == "trigger_slots":
        return "query_trigger_slots", {
            "interaction_type": params.get("interaction_type", ""),
        }
    elif what == "evaluate_trigger":
        # ck-fo7x.4: evaluate a named trigger class against a scope dict.
        # `trigger` is a script keyword like 'is_alive' or a mangled class
        # name like '27CScopedBoolConditionTriggerI17CIsAliveConditionE'.
        # `scope` is a flat dict; today only `character` (and optionally
        # `target`) are wired through to the bridge — see ADR-0001 §ck-fo7x.4.
        scope = params.get("scope") or {}
        bridge_params = {
            "trigger": params.get("trigger") or params.get("trigger_name", ""),
        }
        # Accept either nested scope.character or flat character_id / actor_id.
        character_id = (
            scope.get("character")
            or scope.get("character_id")
            or params.get("character_id")
            or params.get("actor_id")
            or 0
        )
        if character_id:
            bridge_params["character_id"] = int(character_id)
        target_id = (
            scope.get("target")
            or scope.get("target_id")
            or params.get("target_id")
            or 0
        )
        if target_id:
            bridge_params["target_id"] = int(target_id)
        return "evaluate_named_trigger", bridge_params
    elif what == "interaction_validity":
        # ck-fo7x.5: evaluate the full 6-gate cascade for an interaction
        # by name. For declare_war we delegate to query_cb_validity_strict
        # (the 6-gate CB pipeline — ck-5qpg). For other interactions we
        # delegate to query_trigger_result (slot=-1 ⇒ all hard-gate slots).
        # The bridge response is post-processed by
        # shape_interaction_validity_response into a unified
        # {valid, reason, ...} verdict; see ADR-0001 §ck-fo7x.5.
        # ADR-0001 row 5 + commit ab00e35 refer to this as
        # `query_interaction_validity`; shipped as `interaction_validity`
        # to match the existing what= naming (valid_cbs, war_validity,
        # trigger_result). Tracked in ck-zwzba.
        interaction = (
            params.get("interaction")
            or params.get("interaction_type")
            or "declare_war"
        )
        actor_id = int(params.get("actor_id", 0) or 0)
        target_id = int(params.get("target_id", 0) or 0)
        if interaction == "declare_war":
            return "query_cb_validity_strict", {
                "actor_id": actor_id,
                "target_id": target_id,
                "include_ineligible": params.get("include_ineligible", True),
                "timeout_ms": int(params.get("timeout_ms", 6000) or 6000),  # audit B3
            }
        # Other interactions: trigger-slot cascade. Bridge expects the
        # *_interaction suffix on the slot-table key.
        itype = (
            interaction
            if interaction.endswith("_interaction")
            else interaction + "_interaction"
        )
        return "query_trigger_result", {
            "interaction_type": itype,
            "actor_id": actor_id,
            "target_id": target_id,
            "trigger_slot": -1,
        }
    return None


# Hard-gate trigger slots for query_trigger_result — these MUST pass for the
# interaction to be sendable (per re_interaction_slot_layout_corrected.md and
# the bridge slot table in do_query_trigger_result, ck3agent/server/agent.py).
# Slot 0 (is_valid), 1 (is_shown), 4 (can_send) are the hard gates the engine
# enforces; auto_accept (5) and on_auto_accept (6) are effect-side, not
# pre-send gates.
_INTERACTION_HARD_GATE_SLOTS = (0, 1, 4)


def shape_interaction_validity_response(params: dict, response):
    """Reshape the bridge response for what=interaction_validity into a
    unified verdict.

    Args:
        params: original discover params (must include `what` and `interaction`).
        response: the bridge JSON response (already unwrapped to a dict).

    Returns:
        A dict with at minimum a boolean `valid` field, plus diagnostic fields
        (`reason`, `gates_evaluated`, `gates_deferred`, etc.). For unknown
        interactions or invalid responses, returns valid=False with a reason.
    """
    interaction = (
        params.get("interaction")
        or params.get("interaction_type")
        or "declare_war"
    )

    if not isinstance(response, dict):
        return {
            "valid": False,
            "interaction": interaction,
            "reason": "invalid_bridge_response",
            "raw": response,
        }

    # Bridge errors bubble up here as either status=="error"/"timeout" or
    # an explicit error field. Surface them with valid=false.
    err = response.get("error")
    status = response.get("status")
    if err or status in ("error", "timeout", "busy", "bilateral_filter_reject"):
        return {
            "valid": False,
            "interaction": interaction,
            "reason": err or status or "bridge_error",
            "bridge_status": status,
            "raw": response,
        }

    if interaction == "declare_war":
        return _shape_declare_war_validity(params, response)
    return _shape_trigger_result_validity(params, response)


def _shape_declare_war_validity(params: dict, response: dict) -> dict:
    """Extract the verdict for the requested (cb, title) pair from the
    query_cb_validity_strict response. If no `cb` is supplied, return the
    aggregate "any CB valid?" verdict.
    """
    cb_filter = params.get("cb") or params.get("cb_name") or ""
    title = params.get("title") or params.get("cb_title") or None
    results = response.get("results") or []
    summary = response.get("summary") or {}

    if cb_filter:
        match = next(
            (r for r in results if r.get("cb_name") == cb_filter),
            None,
        )
        if match is None:
            return {
                "valid": False,
                "interaction": "declare_war",
                "cb": cb_filter,
                "title": title,
                "reason": "cb_not_in_db",
                "gates_evaluated": response.get("gates_evaluated", []),
                "gates_deferred": response.get("gates_deferred", []),
                "summary": summary,
            }
        outer = match.get("outer_status") or ""
        inner = match.get("inner_status") or ""
        valid = (outer == "pass" and inner == "pass")
        reason = None
        if not valid:
            if outer != "pass":
                reason = f"outer:{match.get('outer_reason') or outer or 'fail'}"
            else:
                reason = f"inner:{inner or 'fail'}"
        return {
            "valid": valid,
            "interaction": "declare_war",
            "cb": cb_filter,
            "cb_index": match.get("cb_index"),
            "target_mode": match.get("target_mode"),
            "title": title,
            "outer_status": outer,
            "inner_status": inner,
            "outer_reason": match.get("outer_reason"),
            "reason": reason,
            "gates_evaluated": response.get("gates_evaluated", []),
            "gates_deferred": response.get("gates_deferred", []),
            "summary": summary,
        }

    # No CB filter — return aggregate verdict from summary.
    inner_valid = int(summary.get("inner_valid", 0) or 0)
    valid_results = [
        r for r in results
        if r.get("outer_status") == "pass" and r.get("inner_status") == "pass"
    ]
    return {
        "valid": inner_valid > 0 or bool(valid_results),
        "interaction": "declare_war",
        "title": title,
        "valid_cb_count": inner_valid,
        "valid_cbs": [r.get("cb_name") for r in valid_results],
        "gates_evaluated": response.get("gates_evaluated", []),
        "gates_deferred": response.get("gates_deferred", []),
        "summary": summary,
    }


def _shape_trigger_result_validity(params: dict, response: dict) -> dict:
    """Aggregate slot-level verdicts from query_trigger_result into a single
    boolean. Only hard-gate slots (is_valid, is_shown, can_send) influence
    the verdict; auto_accept / on_auto_accept are effect-side and ignored.
    """
    interaction = (
        params.get("interaction")
        or params.get("interaction_type")
        or "interaction"
    )
    if interaction.endswith("_interaction"):
        interaction = interaction[: -len("_interaction")]
    results = response.get("results") or []

    failing = []
    for entry in results:
        if not entry.get("present", False):
            continue  # absent slot does not gate
        slot = entry.get("slot")
        if slot in _INTERACTION_HARD_GATE_SLOTS and not entry.get("result", True):
            failing.append(entry.get("name") or f"slot{slot}")

    valid = len(failing) == 0
    return {
        "valid": valid,
        "interaction": interaction,
        "failing_slots": failing,
        "slot_results": results,
        "reason": None if valid else "failing_slots:" + ",".join(failing),
    }


# ---------------------------------------------------------------------------
# Legibility capstone: discover(what="targets", action=...) — "who are the
# valid targets for action X right now". Returns a *plan* the async layer
# executes, because some actions need a candidate pool + per-target annotation
# (multiple bridge round-trips) while others resolve in a single call.
# ---------------------------------------------------------------------------

# Candidate pools the bridge can actually enumerate today. Foreign strangers
# (the marriage/alliance market) are NOT enumerable until
# query_marriage_observations is fixed (ck-leg / Task #28) — `targets` is
# honest about that and returns pool="unavailable" with a reason.
_TARGETS_ACTION_ALIASES = {
    "war": "war", "declare_war": "war",
    "imprison": "imprison", "imprison_interaction": "imprison",
    "execute": "imprison", "execute_prisoner_interaction": "imprison",
    "banish": "imprison",
    "revoke_title": "vassals", "revoke_title_interaction": "vassals",
    "claim": "claimants", "back_claimant": "claimants",
    "press_claim": "claimants",
}


def resolve_targets_plan(action: str, params: dict) -> dict:
    """Plan how discover(what='targets', action=...) enumerates valid targets.

    Returns one of:
      {"mode":"single", "bridge_action":.., "params":..}
         single call whose response is already a per-target list.
      {"mode":"pool_annotate", "pool":{..}, "annotate":{..}, "label":..}
         enumerate a candidate pool, then annotate each entry via a per-target
         bridge call. The async executor in mcp_server runs this.
      {"mode":"error", "reason":..}  unsupported / missing required arg.
    """
    actor_id = params.get("actor_id", 0) or params.get("character_id", 0) or 0
    kind = _TARGETS_ACTION_ALIASES.get((action or "").lower())

    if kind == "war":
        # query_valid_war_targets already returns per-(target,title,CB) verdicts.
        return {
            "mode": "single",
            "bridge_action": "query_valid_war_targets",
            "params": {"actor_id": actor_id},
            "list_key": "targets",
        }
    if kind == "claimants":
        title_id = params.get("title_id", 0)
        if not title_id:
            return {"mode": "error",
                    "reason": "targets(action='claim') requires title_id"}
        return {
            "mode": "single",
            "bridge_action": "query_title_claimants",
            # Forward the actor so do_query_title_claimants emits the
            # attacker-scoped honesty fields (in_attacker_court,
            # backable_by_attacker, not_backable_reason). atk_id==0 (no actor)
            # falls back to the legacy global roster.
            "params": {"title_id": title_id, **({"actor_id": actor_id} if actor_id else {})},
            "list_key": "claimants",
        }
    if kind == "imprison":
        # Candidate pool = my vassals (the enumerable set I have leverage over);
        # annotate each with imprison/banish/execute grounds.
        return {
            "mode": "pool_annotate",
            "label": "imprison",
            "pool": {"bridge_action": "query_realm",
                     "params": {"character_id": actor_id},
                     "list_key": "vassals", "id_key": "id", "name_key": "name"},
            "annotate": {"bridge_action": "query_imprisonment_reasons",
                         "params_keys": {"actor_id": actor_id},
                         "target_key": "target_id",
                         "keep": ["has_imprisonment_reason", "reason_count",
                                  "reasons", "actor_is_liege"]},
        }
    if kind == "vassals":
        # Bare vassal roster (revoke-title etc.); no per-target validity handler
        # wired yet, so just the enumerable pool.
        return {
            "mode": "pool_annotate",
            "label": "vassals",
            "pool": {"bridge_action": "query_realm",
                     "params": {"character_id": actor_id},
                     "list_key": "vassals", "id_key": "id", "name_key": "name"},
            "annotate": None,
        }
    return {
        "mode": "error",
        "reason": (f"targets(action='{action}') not enumerable. Supported: "
                   "war, imprison/execute/banish, revoke_title, claim. "
                   "Marriage/alliance/scheme targets need the marriage market "
                   "(query_marriage_observations) which is not yet working."),
    }


def shape_discover_response(params: dict, response):
    """Dispatch optional response shaping for discover() outputs. Returns
    the original response unchanged unless the `what` value has a registered
    shaper (currently only `interaction_validity`)."""
    what = (params or {}).get("what")
    if what == "interaction_validity":
        return shape_interaction_validity_response(params, response)
    return response


def resolve_intent(params: dict) -> tuple[str, dict] | None:
    """Resolve an intent tool call to (bridge_action, bridge_params)."""
    action = params.get("action")
    if action == "queue":
        return "queue_intent", {
            "interaction_type": params.get("interaction_type", ""),
            "actor_id": params.get("actor_id", 0),
            "target_id": params.get("target_id", 0),
            "max_age_ticks": params.get("max_age_ticks", 500),
        }
    elif action == "list":
        return "query_intents", {}
    elif action == "results":
        return "query_intent_results", {
            "max_count": params.get("max_count", 16),
        }
    elif action == "cancel":
        return "cancel_intent", {"request_id": params.get("request_id", "")}
    elif action == "clear":
        return "clear_intents", {}
    return None


def resolve_game(params: dict) -> tuple[str, dict] | None:
    """Resolve a game tool call to (bridge_action, bridge_params)."""
    action = params.get("action")
    if action == "pause":
        return "pause_game", {}
    elif action == "unpause":
        return "unpause_game", {}
    elif action == "speed":
        return "set_speed", {"speed": params.get("speed", 3)}
    elif action == "auto_events":
        return "set_auto_respond_events", {"enabled": params.get("enabled", True)}
    return None


def resolve_control(params: dict) -> tuple[str, dict] | None:
    """Resolve a control tool call to (bridge_action, bridge_params)."""
    action = params.get("action")
    if action == "select":
        return "set_controlled_character", {
            "character_id": params.get("character_id", 0),
            "disable_native_ai": params.get("disable_native_ai", False),
        }
    elif action == "release":
        return "release_character", {
            "character_id": params.get("character_id", 0),
        }
    return None


# ---------------------------------------------------------------------------
# Tool definitions for the Anthropic API (agent.py uses these directly)
# ---------------------------------------------------------------------------

# Phase-2 migration (_PHASE2_PARITY.md): legacy tools with FULL v2 parity, RETIRED from
# the advertised surface. act → do/declare_war; game/control/bridge → session. Their
# defs remain below for reference but are filtered out of the registered surface so the
# DOCUMENTED surface == the REGISTERED surface (prompts.py drops them too). The PORT-FIRST
# / KEEP legacy tools (observe, query, discover, steer, intent, execute_console_command,
# pass_turn) stay registered — retiring them would silently drop read domains.
#: ck-cjql / AMENDMENT 6. `execute_console_command` joins the four Phase-2
#: retirements. It was deregistered from the MCP surface by the legitimacy
#: audit (its @mcp.tool() is commented out) but stayed in this list's
#: complement, so get_tool_definitions kept ADVERTISING a verb no seat in any
#: arm could call: 16 advertised against 15 registered in arms a/c/e0, 18
#: against 17 in arm b, the difference being this name in every arm.
#:
#: THIS LINE IS ONE ACT WITH THE AMENDMENT and must not be released without
#: it. §5 of the pre-registration declared the advertised name as a
#: held-constant surface member, and the launch witness reads THIS function —
#: so removing the name here without removing it from the fence makes observed
#: disagree with declared and refuses every registered launch with
#: R_TOOL_SURFACE_MISMATCH.
_RETIRED_LEGACY_TOOLS = frozenset({"act", "game", "control", "bridge",
                                   "execute_console_command"})


#: ck-ycim: THE DISCOVER VOCABULARY, AND IT IS NOT THE INTENT VOCABULARY.
#:
#: ONE SOURCE, here rather than in redesign.py, because redesign imports THIS
#: module and not the reverse — so this is the only place both the schema an
#: agent reads and the runtime that validates against it can share.
#:
#: They had drifted. The schema's enum carried NINE of these and the runtime
#: accepted TEN: `formable_titles` was enumerable and no agent could learn it
#: existed. That is ck-ycim's defect with the sign reversed — the surface had a
#: capability its description withheld — and it is the same fault as an agent
#: calling a name the surface lacks: a list restated in two places.
#:
#: AND THE TWO VOCABULARIES OVERLAP IN AN AGENT'S MIND, WHICH IS THE OTHER HALF.
#: `men_at_arms` is a legitimate INTENT and has never been a discover class, and
#: the census caught an agent asking to enumerate it five times — a reasonable
#: guess, since the intent list contains it and this list contains plural nouns
#: like `buildings` and `mercenaries`. There was nothing to enumerate: no bridge
#: verb listed men-at-arms types. lane-query added one (`query_maa_types`), so
#: `men_at_arms` is now BOTH an intent and a class — the one name in both lists,
#: on purpose: the intent's refusal (maa_type_required) points at this class.
ACTION_CLASSES = [
    "interactions", "decisions", "council_tasks", "laws",
    "fabricate_targets", "mercenaries", "factions_to_join",
    "buildings", "imprison_targets", "formable_titles",
    # lane-query item 2: do(intent="men_at_arms") refused maa_type_required and
    # pointed at an enumerate that did not exist. This is it.
    "men_at_arms",
]


def get_tool_definitions() -> list[dict]:
    """Return Anthropic-format tool definitions for the consolidated tools — the legacy
    survivors + the redesigned 8-tool surface, minus the RETIRED full-parity tools."""
    _legacy = [
        # 1. observe
        {
            "name": "observe",
            "description": (
                "Get a combined snapshot of the current game state for decision-making. "
                "Returns the controlled character's full profile (stats, traits, personality "
                "weights, resources), current game date and speed, and a strategic context "
                "summary (military strength, active wars with scores, top rulers). Call this "
                "once at the start of every decision cycle — it replaces calling "
                "get_my_character, get_tick_info, get_ai_personality, and get_ai_evaluations "
                "separately."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "include": {
                        "type": "array",
                        "items": {
                            "type": "string",
                            "enum": ["character", "tick", "personality", "evaluations", "subsystems"],
                        },
                        "description": (
                            "Which sections to include. Omit for all defaults "
                            "(character + tick + personality + evaluations). "
                            "Use selective includes to reduce response size."
                        ),
                    },
                    "character_id": {
                        "type": "integer",
                        "description": "Character to observe (0 = controlled character).",
                        "default": 0,
                    },
                },
                "required": [],
            },
        },
        # 2. query
        {
            "name": "query",
            "description": (
                "Deep-dive query for a specific game domain. Use after observe() reveals "
                "something that needs investigation — wars to analyze, factions to assess, "
                "territory to plan around, or characters to evaluate. Each domain returns "
                "structured data specific to that system. Supports batch queries via "
                "'domains' (max 5) with partial failure and per-domain errors."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "domain": {
                        "type": "string",
                        "enum": QUERY_DOMAINS,
                        "description": "Which game domain to query (single).",
                    },
                    "domains": {
                        "type": "array",
                        "items": {"type": "string", "enum": QUERY_DOMAINS},
                        "description": "Batch query: up to 5 domains at once. Mutually exclusive with domain.",
                        "maxItems": 5,
                    },
                    "character_id": {
                        "type": "integer",
                        "description": "Character context (0 = controlled). Used by most domains.",
                        "default": 0,
                    },
                    "target_id": {
                        "type": "integer",
                        "description": "Secondary character for opinion, valid_cbs, diplomatic_range.",
                        "default": 0,
                    },
                    "title_id": {
                        "type": "integer",
                        "description": "Title context for titles, de_jure, election, neighbors, adjacency, development.",
                        "default": 0,
                    },
                    "trait_id": {
                        "type": "integer",
                        "description": "Trait ID for trait_compatibility domain.",
                        "default": 0,
                    },
                    "category": {
                        "type": "integer",
                        "description": "Building category filter for buildings domain (1095=constructible, 1825=special, 0=all).",
                        "default": 0,
                    },
                    "include_vassals": {
                        "type": "boolean",
                        "description": "Include vassal claims in claims domain.",
                        "default": False,
                    },
                    "war_id": {
                        "type": "integer",
                        "description": "Specific war for war_score domain (0 = all cached).",
                        "default": 0,
                    },
                    "limit": {
                        "type": "integer",
                        "description": "Max results for war_declarations and scheme_inits domains.",
                        "default": 64,
                    },
                    "since_tick": {
                        "type": "integer",
                        "description": "Only return records after this tick for war_declarations and scheme_inits.",
                        "default": 0,
                    },
                    "scheme_type": {
                        "type": "string",
                        "description": "Filter scheme_inits by type (e.g. 'murder', 'fabricate_hook').",
                    },
                    "culture_id": {
                        "type": "integer",
                        "description": "Culture ID for innovation_progress, fascination_progress, research_speed (0 = character's culture).",
                        "default": 0,
                    },
                    "dynasty_id": {
                        "type": "integer",
                        "description": "Dynasty ID for dynasty domain (0 = character's dynasty).",
                        "default": 0,
                    },
                },
                "required": [],
            },
        },
        # 3. discover
        {
            "name": "discover",
            "description": (
                "Pre-action validation and enumeration. List all registered interaction or "
                "decision types, check if a target would accept an interaction, or validate "
                "which casus belli are usable against a defender. Use before act() to verify "
                "feasibility. NOT for data reads — use query() for that."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "what": {
                        "type": "string",
                        "enum": ["interactions", "decisions", "decisions_annotated", "valid_decisions", "decision_why_blocked", "acceptance", "valid_cbs", "valid_cbs_strict", "trigger_result", "war_validity", "trigger_slots", "evaluate_trigger", "interaction_validity", "war_targets", "imprisonment", "targets", "construction_check", "fabricate_targets", "hireable_mercenaries", "joinable_factions"],
                        "description": (
                            "What to discover. trigger_result evaluates individual script "
                            "trigger slots on an interaction to find which one rejects. "
                            "war_validity checks all PDXScript preconditions (A1-A13, R1-R4) "
                            "for declaring war on a target — returns pass/fail per condition. "
                            "trigger_slots inspects trigger slot structure (RTTI class names, "
                            "children count, labels) without evaluation — IPC-thread safe. "
                            "valid_cbs_strict returns per-CB outer/inner verdicts with reason "
                            "strings (Stage 1+2 mirror of AI_CB_EVALUATE_ALL outer filter). "
                            "evaluate_trigger runs a named trigger class (e.g. 'is_alive') "
                            "against a scope dict {character: id, target: id} and returns "
                            "the boolean result + resolved class info (ck-fo7x.4). "
                            "interaction_validity evaluates the full 6-gate cascade for any "
                            "interaction by name and returns a unified {valid, reason, ...} "
                            "verdict. For declare_war the bridge runs query_cb_validity_strict; "
                            "pass cb=<name> (and optionally title=<title>) to scope the verdict "
                            "to one CB. For other interactions the bridge runs "
                            "query_trigger_result against all hard-gate slots (ck-fo7x.5). "
                            "fabricate_targets enumerates counties that pass the council "
                            "task's real eligibility triggers for actor_id — the eligible "
                            "fabricate-claim targets right now (read-only). "
                            "decisions enumerates ALL decisions, each annotated with "
                            "shown/valid/cost_ok/available + a failures[] list of the unmet "
                            "requirement loc-keys + the decision's cost{} and the character's "
                            "current{gold,piety,prestige} (have-vs-need). valid_decisions runs "
                            "the same evaluation but returns ONLY available==true decisions. "
                            "Both take an optional character_id (default: controlled/player). "
                            "decision_why_blocked is the SAFE per-clause why-blocked for the "
                            "SHIELD decisions (decision_type=negotiate_the_danelaw_decision or "
                            "found_kingdom_decision): it derives each is_valid gate (highest "
                            "title<=duke, independent, at-peace, prestige_level, tribal-era, and "
                            "the 2-completely-controlled-k_england-duchies clause) from CHEAP "
                            "STRUCTURAL READS — NOT the hanging engine eval — so it works PAUSED "
                            "and tells you WHICH clause to fix. Returns clauses[] + blocking[] + "
                            "all_structural_pass."
                        ),
                    },
                    "decision_type": {
                        "type": "string",
                        "description": (
                            "For what=decision_why_blocked: the shield decision to decompose "
                            "(negotiate_the_danelaw_decision | found_kingdom_decision)."
                        ),
                    },
                    "task": {
                        "type": "string",
                        "description": (
                            "For what=fabricate_targets: council task id to test "
                            "(default 'task_fabricate_claim')."
                        ),
                    },
                    "max_results": {
                        "type": "integer",
                        "description": "For what=fabricate_targets: cap on eligible counties returned (default 40).",
                        "default": 40,
                    },
                    "interaction_type": {
                        "type": "string",
                        "description": "For what=acceptance/trigger_result: the interaction to check.",
                    },
                    "trigger_slot": {
                        "type": ["integer", "string"],
                        "description": (
                            "For what=trigger_result: which trigger slot to evaluate. "
                            "-1 or 'all' evaluates all 7 slots. 0-6 or name "
                            "(is_valid, is_shown, is_valid_showing_failures_only, "
                            "can_be_sent_despite_rejection, can_send, auto_accept, "
                            "on_auto_accept) for a specific slot."
                        ),
                        "default": -1,
                    },
                    "target_id": {
                        "type": "integer",
                        "description": "Target character for acceptance/valid_cbs/trigger_result checks.",
                    },
                    "actor_id": {
                        "type": "integer",
                        "description": "Acting character (0 = controlled).",
                        "default": 0,
                    },
                    "character_id": {
                        "type": "integer",
                        "description": "For what=decisions/valid_decisions: character to evaluate decisions for (0 = controlled/player).",
                        "default": 0,
                    },
                    "trigger": {
                        "type": "string",
                        "description": (
                            "For what=evaluate_trigger: trigger to run. Either a script "
                            "keyword like 'is_alive' / 'is_at_war' / 'has_raised_armies' "
                            "(see re/findings/re_trigger_subclass_catalogue.md for the "
                            "full alias table), or a raw mangled class name from "
                            "re/rtti/trigger_subclasses.csv for full coverage."
                        ),
                    },
                    "scope": {
                        "type": "object",
                        "description": (
                            "For what=evaluate_trigger: scope dict. Today only "
                            "{character: id} (and optionally {target: id}) is wired "
                            "through to the bridge. Maps onto an interaction-context "
                            "actor/target — adequate for character-scope triggers."
                        ),
                        "additionalProperties": True,
                    },
                    "interaction": {
                        "type": "string",
                        "description": (
                            "For what=interaction_validity: the interaction to check "
                            "(e.g. 'declare_war', 'negotiate_alliance', 'send_gift'). "
                            "Use the script name without the '_interaction' suffix; "
                            "the wrapper appends it where the bridge expects it."
                        ),
                    },
                    "cb": {
                        "type": "string",
                        "description": (
                            "For what=interaction_validity + interaction='declare_war': "
                            "scope the verdict to a specific CB by name (e.g. 'claim_cb', "
                            "'de_jure_cb', 'individual_county_de_jure_cb'). NOTE: "
                            "'de_jure_county_cb' does NOT exist in this build. Omit to get "
                            "the aggregate 'any CB valid?' verdict from the strict pipeline."
                        ),
                    },
                    "title": {
                        "type": "string",
                        "description": (
                            "For what=interaction_validity + interaction='declare_war': "
                            "the title that owns the claim or is being targeted (e.g. "
                            "'d_sistan'). Informational only — the strict pipeline does "
                            "not run per-title inner validation today; pass via "
                            "act(type='war', cb_title_id=...) when dispatching."
                        ),
                    },
                },
                "required": ["what"],
            },
        },
        # 4. act
        {
            "name": "act",
            "description": (
                "Execute a game action. Covers interactions (marriage, alliance, gift, scheme "
                "— 430+ types), war declarations, decisions, event responses, and law changes. "
                "The 'type' parameter selects the action category. Before unfamiliar actions, "
                "call discover() to verify exact names. "
                "For type=war: ALWAYS pass cb_title_id (from query_claims) for claim/de_jure CBs."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "type": {
                        "type": "string",
                        "enum": ["interaction", "war", "decision", "event", "law",
                                 "council_task", "building", "lifestyle", "faction", "scheme", "move_capital", "contract", "appoint", "maa", "hire_mercenary", "raise_armies", "raid", "create_title",
                                 "break_alliance", "decline_call"],
                        "description": "Action category. council_task = assign a council "
                                       "task (e.g. task_fabricate_claim via the Court "
                                       "Chaplain, task_increase_control via the Marshal); "
                                       "building = construct in a holding; lifestyle = set "
                                       "focus/perk. NOTE: break_alliance is NOT on this legacy "
                                       "act() path — the break lever now exists in the bridge; "
                                       "use the redesign surface do(intent='break_alliance', "
                                       "args={'target_id': <ally>}) (default-OFF + observer-save "
                                       "gated). decline_call is a genuine engine gap (no "
                                       "recipient-fireable decline) — mitigate by break_alliance "
                                       "+ never contracting a liability alliance.",
                    },
                    "interaction_type": {
                        "type": "string",
                        "description": "For type=interaction: exact interaction name.",
                    },
                    "decision_type": {
                        "type": "string",
                        "description": "For type=decision: exact decision name.",
                    },
                    "target_id": {
                        "type": "integer",
                        "description": "Target character for interactions and war.",
                        "default": 0,
                    },
                    "actor_id": {
                        "type": "integer",
                        "description": "Acting character (0 = controlled).",
                        "default": 0,
                    },
                    "title_id": {
                        "type": "integer",
                        "description": "Title for grant/revoke interactions.",
                        "default": 0,
                    },
                    "secondary_actor_id": {
                        "type": "integer",
                        "description": "For type=interaction: secondary actor.",
                        "default": 0,
                    },
                    "secondary_recipient_id": {
                        "type": "integer",
                        "description": "For type=interaction: secondary recipient.",
                        "default": 0,
                    },
                    "cb_index": {
                        "type": "integer",
                        "description": (
                            "For type=war: CB index (-1 = auto-select best). "
                            "Get valid indices from discover(what='valid_cbs_strict'). "
                            "Use 11=claim_cb for pressing claims (NOT 4=populist_war)."
                        ),
                        "default": -1,
                    },
                    "cb_title_id": {
                        "type": "integer",
                        "description": (
                            "For type=war: title to claim (REQUIRED for claim_cb/de_jure_cb — "
                            "get from query_claims title_id field). Without this, wars silently fail."
                        ),
                        "default": 0,
                    },
                    "claimant_id": {
                        "type": "integer",
                        "description": (
                            "For type=war: claimant character (0 = self). "
                            "Required when pressing another character's claim. "
                            "For method='steered' this defaults to the actor."
                        ),
                        "default": 0,
                    },
                    "method": {
                        "type": "string",
                        "enum": ["ai", "steered"],
                        "description": (
                            "For type=war: 'ai' (default) routes through the AI war "
                            "evaluator (target/CB are hints; the AI may decline). "
                            "'steered' deterministically declares EXACTLY the chosen "
                            "(target, casus_belli, cb_title_id, claimant_id) via "
                            "WAR_CREATE_CORE direct — named, no crash. Use 'steered' "
                            "to force a specific war the AI keeps declining. Requires "
                            "casus_belli (CB name). NOTE: claim_cb needs a PRESSED "
                            "claim or the engine culls the war."
                        ),
                    },
                    "casus_belli": {
                        "type": "string",
                        "description": (
                            "For type=war: CB script name (e.g. 'claim_cb', "
                            "'county_conquest_cb'). REQUIRED for method='steered' and "
                            "for force_player_path. Get valid names from "
                            "discover(what='valid_cbs_strict')."
                        ),
                    },
                    "force_player_path": {
                        "type": "boolean",
                        "description": (
                            "For type=war: bypass the AI evaluator and route through "
                            "WAR_CREATE_CORE_DIRECT (requires casus_belli). Prefer "
                            "method='steered' for the supported deterministic path."
                        ),
                        "default": False,
                    },
                    "skip_pre_validate": {
                        "type": "boolean",
                        "description": (
                            "For type=war: skip inline pre-validation checks (default false). "
                            "Pre-validation catches definitive blockers (imprisoned, incapacitated, "
                            "already at war, unlanded target) before the expensive intent dispatch. "
                            "Set true only if you already called discover(what='war_validity')."
                        ),
                        "default": False,
                    },
                    "law_name": {
                        "type": "string",
                        "description": "For type=law: target law name (e.g. 'crown_authority_2').",
                    },
                    "option": {
                        "type": "integer",
                        "description": "For type=event: option index (0-based).",
                        "default": 0,
                    },
                    "event_id": {
                        "type": "string",
                        "description": "For type=event: event ID string.",
                        "default": "",
                    },
                    "instance_id": {
                        "type": "integer",
                        "description": "For type=event: event instance ID.",
                        "default": 0,
                    },
                },
                "required": ["type"],
            },
        },
        # 5. steer
        {
            "name": "steer",
            "description": (
                "Read or adjust the AI steering controls: personality weights and subsystem "
                "toggles. This is the core mechanism — shape the native AI's behavior by "
                "tuning 10 personality axes (-100 to +100) and enabling/disabling 38 decision "
                "subsystems. Partial updates supported. No args = read current values."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "character_id": {
                        "type": "integer",
                        "description": "Character to steer (0 = controlled).",
                        "default": 0,
                    },
                    "personality": {
                        "type": "object",
                        "description": (
                            "Personality axis adjustments. Keys: honor, greed, rationality, "
                            "energy, boldness, zeal, vengefulness, compassion, sociability, "
                            "dominance. Values: -100 to +100."
                        ),
                        "additionalProperties": {"type": "integer"},
                    },
                    "subsystems": {
                        "type": "object",
                        "description": (
                            "Subsystem toggle overrides. Keys: Decisions, GrantRevokeTitles, "
                            "DeclareWar, Marriages, Council, etc. Values: true/false."
                        ),
                        "additionalProperties": {"type": "boolean"},
                    },
                },
                "required": [],
            },
        },
        # 6. intent
        {
            "name": "intent",
            "description": (
                "Manage the intent queue — scheduled actions that execute when the AI "
                "evaluation system naturally processes the character. Queue interactions, "
                "list pending intents, cancel by ID, or clear all."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "action": {
                        "type": "string",
                        "enum": ["queue", "list", "cancel", "clear"],
                        "description": "Intent queue operation.",
                    },
                    "interaction_type": {
                        "type": "string",
                        "description": "For action=queue: interaction to schedule.",
                    },
                    "actor_id": {
                        "type": "integer",
                        "description": "For action=queue: acting character (0 = controlled).",
                        "default": 0,
                    },
                    "target_id": {
                        "type": "integer",
                        "description": "For action=queue: target character (0 = AI chooses).",
                        "default": 0,
                    },
                    "max_age_ticks": {
                        "type": "integer",
                        "description": "For action=queue: expire after N ticks.",
                        "default": 500,
                    },
                    "request_id": {
                        "type": "string",
                        "description": "For action=cancel: the request_id to cancel.",
                    },
                },
                "required": ["action"],
            },
        },
        # 7. game
        {
            "name": "game",
            "description": (
                "Control game flow: pause, unpause, set speed, or toggle automatic event "
                "responses. Pause before steering/action changes, unpause to advance time. "
                "Speed 1-5 (lower for crises, higher for peacetime)."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "action": {
                        "type": "string",
                        "enum": ["pause", "unpause", "speed", "auto_events"],
                        "description": "Game flow operation.",
                    },
                    "speed": {
                        "type": "integer",
                        "description": "For action=speed: game speed 1-5.",
                        "default": 3,
                    },
                    "enabled": {
                        "type": "boolean",
                        "description": "For action=auto_events: true = while no session is open the bridge clears the event pause and picks no option.",
                        "default": True,
                    },
                },
                "required": ["action"],
            },
        },
        # 8. control
        {
            "name": "control",
            "description": (
                "Select which AI character the LLM steers, or release one back to native AI. "
                "By default you control the human player's character. Use for multi-character "
                "scenarios or testing."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "action": {
                        "type": "string",
                        "enum": ["select", "release"],
                        "description": "Select a character to control, or release one.",
                    },
                    "character_id": {
                        "type": "integer",
                        "description": "Character to select/release. For select: 0 reverts to human player.",
                    },
                    "disable_native_ai": {
                        "type": "boolean",
                        "description": "For action=select: fully disable native AI.",
                        "default": False,
                    },
                },
                "required": ["action", "character_id"],
            },
        },
        # 9. bridge
        {
            "name": "bridge",
            "description": (
                "Check bridge connection health and diagnostics: session info, controlled "
                "characters, personality locks, intent queue depth, tick count. "
                "Use when troubleshooting connection issues."
            ),
            "input_schema": {
                "type": "object",
                "properties": {},
                "required": [],
            },
        },
        # 10. execute_console_command
        {
            "name": "execute_console_command",
            "description": (
                "Execute a CK3 debug console command. Escape hatch for actions the typed "
                "API cannot express. Bypasses interaction validation. "
                "Examples: 'AI.declare_war 30966' (AI.* delegation) or read-only "
                "diagnostics like 'charinfo 30966'. "
                "NOTE: release builds of the bridge compile this verb out and answer "
                "error=unavailable_in_release; it exists only in builds made with "
                "CK3_BRIDGE_DEBUG_VERBS. Do not use it to grant state (add_claim, add_gold, "
                "give_title, add_trait, ...). Create claims via the legitimate fabricate path "
                "(act type=\"council_task\", task=\"task_fabricate_claim\"), not the console."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "command": {
                        "type": "string",
                        "description": "Full console command string as typed in the game console.",
                    },
                },
                "required": ["command"],
            },
        },
        # 11. pass_turn
        {
            "name": "pass_turn",
            "description": (
                "Explicitly choose not to act this cycle. Current personality weights and "
                "subsystem toggles are satisfactory. Always provide a reason so the decision "
                "log captures why no changes were needed."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "reason": {
                        "type": "string",
                        "description": "Brief explanation for inaction (logged to decision history).",
                        "default": "",
                    },
                },
                "required": [],
            },
        },
    ]
    _legacy = [t for t in _legacy if t.get("name") not in _RETIRED_LEGACY_TOOLS]
    return _legacy + _redesign_tool_definitions() + _board_tool_definitions()


def _board_tool_definitions() -> list[dict]:
    """ck-6kvq.12 (R4): the shared board — ARM B ONLY (decision G5, option A2).

    Returns an EMPTY list in every other arm, which is the manipulation: arm A's
    tool surface differs from arm B's by exactly these two names and by nothing
    else, and the delta is named in the pre-registration rather than buried in
    the held-constant list. A board tool that existed and refused would put arm
    A in a third condition an agent spends budget discovering — see G5.

    The bridge gates the same two verbs independently (board.cpp), so this list
    can never hand an agent a tool the wire would refuse.
    """
    from arm import board_enabled
    if not board_enabled():
        return []
    return [
        {
            "name": "board_post",
            "description": (
                "Post one message to the shared board every English lord can read. "
                "APPEND-ONLY: no edit, no delete, no reply threading. The bridge stamps "
                "your post with who you are, which character you speak for, the in-game "
                "date and the flush it executed in — you cannot set any of those and you "
                "do not need to. An oversized post is REFUSED, never truncated, and there "
                "is a per-flush limit: say the thing that changes someone else's decision."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "text": {"type": "string",
                             "description": "What you want the other lords to read."},
                },
                "required": ["text"],
            },
        },
        {
            "name": "board_read",
            "description": (
                "Read the shared board — every post with seq >= since_seq. Pass back the "
                "next_seq you were last given to see only what is new; pass 0 to read from "
                "the start. Each post carries its author's session, the character they "
                "speak for, the in-game date and the flush it landed in, so 'who said what, "
                "when' is not a matter of trust."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "since_seq": {"type": "integer", "default": 0,
                                  "description": "Resume cursor; 0 reads from the start."},
                    "limit": {"type": "integer", "default": 0,
                              "description": "Max posts to return (0 = server page size)."},
                },
                "required": [],
            },
        },
    ]


def _redesign_tool_definitions() -> list[dict]:
    """The redesigned 8-tool goal-shaped surface (SPEC docs/notes/mcp-redesign/SPEC.md).

    ADDITIVE Phase 1 (SPEC §6.2): registered ALONGSIDE the legacy 11 above so the
    autonomous loop's Anthropic tool list matches the PREFER-THESE surface advertised
    in prompts.py. agent.py._execute_tool dispatches these through redesign.py + the
    _AgentGateway adapter; every composed bridge action is an existing §3 router entry.
    """
    return [
        {
            "name": "observe_v2",
            "description": (
                "Your situation report. Returns de-jure county counts for the titles "
                "nearest and widest around your holdings (de_jure), your current standing "
                "(gold, debt, stress, strongest faction, succession ETA, whether you are at "
                "war), the succession partition, your personality genome, and what changed "
                "since the last call (delta). realm_size is ABSENT because it counts titles, "
                "not counties; de_jure counts counties and self.counties_held counts yours. "
                "Uniform envelope {ok, tool, result, blocked?, "
                "actor?, game, delta?, warnings, errors, bridge_health}."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "character_id": {"type": "integer", "default": 0,
                                     "description": "Ruler to reason about (0 = controlled primary; REQUIRED in multi-actor)."},
                    "depth": {"type": "string", "enum": ["brief", "full"], "default": "brief",
                              "description": "'brief' = de_jure+standing+succession+posture+delta; 'full' adds self vitals, war/council brief."},
                },
                "required": [],
            },
        },
        {
            "name": "plan_war",
            "description": (
                "'What is my single best next war?' Returns a RANKED list of targets that "
                "are ALREADY declarable, ally-free, and goal-advancing — each row's "
                "declare_args goes to declare_war VERBATIM. You never name a casus_belli or "
                "hunt a claimant. Gates on the AUTHORITATIVE strict-CB inner verdict, not the "
                "over-optimistic outer filter (the 17-generation false 'structural wall'). "
                "Empty list ⇒ empty_reason names the single cause (NOT 'walled in')."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "character_id": {"type": "integer", "default": 0,
                                     "description": "Aggressor (0 = controlled primary; REQUIRED in multi-actor)."},
                    "goal_scoped": {"type": "boolean", "default": True,
                                    "description": "True = only de-jure-goal-advancing targets; False = all declarable."},
                    "ally_free_only": {"type": "boolean", "default": True,
                                       "description": "Exclude targets that pull allies you'd fight."},
                    "exclude_straddling": {"type": "boolean", "default": True,
                                           "description": "Exclude wars expected to cross a death ETA."},
                    "max_results": {"type": "integer", "default": 8,
                                    "description": "Top-N by score."},
                },
                "required": [],
            },
        },
        {
            "name": "declare_war",
            "description": (
                "Declare ONE war. Pass a plan_war row's declare_args verbatim (or the fields "
                "explicitly). The only irreversible high-stakes act with its own verb so it "
                "cannot fire by accident. BLOCKS until the war is confirmed PERSISTED (owns "
                "unpause-and-wait) — no sent_no_confirmation ambiguity, no paused-too-fast "
                "cull. Re-checks pressed-claim + target-state up front. Evaporated war ⇒ "
                "verdict:rejected with the exact gate. A straddles_succession war REQUIRES "
                "confirm_straddle=true."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "character_id": {"type": "integer", "default": 0,
                                     "description": "Aggressor (0 = controlled primary)."},
                    "declare_args": {"type": "object",
                                     "description": "A plan_war row's declare_args dict (preferred path)."},
                    "target_id": {"type": "integer", "default": 0,
                                  "description": "Escape-hatch target (required if no declare_args)."},
                    "cb": {"type": "string", "default": "",
                           "description": "Escape-hatch exact CB string."},
                    "cb_title_id": {"type": "integer", "default": 0, "description": "Escape-hatch."},
                    "claimant_id": {"type": "integer", "default": 0, "description": "Escape-hatch."},
                    "confirm_straddle": {"type": "boolean", "default": False,
                                         "description": "HARD GUARD — refuses a near-death-straddling war unless true."},
                },
                "required": [],
            },
        },
        {
            "name": "succession_plan",
            "description": (
                "'Will my realm pass through my heir's succession INTACT?' Returns one "
                "verdict: passes_intact:true, or splits with the cadet who breaks it and the "
                "single lever that WORKS (drive eligible-heir count toward 1) + a wont_work "
                "list. Reads the post-death simulation, never partition_preview.is_partition "
                "(which gave false reassurance that collapsed past runs)."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "character_id": {"type": "integer", "default": 0,
                                     "description": "The ruler whose death you plan around (0 = controlled primary)."},
                },
                "required": [],
            },
        },
        {
            "name": "set_phase",
            "description": (
                "Assert your strategic posture by NAME; the engine sets the 10-axis genome "
                "and subsystem toggles for you. You do NOT hand-tune ten integers. phase=None "
                "reads current. Auto-re-pins the heir each reign. Subsystem toggles are "
                "advisory (the engine does NOT enforce CallAllies) — the genome does the work."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "phase": {"type": "string",
                              "enum": ["expansion", "rebuild", "succession_trough",
                                       "stalled_siege", "fragile_minority", "consolidate"],
                              "description": "Named posture, or omit to read current."},
                    "character_id": {"type": "integer", "default": 0,
                                     "description": "Who to pin (0 = controlled primary; REQUIRED in multi-actor to assert)."},
                    "overrides": {"type": "object",
                                  "description": "Escape hatch — per-axis int overrides (discouraged; warns on CallAllies)."},
                },
                "required": [],
            },
        },
        {
            "name": "do",
            "description": (
                "Perform one concrete non-war game action, OR enumerate what you can do. "
                "Enumerate: do(action_class='council_tasks') returns available actions each "
                "with the EXACT do_call to run it (copy the row; never guess) plus a "
                "blocked-why for each unavailable one. decisions enumerate REQUIRES a bounded "
                "candidates list (#64 freeze safety). Execute: do(intent='council_task', "
                "args={...}) runs ONE action, returning verdict:applied (PERSISTED) or a typed "
                "rejected. council_task/fabricate warn if the AI will REVERT. appoint is an "
                "intent here, not a discoverable interaction."
                # ck-zgh4 deploy-51 item (6): said ONCE here rather than on every
                # refusal — a transcript carries ~500 of them.
                " On a refusal, `retryable` answers whether the call can work later; `severity` does not."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "intent": {"type": "string",
                               "enum": ["mint_title", "take_decision", "interact", "convert_faith",
                                        "convert_culture", "set_doctrine", "change_law", "council_task",
                                        "appoint", "lifestyle", "build", "muster", "men_at_arms",
                                        "hire_merc", "move_capital", "set_obligation", "respond_event",
                                        "scheme", "faction", "scripted_effect"],
                               "description": ("The action to execute. Omit (with action_class) for enumerate mode. "
                                               "'scripted_effect' (args.effect=<name>) RESOLVES a PDXScript scripted_effect "
                                               "template by name (safe, validated); its EXECUTE half (resolve_only=false) is "
                                               "EXPERIMENTAL/validation-pending and additionally requires args.experimental=true "
                                               "+ the bridge default-OFF allow-list — verify any execute by readback.")},
                    "action_class": {"type": "string",
                                     # ck-ycim: FROM the one list, never retyped.
                                     # The retyped copy was missing
                                     # `formable_titles`, so a real capability
                                     # was invisible to every agent.
                                     "enum": list(ACTION_CLASSES),
                                     "description": (
                                         "Enumerate mode only — the class of things to LIST. "
                                         "A DIFFERENT vocabulary from `intent`: these are mostly "
                                         "plural nouns naming what can be listed. `men_at_arms` "
                                         "is in both lists: as a class it lists MaA types with "
                                         "recruit cost and what the realm already fields; as an "
                                         "intent it recruits one (args.maa_type).")},
                    "candidates": {"type": "array", "items": {"type": "string"},
                                   "description": "BOUNDED list for action_class='decisions' (#64 safety)."},
                    "character_id": {"type": "integer", "default": 0,
                                     "description": "Actor (0 = controlled primary; REQUIRED in multi-actor)."},
                    "args": {"type": "object",
                             # ck-nq8d: the per-intent argument list is GENERATED from
                             # `_intent_to_act_params` -- the function that turns
                             # do(intent, args) into the bridge's parameters, so what it
                             # reads off `args` IS the argument list. Regenerate with
                             # `python3 scripts/gen_do_intent_args.py`; the drift guard is
                             # `--check`, and acceptance_nq8d runs it. A table typed by
                             # hand here would be a second home for the same fact.
                             "description": (
                                 "Intent-specific keys. 396 missing_required_argument "
                                 "refusals in the measured corpus were seats learning "
                                 "these BY REFUSAL, so they are listed here. Keys are "
                                 "what the harness READS; the bridge decides which are "
                                 "mandatory and answers with a *_required code.\n"
                                 "ARGUMENTS BY INTENT (generated: scripts/gen_do_intent_args.py):\n  appoint: args.{seat, position_id, councillor_id, character_id}\n  build: args.{location_id, building}\n  change_law: args.{law, scope, title_id}\n  convert_culture: args.{culture, culture_id}\n  convert_faith: args.{faith, faith_id}\n  council_task: args.{task, target_title_id, target_id, target_character_id, seat}\n  faction: args.{action, faction_id, target_id, faction_type}\n  hire_merc: args.{company_id}\n  interact: args.{interaction, interaction_type, target_id, title_id, secondary_actor_id, secondary_recipient_id}\n  lifestyle: args.{focus}\n  men_at_arms: args.{action, maa_type, location_id, regiment_id}\n  mint_title: args.{title_key, title, title_id}\n  move_capital: args.{title_id}\n  muster: args.{mode}\n  respond_event: args.{event_id, option, instance_id}\n  scheme: args.{action, target_id, scheme_type, scheme, scheme_id}\n  set_doctrine: args.{doctrine}\n  set_education_focus: args.{focus}\n  set_obligation: args.{vassal_id, level, obligation}\n  take_decision: args.{decision}"
                             )},
                },
                "required": [],
            },
        },
        {
            "name": "inspect",
            "description": (
                "Deep, typed read of ONE subject (character, realm, council, succession, "
                "claims, wars, armies, factions, vassals, diplomacy, title, dynasty, laws, "
                "schemes, marriage, economy, research, county, opinion, opinion_breakdown, "
                "relations, hooks, title_hierarchy, title_succession, claimants, war_score, "
                "call_to_arms, development, incoming_schemes, lifestyle, heir_plan, "
                "guardian_candidates). heir_plan lists each child and eligible heir with "
                "martial, traits and the lever status available for them; education_focus "
                "and guardian read NULL because the bridge does not expose them. "
                "guardian_candidates lists in-court guardian prospects for a child with "
                "their martial. dynasty_id is null for landed rulers by design. "
                "borders/de_jure are KNOWN_BROKEN — return a warning, not data."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "subject": {"type": "string",
                                "description": "The subject to read (character, realm, council, succession, claims, wars, armies, factions, vassals, diplomacy, title, dynasty, laws, schemes, marriage, economy, research, county, opinion, opinion_breakdown, relations, hooks, title_hierarchy, title_succession, claimants, war_score, call_to_arms, development, incoming_schemes, lifestyle)."},
                    "character_id": {"type": "integer", "default": 0},
                    "target_id": {"type": "integer", "default": 0},
                    "title_id": {"type": "integer", "default": 0},
                    "war_id": {"type": "integer", "default": 0},
                    "title_key": {"type": "string", "default": "",
                                  "description": "For the 'title' (de-jure) subject: a title key such as 'k_england'."},
                    "dynasty_id": {"type": "integer", "default": 0},
                    "include": {"type": "array", "items": {"type": "string"},
                                "description": "Subject-specific sub-sections."},
                    "max_results": {"type": "integer", "default": 20},
                },
                "required": ["subject"],
            },
        },
        {
            "name": "cbs",
            "description": (
                "'Show me EVERY casus belli I can use against THIS target, with tradeoffs.' "
                "Returns the full curated CB list for the (actor,target): each option's scope "
                "(county|duchy|kingdom|realm|faith_counties), what it GRANTS on victory, "
                "requirements_unmet, tradeoffs, and the dispatch-marshalling hint "
                "(needs_cb_title_id / needs_claimant / dispatch_supported). WEIGH options "
                "before declaring — e.g. tribal_subjugation (whole realm, once/lifetime) vs "
                "county_conquest (one county) vs duchy_conquest (a whole duchy). valid_options "
                "is the declarable subset; best is the widest-scope declarable one. Re-point a "
                "plan_war row's declare_args.cb at any option here. UNPAUSED engine read."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "target_id": {"type": "integer",
                                  "description": "The defender character id (REQUIRED)."},
                    "character_id": {"type": "integer", "default": 0,
                                     "description": "Aggressor (0 = controlled primary; REQUIRED in multi-actor)."},
                },
                "required": ["target_id"],
            },
        },
        {
            "name": "session",
            "description": (
                "Game-flow, identity, and liveness control. You rarely call this — observe_v2/"
                "plan_war/do own pause-state for you. Use to: switch which ruler you control "
                "after a succession (control); confirm the controlled ruler is alive+landed "
                "(whoami); check bridge liveness if calls time out (health — 'hung' = STOP "
                "looping); or set speed/pause. Speed capped at 3; 4-5 is REFUSED while at "
                "war, otherwise clamped to 3. The human player can be a target but never an actor. "
                "experimental_flags lists the default-OFF EXPERIMENTAL-unvalidated engine "
                "flags; set_experimental(flag=<name>, enabled=bool) arms ONE for "
                "disposable-save validation ONLY (off_goal_veto / break_alliance / "
                "force_offensive_stance) — never rely on it on the live campaign until its "
                "live smoke passes."
            ),
            "input_schema": {
                "type": "object",
                "properties": {
                    "action": {"type": "string",
                               "enum": ["control", "release", "whoami", "speed", "pause",
                                        "unpause", "wait", "health", "auto_events",
                                        "experimental_flags", "set_experimental"],
                               "description": ("The session operation. wait: let the clock run for "
                                               "`seconds` (unpause, block, pause) and return the "
                                               "start and end dates; use it instead of Bash sleep.")},
                    "seconds": {"type": "integer", "default": 60,
                                "description": "For wait — 1-110 seconds of game clock per call."},
                    "character_id": {"type": "integer", "default": 0, "description": "For control/release."},
                    "speed": {"type": "integer", "default": 3, "description": "1-3 (4-5 refused while at war)."},
                    "enabled": {"type": "boolean", "default": True,
                                "description": "For auto_events AND set_experimental (arm/disarm the flag)."},
                    "disable_native_ai": {"type": "boolean", "default": False, "description": "For control."},
                    "flag": {"type": "string", "default": "",
                             "enum": ["off_goal_veto", "break_alliance", "force_offensive_stance"],
                             "description": "For set_experimental — which EXPERIMENTAL-unvalidated default-OFF bridge flag to arm/disarm."},
                },
                "required": ["action"],
            },
        },
    ]


