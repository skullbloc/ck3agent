# Agent ↔ Character Contract + Escape Hatches

What the bridge guarantees a Claude-driven agent when it takes over a CK3 AI
character — and how an operator (or the agent itself) yanks the steering wheel
back. This is the spec the End-to-End LLM-as-AI-Character demo (ck-3hij9) is
validating; everything here is grounded in `bridge/src/` and `ck3agent/`.

Audience:

- **Agent authors** writing strategy on top of the 11-tool MCP surface — what
  you can rely on, what you must defend against.
- **Operators** running a session — how to interrupt, recover, or hand control
  back to the engine when the agent is wedged or simply done.
- **Bridge contributors** adding handlers — what semantics the rest of the
  stack already expects so new actions slot in correctly.

Companion docs:

- [`operator-workflow.md`](operator-workflow.md) — day-to-day runbook (launch,
  take control, observe). Overlaps with §2 below; this doc is the *semantics*
  reference, that one is the *procedure* reference.
- [`../ck3agent/CLAUDE.md`](../ck3agent/CLAUDE.md) — the agent-facing tool
  surface (the 11 MCP tools, the decision loop). Defines *what the agent
  calls*; this doc defines *what the bridge promises in response*.
- [`../bridge/POLLING_TICK_DESIGN.md`](../bridge/POLLING_TICK_DESIGN.md) — why
  every observation lives on the tick boundary.
- [`../bridge/include/snapshot.h`](../bridge/include/snapshot.h) lines 22–67 —
  the canonical query-safety tier classification this doc draws from.

---

## 1. Scope of the contract

The bridge is a **library**, not a controller. It hooks the AI tick, snapshots
state, dispatches actions, and surfaces observations — but it does not decide
*who* plays *what*. The agent is the consumer; the operator is the supervisor.

That separation defines the contract:

