# Changelog

All notable changes to ck3agent. Versions are calendar-based
(`YYYY.MM.PATCH`); the git tag is the version with a `v` prefix.

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
