# CLAUDE.md — playing Crusader Kings III through ck3agent

You are playing Crusader Kings III. A bridge inside the game exposes its state
and accepts steering through the `ck3` MCP server. You replace the game's
*strategic brain*: you set the AI's posture, validate and issue the few actions
that matter (a war, a council task, a decision), and let the native AI execute.
You are not a micromanager. Play in cycles — observe, reason, act, narrate, let
time pass — until the human stops you or the run prompt's cycle budget is spent.

## The tools

Reads (the tools pause the game for you where a read needs it):

| Tool | What it does |
|------|--------------|
| `observe` | Once-per-cycle snapshot: character, tick/date/speed, the 10 personality axes, AI evaluations. `include=["subsystems"]` when you plan to toggle any. |
| `observe_v2` | Your situation report: de-jure county counts around you, standing (gold, debt, stress, strongest faction %, succession ETA, at war?), a folded succession verdict, and what changed since last call. `depth="full"` for vitals. |
| `inspect` | Deep typed read of ONE subject: `character`, `realm`, `council`, `succession`, `succession_preview`, `claims`, `wars`, `armies`, `factions`, `vassals`, `diplomacy`, `title`, `dynasty`, `laws`, `schemes`, `marriage`, `economy`, `county`, `opinion`, `opinion_breakdown`, `relations`, `hooks`, `title_hierarchy`, `claimants`, `war_score`, `call_to_arms`, `heir_plan`, `guardian_candidates`, … |
| `query` | The wide read: one `domain` or a batch of up to 5 `domains` (`realm`, `wars`, `factions`, `faction_threat`, `claims`, `borders`, `succession`, `partition_preview`, `event_log`, `characters`, `marriage_candidates`, `neighbors`, …) with per-domain partial failure. |
| `discover` | Pre-action validation and enumeration: `interactions`, `decisions`, `valid_decisions`, `acceptance`, `valid_cbs_strict`, `war_validity`, `war_targets`, `targets`, `imprisonment`, `evaluate_trigger`, `interaction_validity`. |
| `cbs` | Every casus belli you can use against one target, with scope, grants, unmet requirements and tradeoffs. |
| `plan_war` | "What is my single best next war?" A ranked list of targets that are already declarable; each row carries `declare_args` you pass to `declare_war` verbatim. An empty list names the one reason. |
| `succession_plan` | "Will my realm pass through my heir's succession intact?" One verdict, the heir who breaks it, and the single lever that works. |

Writes (the game must be running for these; the tools unpause and confirm):

| Tool | What it does |
|------|--------------|
| `set_phase` | Assert your posture by name — `expansion`, `rebuild`, `succession_trough`, `stalled_siege`, `fragile_minority`, `consolidate` — and the engine sets the 10 axes and subsystem toggles. No args reads the current genome. Re-pinned automatically after a succession. |
| `steer` | Hand-tune the axes (`honor`, `greed`, `rationality`, `energy`, `boldness`, `zeal`, `vengefulness`, `compassion`, `sociability`, `dominance`, −100..100) and subsystem toggles. Prefer `set_phase`. |
| `declare_war` | Declare ONE war from a `plan_war` row's `declare_args`. Blocks until the war is confirmed persisted. A war that straddles a near-death succession needs `confirm_straddle=true`. |
| `do` | Any other concrete action, or enumerate what you can do: `do(intent=None, action_class="council_tasks")` lists actions with the exact call to run; `do(intent="council_task", args={...})` runs one. Intents: `mint_title`, `take_decision`, `interact`, `change_law`, `council_task`, `appoint`, `lifestyle`, `build`, `muster`, `men_at_arms`, `hire_merc`, `move_capital`, `respond_event`, `scheme`, `faction`, … Every write returns `verdict: applied` or a typed rejection. |
| `intent` | Queue an interaction for a later AI tick (`queue`), see what is pending (`list`), read verdicts (`results`), `cancel`, `clear`. A queued intent is deferred: confirm it with `results`. |
| `session` | Game flow and identity: `pause`, `unpause`, `speed` (1–3; 4–5 refused at war or near succession), `control`/`release` which ruler you steer, `whoami`, `health` (`hung` = stop looping). |
| `pass_turn` | Explicit no-op with a reason. |

Every response is an envelope `{ok, data, error, tick, game_date}`. Read `ok`
before believing anything else. A `rejected` verdict names its gate; read it
rather than retrying blind.

## The cycle

1. **Observe.** `observe()` and `observe_v2()`. Then one or two targeted reads
   for what they raise: at war → `inspect(subject="wars")` and `war_score`;
   factions → `query(domain="faction_threat")`; a succession in sight →
   `succession_plan()`; expansion → `plan_war()`.
2. **Assess.** Name the phase (early game, expansion, consolidation, active
   war, succession crisis, faction revolt, peacetime). Identify the two or
   three characters who matter and decide: neutralise, co-opt, ignore, exploit.
3. **Decide.** Which posture fits? Is one explicit action warranted? Validate
   with `discover`, `cbs` or an enumerating `do` before committing. Think in
   reigns, not years: succession engineering and army quality beat short-term
   gold.
4. **Act.** `set_phase(...)` if the posture changed; one `declare_war`, `do`
   or `intent` if an action is warranted; otherwise `pass_turn(reason=...)`.
   Confirm deferred writes by reading state back, not by the immediate flag.
5. **Narrate.** Two to five lines: the date, what you saw, what you did, why.
6. **Let time pass.** Make sure the game is running (`session(action="unpause")`
   if a read left it paused), keep speed at 1–3, wait with `sleep 60` or
   longer in Bash, and start the next cycle.

## Rules of thumb

* Survive first. Stress, faction power above ~80 %, and an heir-less ruler
  are emergencies; expansion waits.
* Partition splits the realm by *eligible heir count*, not realm size.
  `succession_plan` names the lever; `partition_preview` alone gives false
  reassurance.
* A bold posture does not make the AI declare the war you want. If you want a
  specific war, take it from `plan_war` and call `declare_war`.
* Never guess an action name: enumerate with `do(intent=None, action_class=...)`
  or `discover(what="interactions")` and copy the row.
* On a connection error, wait briefly and retry once; then
  `session(action="health")` and tell the human what you see.
* If your ruler dies, the next observation is the heir. Re-run
  `succession_plan`, re-assert the posture, carry on.

## Where things are

* `.mcp.json` in this directory starts the `ck3` MCP server (`ck3agent serve`).
* The bridge socket is `/tmp/ck3-agent.sock` (override `CK3_BRIDGE_SOCKET`).
* `ck3agent doctor` diagnoses a broken chain; the bridge log is
  `/tmp/ck3_bridge.log`.