| Party | Responsibility | What it can rely on |
|---|---|---|
| Bridge | Hook integrity, response shapes, snapshot validity, AI-toggle correctness | (it's the implementation — nothing) |
| Agent | Strategy, action selection, recovery from `ok:false` | Everything in §3, §4 |
| Operator | Lifecycle (launch, take control, release, kill), interrupt authority | Everything in §2, §5 |

There is exactly one process boundary: NDJSON over `/tmp/ck3-agent.sock`
(mode `0666`, see [`../CLAUDE.md`](../CLAUDE.md) "Socket permissions"). Every
guarantee below is enforced across that boundary; nothing in the agent
process is privileged.

---

## 2. Control modes

Two control modes — picked by a single flag, with very different blast radii.
The selector is `set_controlled_character` (`bridge/src/dispatch_router.cpp:504`).

### Mode A — Steering (default, `disable_native_ai=false`)

The bridge marks `g_controlled_primary = char_id` and adds the character to
the watchlist. **Native CK3 AI keeps running unchanged.** Personality weights
and subsystem toggles that the agent writes via `set_ai_personality` /
`set_character_subsystems` are honored by the engine on subsequent ticks; the
engine still picks individual marriages, wars, schemes, and council
assignments — the agent only changes the *disposition*.

This is the design intent of ck3proj. Use Mode A unless a scenario explicitly
requires otherwise.

### Mode B — Full takeover (`disable_native_ai=true`)

The bridge does everything Mode A does, *plus* it calls
`g_ai_char_toggle(ai_ctrl, 0)` to write 0 to the character's
`AI_ENABLED` byte (`dispatch_router.cpp:561-568`). It then flips the global
`g_agent_mode = true`. Downstream gates in observation and dispatch code
branch on `g_agent_mode` to skip native-AI calls.

In Mode B the engine no longer chooses interactions for this character.
Every diplomatic move, war declaration, scheme launch, and event response
must be dispatched explicitly by the agent via `act()` or `intent()`.
Mode B exists for harness scenarios that need deterministic per-action
control (see `bridge/harness/scenarios/agent_as_player_*.py`); it is not
the default.

### Roster and primary

A single agent process can hold up to **10 characters** simultaneously
(`MAX_WATCHLIST` in `dispatch_router.cpp:572`). The `controlled_primary` is
the one most queries default to when `character_id=0` is passed; the rest of
the roster is observed but not necessarily the focus of any single decision
cycle.

Trying to add an 11th character is **silently dropped** — the call still
returns `success:true` with the existing roster, but the new character is
not added (`dispatch_router.cpp:577-579`). The log line
`"watchlist full (10/10), not adding %u"` is the only signal. Call
`release_character` to make space first.

### The response envelope

`set_controlled_character` returns:

```json
{
  "success": true,
  "action": "set_controlled_character",
  "controlled_primary": <char_id>,
  "ai_disabled": <bool>,
  "agent_mode": <bool>,
  "roster_size": <int>
}
```

Subtlety: `ai_disabled` can be `false` even in Mode B if the bridge could
not locate the character's AI controller (`!ai_ctrl` at line 562). The
character is still in the roster — but the engine continues to drive it.
For scenarios that *require* the AI off, the agent must verify
`ai_disabled == true` after the call, not trust the request alone.

### Failure modes

| `reason` | When | Recovery |
|---|---|---|
| `character_not_found` | `lookup_character(id)` returned null or a sentinel | Re-resolve via `search_characters` or `query_character` — IDs do change across saves |
| `character_dead` | `g_char_is_alive(ctrl)` returned false | Pick a living character |
| `missing_character_id` | `release_character` called with `character_id <= 0` | Pass a positive ID; revert path is only on `set_controlled_character` |
| (no error, but `ai_disabled:false` in Mode B) | AI controller missing | Mode B degraded to Mode A — character may be a non-ruler |

---

## 3. What the bridge guarantees the agent

### 3.1 Response envelope

Every dispatch over `/tmp/ck3-agent.sock` returns a JSON object on a single
line (NDJSON). The bridge guarantees:

- The response will be **well-formed JSON** even on internal errors. There is
  no partial-line failure mode where the agent sees half a response — the IPC
  server writes complete frames or closes the socket.
- The response will contain `"ok": <bool>` at top level (synthesized by
  `dispatch_action_with_response` when a handler did not produce one).
- For action handlers (mutating calls), the response will contain
  `"action": "<action_name>"` echoing the request — even on failure.
- On failure, the response will contain a `"reason": "<machine_string>"` key
  (e.g. `"character_not_found"`, `"character_dead"`, `"missing_character_id"`,
  `"not_implemented_pending_re"`). The string set is open — new failures may
  add new reasons — but existing reasons are stable.
- The MCP server layer wraps responses again in an outer envelope
  `{ok, data, error, tick, game_date}` before returning to the agent (see
  [`../ck3agent/CLAUDE.md`](../ck3agent/CLAUDE.md) §"MCP Tool Surface"). The
  agent only sees the outer envelope; the inner `success` / `reason` flow
  through as `data` / `error`.

### 3.2 Action verdict semantics

Actions split into three categories with different "success" meanings:

| Category | Examples | `success: true` means |
|---|---|---|
| **Direct calls** | `pause_game`, `set_speed`, `set_ai_personality`, `set_character_subsystems`, `set_controlled_character`, `release_character` | The mutation happened on the game thread before the response was sent. State is committed. |
| **Pipeline calls** | `declare_war`, `send_interaction`, `send_decision`, `change_realm_law` | The bridge dispatched the call into the engine pipeline. The engine still validates and may reject; check the response body for `war_id`/`sent` flags, or query downstream state. |
| **Queued intents** | `queue_intent` and the `intent(action="queue")` MCP path | The intent was enqueued. Execution is deferred to the next tick's serial section. Poll with `query_intent_results` / `intent(action="list")` to learn whether `executed` or `rejected`. |

The bridge does *not* guarantee that a Pipeline call which returns
`success:true` actually changed game state — only that the engine pipeline
accepted the call. The engine itself can silently reject (CB invalid,
character imprisoned, target unreachable) without surfacing that back through
the bridge.

For high-confidence verdicts on diplomatic/military actions, use
`discover(what="interaction_validity")` or `discover(what="valid_cbs_strict")`
*before* dispatching, then dispatch and confirm via the relevant `query_*`
afterward. Trust pre-validation + post-observation, not the dispatch return
alone.

### 3.3 Query safety tiers

The bridge classifies every query into one of three tiers
([`../bridge/include/snapshot.h:22-67`](../bridge/include/snapshot.h)):

- **TIER 1 — Always safe.** Reads from atomics or bridge-internal containers
  with no object-graph traversal. Served immediately at any time.
  - `get_bridge_state`, `get_tick_info`, `query_intents`, `list_watchlist`,
    `get_controlled_roster`, `get_player_character`.
- **TIER 2 — Snapshot-required.** Walks pointer chains through game objects
  (character → realm → vassals → titles → …). Unsafe during parallel AI
  execution. Served from a pre-formatted JSON snapshot captured each tick in
  the serial section.
  - All `query_character`, `query_realm`, `query_wars`, `query_council`,
    `query_factions`, `query_succession`, `query_schemes`, `query_claims`,
    `query_relations`, `query_lifestyle`, `get_ai_personality`,
    `get_ai_evaluations`, `list_interactions`, `list_decisions` calls.
- **TIER 3 — Game-thread only.** Calls engine functions that mutate state.
  Routed through the intent queue and executed during
  `drain_and_execute_intents()` at the next tick boundary.
  - All Pipeline calls in §3.2, plus `respond_event`, `toggle_ai_subsystem`,
    `set_character_ai`.

The agent does not need to know the tier — `dispatch_action_with_response`
routes correctly. But the tier determines **latency**:

- TIER 1: returns within microseconds.
- TIER 2: returns within microseconds (snapshot read) *but the data is up to
  one tick stale*.
- TIER 3: returns when the action lands on the game thread, which can be
  hundreds of milliseconds at low game speed (the bridge will pause the game
  if needed to make this prompt — see `is_paused()` and the auto-pause in
  `poll_and_notify_events()`).

### 3.4 Snapshot freshness

The Tier-2 snapshot is captured during `on_tick()`'s serial section, *before*
the engine launches parallel AI workers (`bridge/include/snapshot.h:14-19`).
Queries served from the snapshot reflect state as of "just after the
previous tick's mutations were committed, just before this tick's AI workers
mutate again."

Guarantees:

- A Tier-2 query made **at any point during a tick** returns the *same*
  snapshot until the next tick captures.
- Two Tier-2 queries in the same tick are **internally consistent** — they
  came from the same atomic capture.
- The snapshot is captured only for characters currently in the watchlist
  (`g_controlled_chars`). Querying outside the watchlist (e.g.
  `query_character(character_id=<random>)`) falls back to live reads, which
  *can* race with parallel workers. This is rare but real; prefer querying
  characters that are in the roster.

Caveat: `query_opinion`, `query_interaction_acceptance`, and
`query_trigger_result` call game functions and may execute *after* parallel
workers have started. The bridge serializes these via the deferred-query
slots (`AcceptanceQuery`, `CBQuery`, `TriggerQuery` in
`ai_eval_hook.cpp`); the IPC thread polls those slots and returns when the
game thread fills them. Expect 1–2 tick latency.

### 3.5 Game-thread `discover()` reads require UNPAUSED (audit B5, 20260612)

A subset of `discover()` operations evaluate live game code (triggers, CB
pipelines, acceptance) and are drained by `on_tick()`. They only resolve when
the game is **UNPAUSED**; called while paused they return
`timeout_waiting_for_game_thread` / `timeout`. The pause-then-observe loop must
`game(action="unpause")` before these:

- `discover(what="acceptance")`, `"valid_cbs"`, `"valid_cbs_strict")`,
  `"trigger_result"`, `"war_validity"`, `"trigger_slots"`,
  `"evaluate_trigger"`, `"interaction_validity"`.

By contrast, the *read snapshot* queries (`query(...)`) and the legibility
enumerators `discover(what="war_targets"|"imprisonment"|"construction_check")`
are Tier-2 inline and resolve **while PAUSED** (the agent's default observe
state). Several pure recorded-data monitor reads
(`query(war_declarations|council_tasks|debug_flags)`) were moved to Tier-2 in
the same audit so they also work paused.

The strict CB pipeline (`valid_cbs_strict` / `interaction_validity`) is a
**single-flight** worker: budget ≥ 6000 ms (a full sweep is ~98 CBs) and do
**not** call it concurrently — overlapping calls return `cb_query_busy`. The
MCP `discover` layer now defaults `timeout_ms=6000`.

---

## 4. Observability

The bridge exposes game-side events to the agent in two patterns:
**polled observations** (the agent calls a query, gets buffered records) and
**event-popup interception** (the engine pauses, the agent picks an option).

There is **no push notification channel**. Every observation comes via a
request/response cycle. The agent's loop must poll on a cadence consistent
with how often it can act (typically once per cycle = once per tick after
pause).

### 4.1 Polled observation hooks

Six post-hooks observe engine activity and write to ring buffers (max 64
entries each, mutex-protected). The agent polls them via dedicated queries:

| What | Hook source | Query | Record fields |
|---|---|---|---|
| Marriage / betrothal / concubinage proposals | `marriage_observation_hook.cpp` post-hook on `INTERACTION_TRY_SEND` | `query_marriage_observations` | `tick, timestamp_ms, actor_id, target_id, secondary_actor_id, secondary_recipient_id, interaction_name, result` (0=rejected, !=0=accepted) |
| War declarations | `war_declaration_hook.cpp` — polls WAR_DB count delta each tick | `query_war_declarations` | `tick, war_id, attacker_id, defender_id, cb_type_name, claimant_id, target_titles[], participants[]` |
| New schemes started | `scheme_init_hook.cpp` post-hook | `query_scheme_inits` | `tick, scheme_owner, target, scheme_type` |
| Council task assignments | `council_task_hook.cpp` post-hook on `SET_COUNCIL_TASK` | `query_council_tasks` | `tick, councillor_id, ruler_id, position_type, task_type, names` |
| AI CB selector output | `ai_cb_selector_hook.cpp` post-hook (gated by `g_in_bridge_war_intent`) | (consumed internally during war intent dispatch) | (not directly polled) |
| Interaction-cascade probe | `phase3_probes.cpp` (test/dev only) | `probe_interaction_cascade` | per-stage rejection breakdown |

Each query accepts an optional `since_tick` filter; without it the agent
gets the most recent up-to-64 records. The buffers do not retain history
indefinitely — if the agent goes more than ~64 events without polling a
given subsystem, older records fall off.

These hooks **fire only when the engine takes the action.** If the agent
queues an interaction via `queue_intent` and the engine later dispatches it,
the marriage observer (for example) will see the result. If the engine
declines to send the interaction, the observer won't see anything — the
intent's own `query_intent_results` is the source of truth for queued work.

### 4.2 Event popup interception

Character events (the modal popups CK3 normally shows the player) get a
different treatment because they block the game.

When an event fires for any character in the roster, the bridge:

1. **Auto-pauses** the game via `game_pause()`
   (`bridge/src/event_handler.cpp`, called from `on_tick`'s
   `poll_and_notify_events`).
2. Marks the event as "sent" in `g_sent_event_instance_ids` so it isn't
   re-notified next tick.
3. Adds it to the list returned by `query_pending_events` — the agent sees
   `events[]` with `event_id, instance_id, title, description, options[]`
   and `event_popup_active: true`.
4. Waits for the agent to call `respond_event` with the chosen option.
   That handler calls `select_event_option` on the game thread, which
   dispatches `CSelectEventOptionCommand`, clears event-pause, and
   `game_unpause()`s.

If the agent never responds, the game stays paused indefinitely — the
operator's escape hatch (§5.3) covers that case.

If the agent has set `game(action="auto_events", enabled=true)`, the bridge
auto-dismisses all pending events by selecting option 0 each tick
(`auto_respond_pending_events()` in `event_handler.cpp`). This is the
"passthrough" mode used when the agent doesn't care about flavor events.

### 4.3 What the agent does NOT learn about

The bridge does not currently observe (no hook, no query):

- **Death events.** The agent must poll `query_character(<roster>)` and
  notice `alive: false`. Same for the controlled primary — if it dies, the
  bridge does *not* automatically remove it from the watchlist or pick a
  successor. Either the operator or the agent's own logic must call
  `release_character` and `set_controlled_character(<new_id>)`.
- **Title transfers** outside of war (e.g. inheritance ticks). The agent
  must compare successive `query_realm` snapshots.
- **Opinion changes.** Polled via `query_opinion` / `query_relations`; no
  push.
- **Faction membership changes.** Polled via `query_factions`.
- **Schemes targeting the agent's character.** The schemer's own
  `scheme_inits` observation does fire, but a comprehensive
  "schemes against me" view requires polling `query_schemes` with the
  controlled character as target.
- **Save/load events.** The bridge tracks `SessionState` transitions
  (DORMANT / ACTIVE / RELOADING) on `get_bridge_state`, but does not send
  unsolicited notification. The agent should check `get_bridge_state` at
  the top of each cycle and abort the cycle if not ACTIVE.

This is the "polled, not pushed" baseline. Future work (ck-3hij9 epic) may
introduce a server-initiated channel; today, the agent must poll.

---

## 5. Escape hatches

Three escape paths exist for the operator (or the agent itself) to return
control to the engine. They are nested in scope from "graceful" to
"yank-the-cable."

### 5.1 Graceful release (agent or operator)

`release_character` (`bridge/src/dispatch_router.cpp:604`) is the symmetric
counterpart to `set_controlled_character`. It:

1. Clears the personality lock for that character (best-effort —
   `try_lock`; if the personality mutex is contended, the clear is deferred
   and logged).
2. **Re-enables native AI** via direct memory write to
   `ai_ctrl + AI_ENABLED = 1` — a pure memory write that does not call any
   game function and therefore cannot hang (`dispatch_router.cpp:631-649`).
3. Removes the character from `g_controlled_chars`.
4. If the released character was the `controlled_primary`, resets primary
   to 0.
5. If the watchlist is now empty, flips `g_agent_mode = false` (drops the
   global takeover gate).

Response:

```json
{
  "success": true,
  "action": "release_character",
  "released_id": <char_id>,
  "controlled_primary": <new_primary_or_0>,
  "agent_mode": <false_if_roster_emptied>,
  "roster_size": <updated_size>
}
```

This is the path the agent itself should use when it decides to step away
from a character — e.g., the character died, the strategy goal is complete,
or the agent is switching primaries. The character resumes being driven by
native CK3 AI from the very next tick.

### 5.2 Revert to engine player (operator)

`set_controlled_character` with `character_id <= 0` is the "you're done,
hand the wheel back" signal (`dispatch_router.cpp:508-521`).

```bash
echo '{"action":"set_controlled_character","character_id":0}' \
  | socat -t5 - UNIX-CONNECT:/tmp/ck3-agent.sock
```

This:

- Resets `g_controlled_primary = 0`. Subsequent queries with
  `character_id=0` now resolve to the engine's player character (whoever
  the save was last controlling).
