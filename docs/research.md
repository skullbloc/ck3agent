# Research notes

What the public release can support in a study, how to set up conditions that
differ only in the model, and what is recorded.

## Prerequisites

- [Scripted runs](scripted-runs.md) working on your machine.
- One observer save per scenario, kept read-only.

## Steps: a treatment and a no-agent control

1. **Fresh game process per condition.** Quit and relaunch CK3 between runs.
   Bridge state, such as claims and personality writes, lives in the
   game process and does not reset on reload.
2. **Same start in both arms.** The same observer save, and the same ruler
   named by `CK3_AGENT_CHARACTER_ID=<id>`.
3. **Same clock rule in both arms.** A loaded save stays paused (the bridge
   never unpauses on its own); unpause explicitly and set the same speed. See [Speed](#speed).
4. **Treatment:** `ck3agent run` with the condition's play directory.
5. **Control:** launch with `CK3_PIPELINE_MODE=null`. Every handshake is
   downgraded to observe (`null_mode_observer_only`), so no client can steer
   and the pre-bind is refused. In this mode only, an observe session may pause,
   unpause and set speed. No observe-only command ships. Write one against `ck3agent/server/bridge_client.py`: handshake with `mode="observe"`, then `set_pause`/`set_speed` to apply the same clock rule as the treatment arm.

**Several rulers in one session.** One run can steer several rulers under one
clock: `CK3_AGENT_CHARACTER_ID=13902,12145` pre-binds both, the goal gives
each its posture, and every read and write names its ruler with
`character_id`. The action log stamps each write with its arguments, so the
`set_phase` per ruler is on record; the manifest's `rulers` lists the rulers
at start and at stop. Where a study needs several rulers' state over time,
not independent agents, this serves without a separate observe client: put a
per-ruler `observe(character_id=...)` in the prompt's cycle and read it from
the transcript. It is one model with one context steering all of them, so the
rulers are not independent arms. Two sessions in one game (one run per arm)
remain unsupported: they share the clock, the teardown pause and any
experimental flag either arms.

Neither "bridge attached, no client" nor the null-mode arm is a clean control:

- A loaded save stays paused: the bridge goes active on its first pass of the
  game's command thread and never unpauses on its own.
- Events of the engine's player character (`PLAYER_CHAR_ID`) depend on whether
  any client socket is open; observe sessions and clients still to handshake
  count. Open: a new event waits for the game's player (no tool answers
  one); the bridge does not pause for it. None open (no switch turns this
  off): while an event is pending it clears the event
  pause and unpauses the game only when that event pause stopped it (a pause
  you set stays), picking no option; events stay queued. Not checked: whether observer saves set `PLAYER_CHAR_ID`; if it is
  0, neither path acts.
- Steering outlives the client. The last personality write stays in game
  memory after disconnect. In `CK3_EPISODE_MODE=experiment` the claim is kept too.

Disclose the event handling with your results; no setting equalises it.

## Speed

Speed is not pinned in the public path.

- The bridge caps every speed write at 3. `session` refuses 4 and 5 while at war.
- The game UI can still set 4 or 5 by hand.
- `CK3_EPISODE_GAME_SPEED` acts only in `CK3_PIPELINE_MODE=pipeline`, which waits
  for a `declare_save_identity` call; `bridge_client.py` has it, no shipped tool calls it.

The manifest does not record speed. Put `session(action="speed", speed=N)` in the run prompt's first cycle and cite N; the `set_speed` lines in `action_log.jsonl` are the evidence. Ignore the bridge log's "resting game speed" line: that value applies only in `pipeline` mode. A slow model at high speed misses events between cycles.

## `set_phase` presets

`set_phase(phase)` writes these 10 personality axes, -100 to 100, for the
controlled ruler; `overrides={axis: int}` replaces single axes. A test in this
repository fails if this table drifts from the code.

