"""
CK3 Agent tool dispatch: maps MCP tool arguments to bridge action strings.

The tool SCHEMAS are not here: they are the FastMCP registrations in
mcp_server.py, and mcp_server.registered_tool_definitions() is the one reader of
them (the agent loop, the reference generator and the surface witness all call it).

Entry points:
  - resolve_query / resolve_discover / resolve_act / resolve_targets_plan —
    per-tool dispatch helpers used by mcp_server.py and redesign.py.
  - QUERY_DOMAINS, DISCOVER_WHATS, ACTION_CLASSES — the vocabularies the tool
    schemas advertise.
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
    # do(intent='scheme', args={action: start, scheme: murder, target_id: N}) when is_kin==false
    # (kinslayer trait → catastrophic stress). (#is_kin)
    "character":            ("query_character",            lambda p: {"character_id": p.get("character_id", 0)}),
    # ck-leg: character browser — scan CHARACTER_DB (≈103k chars) with filter+sort,
    # bounded top-N. filter ∈ all|alive|dead|ruler|independent|ai_controlled;
    # sort ∈ military|gold|realm_size|tier|age|<skill>. The first real way to find
    # strong independent rulers (e.g. marriage-alliance prospects) by name+stats.
    "characters":           ("search_characters",          lambda p: {"filter": p.get("filter") or "alive", "sort": p.get("sort") or "military", "sort_order": p.get("sort_order") or "desc", "max_results": p.get("max_results") or 50, **({"court_of": p["court_of"]} if p.get("court_of") else {}), **({"female": p["female"]} if "female" in p else {}), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    # §1a (re_heir_education_RE.md): per-child/heir education-focus read. education_focus
    # + education_focus_skill are REAL; guardian_id/_martial are always null (no read
    # locates a ward's guardian, re_heir_guardian_RE.md §1b).
    "heir_education":        ("query_heir_education",        lambda p: {"character_id": p.get("character_id", 0)}),
    # ck-leg: marriage-market browser — landed unmarried rulers in diplomatic range
    # of character_id, top-N by `sort` (military default), annotated age/distance.
    # The precise "who can I marry" surface (vs the generic `characters` browser).
    # Pass require_in_range=0 (via max_results/sort params path) to browse globally.
    "marriage_candidates":  ("query_marriage_candidates",  lambda p: {"character_id": p.get("character_id", 0), "sort": p.get("sort") or "military", "max_results": p.get("max_results") or 30, **({"require_in_range": p["require_in_range"]} if "require_in_range" in p else {})}),
    "opinion":              ("query_opinion",              lambda p: {"actor_id": p.get("character_id", 0), "target_id": p.get("target_id", 0)}),
    "realm":                ("query_realm",                lambda p: {"character_id": p.get("character_id", 0), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    "wars":                 ("query_wars",                 lambda p: {"character_id": p.get("character_id", 0), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    # #120: a specific war_id (gen-tagged ref OR bare slot idx) returns the FULL
    # attacker-perspective breakdown — total_warscore (-100..100) + cache_status +
    # the components battle_score/ticking_warscore (always live) and
    # occupation_score/imprisonment_score/landless_score (engine-cached). A
    # high battle_score with occupation_score 0 and cache_status="cached" means
    # battles won but war-goal counties NOT sieged → siege-stall, intervene.
    # cache_status="live_only" means occupation just hasn't been computed yet.
    "war_score":            ("query_war_score_breakdown",  lambda p: {"war_id": p.get("war_id", 0)}),
    "war_declarations":     ("query_war_declarations",     lambda p: {"limit": p.get("limit", 64), "since_tick": p.get("since_tick", 0)}),
    "scheme_inits":         ("query_scheme_inits",         lambda p: {"limit": p.get("limit", 64), "since_tick": p.get("since_tick", 0)}),
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
    # Conquest targets by de-jure duchy (query_conquest_targets): the duchies the ruler
    # partly holds and the ones bordering the realm, plus, with title_id, every de-jure
    # duchy under that title anywhere on the map (in_toward=true, listed first). Each
    # remaining county carries holder + is_kin/is_allied/is_independent, the INDEPENDENT
    # top liege (holder_top_liege_id), conquerable_now (the engine's county_conquest_cb
    # check for the top candidates), engine_valid_cbs (every other CB the engine accepts
    # against that holder) and a `declare` block for declare_war.
    "conquest_targets":     ("query_conquest_targets",     lambda p: {"character_id": p.get("character_id", 0), **({"toward_title_id": p["title_id"]} if p.get("title_id") else {})}),
    "claims":               ("query_claims",               lambda p: {"character_id": p.get("character_id", 0), "include_vassals": p.get("include_vassals", False), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    # Owned schemes (bridge scheme_engine.cpp, re_schemes.md). Each entry: scheme_id,
    # owner_id/owner_name, target_id/target_name, type, category (hostile|personal|
    # political|contract), is_secret, success_chance and success_chance_max (the
    # engine's own getter), secrecy, progress/max_progress, breaches/max_breaches,
    # agent_charges, exposed (+ exposed_date), is_frozen, start_date, and agents:
    # one row per slot with slot, contribution_type, character_id, name, contribution.
    "schemes":              ("query_schemes",              lambda p: {"character_id": p.get("character_id", 0)}),
    # Plots TARGETING me: the same state per scheme, with schemer_id/schemer_name and
    # dangerous (the engine's hostile category); dangerous_count at the top.
    "incoming_schemes":     ("query_incoming_schemes",     lambda p: {"character_id": p.get("character_id", 0)}),
    "succession":           ("query_succession",           lambda p: {"character_id": p.get("character_id", 0)}),
    "lifestyle":            ("query_lifestyle",            lambda p: {"character_id": p.get("character_id", 0)}),
    "armies":               ("query_armies",               lambda p: {"character_id": p.get("character_id", 0), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    "county":               ("query_county_details",       lambda p: {"character_id": p.get("character_id", 0)}),
    "development":          ("query_development_growth",   lambda p: {"title_id": p.get("title_id", 0), "character_id": p.get("character_id", 0)}),
    "buildings":            ("query_building_types",       lambda p: {}),
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
    "de_jure":              ("query_de_jure",              lambda p: {"title_id": p.get("title_id", 0), "character_id": p.get("character_id", 0)}),
    "council":              ("query_council",              lambda p: {"character_id": p.get("character_id", 0), **({"verbose": p["verbose"]} if "verbose" in p else {})}),
    # Read-only enumeration of a character's MEMORIES (CCharacterMemory pool):
    # per-memory ref_id/index/generation, owner_id, primary participant (id+name),
    # participant_count, and creation/end dates (raw + formatted "YYYY.MM.DD").
    # The memory TYPE NAME and per-participant {char,role} decode are
    # unresolvable on this build (type=null) — see re_character_memories.md open
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
    # realm changes. Pure native read: title_id and limit.
    "title_history":        ("query_title_history",        lambda p: {"title_id": p.get("title_id", 0), **({"limit": p["limit"]} if p.get("limit") else {})}),
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
    "diplomatic_range":     ("query_diplomatic_range",     lambda p: {"character_id_a": p.get("character_id", 0), "character_id_b": p.get("target_id", 0)}),
    # ck-leg: real alliance handler (own/ally military strength + why each
    # alliance exists). Previously aliased to query_relations, which hid the
    # actual strength data the agent needs to judge whether allies are useful.
    "alliances":            ("query_alliances",            lambda p: {"character_id": p.get("character_id", 0)}),
    "call_to_arms":         ("query_call_to_arms",         lambda p: {"character_id": p.get("character_id", 0)}),
}

# event_log is a synthetic aggregation domain (handled specially in mcp_server.query,
# not via QUERY_DOMAIN_MAP) — it merges the war/scheme/council since_tick feeds.
QUERY_DOMAINS = sorted(QUERY_DOMAIN_MAP.keys()) + ["event_log", "relative_power"]

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
            # the war the interaction is about: call_ally / join_war carry it as their
            # target scope (the bridge ignores it for every other interaction). Sent
            # only when given: war 0 is a real war.
            **({"war_id": params["war_id"]} if params.get("war_id") is not None else {}),
            # The window's send options by flag name ("matrilineal", "hook"); the bridge
            # takes them comma-separated and refuses a name the interaction does not declare.
            "send_options": ",".join(params.get("send_options") or []),
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
                    "a steered war requires the casus belli name (declare_war cb=...)")
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
    elif act_type == "law":
        # Per-TITLE law (a title's succession law, e.g. feudal_elective_succession_law on
        # a kingdom) when law_scope="title": the bridge queues the title-law window's own
        # command after the engine's validity check says yes, and returns the engine's
        # refusal text when it says no. Default (no law_scope) is the realm-law path.
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
        #     faith_id (int). Works paused or running.
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
        # script KEY (culture="english") or culture_id (int). Works paused or running.
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
        # the readback detects success on a child. Read it back with
        # query(domain="heir_education", character_id=<child>).education_focus.
        return "set_education_focus", {
            "actor_id": actor_id,
            "focus": params.get("focus", ""),
        }
    elif act_type == "building":
        # Order a building in a holding as a player does (construct_building):
        # the game's construction validity decides (its reasons come back as
        # engine_reasons), the price is taken from the actor's gold, and the
        # building stays under construction for its construction days
        # (inspect(subject="county") holdings[].construction). location_id is the
        # holding's barony title id (holdings[].barony_title_id).
        return "construct_building", {
            "actor_id": actor_id,
            "building": params.get("building", ""),
            "location_id": params.get("location_id", 0),
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
        # do(action_class="mercenaries"). Actor must be an AI ruler.
        return "hire_mercenary", {
            "actor_id": actor_id,
            "company_id": params.get("company_id", 0),
            "contract_len": params.get("contract_len", 0),
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
        # query(domain="de_jure")). Game-thread-deferred (paused or running); may
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
        # Lower a vassal's contract obligation by name (leniency: a vassal owes
        # less tax or levies). The liege is actor_id; obligation is the type key
        # (feudal_government_taxes, ...) and level its new level, as
        # inspect(subject="vassals") lists them under each vassal's `contract`. A
        # raise is refused (raise_requires_negotiation): the game asks the vassal's
        # consent and costs opinion, which this write does not do.
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
    elif act_type == "scheme":
        # do(intent="scheme"). scheme_action drains on the game thread. start: the
        # engine's CanStartScheme (the type's allow/valid triggers, an owned scheme on
        # the target, the cooldown), then CreateActiveScheme; a refusal carries the
        # engine's reasons. cancel: RemoveActiveScheme on the actor's own scheme.
        # discover(what="scheme_targets") runs the same check and starts nothing.
        action = params.get("action") or "start"
        if action == "cancel":
            return "scheme_action", {
                "actor_id": actor_id,
                "action": "cancel",
                "scheme_id": params.get("scheme_id", 0),
            }
        return "scheme_action", {
            "actor_id": actor_id,
            "action": action,
            "target_id": params.get("target_id", 0),
            "scheme": params.get("scheme") or "",
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
# (the primary title's tier, the liege chain's top, the war count, prison, per-duchy
# de-jure control; prestige_level and the tribal era no read carries: unmeasured) — the SAME approach observe's `mint_requirements` uses — WITHOUT
# the hanging engine eval. It is additive, crash-safe, and works PAUSED (pure
# reads). It does not REPLACE the engine verdict (do(intent='take_decision') still routes
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


def _clause(key: str, label: str, ok, *, requirement: str,
            measured=None, fix: str = "", unmeasured: str = "") -> dict:
    """One decision-gate clause verdict. `ok` is True or False when the reads
    established it; a clause no read can establish is UNMEASURED: pass None and
    `unmeasured` says why, never a pass by default (L15, witness r4 N4-1)."""
    if unmeasured:
        c = {"clause": key, "label": label, "pass": None, "requirement": requirement,
             "unmeasured": unmeasured}
    else:
        c = {"clause": key, "label": label, "pass": bool(ok), "requirement": requirement}
    if measured is not None:
        c["measured"] = measured
    if (unmeasured or not ok) and fix:
        c["fix"] = fix
    return c


def shield_decision_why_blocked(decision: str, *, realm: dict, character: dict,
                                duchy_control: dict | None = None) -> dict:
    """Compute per-clause pass/fail for a SHIELD decision from structural reads.

    Inputs are raw bridge query results (any may be {} / None — degrades safely),
    read only by keys the bridge writes (query_handlers.cpp):
      realm        — query_realm      (title_tier: the primary title's tier;
                                       war_count)
      character    — query_character  (id, top_liege_id, ruler_tier, prestige,
                                       is_imprisoned)
      duchy_control— optional pre-computed {complete_duchies, candidate_duchies,
                     unmeasured_duchies} for the kingdom (each a list of
                     {title_id,key,held,total}; an unmeasured row carries the reason).
    A clause no read establishes is UNMEASURED (pass None, with the reason):
    prestige_level and the tribal era (government, culture era) are in neither
    reply, duchy control when duchy_control is absent, and duchy control short of
    the count while a held duchy did not read back.

    Returns {decision, gate, clauses[], blocking[], unmeasured[],
    all_structural_pass, note}. NEVER raises — every read is defensive. This is a
    HINT surface; the engine's AI_TRY_DECISION_CORE remains the authority.
    """
    gate = _SHIELD_DECISION_GATES.get(decision)
    if gate is None:
        return {"decision": decision, "supported": False,
                "note": "no structural gate model — supported: "
                        + ", ".join(SHIELD_DECISIONS)}

    realm = realm if isinstance(realm, dict) else {}
    character = character if isinstance(character, dict) else {}

    def _int(v):
        return v if isinstance(v, int) and not isinstance(v, bool) else None

    clauses: list[dict] = []

    # G1/G-tier: highest held title must be <= DUKE (tier 3). A king/emperor is
    # ineligible (forming the kingdom is the TERMINAL act, not a precondition).
    # The primary title's tier (1 barony .. 5 empire): query_realm title_tier, else
    # query_character ruler_tier (the same TITLE_TYPE_TIER read, build_char_profile).
    # query_realm's own ruler_tier is title_realm+0x1D4, another scale (7=empire).
    prim_tier = _int(realm.get("title_tier"))
    if prim_tier is None:
        prim_tier = _int(character.get("ruler_tier"))
    clauses.append(_clause(
        "highest_title_le_duke", "highest held title <= duke (tier 3)",
        prim_tier is not None and 0 < prim_tier <= 3,
        requirement="primary title tier <= 3 (already a KING -> ineligible)",
        measured={"primary_title_tier": prim_tier},
        fix="you already hold a kingdom; this shield is done",
        unmeasured="" if prim_tier is not None else
        "neither query_realm title_tier nor query_character ruler_tier came back"))

    # Independent: top_liege == self. query_character's top_liege_id is the
    # independent ruler at the head of the liege chain (an independent ruler's own
    # id; 0 when the walk could not be made, build_char_profile).
    me = _int(character.get("id"))
    top = _int(character.get("top_liege_id"))
    indep_read = bool(me) and bool(top)
    clauses.append(_clause(
        "independent", "independent (top_liege = self)",
        indep_read and top == me,
        requirement="top_liege == self",
        measured={"character_id": me, "top_liege_id": top},
        fix="you are a vassal — only an independent ruler can form a kingdom",
        unmeasured="" if indep_read else
        "query_character carried no id / top_liege_id (0 = the liege walk failed)"))

    # G7: at peace. query_realm war_count (the realm's war array, do_query_realm).
    wc = _int(realm.get("war_count"))
    clauses.append(_clause(
        "at_peace", "at peace (is_at_war = no)", wc == 0,
        requirement="is_at_war == false",
        measured={"war_count": wc},
        fix="go to peace first: end or white-peace your active wars",
        unmeasured="" if wc is not None else "query_realm carried no war_count"))

    # not imprisoned (showing-failure gate). query_character is_imprisoned.
    imp = character.get("is_imprisoned")
    clauses.append(_clause(
        "not_imprisoned", "not imprisoned", imp is False,
        requirement="imprisoned == false",
        measured={"is_imprisoned": imp},
        unmeasured="" if isinstance(imp, bool) else
        "query_character carried no is_imprisoned"))

    # G4: prestige_level. No reply carries a character's prestige level (only the
    # spendable prestige), so the clause is unmeasured; prestige is shown beside it.
    need_plvl = gate["prestige_level_min"]
    clauses.append(_clause(
        "prestige_level", f"prestige_level >= {need_plvl}",
        None,
        requirement=f"prestige_level >= {need_plvl} "
                    f"({'1000' if need_plvl == 2 else '2000'} prestige)",
        measured={"prestige": character.get("prestige")},
        fix="bank prestige (GHA + create_title + conquests pump it); do not "
            "stress-break (a break dumps prestige)",
        unmeasured="no bridge reply carries a character's prestige level; the "
                   "engine's refusal names it"))

    # G8: tribal era (one-way timer) — only for negotiate_the_danelaw. Neither
    # query_character nor query_realm carries the government or the culture's era.
    if gate["needs_era_tribal"]:
        clauses.append(_clause(
            "tribal_era", "culture still in TRIBAL era (one-way timer)",
            None,
            requirement="government == tribal AND culture era < early_medieval",
            fix="if feudal/era-advanced use found_kingdom_decision (Fallback A) instead",
            unmeasured="neither query_character nor query_realm carries the government "
                       "or the culture's era"))

    # G5: 2 (or 3) completely-controlled k_england duchy TITLES.
    need_duchies = gate["needs_duchies"]
    if duchy_control is not None and isinstance(duchy_control, dict):
        complete = duchy_control.get("complete_duchies") or []
        candidates = duchy_control.get("candidate_duchies") or []
        unread = duchy_control.get("unmeasured_duchies") or []
        n_complete = len(complete)
        # A held duchy that could not be read (its kingdom or its meter) may be a
        # complete one: short of the count with such a duchy is UNMEASURED, not a fail.
        duchy_ok = True if n_complete >= need_duchies else (None if unread else False)
        clauses.append(_clause(
            "controlled_duchies",
            f"control {need_duchies} {gate['kingdom'] or 'held'} duchy titles "
            "(every de-jure county, vassal control counts)",
            duchy_ok,
            requirement=f"count >= {need_duchies} completely-controlled duchy "
                        f"titles de-jure-above = {gate['kingdom'] or '(any)'}",
            measured={"complete": n_complete, "need": need_duchies,
                      "complete_duchies": complete,
                      "incomplete_candidates": candidates,
                      "unmeasured_duchies": unread},
            fix="THE usual blocker: complete one more duchy. A duchy you control by "
                "VASSAL satisfies this gate but to MINT its TITLE create_title needs "
                "a de-jure county in PERSONAL demesne. Re-conquer any lost duchy "
                "county FAST (a mid-build loss breaks this gate).",
            unmeasured="" if duchy_ok is not None else
            f"{len(unread)} held duchy title(s) did not read back (kingdom or meter)"))
    else:
        clauses.append(_clause(
            "controlled_duchies",
            f"control {need_duchies} duchy titles (UNMEASURED)",
            None,
            requirement=f"count >= {need_duchies} completely-controlled duchy titles",
            measured={"need": need_duchies, "note": "duchy control not enumerated "
                      "in this call — pass duchy_control or read query(domain='titles') "
                      "+ per-duchy query(domain='de_jure')"},
            unmeasured="duchy control was not enumerated in this call"))

    blocking = [c["clause"] for c in clauses if c["pass"] is False]
    unmeasured = [c["clause"] for c in clauses if c["pass"] is None]
    all_pass = not blocking and not unmeasured
    if blocking:
        note = "fix the BLOCKING clause(s) before firing the decision; do not spam-retry"
    elif unmeasured:
        note = ("no measured clause blocks; the UNMEASURED ones ("
                + ", ".join(unmeasured) + ") no read here can establish — if "
                "do(intent='take_decision') returns vtable_validation_failed, one of them "
                "or a negotiation rejection (british_isles.1011) is the cause")
    else:
        note = ("all STRUCTURAL clauses pass — if do(intent='take_decision') still returns "
                "vtable_validation_failed, a negotiation rejection (british_isles.1011) "
                "is the cause; re-fire next peace window")
    return {
        "decision": decision,
        "kingdom": gate.get("kingdom"),
        "gate": "negotiate_the_danelaw / found_kingdom shield gate (structural model)",
        "clauses": clauses,
        "blocking": blocking,
        "unmeasured": unmeasured,
        "all_structural_pass": all_pass,
        "is_structural_hint": True,
        "note": note,
        "doc": "re/findings/re_shield_robustness.md; "
               "ck3agent/knowledge/evolving-lessons.md AL-11",
    }


#: discover(what=...) values served by mcp_server.discover itself (not resolve_discover).
#: scheme_types / scheme_targets: redesign.scheme_types_impl / scheme_targets_impl.
_DISCOVER_COMPOSED = ("targets", "decision_why_blocked", "scheme_types", "scheme_targets")

#: Enumerations `do(action_class=...)` owns (cleanup 2026.11 L14, finding 8): the
#: discover names they had, and the do door each became. discover answers one of
#: these names with a refusal whose `next` is that door; nothing is read.
DISCOVER_MOVED_TO_DO = {
    "hireable_mercenaries": "mercenaries",
    "fabricate_targets": "fabricate_targets",
    "joinable_factions": "factions_to_join",
}

#: Every what= value resolve_discover maps to a bridge action. test_surface_contract
#: checks each of these resolves and that the tool's enum is exactly this list plus
#: _DISCOVER_COMPOSED.
_DISCOVER_RESOLVED = (
    "interactions", "decisions", "decisions_annotated", "valid_decisions", "acceptance",
    "valid_cbs", "valid_cbs_strict", "trigger_result", "war_validity",
    "construction_check", "war_targets", "imprisonment", "trigger_slots",
    "interaction_validity",
)

DISCOVER_WHATS = list(_DISCOVER_RESOLVED) + list(_DISCOVER_COMPOSED)


def resolve_discover(params: dict) -> tuple[str, dict] | None:
    """Resolve a discover tool call to (bridge_action, bridge_params)."""
    what = params.get("what")
    if what == "interactions":
        return "list_interactions", {}
    elif what == "decisions":
        # LIGHT list (name+hash) — fast. For validity/requirements use
        # "decisions_annotated" or "valid_decisions" (those run the engine eval on the
        # game thread).
        return "list_decisions", {
            "character_id": params.get("character_id", params.get("actor_id", 0)),
        }
    elif what == "decisions_annotated":
        # ANNOTATED for SPECIFIC decisions: pass decisions=[key names and/or name-hashes]
        # (get the names from what="decisions"). Each returns shown/valid/cost_ok/available
        # + failure requirement loc-keys + cost + the character's current gold/piety/prestige
        # (have-vs-need). Bounded → fast + safe. Deferred game-thread eval.
        # No decisions list → falls back to the light name+hash list. character_id optional.
        return "list_decisions_annotated", {
            "character_id": params.get("character_id", params.get("actor_id", 0)),
            "decisions": params.get("decisions", []),
        }
    elif what == "valid_decisions":
        # Same bounded annotated eval over the passed decisions=[...], returning ONLY the
        # available==true ones (with cost + current values). Deferred game-thread eval.
        # No decisions list → light name+hash list.
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
        # outer filter. Per-CB outer triggers (allowed_for_character /
        # allowed_against_character / valid_to_start) are not evaluated.
        return "query_cb_validity_strict", {
            "actor_id": params.get("actor_id", 0),
            "target_id": params.get("target_id", 0),
            "include_ineligible": params.get("include_ineligible", True),
            # audit B3: the strict CB worker runs on the game thread and needs
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
        # The game's can-build verdict (ADD_CONSTRUCTION_VALIDITY, no mutation):
        # may `actor_id` order `building` at `location_id` (a barony title id)?
        # Returns {buildable, engine_reasons, price, gold, slot}. The slot is read
        # from the holding unless one is given.
        out = {
            "actor_id": params.get("actor_id", 0),
            "building": params.get("building", ""),
            "location_id": params.get("location_id", 0),
        }
        if params.get("slot") is not None:
            out["slot"] = params["slot"]
        return "construction_check", out
    elif what == "war_targets":
        # ck-leg: enumerate every (target, title, CB) the actor can declare war
        # on right now — the "who can I attack and with what" surface. Each entry
        # carries cb_type, pressed (claim already pressed?), tier, title. Feed a
        # chosen entry into declare_war(target_id=..., cb=..., cb_title_id=...).
        return "query_valid_war_targets", {
            "actor_id": params.get("actor_id", 0),
        }
    elif what == "imprisonment":
        # ck-leg: the grounds the game gives the actor against the target (the
        # reason modifiers + has_imprisonment_reason). Not the game's verdict on
        # an imprisonment: that is the send check, do(action_class=
        # 'imprison_targets') / discover(what='targets', action='imprison').
        return "query_imprisonment_reasons", {
            "actor_id": params.get("actor_id", 0),
            "target_id": params.get("target_id", 0),
        }
    elif what == "trigger_slots":
        return "query_trigger_slots", {
            "interaction_type": params.get("interaction_type", ""),
        }
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
        (`reason`, `gates_evaluated`, `gates_unchecked`, etc.). For unknown
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
                "gates_unchecked": response.get("gates_unchecked", []),
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
            "gates_unchecked": response.get("gates_unchecked", []),
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
        "gates_unchecked": response.get("gates_unchecked", []),
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

# Candidate pools the bridge can enumerate. Foreign strangers (the
# marriage/alliance market) are not among them: `targets` answers an action
# outside this table with mode="error" and a reason naming the supported ones.
_TARGETS_ACTION_ALIASES = {
    "war": "war", "declare_war": "war",
    "imprison": "imprison", "imprison_interaction": "imprison",
    "execute": "grounds", "execute_prisoner_interaction": "grounds",
    "banish": "grounds",
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
      {"mode":"imprison_targets", "label":"imprison"}
         the game's own send check per target (redesign.imprison_target_rows, the
         rows do(action_class='imprison_targets') gives): one definition.
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
            return {"mode": "error", "gate": "missing_required_argument",
                    "reason": "discover(what='targets', action='claim') requires title_id"}
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
        # The game's own send check and the recipient's answer per target, the
        # one builder do(action_class='imprison_targets') uses (grounds alone are
        # not the game's verdict: re_imprisonment.md).
        return {"mode": "imprison_targets", "label": "imprison"}
    if kind == "grounds":
        # execute / banish: my vassals with the grounds the game gives against
        # each (has_imprisonment_reason and the reason modifiers), not a send check.
        return {
            "mode": "pool_annotate",
            "label": "grounds",
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
        # Bare vassal roster (revoke-title etc.); there is no per-target validity
        # handler, so just the enumerable pool.
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
        "gate": "missing_required_argument" if not action else "unknown_verb",
        "reason": (f"discover(what='targets', action='{action}') is not enumerable. Supported: "
                   "war, imprison/execute/banish, revoke_title, claim."),
    }


def shape_discover_response(params: dict, response):
    """Dispatch optional response shaping for discover() outputs. Returns
    the original response unchanged unless the `what` value has a registered
    shaper (currently only `interaction_validity`)."""
    what = (params or {}).get("what")
    if what == "interaction_validity":
        return shape_interaction_validity_response(params, response)
    return response


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
