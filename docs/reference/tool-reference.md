# CK3 Bridge Tool Reference

**Auto-generated — do NOT edit by hand.**

Source: `scripts/gen_tool_reference.py`.
Regenerate with `make tool-reference`. CI runs `make tool-reference-check`
to fail on drift.

The bridge exposes two surfaces:

1. **MCP tools** (11) — what `ck3agent` agents see. Defined in
   `ck3agent/server/tools.py::get_tool_definitions`. Each tool's enum
   sub-options (`domain`, `what`, `type`, `action`, `include`) dispatch
   to one or more bridge actions via the `resolve_*` helpers.
2. **Bridge actions** (`180`) — the underlying IPC verbs.
   Defined in `bridge/src/dispatch_router.cpp` via `strcmp(action_type, ...)`.
   Implementations live in `query_handlers.cpp`, `action_handlers.cpp`,
   and the per-subsystem hook files.

The two surfaces should stay in sync — an unwrapped bridge action means
either a missing MCP wrapper or a private lab/test verb.


---

## MCP Tool Surface

`16` consolidated tools. Each tool's required + optional parameters are documented below, followed by its sub-option table mapping enum values to bridge actions.

| # | Tool | Required | Sub-options |
|---|------|----------|-------------|
| 1 | `observe` | — | 5 sub-option(s) |
| 2 | `query` | — | 60 sub-option(s) |
| 3 | `discover` | `what` | 18 sub-option(s) |
| 4 | `steer` | — | 4 sub-option(s) |
| 5 | `intent` | `action` | 4 sub-option(s) |
| 6 | `execute_console_command` | `command` | 1 sub-option(s) |
| 7 | `pass_turn` | — | 0 sub-option(s) |
| 8 | `observe_v2` | — | 0 sub-option(s) |
| 9 | `plan_war` | — | 0 sub-option(s) |
| 10 | `declare_war` | — | 0 sub-option(s) |
| 11 | `succession_plan` | — | 0 sub-option(s) |
| 12 | `set_phase` | — | 0 sub-option(s) |
| 13 | `do` | — | 0 sub-option(s) |
| 14 | `inspect` | `subject` | 0 sub-option(s) |
| 15 | `cbs` | `target_id` | 0 sub-option(s) |
| 16 | `session` | `action` | 0 sub-option(s) |

### `observe`

Get a combined snapshot of the current game state for decision-making. Returns the controlled character's full profile (stats, traits, personality weights, resources), current game date and speed, and a strategic context summary (military strength, active wars with scores, top rulers). Call this once at the start of every decision cycle — it replaces calling get_my_character, get_tick_info, get_ai_personality, and get_ai_evaluations separately.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `include` | array[string] | no | — | Which sections to include. Omit for all defaults (character + tick + personality + evaluations). Use selective includes to reduce response size. |
| `character_id` | integer | no | 0 | Character to observe (0 = controlled character). |

**Bridge dispatch:**

| Sub-option | Bridge action |
|------------|---------------|
| `character` | `query_character` |
| `tick` | `get_tick_info` |
| `personality` | `get_ai_personality` |
| `evaluations` | `get_ai_evaluations` |
| `subsystems` | `get_character_subsystems` |

### `query`

