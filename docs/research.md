# Research notes

What the public release can support in a study, how to set up conditions that
differ only in the model, and what is recorded.

## Prerequisites

- [Scripted runs](scripted-runs.md) working on your machine.
- One observer save per scenario, kept read-only.

## Steps: a treatment and a no-agent control

1. **Fresh game process per condition.** Quit and relaunch CK3 between runs.
   Bridge state, such as claims, toggles and personality writes, lives in the
   game process and does not reset on reload.
2. **Same start in both arms.** The same observer save, and the same ruler
   named by `CK3_AGENT_CHARACTER_ID=<id>`.
3. **Same clock rule in both arms.** Set `CK3_SUPPRESS_AUTO_UNPAUSE=1`, then
   unpause explicitly and set the same speed. See [Speed](#speed).
4. **Treatment:** `ck3agent run` with the condition's play directory.
5. **Control:** launch with `CK3_PIPELINE_MODE=null`. Every handshake is
   downgraded to observe (`null_mode_observer_only`), so no client can steer
   and the pre-bind is refused. In this mode only, an observe session may pause,
   unpause and set speed. No control harness ships; use your own observe client.

Neither "bridge attached, no client" nor the null-mode arm is a clean control:

- Until the first game tick, the bridge unpauses a paused game every ~3 s.
- Events of the engine's player character (`PLAYER_CHAR_ID`) depend on whether
  any client socket is open; observe sessions and clients not yet handshaken
  count. Open: the bridge pauses on a new event and waits for `respond_event`,
  which observers are refused, as is `auto_events`. None open, auto-events on
  (the default): while an event is pending it clears the event pause and
  unpauses the game, even a pause you set, picking no option; events stay
  queued. Not checked: whether observer saves set `PLAYER_CHAR_ID`; if it is
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

Record the speed you used. A slow model at high speed misses events between cycles.

## `set_phase` presets

`set_phase(phase)` writes these 10 personality axes, -100 to 100, for the
controlled ruler; `overrides={axis: int}` replaces single axes. Values are from
`PHASE_GENOME` in `ck3agent/server/redesign.py`, checked by `test_release_docs_drift.py`.

| Phase | honor | greed | ration. | energy | bold. | zeal | venge. | compas. | social. | domin. |
|-------|------:|------:|--------:|-------:|------:|-----:|-------:|--------:|--------:|-------:|
| `expansion` | 20 | -20 | 50 | 90 | 85 | 40 | 30 | -10 | 30 | 60 |
| `rebuild` | 40 | 0 | 70 | 55 | 45 | 30 | 10 | 30 | 50 | 60 |
| `succession_trough` | 40 | 0 | 80 | 55 | 40 | 30 | 10 | 20 | 40 | 90 |
| `stalled_siege` | 10 | -20 | 40 | 100 | 95 | 50 | 40 | -20 | 20 | 70 |
| `fragile_minority` | 50 | 0 | 80 | 50 | 30 | 30 | 10 | 40 | 50 | 90 |
| `consolidate` | 40 | -10 | 70 | 60 | 35 | 40 | 20 | 30 | 50 | 70 |

Every call also writes, for the ruler:

- `set_character_subsystems`: `DeclareWar`, on for `expansion` and
  `stalled_siege`, and `Decisions`, always on. The engine does not enforce
  these toggles; they are bookkeeping.
- `set_character_ai`: re-enables the ruler's native AI tick.

**`expansion` also arms the off-goal war veto**: AI-picked wars are vetoed,
`declare_war` wars are not. It is one process-wide flag for every controlled
character of every session; `character_id` is ignored, no phase disarms it,
and it lasts until `set_experimental` disarms it or the game exits. The hook
is experimental and not live-validated. The result reports
`offgoal_veto_armed`, true only when the bridge confirms the write, and
`offgoal_veto_scope`. The bridge has no readback; `experimental_flags` shows
only this server's last confirmed write, so with several seats a null there
does not mean off.

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
| `kind` | `action`, or `management` for the logged management verbs |
| `verb` | bridge action name |
| `args` | request parameters |
| `result` | the bridge's reply |

Logged as `management`: `pause_game`, `unpause_game`, `pause`, `set_pause`, `set_speed`,
`set_character_ai`, `set_character_subsystems`, `set_offgoal_veto`, `set_force_offensive_stance`.
Not logged: reads, and `set_controlled_character`, `release_character`, `set_auto_respond_events`,
`set_break_alliance_enabled`, `set_cb_selector_override`, `set_vacant_fill_enabled`,
`set_war_stance_record_walk`, `toggle_all_ai`, `enable_all_ai`, `toggle_ai_subsystem`,
`select_observe_default`, `enter_observe_mode`, `inject_mouse_click`, `report_usage`,
`set_budget_config`, `declare_save_identity`. `select_observe_default`,
`inject_mouse_click` and `set_cb_selector_override` exist only in debug builds.

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

Every variable the bridge reads by name in `bridge/src` and `bridge/include`.
`test_release_docs_drift.py` checks the list against string literals of the
forms `CK3*`, `BRIDGE_*`, `XDG_*`, `HOME`, `DISPLAY` and `TMPDIR`; a name of
another form would not be caught. Not listed: `LD_PRELOAD`, which the bridge
reads from the environment of each program the launcher execs, to stay preloaded. Tags:
**paths**, **experiment**, **diagnostic**. Unset means default behaviour.

| Variable | Tag | Effect |
|----------|-----|--------|
| `XDG_RUNTIME_DIR` | paths | base of the per-user directory; see [files and paths](reference/files-and-paths.md#what-goes-where) |
| `HOME`, `XDG_DATA_HOME` | paths | fallbacks for the per-user directory and the trigger catalogue |
| `CK3_INSTANCE_LAYOUT` | paths | `user` by default; `legacy` restores the shared `/tmp` layout and a 0666 socket |
| `CK3_INSTANCE_ID` | paths | instance name; moves socket, log and telemetry under `<root>/<id>/` |
| `CK3_INSTANCE_ROOT` | paths | parent directory for named instances |
| `CK3_INSTANCE_DIR` | paths | episode directory fallback: `$CK3_INSTANCE_DIR/episode` |
| `CK3_BRIDGE_SOCKET` | paths | socket path |
| `CK3_BRIDGE_LOG` | paths | bridge log path |
| `CK3_BRIDGE_LOG_ARCHIVE_DIR` | paths | where rotated bridge logs are gzipped |
| `CK3_TELE` | paths | telemetry directory, which holds the action log |
| `CK3_TRIGGER_CATALOGUE_CSV` | paths | trigger catalogue for `evaluate_trigger` |
| `CK3MAP_BIN` | paths | map-graph artifact; default `<state dir>/ck3map.bin` (`/tmp/ck3map.bin` in the legacy layout) |
| `CK3_STRICT_BUILD_ID` | experiment | any value not starting with `1` (`0`, `false`, `yes`) loosens the BuildID refusal |
| `CK3_PIPELINE_MODE` | experiment | `legacy` by default; `pipeline` buffers writes and gives the bridge the clock; `null` is the observe-only control |
| `CK3_SUPPRESS_AUTO_UNPAUSE` | experiment | `1` stops the repeated unpause before the first tick |
| `CK3_EPISODE_MODE` | experiment | `experiment` pins claims across disconnects and arms containment |
| `CK3_EPISODE_ID` | experiment | id stamped into logs and the ledger |
| `CK3_EPISODE_ARM` | experiment | arm label; arm `b` enables the board |
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
| `CK3_BOARD_ENABLED` | experiment | shared board on; overrides the arm |
| `CK3_BOARD_ADMIN_TOKEN` | experiment | enables `board_admin` |
| `CK3_BOARD_CARRIER_PATH` | experiment | board persistence file |
| `CK3_BOARD_MAX_POST_BYTES` | experiment | board post size limit |
| `CK3_BOARD_MAX_POSTS_PER_FLUSH` | experiment | posts per session per flush |
| `CK3_BOARD_MAX_POSTS_PER_EPISODE` | experiment | posts per episode |
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
- Arms differ in start date: one of them auto-unpaused. Set
  `CK3_SUPPRESS_AUTO_UNPAUSE=1` in both.
