# Scripted runs

Run Claude unattended with a goal and a stopping rule, and keep a record.
`ck3agent run` drives a headless Claude Code session (`claude -p`).

## Prerequisites

- [Getting started](getting-started.md) done.
- A goal that names the AI ruler to play, or a ruler already bound. The run
  prompt stops if neither is true.
- For repeatable runs, a fixed save file per condition.

## Steps

1. Start the game from a known save:

   ```bash
   ck3agent launch --save ~/saves/start_867.ck3
   ```

   `--save` implies `--continue` and records the save's SHA-256, which the
   run manifest copies. Without a fixed save, use `ck3agent launch --continue`.

2. Start the run:

   ```bash
   ck3agent run --goal "Hold the kingdom together through the next succession." \
                --cycles 60 --until-date 900.1.1 --wall-seconds 7200
   ```

   The run stops at the first limit reached: the cycle budget in the prompt,
   `--max-turns`, the game date, or the wall-clock limit.

3. To stop at a limit, `ck3agent run` sends Claude SIGINT, pauses the game,
   and waits 30 seconds for Claude to finish its stream. Then it sends SIGTERM,
   and after 10 more seconds SIGKILL.

   The bridge lets only the session that bound a ruler release it. The MCP
   server releases its session's rulers as soon as Claude Code signals it to
   stop, but Claude Code kills it 500 ms later. So once Claude has exited,
   `ck3agent run` resumes the server's session with the token the server
   wrote when it connected (`<stamp>.session.json`, deleted afterwards) and
   releases whatever that session still holds. Then it pauses the game and
   checks that no ruler is bound. This also runs on Ctrl-C, SIGTERM and
   SIGHUP, including a closed terminal: the manifest is written before
   anything is printed. It does not run after `kill -9` to `ck3agent`; see
   [If it fails](#if-it-fails).

## Options

| Flag | Meaning |
|------|---------|
| `--goal TEXT` | replaces `{{goal}}` in the run prompt |
| `--prompt-file FILE` | use FILE as the whole prompt; `{{goal}}` and `{{cycles}}` are still replaced |
| `--cycles N` | cycle budget written into the prompt, default 50 |
| `--max-turns N` | `claude --max-turns`, default 400, `0` for no cap |
| `--until-date Y.M.D` | stop at this game date, checked every 15 seconds |
| `--wall-seconds N` | stop after N seconds |
| `--model NAME` | `claude --model` |
| `--play-dir DIR` | another play directory, for example one per condition |
| `--quiet` | do not echo the session to the terminal |
| `--no-game-check` | start even if the bridge socket is not up |
| `--dangerously-skip-permissions` | no allow-list: Claude may run any tool. Only in a disposable VM; see [SECURITY.md](../SECURITY.md). The default allows `mcp__ck3` and `Bash(sleep:*)`. |
| `-- ARGS` | passed to `claude` unchanged |

A stop by `--until-date` or `--wall-seconds` exits 0. Ctrl-C exits 130,
SIGTERM 143, SIGHUP 129.

## What a run writes

All under `<play dir>/runs/`, one timestamp per run (`-2`, `-3`, ... added
when two runs start in the same second):

| File | Contents |
|------|----------|
| `<stamp>.prompt.md` | the exact prompt sent |
| `<stamp>.jsonl` | the full transcript, `claude --output-format stream-json --verbose` |
| `<stamp>.json` | the manifest: model, session id, Claude Code and ck3agent versions, the last launch record with any save hash, and whether its pid is the game's (`launch_check`). Also the start and end game date, usage, cost, exit code, stop reason, how Claude was stopped (`claude_stop`), the server's release, the release through its resumed session (`resumed_release`) and the teardown result |
| `<stamp>.stderr.log` | Claude Code's stderr |
| `<stamp>.tools.json` | the tools the MCP server serves: names, descriptions and input schemas, with a hash of them |
| `<stamp>.release.json` | the claims the MCP server held at shutdown, and which ones it released |
| `<stamp>.session.json` | the MCP server's current bridge session id and resume token, rewritten at every handshake including a reconnect. Mode 0600: the token is a credential for that session. `ck3agent run` resumes the session with it, releases what is left, then deletes the file; it is left behind only if `ck3agent` itself is SIGKILLed |
| `<stamp>.actions.jsonl` | the bridge action-log lines written during the run |
| `<stamp>.mcp.json` | the MCP config the run used |

## Check it worked

- `<stamp>.json` has `end_game_date`, `exit_code` and `stop_reason`.
- The teardown has `release` ok: no ruler is bound.
- `<stamp>.jsonl` ends with a `result` event only if Claude finished, or
  stopped cleanly on SIGINT. Otherwise `result_event` is false, and usage and
  cost read `unrecorded: terminated`, or `unrecorded: no result event` if
  Claude exited by itself.

## If it fails

- `bridge socket not listening`: the game is not running. Launch it first.
- No ruler bound: put the ruler's name in `--goal`, or bind one interactively first.
- `teardown release: N still held by session ...`: neither the MCP server nor
  the resumed session released it, for example after `kill -9` to
  `ck3agent`. The manifest's `resumed_release.error` says why. Quit the game.

Research use, controls and recorded variables: [research notes](research.md).
