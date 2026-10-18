# Scripted runs

For research you often want the same thing many times, unattended: a
starting prompt, a goal, a cycle budget, logs. `ck3agent run` does that with a
headless Claude Code session (`claude -p`), which is the same mechanism the
authors used for their own experiments.

```bash
ck3agent launch --continue --wait 120
ck3agent run --goal "Hold the kingdom together through the next succession." --cycles 60
```

What happens:

1. The play directory's `.mcp.json` and `CLAUDE.md` are used as in an
   interactive session.
2. A starting prompt is rendered from `ck3agent/protocol/run-prompt.md` with
   your `--goal` and `--cycles`, saved to `runs/<timestamp>.prompt.md`, and
   passed to `claude -p`.
3. The session's output streams to your terminal and to
   `runs/<timestamp>.log`. The bridge's own action log is in
   `/tmp/ck3-telemetry/action_log.jsonl` (override with `CK3_TELE`).
4. The prompt asks the model to stop after the cycle budget and write a short
   summary; `--max-turns N` is a hard cap on agentic turns on top of that.

Options:

| Flag | Meaning |
|------|---------|
| `--goal TEXT` | substituted for `{{goal}}` in the run prompt |
| `--prompt-file FILE` | use this file as the entire starting prompt (`{{goal}}`/`{{cycles}}` still substituted) |
| `--cycles N` | cycle budget written into the prompt (default 50) |
| `--max-turns N` | `claude --max-turns`; hard stop |
| `--model NAME` | `claude --model` |
| `--play-dir DIR` | another play directory (so runs can have different `CLAUDE.md`s) |
| `--yolo` | `--dangerously-skip-permissions` instead of the allow-list (`mcp__ck3`, `Bash(sleep:*)`) |
| `--quiet` | do not echo to stdout |
| `-- <args>` | anything after `--` goes to `claude` unchanged |

## Reproducibility

This release keeps it minimal: the prompt and the transcript are saved, the
bridge logs every action with the game date, and `ck3agent version` reports
the bridge's supported BuildID. Save files, seeds and the model are yours to
record; a convenient pattern is one play directory per condition with its own
`CLAUDE.md`, and a shell loop over saves. Sealed, hashed result bundles are not
part of this release.

## Control conditions

A run with no model is just the game running with the bridge attached and no
client connected (`ck3agent launch --continue --wait 300` and walk away). The bridge does not steer anything on its own; the native AI
plays. Launch, let it run, and read the action log and the save.