Deep-dive query for a specific game domain. Use after observe() reveals something that needs investigation — wars to analyze, factions to assess, territory to plan around, or characters to evaluate. Each domain returns structured data specific to that system. Supports batch queries via 'domains' (max 5) with partial failure and per-domain errors.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `domain` | enum(62) | no | — | Which game domain to query (single). |
| `domains` | array[string] | no | — | Batch query: up to 5 domains at once. Mutually exclusive with domain. |
| `character_id` | integer | no | 0 | Character context (0 = controlled). Used by most domains. |
| `target_id` | integer | no | 0 | Secondary character for opinion, valid_cbs, diplomatic_range. |
| `title_id` | integer | no | 0 | Title context for titles, de_jure, election, neighbors, adjacency, development. |
| `trait_id` | integer | no | 0 | Trait ID for trait_compatibility domain. |
| `category` | integer | no | 0 | Building category filter for buildings domain (1095=constructible, 1825=special, 0=all). |
| `include_vassals` | boolean | no | false | Include vassal claims in claims domain. |
| `war_id` | integer | no | 0 | Specific war for war_score domain (0 = all cached). |
| `limit` | integer | no | 64 | Max results for war_declarations and scheme_inits domains. |
| `since_tick` | integer | no | 0 | Only return records after this tick for war_declarations and scheme_inits. |
| `scheme_type` | string | no | `""` | Filter scheme_inits by type (e.g. 'murder', 'fabricate_hook'). |
| `culture_id` | integer | no | 0 | Culture ID for innovation_progress, fascination_progress, research_speed (0 = character's culture). |
| `dynasty_id` | integer | no | 0 | Dynasty ID for dynasty domain (0 = character's dynasty). |

**Bridge dispatch:**

| Sub-option | Bridge action |
|------------|---------------|
| `active_wars` | `get_active_wars` |
| `adjacency` | `query_province_adjacency` |
| `alliances` | `query_alliances` |
| `armies` | `query_armies` |
| `borders` | `query_borders` |
| `buildings` | `query_building_types` |
| `call_to_arms` | `query_call_to_arms` |
| `character` | `query_character` |
| `characters` | `search_characters` |
| `claimants` | `query_title_claimants` |
| `claims` | `query_claims` |
| `conquest_targets` | `query_conquest_targets` |
| `council` | `query_council` |
| `council_tasks` | `query_council_tasks` |
| `county` | `query_county_details` |
| `de_jure` | `query_de_jure` |
| `debug_flags` | `query_debug_flags` |
| `dejure` | `query_dejure_progress` |
| `development` | `query_development_growth` |
| `diplomatic_range` | `query_diplomatic_range` |
| `dynasty` | `query_dynasty` |
| `election` | `query_election_state` |
| `event_probabilities` | `query_event_probabilities` |
| `faction_threat` | `query_faction_threat` |
| `factions` | `query_factions` |
| `fascination_progress` | `query_fascination_progress` |
| `goal` | `query_dejure_progress` |
| `heir_education` | `query_heir_education` |
| `hooks` | `query_hooks` |
| `incoming_schemes` | `query_incoming_schemes` |
| `innovation_progress` | `query_innovation_progress` |
| `law_groups` | `query_law_groups` |
| `law_options` | `query_available_law_changes` |
| `laws` | `query_realm_laws` |
| `levy_readiness` | `query_levy_readiness` |
| `lifestyle` | `query_lifestyle` |
| `map_graph` | `query_map_graph` |
| `marriage_candidates` | `query_marriage_candidates` |
| `memories` | `query_character_memories` |
| `neighbors` | `query_county_neighbors` |
| `opinion` | `query_opinion` |
| `opinion_breakdown` | `query_opinion_breakdown` |
| `partition_preview` | `query_partition_preview` |
| `realm` | `query_realm` |
| `relations` | `query_relations` |
| `research_speed` | `query_research_speed` |
| `scheme_inits` | `query_scheme_inits` |
| `schemes` | `query_schemes` |
| `succession` | `query_succession` |
| `succession_preview` | `query_title_succession_map` |
| `succession_simulation` | `query_succession_simulation` |
| `title_history` | `query_title_history` |
| `title_succession` | `query_title_succession` |
| `titles` | `query_title_hierarchy` |
| `trait_compatibility` | `query_trait_compatibility` |
| `traits` | `query_trait_db` |
| `vassal_loyalty` | `query_vassal_loyalty` |
| `war_declarations` | `query_war_declarations` |
| `war_score` | `query_war_score_breakdown` |
| `wars` | `query_wars` |

### `discover`

Pre-action validation and enumeration. List all registered interaction or decision types, check if a target would accept an interaction, or validate which casus belli are usable against a defender. Use before act() to verify feasibility. NOT for data reads — use query() for that.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `what` | enum(20) | yes | — | What to discover. trigger_result evaluates individual script trigger slots on an interaction to find which one rejects. war_validity checks all PDXScript preconditions (A1-A13, R1-R4) for declaring war on a target — r... |
| `decision_type` | string | no | `""` | For what=decision_why_blocked: the shield decision to decompose (negotiate_the_danelaw_decision | found_kingdom_decision). |
| `task` | string | no | `""` | For what=fabricate_targets: council task id to test (default 'task_fabricate_claim'). |
| `max_results` | integer | no | 40 | For what=fabricate_targets: cap on eligible counties returned (default 40). |
| `interaction_type` | string | no | `""` | For what=acceptance/trigger_result: the interaction to check. |
| `trigger_slot` | integer \| string | no | -1 | For what=trigger_result: which trigger slot to evaluate. -1 or 'all' evaluates all 7 slots. 0-6 or name (is_valid, is_shown, is_valid_showing_failures_only, can_be_sent_despite_rejection, can_send, auto_accept, on_aut... |
| `target_id` | integer | no | — | Target character for acceptance/valid_cbs/trigger_result checks. |
| `actor_id` | integer | no | 0 | Acting character (0 = controlled). |
| `character_id` | integer | no | 0 | For what=decisions/valid_decisions: character to evaluate decisions for (0 = controlled/player). |
| `trigger` | string | no | `""` | For what=evaluate_trigger: trigger to run. Either a script keyword like 'is_alive' / 'is_at_war' / 'has_raised_armies' (see re/findings/re_trigger_subclass_catalogue.md for the full alias table), or a raw mangled clas... |
| `scope` | object | no | — | For what=evaluate_trigger: scope dict. Today only {character: id} (and optionally {target: id}) is wired through to the bridge. Maps onto an interaction-context actor/target — adequate for character-scope triggers. |
| `interaction` | string | no | `""` | For what=interaction_validity: the interaction to check (e.g. 'declare_war', 'negotiate_alliance', 'send_gift'). Use the script name without the '_interaction' suffix; the wrapper appends it where the bridge expects it. |
| `cb` | string | no | `""` | For what=interaction_validity + interaction='declare_war': scope the verdict to a specific CB by name (e.g. 'claim_cb', 'de_jure_cb', 'individual_county_de_jure_cb'). NOTE: 'de_jure_county_cb' does NOT exist in this b... |
| `title` | string | no | `""` | For what=interaction_validity + interaction='declare_war': the title that owns the claim or is being targeted (e.g. 'd_sistan'). Informational only — the strict pipeline does not run per-title inner validation today; ... |

**Bridge dispatch:**

| Sub-option | Bridge action |
|------------|---------------|
| `interactions` | `list_interactions` |
| `decisions` | `list_decisions` |
| `decisions_annotated` | `list_decisions_annotated` |
| `valid_decisions` | `list_valid_decisions` |
| `acceptance` | `query_interaction_acceptance` |
| `valid_cbs` | `query_valid_cbs` |
| `valid_cbs_strict` | `query_cb_validity_strict` |
| `trigger_result` | `query_trigger_result` |
| `war_validity` | `pre_validate_war` |
| `trigger_slots` | `query_trigger_slots` |
| `evaluate_trigger` | `evaluate_named_trigger` |
| `interaction_validity` | `query_cb_validity_strict` |
| `war_targets` | `query_valid_war_targets` |
| `imprisonment` | `query_imprisonment_reasons` |
| `construction_check` | `construction_check` |
| `fabricate_targets` | `query_fabricate_targets` |
| `hireable_mercenaries` | `query_hireable_mercenaries` |
| `joinable_factions` | `query_joinable_factions` |

### `steer`

Read or adjust the AI steering controls: personality weights and subsystem toggles. This is the core mechanism — shape the native AI's behavior by tuning 10 personality axes (-100 to +100) and enabling/disabling 38 decision subsystems. Partial updates supported. No args = read current values.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `character_id` | integer | no | 0 | Character to steer (0 = controlled). |
| `personality` | object | no | — | Personality axis adjustments. Keys: honor, greed, rationality, energy, boldness, zeal, vengefulness, compassion, sociability, dominance. Values: -100 to +100. |
| `subsystems` | object | no | — | Subsystem toggle overrides. Keys: Decisions, GrantRevokeTitles, DeclareWar, Marriages, Council, etc. Values: true/false. |

**Bridge dispatch:**

| Sub-option | Bridge action |
|------------|---------------|
| `read personality` | `get_ai_personality` |
| `read subsystems` | `get_character_subsystems` |
| `write personality` | `set_ai_personality` |
| `write subsystems` | `set_character_subsystems` |

### `intent`

Manage the intent queue — scheduled actions that execute when the AI evaluation system naturally processes the character. Queue interactions, list pending intents, cancel by ID, or clear all.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `action` | enum(4) | yes | — | Intent queue operation. |
| `interaction_type` | string | no | `""` | For action=queue: interaction to schedule. |
| `actor_id` | integer | no | 0 | For action=queue: acting character (0 = controlled). |
| `target_id` | integer | no | 0 | For action=queue: target character (0 = AI chooses). |
| `max_age_ticks` | integer | no | 500 | For action=queue: expire after N ticks. |
| `request_id` | string | no | `""` | For action=cancel: the request_id to cancel. |

**Bridge dispatch:**

| Sub-option | Bridge action |
|------------|---------------|
| `queue` | `queue_intent` |
| `list` | `query_intents` |
| `cancel` | `cancel_intent` |
| `clear` | `clear_intents` |

### `execute_console_command`

Execute a CK3 debug console command. Escape hatch for actions the typed API cannot express. Bypasses interaction validation. Examples: 'AI.declare_war 30966' (AI.* delegation) or read-only diagnostics like 'charinfo 30966'. NOTE: state-granting verbs such as add_claim, add_pressed_claim, add_title_claim, add_gold, add_piety, give_title, add_trait, add_realm_law, add_doctrine are DENY-LISTED at the agent layer per the write-action legitimacy audit and will be Refused. Create claims via the legitimate fabricate path (act type="council_task", task="task_fabricate_claim"), not the console.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `command` | string | yes | `""` | Full console command string as typed in the game console. |

**Bridge dispatch:**

| Sub-option | Bridge action |
|------------|---------------|
| `(single call)` | `execute_console_command` |

### `pass_turn`

Explicitly choose not to act this cycle. Current personality weights and subsystem toggles are satisfactory. Always provide a reason so the decision log captures why no changes were needed.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `reason` | string | no | `""` | Brief explanation for inaction (logged to decision history). |

### `observe_v2`

Your situation report. Returns de-jure county counts for the titles nearest and widest around your holdings (de_jure), your current standing (gold, debt, stress, strongest faction, succession ETA, whether you are at war), the succession partition, your personality genome, and what changed since the last call (delta). realm_size is ABSENT because it counts titles, not counties; de_jure counts counties and self.counties_held counts yours. Uniform envelope {ok, tool, result, blocked?, actor?, game, delta?, warnings, errors, bridge_health}.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `character_id` | integer | no | 0 | Ruler to reason about (0 = controlled primary; REQUIRED in multi-actor). |
| `depth` | enum(2) | no | 'brief' | 'brief' = de_jure+standing+succession+posture+delta; 'full' adds self vitals, war/council brief. |

### `plan_war`

'What is my single best next war?' Returns a RANKED list of targets that are ALREADY declarable, ally-free, and goal-advancing — each row's declare_args goes to declare_war VERBATIM. You never name a casus_belli or hunt a claimant. Gates on the AUTHORITATIVE strict-CB inner verdict, not the over-optimistic outer filter (the 17-generation false 'structural wall'). Empty list ⇒ empty_reason names the single cause (NOT 'walled in').

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `character_id` | integer | no | 0 | Aggressor (0 = controlled primary; REQUIRED in multi-actor). |
| `goal_scoped` | boolean | no | true | True = only de-jure-goal-advancing targets; False = all declarable. |
| `ally_free_only` | boolean | no | true | Exclude targets that pull allies you'd fight. |
| `exclude_straddling` | boolean | no | true | Exclude wars expected to cross a death ETA. |
| `max_results` | integer | no | 8 | Top-N by score. |

### `declare_war`

Declare ONE war. Pass a plan_war row's declare_args verbatim (or the fields explicitly). The only irreversible high-stakes act with its own verb so it cannot fire by accident. BLOCKS until the war is confirmed PERSISTED (owns unpause-and-wait) — no sent_no_confirmation ambiguity, no paused-too-fast cull. Re-checks pressed-claim + target-state up front. Evaporated war ⇒ verdict:rejected with the exact gate. A straddles_succession war REQUIRES confirm_straddle=true.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `character_id` | integer | no | 0 | Aggressor (0 = controlled primary). |
| `declare_args` | object | no | — | A plan_war row's declare_args dict (preferred path). |
| `target_id` | integer | no | 0 | Escape-hatch target (required if no declare_args). |
| `cb` | string | no | `""` | Escape-hatch exact CB string. |
| `cb_title_id` | integer | no | 0 | Escape-hatch. |
| `claimant_id` | integer | no | 0 | Escape-hatch. |
| `confirm_straddle` | boolean | no | false | HARD GUARD — refuses a near-death-straddling war unless true. |

### `succession_plan`

'Will my realm pass through my heir's succession INTACT?' Returns one verdict: passes_intact:true, or splits with the cadet who breaks it and the single lever that WORKS (drive eligible-heir count toward 1) + a wont_work list. Reads the post-death simulation, never partition_preview.is_partition (which gave false reassurance that collapsed past runs).

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `character_id` | integer | no | 0 | The ruler whose death you plan around (0 = controlled primary). |

### `set_phase`

Assert your strategic posture by NAME; the engine sets the 10-axis genome and subsystem toggles for you. You do NOT hand-tune ten integers. phase=None reads current. Auto-re-pins the heir each reign. Subsystem toggles are advisory (the engine does NOT enforce CallAllies) — the genome does the work.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `phase` | enum(6) | no | — | Named posture, or omit to read current. |
| `character_id` | integer | no | 0 | Who to pin (0 = controlled primary; REQUIRED in multi-actor to assert). |
| `overrides` | object | no | — | Escape hatch — per-axis int overrides (discouraged; warns on CallAllies). |

### `do`

Perform one concrete non-war game action, OR enumerate what you can do. Enumerate: do(action_class='council_tasks') returns available actions each with the EXACT do_call to run it (copy the row; never guess) plus a blocked-why for each unavailable one. decisions enumerate REQUIRES a bounded candidates list (#64 freeze safety). Execute: do(intent='council_task', args={...}) runs ONE action, returning verdict:applied (PERSISTED) or a typed rejected. council_task/fabricate warn if the AI will REVERT. appoint is an intent here, not a discoverable interaction. On a refusal, `retryable` answers whether the call can work later; `severity` does not.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `intent` | enum(20) | no | — | The action to execute. Omit (with action_class) for enumerate mode. 'scripted_effect' (args.effect=<name>) RESOLVES a PDXScript scripted_effect template by name (safe, validated); its EXECUTE half (resolve_only=false)... |
| `action_class` | enum(10) | no | — | Enumerate mode only — the class of things to LIST. A DIFFERENT vocabulary from `intent`: these are plural nouns naming what can be listed, and no value here is ever an intent. In particular `men_at_arms` is an INTENT ... |
| `candidates` | array[string] | no | — | BOUNDED list for action_class='decisions' (#64 safety). |
| `character_id` | integer | no | 0 | Actor (0 = controlled primary; REQUIRED in multi-actor). |
| `args` | object | no | — | Intent-specific keys (see SPEC §2 intent table). |

### `inspect`

Deep, typed read of ONE subject (character, realm, council, succession, claims, wars, armies, factions, vassals, diplomacy, title, dynasty, laws, schemes, marriage, economy, research, county, opinion, opinion_breakdown, relations, hooks, title_hierarchy, title_succession, claimants, war_score, call_to_arms, development, incoming_schemes, lifestyle, heir_plan, guardian_candidates). heir_plan lists each child and eligible heir with martial, traits and the lever status available for them; education_focus and guardian read NULL because the bridge does not expose them. guardian_candidates lists in-court guardian prospects for a child with their martial. dynasty_id is null for landed rulers by design. borders/de_jure are KNOWN_BROKEN — return a warning, not data.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `subject` | string | yes | `""` | The subject to read (character, realm, council, succession, claims, wars, armies, factions, vassals, diplomacy, title, dynasty, laws, schemes, marriage, economy, research, county, opinion, opinion_breakdown, relations... |
| `character_id` | integer | no | 0 |  |
| `target_id` | integer | no | 0 |  |
| `title_id` | integer | no | 0 |  |
| `war_id` | integer | no | 0 |  |
| `title_key` | string | no | `""` | For the 'title' (de-jure) subject: a title key such as 'k_england'. |
| `dynasty_id` | integer | no | 0 |  |
| `include` | array[string] | no | — | Subject-specific sub-sections. |
| `max_results` | integer | no | 20 |  |

### `cbs`

'Show me EVERY casus belli I can use against THIS target, with tradeoffs.' Returns the full curated CB list for the (actor,target): each option's scope (county|duchy|kingdom|realm|faith_counties), what it GRANTS on victory, requirements_unmet, tradeoffs, and the dispatch-marshalling hint (needs_cb_title_id / needs_claimant / dispatch_supported). WEIGH options before declaring — e.g. tribal_subjugation (whole realm, once/lifetime) vs county_conquest (one county) vs duchy_conquest (a whole duchy). valid_options is the declarable subset; best is the widest-scope declarable one. Re-point a plan_war row's declare_args.cb at any option here. UNPAUSED engine read.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `target_id` | integer | yes | — | The defender character id (REQUIRED). |
| `character_id` | integer | no | 0 | Aggressor (0 = controlled primary; REQUIRED in multi-actor). |

### `session`

Game-flow, identity, and liveness control. You rarely call this — observe_v2/plan_war/do own pause-state for you. Use to: switch which ruler you control after a succession (control); confirm the controlled ruler is alive+landed (whoami); check bridge liveness if calls time out (health — 'hung' = STOP looping); or set speed/pause. Speed capped at 3; 4-5 while at war/near-succession is REFUSED. The human player can be a target but never an actor. experimental_flags lists the default-OFF EXPERIMENTAL-unvalidated engine flags; set_experimental(flag=<name>, enabled=bool) arms ONE for disposable-save validation ONLY (off_goal_veto / break_alliance / force_offensive_stance) — never rely on it on the live campaign until its live smoke passes.

**Parameters:**

| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `action` | enum(10) | yes | — | The session operation. |
| `character_id` | integer | no | 0 | For control/release. |
| `speed` | integer | no | 3 | 1-3 (4-5 refused while at war). |
| `enabled` | boolean | no | true | For auto_events AND set_experimental (arm/disarm the flag). |
| `disable_native_ai` | boolean | no | false | For control. |
| `flag` | enum(3) | no | — | For set_experimental — which EXPERIMENTAL-unvalidated default-OFF bridge flag to arm/disarm. |

---

## Bridge Action Catalogue

All `180` bridge action_type strings the dispatch router matches. Categories follow the comment banners in `bridge/src/dispatch_router.cpp`.

**Wrapping legend:**

- *MCP* — wrapped by an MCP tool (listed in the third column).
- *raw* — no MCP wrapper. Used directly by lab tools, the harness, or the bridge test suite.

### AI control

| Action | Wrapped by |
|--------|------------|
| `toggle_all_ai` | **not agent-callable** (no MCP tool wraps it) |
| `enable_all_ai` | **not agent-callable** (no MCP tool wraps it) |
| `send_interaction` | **not agent-callable** (no MCP tool wraps it) |

### Intent Queue (AI evaluation hook)

| Action | Wrapped by |
|--------|------------|
| `queue_intent` | `intent(queue)` |
| `query_intents` | `intent(list)` |
| `query_intent_results` | **not agent-callable** (no MCP tool wraps it) |
| `query_dispatch_result` | **not agent-callable** (no MCP tool wraps it) |
| `cancel_intent` | `intent(cancel)` |
| `clear_intents` | `intent(clear)` |
| `send_decision` | **not agent-callable** (no MCP tool wraps it) |
| `set_lifestyle_focus` | **not agent-callable** (no MCP tool wraps it) |
| `set_education_focus` | **not agent-callable** (no MCP tool wraps it) |
| `set_council_task` | **not agent-callable** (no MCP tool wraps it) |
| `construction_check` | `discover(construction_check)` |
| `add_construction` | **not agent-callable** (no MCP tool wraps it) |
| `construct_building` | **not agent-callable** (no MCP tool wraps it) |
| `declare_war` | **not agent-callable** (no MCP tool wraps it) |
| `declare_war_debug` | **not agent-callable** (no MCP tool wraps it) |
| `pre_validate_war` | `discover(war_validity)` |
| `set_cb_selector_override` | **not agent-callable** (no MCP tool wraps it) |
| `set_offgoal_veto` | **not agent-callable** (no MCP tool wraps it) |

### Console Command Passthrough

| Action | Wrapped by |
|--------|------------|
| `execute_console_command` | `execute_console_command` |
| `execute_scripted_effect` | **not agent-callable** (no MCP tool wraps it) |
| `execute_engine_effect` | **not agent-callable** (no MCP tool wraps it) |
| `break_alliance` | **not agent-callable** (no MCP tool wraps it) |
| `set_break_alliance_enabled` | **not agent-callable** (no MCP tool wraps it) |
| `set_vacant_fill_enabled` | **not agent-callable** (no MCP tool wraps it) |
| `set_force_offensive_stance` | **not agent-callable** (no MCP tool wraps it) |
| `force_offensive_stance` | **not agent-callable** (no MCP tool wraps it) |
| `call_war_create_core` | **not agent-callable** (no MCP tool wraps it) |
| `do_call_war_create_core` | **not agent-callable** (no MCP tool wraps it) |
| `raise_armies` | **not agent-callable** (no MCP tool wraps it) |
| `raise_raiders` | **not agent-callable** (no MCP tool wraps it) |
| `raid` | **not agent-callable** (no MCP tool wraps it) |
| `create_title` | **not agent-callable** (no MCP tool wraps it) |
| `execute_script_block` | **not agent-callable** (no MCP tool wraps it) |

### Pause / Speed

| Action | Wrapped by |
|--------|------------|
| `pause_game` | **not agent-callable** (no MCP tool wraps it) |
| `unpause_game` | **not agent-callable** (no MCP tool wraps it) |
| `set_speed` | **not agent-callable** (no MCP tool wraps it) |

### SDL Input Injection (ck-77pb)

| Action | Wrapped by |
|--------|------------|
| `inject_mouse_click` | **not agent-callable** (no MCP tool wraps it) |
| `select_observe_default` | **not agent-callable** (no MCP tool wraps it) |
| `enter_observe_mode` | **not agent-callable** (no MCP tool wraps it) |

### AI Subsystem Control

| Action | Wrapped by |
|--------|------------|
| `toggle_ai_subsystem` | **not agent-callable** (no MCP tool wraps it) |
| `set_character_ai` | **not agent-callable** (no MCP tool wraps it) |

### Per-Character Subsystem Control (Phase C)

| Action | Wrapped by |
|--------|------------|
| `set_character_subsystems` | `steer(write subsystems)` |
| `get_character_subsystems` | `observe(subsystems)`, `steer(read subsystems)` |

### Decision-budget governor (ck-6kvq.13, R6)

| Action | Wrapped by |
|--------|------------|
| `report_usage` | **not agent-callable** (no MCP tool wraps it) |
| `set_budget_config` | **not agent-callable** (no MCP tool wraps it) |
| `get_budget_state` | **not agent-callable** (no MCP tool wraps it) |

### The shared append-only board (ck-6kvq.12, R4)

| Action | Wrapped by |
|--------|------------|
| `board_post` | **not agent-callable** (no MCP tool wraps it) |
| `board_read` | **not agent-callable** (no MCP tool wraps it) |
| `board_admin` | **not agent-callable** (no MCP tool wraps it) |

### Bridge State Recovery

| Action | Wrapped by |
|--------|------------|
| `get_bridge_state` | `bridge` |
| `who_holds_the_lock` | **not agent-callable** (no MCP tool wraps it) |
| `get_offset_validation` | **not agent-callable** (no MCP tool wraps it) |

### Controlled Character Selection (Phase A)

| Action | Wrapped by |
|--------|------------|
| `set_controlled_character` | **not agent-callable** (no MCP tool wraps it) |
| `release_character` | **not agent-callable** (no MCP tool wraps it) |
| `list_watchlist` | **not agent-callable** (no MCP tool wraps it) |
| `get_player_character` | **not agent-callable** (no MCP tool wraps it) |
| `get_controlled_roster` | **not agent-callable** (no MCP tool wraps it) |

### Query Handlers (return JSON data)

| Action | Wrapped by |
|--------|------------|
| `query_character` | `observe(character)`, `query(character)` |
| `query_opinion` | `query(opinion)` |
| `query_opinion_breakdown` | `query(opinion_breakdown)` |
| `query_imprisonment_reasons` | `discover(imprisonment)` |
| `query_realm` | `query(realm)` |
| `query_dejure_progress` | `query(dejure)`, `query(goal)` |
| `query_succession` | `query(succession)` |
| `query_borders` | `query(borders)` |
| `query_claims` | `query(claims)` |
| `query_de_jure` | `query(de_jure)` |
| `query_county_details` | `query(county)` |
| `query_development_growth` | `query(development)` |
| `query_county_neighbors` | `query(neighbors)` |
| `query_province_adjacency` | `query(adjacency)` |
| `query_map_graph` | `query(map_graph)` |
| `query_wars` | `query(wars)` |
| `get_active_wars` | `query(active_wars)` |
| `query_war_db_scan` | **not agent-callable** (no MCP tool wraps it) |
| `query_war_score_breakdown` | `query(war_score)` |
| `query_war_stance` | **not agent-callable** (no MCP tool wraps it) |
| `set_war_stance_record_walk` | **not agent-callable** (no MCP tool wraps it) |
| `query_war_declarations` | `query(war_declarations)` |
| `query_war_history` | **not agent-callable** (no MCP tool wraps it) |
| `query_scheme_inits` | `query(scheme_inits)` |
| `query_marriage_observations` | **not agent-callable** (no MCP tool wraps it) |
| `query_council_tasks` | `query(council_tasks)` |
| `query_council` | `query(council)` |
| `query_character_memories` | `query(memories)` |
| `query_fabricate_targets` | `discover(fabricate_targets)` |
| `query_factions` | `query(factions)` |
| `query_faction_threat` | `query(faction_threat)` |
| `query_vassal_loyalty` | `query(vassal_loyalty)` |
| `query_schemes` | `query(schemes)` |
| `query_incoming_schemes` | `query(incoming_schemes)` |
| `query_armies` | `query(armies)` |
| `query_lifestyle` | `query(lifestyle)` |
| `query_interaction` | **not agent-callable** (no MCP tool wraps it) |
| `query_interaction_acceptance` | `discover(acceptance)` |
| `query_trigger_result` | `discover(trigger_result)` |
| `query_trigger_slots` | `discover(trigger_slots)` |
| `evaluate_named_trigger` | `discover(evaluate_trigger)` |
| `query_debug_flags` | `query(debug_flags)` |
| `query_relations` | `query(relations)` |
| `query_hooks` | `query(hooks)` |
| `query_available_cbs` | **not agent-callable** (no MCP tool wraps it) |
| `query_valid_cbs` | `discover(valid_cbs)` |
| `query_cb_validity_strict` | `discover(valid_cbs_strict)`, `discover(interaction_validity)` |
| `query_cbs_for_target` | **not agent-callable** (no MCP tool wraps it) |
| `query_cbs_for_targets` | **not agent-callable** (no MCP tool wraps it) |
| `query_alliances` | `query(alliances)` |
| `query_call_to_arms` | `query(call_to_arms)` |
| `query_diplomatic_range` | `query(diplomatic_range)` |
| `query_cb_catalog` | **not agent-callable** (no MCP tool wraps it) |
| `query_building_types` | `query(buildings)` |

### Phase-3 probes (ck-e58l.8)

| Action | Wrapped by |
|--------|------------|
| `probe_interaction_cascade` | **not agent-callable** (no MCP tool wraps it) |
| `probe_gate_4_slots` | **not agent-callable** (no MCP tool wraps it) |
| `list_pending_interactions` | **not agent-callable** (no MCP tool wraps it) |
| `tick_diff` | **not agent-callable** (no MCP tool wraps it) |
| `probe_war_orphans` | **not agent-callable** (no MCP tool wraps it) |
| `list_interactions` | `discover(interactions)` |
| `list_decisions` | `discover(decisions)` |
| `list_valid_decisions` | `discover(valid_decisions)` |
| `list_decisions_annotated` | `discover(decisions_annotated)` |

### New observation tools

| Action | Wrapped by |
|--------|------------|
| `get_tick_info` | `observe(tick)` |
| `declare_save_identity` | **not agent-callable** (no MCP tool wraps it) |
| `get_ai_personality` | `observe(personality)`, `steer(read personality)` |
| `set_ai_personality` | `steer(write personality)` |
| `get_ai_evaluations` | `observe(evaluations)` |
| `search_characters` | `query(characters)` |
| `query_heir_education` | `query(heir_education)` |
| `query_marriage_candidates` | `query(marriage_candidates)` |
| `query_title_hierarchy` | `query(titles)` |
| `query_title_claimants` | `query(claimants)` |
| `query_title_history` | `query(title_history)` |
| `query_title_succession` | `query(title_succession)` |
| `query_title_succession_map` | `query(succession_preview)` |
| `query_joinable_factions` | `discover(joinable_factions)` |
| `faction_action` | **not agent-callable** (no MCP tool wraps it) |
| `scheme_action` | **not agent-callable** (no MCP tool wraps it) |
| `move_capital` | **not agent-callable** (no MCP tool wraps it) |
| `set_obligation_level` | **not agent-callable** (no MCP tool wraps it) |
| `appoint_councillor` | **not agent-callable** (no MCP tool wraps it) |
| `maa_recruit` | **not agent-callable** (no MCP tool wraps it) |
| `maa_station` | **not agent-callable** (no MCP tool wraps it) |
| `maa_unstation` | **not agent-callable** (no MCP tool wraps it) |
| `hire_mercenary` | **not agent-callable** (no MCP tool wraps it) |
| `set_doctrine` | **not agent-callable** (no MCP tool wraps it) |
| `set_character_faith` | **not agent-callable** (no MCP tool wraps it) |
| `set_character_culture` | **not agent-callable** (no MCP tool wraps it) |
| `set_title_law` | **not agent-callable** (no MCP tool wraps it) |
| `query_hireable_mercenaries` | `discover(hireable_mercenaries)` |
| `query_valid_war_targets` | `discover(war_targets)` |
| `query_conquest_targets` | `query(conquest_targets)` |
| `query_cb_options` | **not agent-callable** (no MCP tool wraps it) |
| `query_election_state` | `query(election)` |
| `query_dynasty` | `query(dynasty)` |
| `query_levy_readiness` | `query(levy_readiness)` |
| `query_partition_preview` | `query(partition_preview)` |
| `query_succession_simulation` | `query(succession_simulation)` |
| `query_trait_db` | `query(traits)` |
| `query_trait_compatibility` | `query(trait_compatibility)` |

### Law Queries & Actions

| Action | Wrapped by |
|--------|------------|
| `query_realm_laws` | `query(laws)` |
| `query_law_groups` | `query(law_groups)` |
| `query_available_law_changes` | `query(law_options)` |
| `change_realm_law` | **not agent-callable** (no MCP tool wraps it) |
| `query_innovation_progress` | `query(innovation_progress)` |
| `query_fascination_progress` | `query(fascination_progress)` |
| `query_research_speed` | `query(research_speed)` |

### RE Validation Endpoints

| Action | Wrapped by |
|--------|------------|
| `memory_probe` | **not agent-callable** (no MCP tool wraps it) |
| `resolve_global` | **not agent-callable** (no MCP tool wraps it) |

### Event Queries

| Action | Wrapped by |
|--------|------------|
| `pending_events` | **not agent-callable** (no MCP tool wraps it) |
| `notifications` | **not agent-callable** (no MCP tool wraps it) |
| `query_pending_events` | **not agent-callable** (no MCP tool wraps it) |
| `query_event_probabilities` | `query(event_probabilities)` |
| `event_probabilities` | **not agent-callable** (no MCP tool wraps it) |

### Event Response

| Action | Wrapped by |
|--------|------------|
| `respond_event` | **not agent-callable** (no MCP tool wraps it) |

### Auto-respond events toggle

| Action | Wrapped by |
|--------|------------|
| `set_auto_respond_events` | **not agent-callable** (no MCP tool wraps it) |

### Pause/Speed via event_handler (new protocol names)

| Action | Wrapped by |
|--------|------------|
| `pause` | **not agent-callable** (no MCP tool wraps it) |
| `set_pause` | **not agent-callable** (no MCP tool wraps it) |

### Query dispatch (new tool schema names)

| Action | Wrapped by |
|--------|------------|
| `query` | **not agent-callable** (no MCP tool wraps it) |

---

## Unwrapped Bridge Actions

**NOT CALLABLE BY AN AGENT.** `92` bridge actions are not wrapped by any MCP tool. An agent cannot invoke anything in this list: there is no tool that reaches it, and naming one as an `intent` is refused. They appear here because this document describes the BRIDGE surface as well as the agent surface.

_(ck-ycim: an agent read `set_character_ai` from this document and called it 14 times in one episode. It is a real bridge verb and was never reachable.)_

For maintainers, each is a candidate for either:

- Promotion into an MCP sub-option (if useful to agents), or
- Keeping them as lab/test-only verbs (probes, diagnostics).

- `toggle_all_ai`
- `enable_all_ai`
- `send_interaction`
- `query_intent_results`
- `query_dispatch_result`
- `send_decision`
- `set_lifestyle_focus`
- `set_education_focus`
- `set_council_task`
- `add_construction`
- `construct_building`
- `declare_war`
- `declare_war_debug`
- `set_cb_selector_override`
- `set_offgoal_veto`
- `execute_scripted_effect`
- `execute_engine_effect`
- `break_alliance`
- `set_break_alliance_enabled`
- `set_vacant_fill_enabled`
- `set_force_offensive_stance`
- `force_offensive_stance`
- `call_war_create_core`
- `do_call_war_create_core`
- `raise_armies`
- `raise_raiders`
- `raid`
- `create_title`
- `execute_script_block`
- `pause_game`
- `unpause_game`
- `set_speed`
- `inject_mouse_click`
- `select_observe_default`
- `enter_observe_mode`
- `toggle_ai_subsystem`
- `set_character_ai`
- `report_usage`
- `set_budget_config`
- `get_budget_state`
- `board_post`
- `board_read`
- `board_admin`
- `who_holds_the_lock`
- `get_offset_validation`
- `set_controlled_character`
- `release_character`
- `list_watchlist`
- `get_player_character`
- `get_controlled_roster`
- `query_war_db_scan`
- `query_war_stance`
- `set_war_stance_record_walk`
- `query_war_history`
- `query_marriage_observations`
- `query_interaction`
- `query_available_cbs`
- `query_cbs_for_target`
- `query_cbs_for_targets`
- `query_cb_catalog`
- `probe_interaction_cascade`
- `probe_gate_4_slots`
- `list_pending_interactions`
- `tick_diff`
- `probe_war_orphans`
- `declare_save_identity`
- `faction_action`
- `scheme_action`
- `move_capital`
- `set_obligation_level`
- `appoint_councillor`
- `maa_recruit`
- `maa_station`
- `maa_unstation`
- `hire_mercenary`
- `set_doctrine`
- `set_character_faith`
- `set_character_culture`
- `set_title_law`
- `query_cb_options`
- `change_realm_law`
- `memory_probe`
- `resolve_global`
- `pending_events`
- `notifications`
- `query_pending_events`
- `event_probabilities`
- `respond_event`
- `set_auto_respond_events`
- `pause`
- `set_pause`
- `query`

