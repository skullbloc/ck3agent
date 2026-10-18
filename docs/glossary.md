# Glossary

The terms the docs, tools and logs use.

| Term | Meaning |
|------|---------|
| **Bridge** | `libck3_bridge.so`, the library loaded into the game. It reads game state and applies Claude's writes. |
| **MCP** | Model Context Protocol, the way Claude calls external tools. `ck3agent serve` is the MCP server. |
| **Play directory** | `~/ck3agent-play`, where you start `claude`. Holds `.mcp.json`, `CLAUDE.md` and settings. |
| **Protocol** | the `CLAUDE.md` in the play directory: the rules Claude follows each cycle. |
| **Cycle** | one round of observe, decide, act, narrate, wait. |
| **Controlled ruler** | the AI character Claude steers, bound with `session(action="control")`. Never the player's own character. |
| **Observer save** | a save started in CK3's Observe mode, with no player character. The cleanest start for Claude. |
| **Steering** | changing how the game's AI behaves, through personality and toggles, instead of issuing each action. |
| **Posture / phase** | a named preset, such as `expansion` or `consolidate`, that `set_phase` turns into personality values. |
| **Personality axes** | the 10 values, -100 to 100, the AI reads: honor, greed, rationality, energy, boldness, zeal, vengefulness, compassion, sociability, dominance. |
| **Envelope** | the JSON wrapper every tool returns. Read `ok` first. See [architecture](architecture.md#response-envelopes). |
| **Verdict** | a write's outcome: `applied`, or a typed rejection naming the gate that refused it. |
| **Dormant / active** | bridge states. Dormant: loaded, no game running. Active: a game is running and tools work. |
| **BuildID** | the fingerprint of the game binary. Each release supports one and refuses all others. |
| **Action log** | `action_log.jsonl`, one line per bridge call, except reads and most management verbs, such as `set_controlled_character`. See [research notes](research.md#what-is-recorded). |