- **Does NOT** clear the watchlist or re-enable native AI for characters
  that were Mode-B-controlled. The agent still has those characters under
  takeover.

For a full hand-off, the operator must call `release_character` for each
character in the roster *before* this revert. The two-step is intentional —
the revert is a fast "stop targeting" signal; release is the deeper "give
them their AI back."

`list_watchlist` enumerates everything currently held so the operator can
sweep:

```bash
echo '{"action":"list_watchlist"}' \
  | socat -t5 - UNIX-CONNECT:/tmp/ck3-agent.sock
```

### 5.3 Operator override — bypass the agent entirely

If the agent is wedged (deadlocked on an event popup, stuck in a reasoning
loop, disconnected), the operator can talk to the bridge directly. The
bridge does not authenticate; any process with socket access can dispatch.
This is *the* failsafe.

Common interventions:

| Symptom | Operator command |
|---|---|
| Game paused, event popup waiting on agent | `echo '{"action":"set_auto_respond_events","enabled":true}'` — auto-dismiss to clear, then `unpause_game` |
| Agent stuck mid-cycle | Kill agent process; the bridge survives. New agent connects and resumes from current state. |
| Need full hand-back to native | Loop `list_watchlist` → `release_character` for each → `set_controlled_character(0)` |
| Need to suspend the world | `pause_game` (Tier 1 — instant) |