| Phase | honor | greed | ration. | energy | bold. | zeal | venge. | compas. | social. | domin. |
|-------|------:|------:|--------:|-------:|------:|-----:|-------:|--------:|--------:|-------:|
| `expansion` | 20 | -20 | 50 | 90 | 85 | 40 | 30 | -10 | 30 | 60 |
| `rebuild` | 40 | 0 | 70 | 55 | 45 | 30 | 10 | 30 | 50 | 60 |
| `succession_trough` | 40 | 0 | 80 | 55 | 40 | 30 | 10 | 20 | 40 | 90 |
| `stalled_siege` | 10 | -20 | 40 | 100 | 95 | 50 | 40 | -20 | 20 | 70 |
| `fragile_minority` | 50 | 0 | 80 | 50 | 30 | 30 | 10 | 40 | 50 | 90 |
| `consolidate` | 40 | -10 | 70 | 60 | 35 | 40 | 20 | 30 | 50 | 70 |

Every call also writes, for the ruler:

- `set_character_ai`: re-enables the ruler's native AI tick.

No phase arms the off-goal war veto (AI-picked wars vetoed, `declare_war`
wars not): it is an experimental hook with no live validation, armed only by
`session(action="set_experimental", flag="off_goal_veto", enabled=true)`.
It is one process-wide flag for every controlled character of every session;
`character_id` is ignored, and it lasts until `set_experimental` disarms it or
the game exits. The bridge has no readback; `experimental_flags` shows only
this server's last confirmed write, so with several seats a null there does
not mean off.

## What is recorded

**Action log**, `action_log.jsonl`; path in
[files and paths](reference/files-and-paths.md). One JSON line per call of a
state-changing bridge verb, refused calls included (`observe_mode`,
`budget_exhausted`, ...). `result` tells them apart.

| Field | Contents |
|-------|----------|
| `ts` | wall-clock time |
| `game_date` | game date when the verb ran |
| `session_id` | the bridge session that sent it, `""` if none; not the Claude Code session id |
| `kind` | `action`, `management` for the logged management verbs, or `outcome` (below) |
| `verb` | bridge action name; `intent_outcome` on an `outcome` row |
| `verdict` | what happened to the action: `applied`, `rejected`, `pending` (queued for the game thread; its `outcome` row follows) or `unknown` |
| `poll` | what the bridge's wait for it did, separately: `answered`, `skipped_on_game_thread`, `timed_out` or `not_applicable` |
| `args` | request parameters |
| `result` | the bridge's reply |

