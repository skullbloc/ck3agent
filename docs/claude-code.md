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
| `.claude/settings.json` | the live-state hook, the tool allow-list and the `env` block |
| `runs/` | output of `ck3agent run` |

**Permissions.** Setup merges `"permissions": {"allow": ["mcp__ck3", "Bash(sleep:*)"]}`
into `.claude/settings.json` and keeps your other settings. The `ck3` tools
then run without a prompt, once you have accepted Claude Code's trust dialog
for the play directory (it asks the first time you start `claude` there; until
then the allow-list is ignored). Remove the entries to be asked each time, or
run `ck3agent setup --no-allow`.

**Time passes through the server.** Between cycles Claude calls
`session(action="wait", seconds=60)`: the server unpauses, blocks, pauses and
reports the dates. Claude Code refuses a Bash `sleep` of 30 seconds or more,
so the protocol never uses one. Setup also writes
`"env": {"CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS": "0"}` into the settings, because
Claude Code otherwise moves a tool call still running after 120 seconds into a
background task; a wait is at most 110 seconds per call.

**Live-state hook.** Before each prompt, `ck3agent hook-state` adds a short
state block. While the game is paused it covers the ruler, wars, succession,
factions, schemes and council. While the game runs it shows the date, speed,
the ruler's id or `none`, wars, and what it could not read. When Claude
holds several rulers it lists each one: id, name and title (paused), at war,
imprisoned, dead. It reports the rulers of the session that holds the
bridge's primary ruler, which is Claude's when one session is bound. Turn it
off with `ck3agent setup --no-hook`.

## Steps

1. `cd ~/ck3agent-play && claude`
2. If no ruler is bound, Claude asks which one to take. See
   [choose the ruler](getting-started.md#4-choose-the-ruler-claude-plays).
3. Ask for what you want: "brief me", "play until 1080 and keep the realm
   intact", "we are at war with Norway, what are my options?".
   For several rulers, say "also take <name>" for each: Claude adds them with
   `session(action="add", character_id=...)`, gives each its own posture, and
   lets time pass once per cycle for all of them (one game clock). See
   "Several rulers" in the play directory's `CLAUDE.md`.
4. Claude runs cycles: observe, decide, act, narrate, wait. Press Esc to
   interrupt and redirect.
5. **If you are playing your own character in the same game:** Claude owns
   the clock. It pauses to read and unpauses with `session(action="wait")` to
   let time pass, usually 60 seconds at a time. Leave the pause key alone
   while Claude is mid-cycle; if you pause by hand, its next `wait` unpauses
   again. Play your moves between cycles, or say "stop" first. When Claude's
   session ends, the game stays paused or running as it was.
6. One Claude session per game. A second `claude` in the same play directory
   also writes to the bridge, and the last write wins. For several rulers, use
   one session that holds them all, not one session per ruler.

## Time and cost

| Measured 2026-10-18, claude-opus-5-5, one observe-only cycle (observe, observe_v2, health; 5 turns, no writes) | Per cycle |
|------|------|
| Wall time | 10 seconds (model latency only; a play cycle adds the wait you choose) |
| Game time at speed 3 | 4 days, during the unpause-pause gaps between reads; a play cycle adds the wait you choose |
| Tokens (input / output) | 120,049 (mostly cached) / 451 |
| Cost at API prices | $0.20; a full play cycle with writes and a `plan_war` read costs more, unmeasured |

A play cycle with writes measured $1.35 to $1.60 on the same day and model
(one `set_phase`, one `do`, a `plan_war` read); even "stop" costs about $1.35,
because the whole conversation is resent. Two game months of play came to
about $6. A cycle is a handful of tool calls. On a subscription plan, long
sessions count against your usage limits.

## Stopping

| To | Do | Then |
|----|----|------|
| interrupt | Esc | Claude stops mid-cycle. |
| end cleanly | say "stop" | Claude pauses the game and reports. It cannot hand its only ruler back while the session runs; `/exit` does that. |
| quit | `/exit` | The server's session ends and the ruler returns to the game's AI, keeping Claude's last personality values until the game rewrites them. The game stays paused or running as it was. |

## Check it worked

- Claude's replies name the ruler and the current game date.
- `session(action="whoami")` returns the ruler you chose.

## If it fails

- No `ck3` tools: start `claude` inside the play directory, and check that
  `ck3agent` is on `PATH`.
- `game_unpaused`: `observe`, `query` and `discover` need the game paused.
  Ask Claude to pause first, as the protocol says.
- Anything else: `ck3agent doctor` in another terminal, then
  [troubleshooting](troubleshooting.md).
