# Changelog

All notable changes to ck3agent. Versions are calendar-based
(`YYYY.MM.PATCH`); the git tag is the version with a `v` prefix.

## 2026.10.0 — unreleased
<!-- The exporter/release stamps the date here; the release fails while this says "unreleased". -->

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