A write the game thread runs later (a war declaration, a title, a faction, a
scheme, an interaction, a decision) is logged twice: the call, with
`verdict: pending` and the reply's `result.dispatch_id`; then, when the game
thread has run it, an `outcome` row with the same `dispatch_id`, the final
`verdict`, a `reason` when rejected, `intent_type` and `result_raw` (the
executor's report, e.g. `war_id`). Read the `outcome` row for whether the war
was made; the call row's `result.success` is the queueing receipt's and is
`false` for every queued write.

Logged as `management`: `pause_game`, `unpause_game`, `set_pause`, `set_speed`,
`set_character_ai`, `set_offgoal_veto`,
`set_faction_demand_response`.
Not logged: reads, and `set_controlled_character`, `release_character`,
`set_cb_selector_override`, `set_war_stance_record_walk`, `toggle_all_ai`, `toggle_ai_subsystem`,
`select_observe_default`, `enter_observe_mode`, `inject_mouse_click`, `report_usage`,
`set_budget_config`, `declare_save_identity`. `select_observe_default`,
`inject_mouse_click` and `set_cb_selector_override` exist only in debug builds.

The bridge log also prints lines from the experiment machinery (`clock-start
gate`, `Q5`, `X=UNARMED`); they are inert outside an episode and can be ignored.

**Handshake ack.** It carries `bridge_version`, `instance_layout` and
`effective_config`: the value of each `CK3_*` and `BRIDGE_*` variable below,
`""` when unset, except `CK3MAP_BIN` and `CK3_BRIDGE_BOOT_LOG`. Tokens are
reported as `<set>`.

**Run files** from `ck3agent run`: prompt, stream-json transcript, manifest
with model, session id, versions, usage, cost, and start and end date. See
[scripted runs](scripted-runs.md#what-a-run-writes). Usage and cost are
recorded only if Claude wrote its result event; see
[check it worked](scripted-runs.md#check-it-worked) for when it does not.

Nothing is sealed or signed. The manifest hashes only the save and the tool
schemas (`tool_surface.schemas_sha256`).

## Budget governor

`CK3_BUDGET_PER_AGENT_PER_YEAR=N` sets a per-session ceiling of N tokens,
output plus uncached input, per game year. A session over it gets
`budget_exhausted` on writes until the year rolls over. Enforcement is on
unless `CK3_BUDGET_ENFORCE=0`. With the variable unset, any session can set a
ceiling with `set_budget_config`; with it set, that call is refused. Usage
reaches the bridge only through a `report_usage` call. Nothing in this release
makes one, so the governor is inert unless you add a reporter.

## Bridge environment variables

Every variable the bridge reads by name in `bridge/src` and `bridge/include`,
tests excluded.
A test in this repository checks the list against string literals of the
forms `CK3*`, `BRIDGE_*`, `XDG_*`, `HOME`, `DISPLAY` and `TMPDIR`; a name of
another form would not be caught. Not listed: `LD_PRELOAD`, which the bridge
reads from the environment of each program the launcher execs, to stay preloaded. Tags:
**paths**, **experiment**, **diagnostic**. Unset means default behaviour.

| Variable | Tag | Effect |
|----------|-----|--------|
| `XDG_RUNTIME_DIR` | paths | base of the per-user directory; see [files and paths](reference/files-and-paths.md#what-goes-where) |
| `HOME` | paths | fallback for the per-user directory |
| `CK3_INSTANCE_LAYOUT` | paths | `user` by default; `legacy` restores the shared `/tmp` layout and a 0666 socket |
| `CK3_INSTANCE_ID` | paths | instance name; moves socket, log and telemetry under `<root>/<id>/` |
| `CK3_INSTANCE_ROOT` | paths | parent directory for named instances |
| `CK3_INSTANCE_DIR` | paths | episode directory fallback: `$CK3_INSTANCE_DIR/episode` |
| `CK3_BRIDGE_SOCKET` | paths | socket path |
| `CK3_BRIDGE_PORT` | paths | Windows only: the TCP loopback port to listen on; a free port by default (the bound address goes to `endpoint.json`) |
| `CK3_BRIDGE_LOG` | paths | bridge log path |
| `CK3_BRIDGE_LOG_ARCHIVE_DIR` | paths | where rotated bridge logs are gzipped |
| `CK3_TELE` | paths | telemetry directory, which holds the action log |
| `CK3MAP_BIN` | paths | map-graph artifact; default `<state dir>/ck3map.bin` (`/tmp/ck3map.bin` in the legacy layout) |
| `CK3_STRICT_BUILD_ID` | experiment | any value not starting with `1` (`0`, `false`, `yes`) loosens the BuildID refusal |
| `CK3_PIPELINE_MODE` | experiment | `legacy` by default; `pipeline` buffers writes and gives the bridge the clock; `null` is the observe-only control |
| `CK3_EPISODE_MODE` | experiment | `experiment` pins claims across disconnects and arms containment |
| `CK3_EPISODE_ID` | experiment | id stamped into logs and the ledger |
| `CK3_EPISODE_ARM` | experiment | arm label, stamped into the handshake and the launch record |
| `CK3_EPISODE_DIR` | experiment | session ledger directory |
| `CK3_EPISODE_GAME_SPEED` | experiment | resting speed, `pipeline` mode only |
| `CK3_EPISODE_HORIZON_START` | experiment | declared start date for the unavailability rule |
| `CK3_EPISODE_HORIZON_END` | experiment | declared end date for the unavailability rule |
| `CK3_EP_MAX_DETACH_S` | experiment | seconds a detached session may stay away before it is reported lost, default 900 |
| `CK3_UNAVAIL_EXCLUSION_FRACTION` | experiment | fraction of the horizon a seat may be unavailable, in (0, 1] |
| `CK3_UNAVAIL_ANOMALY_WALL_S` | experiment | wall-clock threshold for an unavailability anomaly |
| `CK3_UNAVAIL_X_DAYS` | experiment | retired; read only to report that it is ignored |
| `CK3_BUDGET_PER_AGENT_PER_YEAR` | experiment | token ceiling per session per game year |
| `CK3_BUDGET_ENFORCE` | experiment | `0`, `off` or `false`: record without refusing |
| `BRIDGE_NO_STAGE2_GROUP_TRIGGER` | experiment | `1` disables stage 2 of the CB validator |
| `BRIDGE_NO_STAGE3_KIND_RELATION_MODE` | experiment | `1` disables stage 3 of the CB validator |
| `BRIDGE_NO_PER_CB_OUTER_TRIGGER` | experiment | `1` disables the per-CB outer trigger check |
| `BRIDGE_DISTRUST_INNER_CB_VALIDITY` | experiment | `1` restores the old inner-validity handling |
| `CK3_BRIDGE_TRACE` | diagnostic | `1` logs per-query traces |
| `CK3_BRIDGE_STRICT` | diagnostic | `1` turns caught faults into crashes |
| `CK3_BRIDGE_BOOT_LOG` | diagnostic | `1` writes a boot log, in every build: to `<private base>/bridge_boot.log`, or `/tmp/ck3_bridge_boot.log` in the legacy layout. Unset, no boot log |
| `CK3_AI_WATCH` | diagnostic | `1` logs changes to AI-enabled flags |
| `CK3_LAW_TRIGGER_HARVEST` | diagnostic | set: re-enables law-trigger failure extraction, which is known to hang |
| `CK3_BRIDGE_SEND_TIMEOUT_MS` | diagnostic | socket send timeout |
| `DISPLAY` | diagnostic | X display for synthetic clicks |

The four `BRIDGE_*` variables are ablations of the war-validity checker.
Variables read by the Python side are in
[files and paths](reference/files-and-paths.md#environment-variables).

## Default-off writes

No bridge write is off by default in this release. `scheme_action`, behind
`do(intent="scheme")`, was validated live on 2026-10-18, and `faction_action`,
behind `do(intent="faction")`, on 2026-10-21; both are on. The bridge keeps the
gate for a write that has not been run crash-free on a live game, because a
fault in engine code can surface ticks later, outside the bridge's crash net. A
gated write called while off is refused with
`reason: "experimental_write_disabled"`, and the bridge names the file it reads
in `enabled_writes_file`. The MCP `do` reply carries the same path in
`blocked.fix`. Claude cannot arm a write; an operator can, for a controlled run.

To arm a gated write, add its bridge action name, one per line, to
`ck3-enabled-writes` in the bridge's state directory: `RUN/ck3-enabled-writes`
in the default layout (`RUN` is in [files and paths](reference/files-and-paths.md)),
and the instance directory for a named instance. Use the path in
`enabled_writes_file` when in doubt. The bridge re-reads the file on every call,
so arming and disarming need no relaunch. A line must match exactly;
`scheme_action_x` does not arm `scheme_action`. Delete the line to disarm.

`do(intent="scheme")` (bridge `scheme_action`) starts a scheme for an AI
ruler, or cancels one it owns. A start first asks the game's own start check
(the check behind the script trigger `can_start_scheme`: the scheme's `allow`
and `valid` triggers, an existing scheme or cooldown on that target), then
calls the engine's scheme constructor. A refusal comes back as
`scheme_start_refused` with `blocked.measured.engine_reasons`: each failing
trigger with its block (`allow` or `valid`), its script key, and the file and
line in the game's scripts that hold it. `discover(what="scheme_targets",
scheme=...)` runs the same check without starting anything, for one
`target_id`, a `candidates` list, or (neither) the ruler's court and vassals,
his liege and the liege's court, the holders of titles he claims and their
courts, and
`discover(what="scheme_types")` lists the game's schemes with what each targets.
The ruler must be AI-controlled. The call works with the game paused or
running (the bridge's drain on the game's command thread picks it up within a
pass). A call no drain picked up within 5 s is withdrawn and returns
`intent_timeout_cancelled_not_executed`; it does not run later. A start is
`applied` once `inspect(subject="schemes")` lists the new `scheme_id` with the
type and target asked. A cancel (`args={"action": "cancel", "scheme_id": <id>}`)
first reads the ruler's schemes: an id he does not hold (another character's
scheme, or one already gone) is refused `name_not_found` and nothing is sent.
It is `applied` once that scheme (its id, type, target and start date) is no
longer listed. The game gives a freed id to the next scheme it creates; when
that has happened, the reply names the new scheme under `result.id_reused_by`.
A scheme type the
game creates from a task contract (the `contract` category) or only starts with
an artifact is refused
`scheme_type_unsupported`.

`do(intent="faction")` (bridge `faction_action`) manages a vassal's factions
against its liege: `create` (`faction_type`), `join`, `leave` and
`press_demand` (`faction_id`). The bridge's own pre-checks run first (only the
leader presses, only a member leaves, a vassal needs a liege); their refusal
comes back as `faction_precondition`, said in plain words. Then the game's own
faction rules run, and their refusal comes back as `faction_refused` with the
game's sentence in `blocked.summary`. Every success is checked by a second read before the reply
says `applied`: `create`, `join` and `leave` by the member list of the liege the
faction is aimed at, `press_demand` by the leader's own faction read on a later
tick, where the game must now refuse a second press. When the second read
cannot see the faction, the reply is `write_unconfirmed`, not `applied`. A
pressed demand is an event the liege answers within days;
`inspect(subject="factions")` on the liege lists the answer. `do(action_class="factions_to_join")` lists
what the game allows now, with the reason for what it does not. A faction whose AI leader may
press its demand now reads `ultimatum: "ai_may_press"`, with his chance at each
monthly faction tick, and its `threat_level` is `critical`; the game keeps no
countdown to a due date. A liege sets
its standing answer to a faction's ultimatum with `do(intent="faction",
args={"action": "respond_demand", "response": "accept" | "refuse" | "ai"})`;
the bridge answers the demand event for that ruler with the game's own
accept or refuse option, and `inspect(subject="factions")` lists the answers
given.

## Limits

- Headless or batch runs on a server are unsupported in this release. The
  game needs a display and a Steam session.
- The bridge sees game state, not the RNG. Runs from one save diverge; use
  many saves rather than reruns of one.
- `claude -p` records tool calls and text, not hidden reasoning.

## Ethics and terms

Use single-player games only. Do not use Ironman or achievement runs, and
never multiplayer. You are responsible for complying with the Paradox EULA and
the Steam Subscriber Agreement. Every cycle calls a paid model. Report model,
token usage and cost with your results; the run manifest has them.

## Citing

Cite the exact version from `ck3agent version` through
[CITATION.cff](../CITATION.cff). Report the game build, the save, the speed
and the model.

## Check it worked

- The control run's bridge log shows `[pipeline] mode=null`.
- The treatment's manifest names the model and the same save hash as the control.

## If it fails

- A control session is refused in the control arm: expected in `null` mode.
  Use an observe session.
- Arms differ in start date: one of them was unpaused earlier than the other.
  Unpause both explicitly at the same step.
