# Changelog

All notable changes to ck3agent. Versions are calendar-based
(`YYYY.MM.PATCH`); the git tag is the version with a `v` prefix.

## 2026.11.0 — unreleased

* **Names the bridge has not localized yet.** Only the game thread can
  turn a character's localization key (`E_thelred` for Æthelred) into a
  name, on its next tick; a paused game — the first reads of a session —
  has no tick. The bridge used to answer that miss with the key as the
  `name`. It now omits `name` and sends `name_key` and `name_pending: true`
  (`query_character`, the `characters` listing, marriage candidates,
  claimants, every reply built from the character profile); the MCP server's
  actor echo (`controlled`, `added`, `actor`) carries the same two keys; the
  live-state hook lists the ruler without a name and a note, never the key.
  See the bridge contract, 3.6.

* **Replies that were wrong on Linux**, found by diffing them against the same
  build running under Proton (ADR 0001, phase P5). Succession replies
  (`succession_plan`; the `succession`, `succession_simulation` and partition
  queries) took the title's law-vector pointer for its succession rule, so the
  rule changed from one run to the next; one resolver now reads the title's laws,
  else its holder's realm law. `held_by_target` in war-target replies read a
  field that is zero on nearly every title; it now reads the holder. Known
  innovations were read as instances instead of types, so era counts were bytes
  of a key string. An army's commander was a truncated pointer. Lists with equal
  sort keys now come back in a stable order (mercenaries tie-break by company
  id), and a reply degraded by an unreadable address logs the address instead of
  carrying it. Some values are still not fixed between two Linux runs of one
  build (trait value and age bounds, a law group id, the second council slot,
  the piety and prestige caps before the game computes them); the list is in
  the port findings. `toggle_ai_subsystem` sent no reply when it succeeded; it
  now answers with the subsystem, the requested state and the flag byte read
  back after the write (P6). Claiming a character (`set_controlled_character`)
  before the game's first tick, e.g. in a game loaded paused, answered
  `memory_access_fault` (the handler called a game function the bridge had not
  resolved yet); those functions are now resolved at load. A pause issued
  shortly after an unpause (including the bridge's own re-pause after a
  deferred query's clock pulse) could be undone on the next tick by the
  unpause's queued follow-up; a pause now cancels it. The council-task
  observer hook is no longer installed: it hooked the game's task-availability
  check under a setter's signature, so that check's callers got an undefined
  answer; `query_council_tasks` without a character now lists no hook
  assignments (the per-seat read is unchanged). `get_ai_personality` gains
  an `engine` object: the nine axis bytes read from the game, whatever a held
  lock reports in `personality` (additive; no existing key moved).
* **Windows port status** (docs, and code that runs on Windows only): ADR 0001 records P5, the bridge loaded
  inside `ck3.exe` under Proton with observe parity in progress; the Proton
  runbook gains a section on talking to that bridge; `CK3_BRIDGE_PORT`
  (Windows only) is in the environment table. P6 and P7 preparation (still a
  preview): the AI-tick hook and steering run inside `ck3.exe` under Proton
  (runbook section 8), including the interaction trigger query and acceptance and the direct interaction dispatch; a Windows installer (`installer/windows/install.ps1`) and the
  P7 real-Windows runbook; the MCP client finds the Windows bridge's
  `endpoint.json` on its own (`CK3_BRIDGE_ENDPOINT` overrides); the public Windows
  build is the full bridge, from Windows offset headers generated at export
  (`re/offsets/windows/`, each naming its input shas and the one ck3.exe it is valid
  for); the research inputs behind them do not ship.

## 2026.10.0 — 2026-10-19

First public release.

* **Bridge**, `libck3_bridge.so` for Linux x86_64, for Crusader Kings III
  1.18.3.1 (Crane), BuildID `e61ff264…`. Loaded with `LD_PRELOAD`. It hooks the
  AI tick, reads game state, writes personality weights and subsystem toggles,
  and serves NDJSON over a Unix socket in a private per-user directory. Release
  builds refuse any other game build at load and leave out the debug verbs.
* **MCP server** with 15 tools over stdio and streamable HTTP. Reads: `observe`,
  `observe_v2`, `inspect`, `query`, `discover`, `cbs`, `plan_war`,
  `succession_plan`. Writes: `set_phase`, `steer`, `declare_war`, `do`,
  `intent`, `session`, `pass_turn`.
* **`ck3agent` CLI**: `setup` finds the game, checks its build and writes a
  play directory with `.mcp.json`, the play protocol, the live-state hook and
  an allow-list. Also `doctor`, `launch`, `serve`, `config`, `version`, and
  `run`, an unattended Claude Code session that records a transcript and a manifest.
* **Several rulers in one session.** `session(action="add")` binds one more
  ruler and keeps the first as the primary (`control` switches); a
  comma-separated `CK3_AGENT_CHARACTER_ID` pre-binds several. The live-state
  hook lists every ruler held, and the `ck3agent run` manifest records them
  at start and stop. One posture per ruler, one game clock. The pre-bind
  waits for the game to be active (`CK3_AGENT_PREBIND_WAIT_S`, default 120 s)
  instead of being refused when the run starts before the save has loaded;
  the manifest notes the tick and date each bind landed.
* **Live-state hook.** A ruler whose name the bridge has not localized yet
  (it answers with the game's key, `E_thelred` for Æthelred, until a tick
  resolves it) is listed without a name and a note, never with the key.
* **Time passes through the server.** `session(action="wait", seconds=N)`
  unpauses, blocks up to 110 s, pauses and returns the dates; the play
  protocol never uses a shell `sleep`, which Claude Code refuses at 30 s or
  more. `ck3agent launch` pauses the game once it is active (`--no-pause`
  keeps it running).
* **Writes.** `do(intent="scheme")` starts and cancels schemes for an AI
  ruler, validated live; `faction_action` and `break_alliance` stay off by
  default behind an operator-armed file. `set_phase` reports whether it
  re-enabled the ruler's AI tick.
* **Reads.** `query` characters listing adds `realm_counties` and
  `held_title_count` and one `is_ai` definition; `neighbors` resolves from a
  character; `do` enumerates men-at-arms types, laws and mercenaries with
  labels; council tasks take and report a `seat`; `discover(war_targets)`
  says it lists candidates and `cbs` is authoritative.
* **Linux tarball** with installer, split debug symbols, SHA256SUMS and build
  provenance attestation; wheel attached to the release.
* **Docs**: getting started, Claude Code, Claude Desktop, scripted runs,
  troubleshooting, generated tool and bridge-action references, the bridge
  contract, architecture, supported versions, research notes, and the
  Windows and macOS port design record.

Known limitations: Linux only; one supported game build; Flatpak and Snap
Steam are unsupported; Claude Desktop connects through `ck3agent serve
--transport http` and `mcp-remote`, except on its Linux beta. Claude never
plays the human's character, not even as heir. The off-goal veto that
`set_phase("expansion")` arms is process-wide. See the research notes.

### Contributors

* Harrison Stebbins
