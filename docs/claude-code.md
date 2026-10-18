# Playing with Claude Code

Claude Code is the supported interactive host: it runs on Linux next to the
game, starts the MCP server itself, and the play protocol lives in a
`CLAUDE.md` it reads automatically.

## The play directory

`ck3agent setup` creates it (default `~/ck3agent-play`):

```
.mcp.json                 → {"mcpServers": {"ck3": {"command": "ck3agent", "args": ["serve"]}}}
CLAUDE.md                 → the play protocol (tools, pause discipline, the cycle)
.claude/settings.json     → a UserPromptSubmit hook: `ck3agent hook-state`
runs/                     → logs from `ck3agent run`
```

You can edit `CLAUDE.md` freely: add a persona, a goal, house rules, a
narration style. Re-run `ck3agent setup --force` to get the stock version back.

## The live-state hook

`ck3agent hook-state` runs before every prompt and prints a short
"Live game state" block (date, ruler, gold, wars, succession, factions,
schemes, council) that Claude Code injects into context. It has a 2.5 s
budget and degrades to a one-line note when the game is unpaused or the
bridge is away, so it never blocks a turn. Disable it with
`ck3agent setup --no-hook --force` or by deleting the hook from
`.claude/settings.json`.

## A session

1. `ck3agent launch --continue --wait 120` (or launch via Steam with
   `LD_PRELOAD`), load a save.
2. `cd ~/ck3agent-play && claude`.
3. Ask for what you want: "observe and brief me", "play until 1080, keep the
   succession intact", "we are at war with Norway, what are my options".

Claude follows the cycle in `CLAUDE.md`: pause, read, decide, unpause, act,
narrate, wait (`sleep` in Bash), repeat. Approve the `ck3` tools and
`Bash(sleep:*)` when prompted, or pre-approve them in
`.claude/settings.json`:

```json
{ "permissions": { "allow": ["mcp__ck3", "Bash(sleep:*)"] } }
```

## Tips

* Writes need the game unpaused and some raw reads need it paused; the
  redesigned tools handle that, and `session(action="pause"|"unpause")` is the
  manual lever. If Claude seems stuck, it is usually waiting on a pause state.
* `session` caps speed at 3 and refuses 4–5 at war or near a succession;
  faster outruns the reads.
* `ck3agent doctor` from another terminal diagnoses a dead bridge without
  disturbing the session.
* One control session at a time. A second Claude Code session in the same
  play directory shares the bridge, which is fine for reading but confusing
  for writing.
