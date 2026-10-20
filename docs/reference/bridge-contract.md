# Bridge contract

What the bridge guarantees an agent that controls a CK3 ruler, what it does
not, and how to hand control back. Read this before writing an agent or a
bridge handler. Tool names are the 15 MCP tools in [tools.md](tools.md).
Bridge handler names, such as `query_*` and `set_controlled_character`, are
the verbs those tools call, listed in [bridge-actions.md](bridge-actions.md).

## Prerequisites

- [Architecture](../architecture.md) and the [glossary](../glossary.md).

## 1. Roles

The bridge is a library, not a controller: it hooks the AI tick, snapshots
state, dispatches actions and reports observations. It does not decide who
plays what.

| Party | Owns | Relies on |
|-------|------|-----------|
| Bridge | hook integrity, reply shapes, snapshot validity, AI toggles | nothing |
| Agent | strategy, action choice, recovery from `ok: false` | sections 3 and 4 |
| Operator (you) | launch, bind, release, kill | sections 2 and 5 |

The one process boundary is NDJSON over a Unix socket owned by your user,
mode 0600. `ck3agent config` prints the path. Nothing on the agent side is privileged.

## 2. Control modes

`session(action="control", character_id=N)` binds a ruler (bridge handler
`set_controlled_character`). The human player's own character is refused.

**Mode A, steering (default).** The ruler joins your roster and the native AI
keeps running. Personality and toggle writes change its disposition; the
engine still picks the marriages, wars and schemes.

**Mode B, full takeover**, with `disable_native_ai=true`. The bridge also
turns off the ruler's AI tick, so every move must be explicit through `do`,
`declare_war` or `intent`. For test scenarios only. `set_phase` re-enables
the AI tick on every call, so do not mix it with Mode B.

**Roster.** The bridge holds up to 10 controlled characters. The primary is
the one calls default to with `character_id=0`. `set_controlled_character`
switches a session's primary to the new character (a swap: the old one is let
go); with `add: true` it claims the character as one more of the session's
rulers and keeps the primary, and its reply carries `added: true`,
`controlled_primary` and `claimed_character_ids`. The bridge binds the heir
of a session's primary only. With none bound, `whoami`,
`observe_v2`, `inspect`, `plan_war`, `declare_war`, `cbs`, `succession_plan`,
`set_phase` and `do` return `no_ruler_bound`; `whoami` adds
`blocked.measured.unbound_reason` (`heir_is_player`, or `""` when the bridge
recorded none). `observe`, `query`, `discover`, `steer` and `intent` return
`ok: true` with the bridge's empty or declined reply, and
`discover(what="evaluate_trigger")` falls back to the bridge's primary, then to
the engine's player character. `control` without an id is refused (`missing_character_id`).

**Control refusals** (`blocked.gate`):

| Reason | Fix |
|--------|-----|
| `cannot_act_as_player` | the human's own character; bind an AI ruler |
| `character_not_found` | re-resolve the id with `query(domain="characters")`; ids change across saves |
| `character_dead` | bind the heir |
| `owned_by_other` | another session holds it |
| `watchlist_full` | all 10 slots are taken; release one first |
| `observe_mode` | the session is observe-only, as under `CK3_PIPELINE_MODE=null` |
| `no_session`, `invalid_character`, `ownership_divergence` | reconnect, check the id, or report the last as a bug |

In Mode B, check that the reply reports the AI disabled. If the bridge could
not find the ruler's AI controller, it stays in Mode A.

## 3. What the bridge guarantees

### 3.1 Replies

- Every reply is one complete JSON line, also on internal errors.
- Action replies carry `success` and echo `action`; failures add a `reason`
  string. New reasons may appear; existing ones are stable.
- The MCP server wraps replies in an envelope. See
  [architecture](../architecture.md#response-envelopes).

### 3.2 What "success" means

| Category | Examples | Success means |
|----------|----------|---------------|
| Direct | pause, speed, personality, toggles, control, release | done on the game thread before the reply |
| Pipeline | declare war, interactions, decisions, laws | handed to the engine, which may still reject it |
| Queued | `intent(action="queue")` | enqueued; confirm with `intent(action="results")` |

A pipeline success does not prove the game state changed. Validate first
(`discover(what="interaction_validity")`, `discover(what="valid_cbs_strict")`,
`cbs`) and read the state back after.

### 3.3 Query tiers

Classified in `bridge/src/snapshot.cpp` and served in `bridge/src/ipc_server.cpp`:

- **Tier 1, always safe:** bridge-internal reads such as bridge state, tick
  and roster. Immediate, paused or not.
- **Tier 2, live reads:** object-graph reads such as character, realm, wars,
  council, factions and succession. Served only while the game is paused,
  when no AI workers run. While time runs they return `game_unpaused`.
- **Tier 3, game thread:** engine calls, run at the next tick boundary.
  Latency depends on game speed.

### 3.4 Snapshot freshness

There is no per-tick snapshot; `bridge/include/snapshot.h` describes a
retired design. A Tier-2 read sees the paused game as it is. Opinion,
acceptance and trigger queries call game code through deferred slots and
take one or two ticks.

### 3.5 Reads that need the game running

These run on the game thread: `discover(what=...)` with `acceptance`,
`valid_cbs`, `valid_cbs_strict`, `trigger_result`, `evaluate_trigger`,
`interaction_validity`, `valid_decisions` and `decisions_annotated`. Called
while paused, the bridge unpauses for one drain and restores the pause flag;
it never changes the speed. A drain that does not finish in time returns
`cb_query_timeout` or `timeout_waiting_for_game_thread`.

`valid_cbs_strict` and `interaction_validity` are single-flight: allow 6 s,
and never call them in parallel. A second call gets `cb_query_busy`.

### 3.6 Character names

The game stores a localization key on each character (`E_thelred` for
Æthelred); only the game thread can turn it into a name. A read served from
the bridge's name cache carries `name`. A read that misses the cache carries
no `name`: it carries `name_key` (the key, for diagnosis — it is not a name)
and `name_pending: true`, and the id is queued for the game thread, which
resolves it on its next tick. A paused game runs no tick, so the first reads
of a session stay pending until time moves; the bridge never unpauses for a
name read. A character-bearing reply with neither `name` nor `name_pending`
had no readable name at all. The MCP server's actor echo (`controlled`,
`added`, the `actor` of every actor-bearing tool) carries the same two keys
when the name is pending. Show the note, never the key.

