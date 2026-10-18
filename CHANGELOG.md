# Changelog

All notable changes to ck3agent. Versions are calendar-based
(`YYYY.MM.PATCH`); the git tag is the version with a `v` prefix.

## 2026.10.0 — unreleased

First public release.

* **Bridge** (`libck3_bridge.so`, Linux x86_64) for Crusader Kings III
  1.18.3.1 (Crane), BuildID `e61ff264…`. Loaded with `LD_PRELOAD`; hooks the AI
  tick, reads game state, writes personality weights and subsystem toggles,
  and serves NDJSON over a Unix socket. Refuses any other game build at load
  time (strict BuildID check, compiled on in release builds).
* **MCP server** with 15 tools — reads `observe`, `observe_v2`, `inspect`,
  `query`, `discover`, `cbs`, `plan_war`, `succession_plan`; writes
  `set_phase`, `steer`, `declare_war`, `do`, `intent`, `session`, `pass_turn`
  — over stdio and streamable-HTTP transports. The console-command
  passthrough is a bridge action, not an MCP tool.
* **`ck3agent` CLI**: `setup` (Steam discovery, build check, play directory
  with `.mcp.json`, play protocol and live-state hook), `doctor`, `launch`,
  `serve`, `run` (unattended Claude Code session from a starting prompt),
  `hook-state`, `config`, `version`.
* **Linux tarball** with installer, split debug symbols, SHA256SUMS and build
  provenance attestation; wheel attached to the release.
* Docs: quickstart, install, Claude Code / Claude Desktop / scripted runs,
  tool reference, agent–character contract, architecture, supported versions,
  research notes, and the Windows/macOS port design record.

Known limitations: Linux only; one supported game build; the bridge socket and
logs live under `/tmp` by default; Claude Desktop needs an HTTP transport or a
stdio shim because it has no Linux build.