The bridge does not enforce a single-agent-connection invariant — if a
second client connects while the first is still attached, both will see
broadcasts and both can dispatch actions. The operator-workflow doc
recommends against this; the contract here only states that the bridge does
not prevent it. If two agents fight over the same character, the last
action wins.

### 5.4 What surviving a release looks like

After `release_character`, the next AI tick:

- The engine reads `AI_ENABLED = 1` and resumes scoring decisions for that
  character.
- Any personality weights the agent set are still in memory, but without
  the personality lock the engine's own getters now win subsequent reads.
  The next time the engine writes a personality value (e.g. trait change,
  lifestyle perk), the override is gone.
- Subsystem masks set via `set_character_subsystems` *do* persist (they
  live in bridge-internal memory, not the engine's AI controller), but the
  bridge only consults them when `g_agent_mode == true`. Once `agent_mode`
  drops, they have no effect on the engine.

In short: a released character is **fully restored** to engine control on
the next tick, with no residue. This is by design — Mode A's promise that
"the engine still drives" is the same promise that lets the operator walk
away cleanly.

---

## 6. What the bridge does NOT guarantee

A clear list, because the negatives are as load-bearing as the positives:

- **No causal ordering across actions.** Two `queue_intent` calls
  enqueued in a single agent cycle are executed on the next tick in
  insertion order, but the engine may interleave them with its own
  decisions. The agent should not assume "queue X then queue Y" produces
  the same world as "queue Y then queue X" if both target the same
  character.
- **No transactional rollback.** If a Pipeline call's first stage
  succeeds (e.g. war is declared) and a later stage in the same agent
  cycle fails (e.g. a follow-up interaction is rejected), the bridge does
  not unwind. The agent owns recovery.
- **No replay across save/load.** When the bridge sees a `RELOADING` →
  `ACTIVE` transition (`get_session_state` in
  [`../bridge/include/bridge.h:603-613`](../bridge/include/bridge.h)),
  it drops all in-flight intents, clears observation buffers, and resets
  the watchlist roster. The agent must re-issue `set_controlled_character`
  after every save reload.
- **No guarantee that `character_id` is stable across saves.** IDs come
  from the save's character database; the same character can have
  different IDs after a save/reload chain. The agent should re-resolve via
  `search_characters` or persistent name lookup after a reload.
- **No guaranteed action latency.** TIER 3 actions queue for the next
  tick; at game speed 5 with a fast machine that's ~80ms, but a paused
  game holds the action until unpause. The agent must not depend on
  wall-clock latency.
- **No native-AI determinism in Mode A.** Personality and subsystem
  steering changes the *distribution* of engine decisions, not the
  specific outcome. The same steering at the same tick will not always
  produce the same engine choice (the engine RNG advances).
- **No automatic recovery from a crashed game.** If CK3 segfaults, the
  bridge's IPC thread continues serving for a few seconds before the
  socket closes; queries during that window may return stale snapshot
  data. The agent should treat connection errors as authoritative — when
  the socket closes, the world is gone.

---

## 7. Reference

### 7.1 Response envelope schema (bridge → MCP server → agent)

The bridge writes (inner):

```json
{
  "ok": <bool>,           // top-level success/failure
  "success": <bool>,      // action-handler echo (same value)
  "action": "<name>",     // for action handlers; absent for raw queries
  "reason": "<string>",   // present on failure
  ...handler-specific fields...
}
```

The MCP server wraps (outer) before returning to the agent:

```json
{
  "ok": <bool>,
  "data": {<inner-without-ok>},  // present on success
  "error": {"code": "<string>", "message": "<string>"},  // present on failure
  "tick": <int>,
  "game_date": "<YYYY.M.D>"
}
```

### 7.2 Canonical handler index

The authoritative list of every action and query the bridge dispatches is
[`../bridge/include/action_dispatch_internal.h`](../bridge/include/action_dispatch_internal.h)
(query handlers around lines 202–258, action handlers around lines 265–333,
observation handlers around lines 297–326). Treat that header as the
machine-readable surface; this doc is the semantics layer.

### 7.3 Key globals

State the agent and operator can rely on being present (from
[`../bridge/include/bridge.h:619-644`](../bridge/include/bridge.h)):

| Global | Type | Meaning |
|---|---|---|
| `g_initialized` | atomic<bool> | True once `bridge_init()` completed |
| `g_agent_mode` | atomic<bool> | True when ≥1 Mode-B character is held |
| `g_auto_respond_events` | atomic<bool> | True when event auto-dismiss is on |
| `g_tick_id` | atomic<uint64> | Monotonic tick counter |
| `g_controlled_primary` | atomic<uint32> | Primary character ID (0 = engine player) |
| `g_controlled_chars` | vector<uint32> + mutex | Full watchlist roster |

The agent reads all of these indirectly through `get_bridge_state` and
`list_watchlist`. The operator reads them the same way over the socket.

### 7.4 Filing escape-hatch issues

If the contract is violated — a release didn't actually re-enable native AI,
a Tier-2 query returned cross-tick-inconsistent data, an action returned
`success:true` but had no effect — file a bead with label `bridge-contract`
and reference this doc. The fix belongs in `bridge/src/`, not in the agent.
