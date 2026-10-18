# Playing with Claude Code

Claude Code is the supported host. It starts the MCP server and reads the play
protocol from `CLAUDE.md` in the play directory.

## Prerequisites

- [Getting started](getting-started.md) done, with the game running.

## The play directory

| File | Purpose |
|------|---------|
| `.mcp.json` | starts the `ck3` MCP server (`ck3agent serve`) |
| `CLAUDE.md` | the play protocol. Edit it to add a persona, goal or house rules. `ck3agent setup --force` restores it. |
| `.claude/settings.json` | the live-state hook and the tool allow-list |
| `runs/` | output of `ck3agent run` |

**Permissions.** Setup merges `"permissions": {"allow": ["mcp__ck3", "Bash(sleep:*)"]}`
into `.claude/settings.json` and keeps your other settings. The `ck3` tools
and the wait between cycles then run without a prompt. Remove the entries to
be asked each time, or run `ck3agent setup --no-allow`.

**Live-state hook.** Before each prompt, `ck3agent hook-state` adds a short
state block. While the game is paused it covers the ruler, wars, succession,
factions, schemes and council. While the game runs it shows the date, speed,
the ruler's id or `none`, wars, and what it could not read. It reports the
bridge-wide ruler, which is Claude's only when one session is bound. Turn it
off with `ck3agent setup --no-hook`.

## Steps

1. `cd ~/ck3agent-play && claude`
2. If no ruler is bound, Claude asks which one to take. See
   [choose the ruler](getting-started.md#4-choose-the-ruler-claude-plays).
3. Ask for what you want: "brief me", "play until 1080 and keep the realm
   intact", "we are at war with Norway, what are my options?".
4. Claude runs cycles: observe, decide, act, narrate, wait. Press Esc to
   interrupt and redirect.

## Time and cost

| Measured 2026-10-18, claude-opus-5-5, one observe-only cycle (observe, observe_v2, health; 5 turns, no writes) | Per cycle |
|------|------|
| Wall time | 10 seconds (model latency only; a play cycle adds the `sleep` you choose) |
| Game time at speed 3 | 4 days while paused reads ran; a play cycle adds the `sleep` you choose |
| Tokens (input / output) | 120,049 (mostly cached) / 451 |
| Cost at API prices | $0.20; a full play cycle with writes and a `plan_war` read costs more, unmeasured |

A cycle is a handful of tool calls. On a subscription plan, long sessions
count against your usage limits.

## Stopping

| To | Do | Then |
|----|----|------|
| interrupt | Esc | Claude stops mid-cycle. |
| end cleanly | say "stop" | Claude pauses the game and reports. Say "release control" to hand the ruler back to the game's AI. |
| quit | `/exit` | The game stays paused or running as it was. The ruler keeps Claude's last personality values until the game rewrites them. |

## Check it worked

- Claude's replies name the ruler and the current game date.
- `session(action="whoami")` returns the ruler you chose.

## If it fails

- No `ck3` tools: start `claude` inside the play directory, and check that
  `ck3agent` is on `PATH`.
- `game_unpaused`: `observe`, `query` and `discover` need the game paused.
  Ask Claude to pause first, as the protocol says.
- Run one Claude session per game. Two sessions can both write; the last write wins.
- Anything else: `ck3agent doctor` in another terminal, then
  [troubleshooting](troubleshooting.md).
