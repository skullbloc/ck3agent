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
| **Primary** | with several rulers, the one a call without `character_id` acts as: the first bound. `control` switches it; `add` keeps it. |
| **Several rulers** | one session holding more than one ruler (`session(action="add")` or a `CK3_AGENT_CHARACTER_ID` list): one posture each, one game clock. Two sessions in one game is a different, unsupported shape. |
| **Observer save** | a save started in CK3's Observe mode, with no player character. The cleanest start for Claude. |
| **Steering** | changing how the game's AI behaves, through personality and toggles, instead of issuing each action. |
| **Posture / phase** | a named preset, such as `expansion` or `consolidate`, that `set_phase` turns into personality values. |
| **Personality axes** | the 10 values, -100 to 100, the AI reads: honor, greed, rationality, energy, boldness, zeal, vengefulness, compassion, sociability, dominance. |
| **Envelope** | the JSON wrapper every tool returns. Read `ok` first. See [architecture](architecture.md#response-envelopes). |
| **Verdict** | a write's outcome: `applied`, or a typed rejection naming the gate that refused it. |
| **Dormant / active** | bridge states. Dormant: loaded, no game running. Active: a game is running and tools work. |
| **BuildID** | the fingerprint of the game binary. Each release supports one and refuses all others. |
| **Seat** | one controlled ruler as held by one session. "Claude's seat" is the ruler Claude is bound to; the seat moves to the heir when the ruler dies, unless the heir is the human player. |
| **Session** | one client's connection to the bridge. A *control* session may bind and steer; an *observe* session may only read. One Claude Code window is one session. |
| **Claim / roster** | the rulers a session holds; `whoami` lists them under `my_claims`. |
| **Handshake** | the first exchange on connect, which fixes the session's mode and reports the bridge's state (`dormant`, `active`) and configuration. |
| **Instance** | one game plus its bridge socket, logs and telemetry, named by `CK3_INSTANCE_ID` when more than one game runs on a machine. |
| **Arm, episode** | research terms: an *episode* is one recorded run; an *arm* is one condition of a study (treatment, control). Only the research notes use them. |
| **Hook** | inside the game, the bridge's interception of an engine function (the AI tick, war declaration). In Claude Code, the *live-state hook* `ck3agent hook-state` that prepends game state to each prompt. |
| **Action log** | `action_log.jsonl`, one line per bridge call, except reads and most management verbs, such as `set_controlled_character`. See [research notes](research.md#what-is-recorded). |