## 4. Observation

Nothing is pushed. Every observation is a request.

### 4.1 Polled hooks

Post-hooks write to ring buffers of 64 entries. Poll them with
`query(domain=...)` and `since_tick`. Older entries fall off.

| What | Fields |
|------|--------|
| marriage, betrothal, concubinage proposals | tick, actor, target, interaction, result |
| war declarations | tick, war id, attacker, defender, CB, claimant, titles |
| new schemes | tick, owner, target, type |
| council task assignments | tick, councillor, ruler, position, task |

Hooks fire only when the engine acts. A queued intent the engine never sends
shows up only in `intent(action="results")`.

### 4.2 Event popups

Only the engine player's (`PLAYER_CHAR_ID`) events are handled. With any
client socket open, observe or not yet handshaken included, a new one pauses
the game and appears in the pending-events read; it stays paused until a
control session answers with `do(intent="respond_event", ...)`. With none open
and `auto_events` on (the default), the bridge clears the event pause and
unpauses the game while an event is pending, even if you paused it. It picks
no option; the events stay queued. `auto_events` therefore does nothing while
you are connected.

### 4.3 Not observed

- **Deaths.** Not pushed. The bridge moves the seat to the new holder of the
  dead ruler's primary title; `whoami` shows it. If that is the human's
  character it refuses (`heir_is_player`) and leaves the seat unbound. The
  reverse is not guarded: if the human inherits the seat's living ruler, the
  seat keeps it, `whoami` returns `cannot_act_as_player` and writes are refused.
- **Title transfers** outside war: compare realm reads.
- **Opinion and faction changes:** poll.
- **Schemes against you:** poll schemes with your ruler as target.
- **Save and load:** the bridge state goes `dormant`, `reloading`, `active`.
  Check `session(action="health")` at the top of each cycle.

## 5. Handing control back

### 5.1 Release (agent or operator)

`session(action="release", character_id=N)` re-enables the ruler's AI tick,
removes it from the roster, and clears the primary if it was the primary.
The engine drives the ruler from the next tick. A control session cannot
release its last ruler: the reply is `last_claim_of_control_session` and
nothing changes. Switch instead with `session(action="control", character_id=M)`,
or end the session: outside an experiment episode the bridge destroys a
session when its socket closes and frees every ruler it held. Other release
refusals: `missing_character_id`, `not_in_roster`, `not_owned_by_session`,
`observe_mode`.

### 5.2 Operator override

The bridge does not authenticate beyond the socket owner. If the agent is
wedged:

| Symptom | Fix |
|---------|-----|
| game paused on an event | answer it: `do(intent="respond_event", ...)` |
| agent stuck | kill the agent; the bridge survives; a new session resumes from current state |
| full hand-back | `session(action="whoami")` lists your claims; release each |
| stop the world | `session(action="pause")` |

The bridge does not stop a second client from connecting. Both can write; the
last write wins.

### 5.3 After a release

The ruler's AI tick is back on. Personality values you wrote stay in memory
until the engine rewrites them, for example on a trait change or a lifestyle
perk. Subsystem toggles stay in bridge memory but have no effect once the
ruler is released.

## 6. Not guaranteed

- **Ordering.** Two queued intents run in order, but the engine may interleave
  its own decisions.
- **Rollback.** A failed follow-up does not undo an earlier step.
- **Replay across save and load.** A reload drops in-flight intents, clears
  observation buffers and resets the roster. Bind again after every reload.
- **Stable ids.** Character ids can change across saves; re-resolve by name.
- **Latency.** Tier-3 actions wait for a tick; a paused game holds them.
- **Determinism.** Steering changes the distribution of engine choices, not
  the outcome.
- **Crash recovery.** If the game crashes, treat the closed socket as final.

## 7. Reference

- Handler index: [`bridge/include/action_dispatch_internal.h`](../../bridge/include/action_dispatch_internal.h)
  and the table in `bridge/src/dispatch_router.cpp`.
- Bridge globals and session states: [`bridge/include/bridge.h`](../../bridge/include/bridge.h).
- The play protocol agents follow: [`ck3agent/protocol/CLAUDE.md`](../../ck3agent/protocol/CLAUDE.md).

## Check it worked

After `session(action="control")`, `session(action="whoami")` lists the ruler
under `my_claims`. After release, it does not.

## If it fails

A release that left the ruler bound is a contract violation. Open an issue
with the label `bridge-contract`, the `whoami` output, and the bridge log.
