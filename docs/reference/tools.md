# MCP tools

*generated when the release is built, from the server's tool registry and the bridge's dispatch table; do not edit*

The 13 tools `ck3agent serve` registers. Bridge verbs behind them: [bridge-actions.md](bridge-actions.md).

| Tool | Summary |
|------|---------|
| [`query`](#query) | One bridge read per domain, its reply under result (domains=[...]: up to 5, each under its own key; one that failed is an errors[] row, ok false). |
| [`discover`](#discover) | Check one action before sending it, or list the names a do or declare_war call needs. |
| [`steer`](#steer) | Read (no arguments) or write the 9 personality axes that steer what the game's AI chooses for this ruler: honor, greed, rationality, energy, boldness, zeal, vengefulness, compassion, sociability, integers -100 to 100. |
| [`pass_turn`](#pass_turn) | Choose not to act this cycle; the reason goes to the decision log. |
| [`observe`](#observe) | Your situation, first every cycle. |
| [`plan_war`](#plan_war) | Your best next wars, ranked: targets already declarable, ally-free and goal-advancing, each with declare_args for declare_war as is. |
| [`declare_war`](#declare_war) | Declare one war: pass a plan_war row's declare_args unchanged, or target_id, cb, cb_title_id (a key or an id) and claimant_id. |
| [`succession_plan`](#succession_plan) | Will your realm pass to your heir intact? |
| [`set_phase`](#set_phase) | Write a named posture: every personality axis of the phase's genome (the phase description gives each phase's leading values), plus the native AI's tick re-enabled. |
| [`do`](#do) | Act, or list what you can do. |
| [`inspect`](#inspect) | A typed read of one subject. |
| [`cbs`](#cbs) | Every casus belli against one target (target_id, required), by the game's check: valid_options whole (family, scope, grants, tradeoffs, cost, declare_args for declare_war), best the first's cb; blocked_now: refused now by debt, cost, truce or raised armies; the rest counted by first unmet requirement (invalid_by_gate), whole with include_invalid. |
| [`session`](#session) | Identity and liveness. |

## query

One bridge read per domain, its reply under result (domains=[...]: up to 5, each under its own key; one that failed is an errors[] row, ok false). inspect: composed subjects; observe: the cycle's view. Each argument applies to the domains it names. characters: the character browser (filter, sort, court_of, female; default 50 rows); marriage_candidates: landed unmarried rulers in range (default 30), rows paired with you or your child; neighbors: a county's neighbours, or with title_id 0 your capital's plus bordering_realms; conquest_targets: 10 counties outside your realm, conquerable first, each with declare_args; buildings: your title_id's slots and what can be built, priced; the feeds (war_declarations, scheme_inits, council_tasks, event_log) take since_tick (the previous reply's current_tick) and limit. A read the bridge declined is refused under its reason's gate. Over 8 KB rows are cut (truncated). title_id takes a key ('c_dublin') or an id; a key is echoed in result.titles_named [{title_key, title_id, name}].

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | enum: `adjacency`, `alliances`, `armies`, `buildings`, `call_to_arms`, `character`, `characters`, `claimants`, `claims`, `conquest_targets`, `council`, `council_tasks`, `county`, `de_jure`, `development`, `diplomatic_range`, `dynasty`, `election`, `faction_threat`, `factions`, `fascination_progress`, `heir_education`, `hooks`, `incoming_schemes`, `innovation_progress`, `law_groups`, `law_options`, `laws`, `levy_readiness`, `lifestyle`, `map_graph`, `marriage_candidates`, `memories`, `neighbors`, `opinion`, `opinion_breakdown`, `realm`, `relations`, `research_speed`, `scheme_inits`, `schemes`, `succession`, `title_history`, `title_succession`, `titles`, `trait_compatibility`, `traits`, `vassal_loyalty`, `war_declarations`, `war_score`, `wars`, `event_log`, `relative_power` |  | `""` | One domain. |
| `domains` | array[enum: `adjacency`, `alliances`, `armies`, `buildings`, `call_to_arms`, `character`, `characters`, `claimants`, `claims`, `conquest_targets`, `council`, `council_tasks`, `county`, `de_jure`, `development`, `diplomatic_range`, `dynasty`, `election`, `faction_threat`, `factions`, `fascination_progress`, `heir_education`, `hooks`, `incoming_schemes`, `innovation_progress`, `law_groups`, `law_options`, `laws`, `levy_readiness`, `lifestyle`, `map_graph`, `marriage_candidates`, `memories`, `neighbors`, `opinion`, `opinion_breakdown`, `realm`, `relations`, `research_speed`, `scheme_inits`, `schemes`, `succession`, `title_history`, `title_succession`, `titles`, `trait_compatibility`, `traits`, `vassal_loyalty`, `war_declarations`, `war_score`, `wars`, `event_log`, `relative_power`] |  | `null` | Up to 5 domains at once; not with domain. |
| `character_id` | integer |  | `0` | The character read (0 = yours). |
| `target_id` | integer |  | `0` | The other character: opinion, opinion_breakdown, diplomatic_range, relative_power. |
| `title_id` | integer or string |  | `0` | The title (a key or an id): titles, de_jure, election, claimants, title_history, title_succession, neighbors (a county), adjacency, development, buildings; conquest_targets: conquer toward it. |
| `trait_id` | integer |  | `0` | trait_compatibility. |
| `include_vassals` | boolean |  | `false` | claims: your vassals' claims too. |
| `war_id` | integer |  | `0` | war_score: one war (0 = every war). |
| `culture_id` | integer |  | `0` | innovation_progress, fascination_progress, research_speed (0 = yours). |
| `dynasty_id` | integer |  | `0` | dynasty (0 = yours). |
| `filter` | string |  | `""` | characters: all \| alive \| dead \| ruler \| independent \| ai_controlled (default alive); event_log: a comma list of war, scheme, council_task; buildings: a name part. |
| `sort` | enum: ``, `military`, `gold`, `realm_size`, `realm_counties`, `tier`, `age`, `diplomacy`, `martial`, `stewardship`, `intrigue`, `learning`, `prowess` |  | `""` | characters and marriage_candidates: the order (default military). realm_size = titles held directly; realm_counties = counties in the realm. |
| `sort_order` | enum: ``, `desc`, `asc` |  | `""` | characters (default desc). |
| `max_results` | integer |  | `0` | Row cap: characters, marriage_candidates, conquest_targets. |
| `court_of` | integer |  | `0` | characters: only those whose liege is this ruler (his courtiers and direct vassals). |
| `female` | boolean or null |  | `null` | characters: true women, false men, unset both. |
| `since_tick` | integer |  | `0` | Feeds: only records after this tick. |
| `limit` | integer |  | `0` | Feeds: at most this many records; title_history: holders. |
| `require_in_range` | boolean |  | `true` | marriage_candidates: only in diplomatic range (false: anywhere). |
| `verbose` | boolean |  | `false` | The bridge's full reply (more fields per row) for characters, wars, council, armies, realm, claims, conquest_targets. |
| `detail` | enum: ``, `brief`, `full` |  | `""` | map_graph: brief drops distances and the live overlay; conquest_targets: full gives every field per row. |

## discover

Check one action before sending it, or list the names a do or declare_war call needs. interactions / decisions: the names (decisions in a do call come only from here); decisions=[fragments] filters the names; decisions_annotated / valid_decisions (decisions=[names]): each with its unmet requirements and cost; acceptance (interaction_type, target_id): the send's own checks and his answer, nothing sent; interaction_validity (interaction, target_id, cb, title): the game's own check; valid_cbs / valid_cbs_strict / war_validity: casus belli checks; war_targets: candidates judged by cbs's read; construction_check (building, location_id, slot): the build button's verdict; imprisonment: the grounds against target_id; targets (action, title_id for claim): the valid targets for one action; decision_why_blocked (decision_type): per-clause pass or fail; scheme_types; scheme_targets (scheme, target_id or candidates, max_results default 20): whom a scheme can start against, by the game's check (a refused row lists the game's reasons, each {check, summary}). Lists with ready calls (mercenaries, fabricate-claim targets, factions) are do(action_class=...). actor_id or character_id is the actor (0 = yours). A title argument (title_id, cb_title_id, target_title_id, location_id) takes a key ('c_dublin') or an id; a key is answered with result.titles_named [{title_key, title_id, name}]. notes (topic): game rules and tool use, by topic; no topic: the index.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `what` | enum: `interactions`, `decisions`, `decisions_annotated`, `valid_decisions`, `valid_cbs`, `valid_cbs_strict`, `trigger_result`, `war_validity`, `construction_check`, `imprisonment`, `trigger_slots`, `interaction_validity`, `targets`, `decision_why_blocked`, `scheme_types`, `scheme_targets`, `acceptance`, `war_targets`, `notes` |  | `""` | What to check or list. |
| `interaction_type` | string |  | `""` | acceptance, trigger_result: the full *_interaction key. |
| `target_id` | integer |  | `0` | The other character. |
| `actor_id` | integer |  | `0` | The acting character (0 = yours). |
| `interaction` | string |  | `""` | interaction_validity: the short name (e.g. declare_war). |
| `cb` | string |  | `""` | interaction_validity for declare_war: the casus belli. |
| `title` | string |  | `""` | interaction_validity for declare_war: the claimed title (informational). |
| `action` | enum: ``, `back_claimant`, `banish`, `claim`, `declare_war`, `execute`, `execute_prisoner_interaction`, `imprison`, `imprison_interaction`, `press_claim`, `revoke_title`, `revoke_title_interaction`, `war` |  | `""` | targets: the action. |
| `title_id` | integer or string |  | `0` | targets with action=claim: the title. |
| `building` | string |  | `""` | construction_check: the building key. |
| `location_id` | integer or string |  | `0` | construction_check: the holding's barony or county. |
| `slot` | integer or null |  | `null` | construction_check: the building slot (read from the holding when omitted). |
| `max_results` | integer |  | `0` | scheme_targets: stop at this many valid. |
| `character_id` | integer |  | `0` | decisions, decisions_annotated, valid_decisions, scheme_targets: the character (0 = yours). |
| `decision_type` | enum: ``, `negotiate_the_danelaw_decision`, `negotiate_the_danelaw`, `found_kingdom_decision`, `found_kingdom` |  | `""` | decision_why_blocked: the two it models. |
| `decisions` | array[any] |  | `null` | decisions_annotated, valid_decisions: keys to evaluate; decisions: name fragments. |
| `scheme` | string |  | `""` | scheme_targets: the scheme key (from scheme_types). |
| `candidates` | array[any] |  | `null` | scheme_targets: character ids to check. |
| `topic` | enum: ``, `claims`, `war`, `economy`, `prestige`, `succession`, `titles`, `tools` |  | `""` | notes: the topic (empty: the index). |

## steer

Read (no arguments) or write the 9 personality axes that steer what the game's AI chooses for this ruler: honor, greed, rationality, energy, boldness, zeal, vengefulness, compassion, sociability, integers -100 to 100. The axes you pass are written and held; the rest keep their values. Any other key or value is refused and nothing is written. A write answers applied with result.readback, rejected or pending. set_phase writes a whole named posture.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `character_id` | integer |  | `0` | The ruler (0 = yours). |
| `personality` | object |  | `null` | {axis: integer}; omit to read. |

## pass_turn

Choose not to act this cycle; the reason goes to the decision log.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `reason` | string |  | `""` | Why you do not act. |
| `character_id` | integer |  | `0` | The ruler who passes (0 = the primary); one pass per ruler. |

## observe

Your situation, first every cycle. brief: de_jure (your held titles' de-jure county counts; next_title: the title above them and every requirement to create it), standing (gold, debt, stress, strongest faction %, succession ETA, at_war), wars (each war, your side, war_score when the game has computed it), money, military (strength, levies, men-at-arms), liege, council (seat, holder, task), succession (the verdict succession_plan gives), posture (the personality genome), news (what changed since your last observe: claims, wars started and ended, schemes ended, titles gained and lost, ruler, liege, heir and council changes, fabrication, wars at we_win, purse moves (what priced them), idle (what nothing uses), each with a `next` call; offers are incoming_proposals when served), since (records naming your realm; withheld counts the rest) and goal. Any other value is refused. Which tool: query, one raw read per domain (up to 5); inspect, a typed or composed subject; discover, whether one action would pass; do, ready calls and acts; plan_war, cbs, declare_war for wars; succession_plan; set_phase a posture, steer single axes; session. Every reply: {ok, tool, result, blocked, warnings[], errors[], bridge_health, game: {date, tick, paused, speed}, actor}; a write adds verdict (applied | rejected | pending) and pending {intent_id, drains_when}. ok=false carries blocked {gate, summary, retryable, fix?, measured?}: the gate names why, fix what to send instead. errors[] rows are reads that failed (that section is unread, never empty). Units: gold, prestige and piety are the game's own numbers (monthly_income is gold per game month); military_strength is the game's soldier-strength figure, comparable between realms; war_score runs -100 to 100, positive favours the attacker (favored_side names the side, my_side yours); game.date is the game date (Y.M.D), game.tick the bridge's frame counter.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `character_id` | integer |  | `0` | The ruler (0 = your primary). |
| `depth` | enum: `brief`, `full`, `news` |  | `"brief"` | brief; full adds self, war, council, AI evaluations; news: only news, since the last news read. |

## plan_war

Your best next wars, ranked: targets already declarable, ally-free and goal-advancing, each with declare_args for declare_war as is. Each row's casus belli passed the game's own check when read (precheck says when and what it does not cover); strength (yours and the target's, with allies, and their ratio), score (de-jure counties gained times that ratio), truce and affordability (the casus belli's base cost; the game prices it per target). Rows that fail are in excluded with their gate. A claim casus belli is offered on your own claim or on a courtier's pressed claim (claim_pressed). Empty: empty_reason names the commonest gate, and fix the argument that would admit those rows. goal_scoped false adds every duchy bordering your realm; toward (a title key) puts that title's duchies first. A refused declaration returns after a day or a write of yours.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `character_id` | integer |  | `0` | The aggressor (0 = your primary). |
| `goal_scoped` | boolean |  | `true` | true: claims, duchies you partly hold and toward; false adds every bordering duchy. |
| `ally_free_only` | boolean |  | `true` | Leave out targets whose allies you would fight. |
| `exclude_straddling` | boolean |  | `true` | Leave out every row while a war would run across your succession. |
| `max_results` | integer |  | `8` | Top rows by score. |
| `toward` | string |  | `""` | A de-jure title key to conquer toward (e.g. k_scotland). |

## declare_war

Declare one war: pass a plan_war row's declare_args unchanged, or target_id, cb, cb_title_id (a key or an id) and claimant_id. The war list is read back: applied when the war is listed, rejected with the gate (debt, the claim war's price for this target and a truce are checked before sending, raised armies read; the game's own refusal names its failed condition), pending when it is not listed yet (read inspect(subject='wars') before declaring again). A war that would run across your succession (you are 62 or older) needs confirm_straddle=true.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `character_id` | integer |  | `0` | The aggressor (0 = your primary). |
| `declare_args` | object |  | `null` | A plan_war row's declare_args, unchanged. |
| `target_id` | integer |  | `0` | Without declare_args: the defender. |
| `cb` | string |  | `""` | Without declare_args: the casus belli key. |
| `cb_title_id` | integer or string |  | `0` | Without declare_args: the title fought for (key or id). |
| `claimant_id` | integer or null |  | `0` | A claim casus belli's claimant; null or 0 = none. |
| `confirm_straddle` | boolean |  | `false` | Required true for a war across your succession. |

## succession_plan

Will your realm pass to your heir intact? outcome intact, splits (breaker: the heir who takes titles away) or undetermined (undetermined_reason); partition_forecast: who takes land (your eligible children) and whose share leaves the realm; the per-title map; death_estimate from health; levers with do calls: take_out_of_split (a son's vows or disinheritance, priced, the forecast after), designate_heir, disinherit, grant_title, revoke, title_law, marriage. observe's succession block is the same verdict.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `character_id` | integer |  | `0` | The ruler whose death you plan around (0 = your primary). |

## set_phase

Write a named posture: every personality axis of the phase's genome (the phase description gives each phase's leading values), plus the native AI's tick re-enabled. phase null reads the genome and names the phase it matches (null when none). result.lock_state is the bridge's word on the write (live: held against the AI's rewrites). Nothing re-applies it after a succession. Use steer for single axes.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `phase` | enum: `expansion`, `rebuild`, `succession_trough`, `stalled_siege`, `fragile_minority`, `consolidate` or null |  | `null` | The posture, or null to read: expansion (energy 90, boldness 85, rationality 50); rebuild (rationality 70, energy 55, sociability 50); succession_trough (rationality 80, energy 55, boldness 40); stalled_siege (energy 100, boldness 95, zeal 50); fragile_minority (rationality 80, energy 50, honor 50); consolidate (rationality 70, energy 60, sociability 50). |
| `character_id` | integer |  | `0` | The ruler (0 = your primary). |
| `overrides` | object |  | `null` | {axis: integer} written over the phase's genome; an unknown axis or a value out of range is refused. |

## do

Act, or list what you can do. Enumerate: do(action_class=..., candidates=[...]) answers available and unavailable rows, each with its do_call and, when unavailable, blocked with the gate. decisions needs candidates (names from discover(what='decisions')); formable_titles needs candidates (title keys) and gives each one's requirements, met or not: de-jure counties held vs needed, the price vs your purse, the game's check and its words. fabricate_targets: args={'within': key} or candidates=[county keys] judges them. interactions: candidates are name fragments. income_levers: the writes that bring gold or prestige. Execute: do(intent=..., args={...}) runs one action. Every write answers verdict applied (read back), rejected (blocked: the gate, and the game's words in blocked.detail when given) or pending (pending.drains_when next_pass: the verdict was not read in time; clock_advances: it ran and its effect needs game days). inspect(subject='pending') reads a pending write; do(intent='cancel_pending', args={'request_id': <intent_id>}) withdraws one not yet run. Peace: make_peace offers white_peace, surrender (you lose, always taken) or enforce_demands (you win); end_war ends a war you lead against enemy_id with the casus belli's outcome (we_win, white_peace, we_yield = you lose); check=true reads the open terms and prices each (gold, prestige, truce). Armies: the game's AI raises and disbands them, no tool does, and the game's declare-war refuses while they are raised. appoint only swaps two seated councillors. event_rule: a standing answer to your ruler's event (decline = the other of two options; news events_answered). gold_reserve: reserved gold, not a stop on AI buying (ai_veto is). ai_veto: stops your ruler's AI acts (news ai_vetoed). A refused write is re-sent after a day or a write of yours. A title argument (title_id, cb_title_id, target_title_id, location_id) takes a key ('c_dublin') or an id; a key is answered with result.titles_named [{title_key, title_id, name}].

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `intent` | enum: `mint_title`, `take_decision`, `interact`, `convert_faith`, `convert_culture`, `set_doctrine`, `change_law`, `council_task`, `appoint`, `lifestyle`, `set_education_focus`, `build`, `men_at_arms`, `hire_merc`, `move_capital`, `set_obligation`, `scheme`, `make_peace`, `call_ally`, `join_war`, `cancel_pending`, `faction`, `set_policy`, `answer_proposal`, `end_war`, `event_rule`, `gold_reserve`, `ai_veto`, `host_activity` or null |  | `null` | The action to run; null with action_class lists. |
| `action_class` | enum: `interactions`, `decisions`, `council_tasks`, `laws`, `fabricate_targets`, `mercenaries`, `factions_to_join`, `buildings`, `imprison_targets`, `formable_titles`, `men_at_arms`, `income_levers`, `activities` or null |  | `null` | What to list. |
| `candidates` | array[any] |  | `null` | decisions, formable_titles: names (required); fabricate_targets: title keys; men_at_arms: types; imprison_targets: ids. |
| `character_id` | integer |  | `0` | The actor (0 = your primary). |
| `args` | object or null |  | `null` | see below |

`args`:

```text
Intent-specific keys; values after = are the closed set the code takes. An enumerate row's do_call carries them; a missing one is refused with fix.rejected_fields. ARGUMENTS BY INTENT (read from the code):
  ai_veto: args.{what=create_title|law|mercenary|construction|ally_call|perk|decision|vassal_offer|men_at_arms|war|court_type|faction|legacy|call_in|activity|spending, keys, replace, op}
  answer_proposal: args.{proposal_id, answer=accept|decline|native}
  appoint: args.{seat, position_id, councillor_id, character_id}
  build: args.{location_id, building}
  call_ally: args.{war_id, ally_id}
  change_law: args.{law, scope=title, title_id}
  convert_culture: args.{culture, culture_id}
  convert_faith: args.{faith, faith_id}
  council_task: args.{task, target_title_id, target_character_id, seat}
  end_war: args.{terms=we_win|white_peace|we_yield, enemy_id, check}
  event_rule: args.{answer=option|decline|pay_if, option, currency=gold|prestige|piety, amount, else, op=set|clear|watch|list, event, debt}
  faction: args.{faction_id, liege_id, action=create|join|leave|press_demand|respond_demand, faction_type, response=accept|refuse|ai}
  gold_reserve: args.{amount, move}
  hire_merc: args.{company_id, check, confirm_debt}
  host_activity: args.{activity=feast|hunt|pilgrimage|leave}
  interact: args.{interaction, interaction_type, target_id, title_id, secondary_actor_id, secondary_recipient_id, war_id, send_options}
  join_war: args.{war_id, leader_id}
  lifestyle: args.{focus, perk}
  make_peace: args.{war_id, terms=white_peace|surrender|enforce_demands, check}
  men_at_arms: args.{action=recruit|station|unstation|disband, maa_type, location_id, regiment_id, check}
  mint_title: args.{title_key}
  move_capital: args.{title_id}
  scheme: args.{action=start|cancel, target_id, scheme, scheme_id}
  set_doctrine: args.{doctrine}
  set_education_focus: args.{focus}
  set_obligation: args.{vassal_id, level, obligation}
  set_policy: args.{on, policy=join_liege_wars|stay_out_of_liege_factions|proposals, class=marriage|alliance|call_to_war|peace|vassalage, answer=accept|decline|ask_me|native}
  take_decision: args.{decision}
```

## inspect

A typed read of one subject. title (title_key='k_scotland'): every de-jure county with its id, key, name, duchy, holder, the realm it is in (realm_ruler_id, name, strength), whether you control it, your claim on it and next_call (declare, fabricate or the gate); the de-jure duchies (who holds each, how many counties you control); the realms present; and the counties the title's creation needs. map: one line per county of a kingdom (title_key; default your primary title's) and its border, layers chosen (holders, casus belli, your wars' sieges and armies, strength, neighbours); ? = not read. heir_plan: each child and heir with the levers that exist for them. guardian_candidates: guardians for a child. threats: who can declare war on you now and with what (the game's check, rulers near you as attacker) and the vassalization rule. war_stance: each war's side, stance, occupation and a stall verdict. war_score: your wars, or war_id's. factions: as liege, each faction with its threat and the levers against its leader. pending: the writes still queued and the outcomes of the last 128 drained (a 129th drops the oldest); max_results lists the newest (default 20, at most 64). marriage: the same rows. The other subjects are one bridge read each. A title argument (title_id, cb_title_id, target_title_id, location_id) takes a key ('c_dublin') or an id; a key is answered with result.titles_named [{title_key, title_id, name}].

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `subject` | enum: `armies`, `call_to_arms`, `character`, `claimants`, `claims`, `council`, `county`, `development`, `diplomacy`, `dynasty`, `economy`, `factions`, `guardian_candidates`, `heir_education`, `heir_plan`, `hooks`, `incoming_schemes`, `laws`, `lifestyle`, `map`, `marriage`, `more`, `opinion`, `opinion_breakdown`, `pending`, `realm`, `relations`, `research`, `schemes`, `succession`, `threats`, `title`, `title_hierarchy`, `title_succession`, `vassals`, `war_score`, `war_stance`, `wars` |  | `""` | The subject. |
| `character_id` | integer |  | `0` | The character (0 = yours). |
| `target_id` | integer |  | `0` | opinion, opinion_breakdown: the other character. |
| `title_id` | integer or string |  | `0` | title_hierarchy, title_succession, claimants, development: the title (key or id). |
| `war_id` | integer |  | `0` | war_score: one war. |
| `title_key` | string |  | `""` | title, map: the de-jure title, e.g. k_scotland. |
| `dynasty_id` | integer |  | `0` | dynasty: the dynasty (0 = yours). |
| `max_results` | integer |  | `20` | Row cap. |
| `layers` | array[enum: `holders`, `cb`, `mil`, `str`, `adj`, `duchy`, `land`] |  | `null` | map: columns (default holders,cb,mil,str,adj). |

## cbs

Every casus belli against one target (target_id, required), by the game's check: valid_options whole (family, scope, grants, tradeoffs, cost, declare_args for declare_war), best the first's cb; blocked_now: refused now by debt, cost, truce or raised armies; the rest counted by first unmet requirement (invalid_by_gate), whole with include_invalid.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `target_id` | integer |  | `0` | The defender: an independent ruler (required). |
| `character_id` | integer |  | `0` | The aggressor (0 = yours). |
| `include_invalid` | boolean |  | `false` | Also list every invalid option whole. |

## session

Identity and liveness. whoami: the ruler you control (control can drift: confirm it is alive and landed); with none bound, ok=false with gate no_ruler_bound. control switches to another ruler after a succession (character_id required); add claims one more (the first stays primary: a call without character_id acts as it; with several, pass character_id on every call); release frees one. A character reserved for another seat is refused (reserved_for_another_seat). health: the bridge's liveness ('hung' = stop calling). experimental_flags lists the off-by-default engine flags; set_experimental (flag, enabled required) arms or disarms one. The clock: only this tool changes it; every other tool works paused or running. speed 1 to 3 (anything else is refused, at war by speed_refused_at_war); pause; unpause; wait unpauses, lets the clock run, pauses again and returns start_date and end_date: seconds (1 to 110) or game_days (stops at the first day at or past start plus game_days). A registered cadence or horizon refuses a wait beyond it (cycle_game_days_over_cadence, horizon_reached); in a scheduled run every wait ends at the cycle's end and speed is refused (speed_registered).

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | enum: `control`, `add`, `release`, `whoami`, `speed`, `pause`, `unpause`, `wait`, `health`, `experimental_flags`, `set_experimental` |  | `""` | The operation. |
| `character_id` | integer |  | `0` | control and add (required), release. |
| `speed` | integer |  | `3` | speed: 1 to 3. |
| `enabled` | boolean or null |  | `null` | set_experimental: arm or disarm (required). |
| `disable_native_ai` | boolean |  | `false` | control and add: also switch the game's own AI off for the ruler (it then neither declares wars nor sieges; leave it false). |
| `flag` | string |  | `""` | set_experimental: the flag (from experimental_flags). |
| `seconds` | integer |  | `60` | wait: seconds (1 to 110); with game_days, the longest single wait. |
| `game_days` | integer |  | `0` | wait: run until the date has advanced this many game days (0 = wait seconds once). |
