# Scripted runs

Run Claude unattended with a goal and a stopping rule, and keep a record.
`ck3agent run` drives a headless Claude Code session (`claude -p`).

## Prerequisites

- [Getting started](getting-started.md) done.
- A goal that names the AI ruler to play, or a ruler already bound, or
  `CK3_AGENT_CHARACTER_ID` set (below). The run prompt stops if none is true.
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

   **Pre-bind.** `CK3_AGENT_CHARACTER_ID` binds rulers before Claude's first
   turn: one id, or a comma-separated list. The first is the primary (a call
   without `character_id` acts as it); each further one is added beside it.
   One malformed entry, or more than 10 ids, binds none. A ruler the bridge
   refuses (another session holds it, it is dead, it is the player's) is left
   out and the run report warns. A run that starts while the save is still
   loading is not refused: the server waits for the game to be active (up to
   `CK3_AGENT_PREBIND_WAIT_S` seconds, default 120, reading the bridge every
   2 s) and binds then; the manifest's `rulers.bound_at` gives the tick and
   date each bind landed and `rulers.prebind_wait` how long it waited. Past the
   wait the bind is attempted anyway and its refusal reported as above.
   Several rulers in one run, each with its own posture:

   ```bash
   CK3_AGENT_CHARACTER_ID=13902,12145 ck3agent run \
       --goal "Rulers: 13902 (Æthelred of Wessex) and 12145 (Boris of Bulgaria). \
   Æthelred: set_phase expansion. Boris: set_phase consolidate, no wars." \
       --cycles 3 --wall-seconds 480
   ```

   Both rulers share the game clock: each cycle observes and acts for both,
   then waits once. Two runs (two sessions) in one game is not the same thing
   and is unsupported; see [If it fails](#if-it-fails).

3. When a limit is reached, `ck3agent run` interrupts Claude (SIGINT, then
   SIGTERM after 30 s, then SIGKILL after 10 more), pauses the game, releases
   every ruler the run held (the server releases all but its last; the last is
   freed when the session ends), and writes the manifest. The manifest's last
   check, "no ruler is bound", is the verdict. The same teardown runs on
   Ctrl-C, SIGTERM, SIGHUP and a closed terminal, but not after `kill -9` to
   `ck3agent` itself; see [If it fails](#if-it-fails).

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
| `--play-dir DIR` | another play directory, for example one per condition. Create it with `ck3agent setup --play-dir DIR`; note that setup also makes it the default play directory in `config.toml`, so pass `--play-dir` to every run |
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
| `<stamp>.json` | the manifest: model (as Claude Code reports it, which may be an alias such as `claude-opus-5[1m]`; cite `ck3agent version` and the model id from the transcript's init event for a paper), session id, Claude Code and ck3agent versions, the last launch record with any save hash, and whether its pid is the game's (`launch_check`). Also the start and end game date, `usage` and `total_cost_usd`, exit code, stop reason, how Claude was stopped (`claude_stop`), the server's release, the release through its resumed session (`resumed_release`) and the teardown result. `rulers` lists every ruler of the run: `declared` (`CK3_AGENT_CHARACTER_ID`), `bound_at_start`, `bound_at` (the tick and date each bind landed), `refused_at_start`, `prebind_wait` (`waited_s`, `polls`, `game_active`: how long the server waited for the game before binding), `held_at_stop` and `held_after_teardown` (empty when the teardown is clean) |
| `<stamp>.stderr.log` | Claude Code's stderr |
| `<stamp>.tools.json` | the tools the MCP server serves: names, descriptions and input schemas, with a hash of them |
| `<stamp>.release.json` | the claims the MCP server held at shutdown, and which ones it released |
| `<stamp>.session.json` | the MCP server's current bridge session id and resume token, rewritten at every handshake including a reconnect. Mode 0600: the token is a credential for that session. How the release works: the bridge frees a session's rulers when the session ends and refuses to release its last ruler before then, so the server releases any extra rulers when Claude Code signals it to stop, its exit frees the last one, and `ck3agent run` then resumes the session with this token to release anything left, and deletes the file. It is left behind only if `ck3agent` itself is SIGKILLed |
| `<stamp>.actions.jsonl` | the bridge action-log lines written during the run |
| `<stamp>.mcp.json` | the MCP config the run used |

## Check it worked

- `<stamp>.json` has `end_game_date`, `exit_code` and `stop_reason`.
- The teardown has `release` ok: no ruler is bound, and `rulers.held_after_teardown` is empty.
- With a pre-bind, `rulers.bound_at_start` names every ruler you declared.
- `<stamp>.jsonl` ends with a `result` event only if Claude finished, or
  stopped cleanly on SIGINT. Otherwise `result_event` is false, and usage and
  cost read `unrecorded: terminated`, or `unrecorded: no result event` if
  Claude exited by itself.

## If it fails

- `bridge socket not listening`: the game is not running. Launch it first.
- No ruler bound: put the ruler's name in `--goal`, or bind one interactively first.
- `teardown release: N still held by session ...` while another `ck3agent run`
  is still going in the same game: that run's ruler, not yours. Two runs in one
  game share the clock and the teardown pause, and `set_phase("expansion")`
  arms a process-wide war veto for both; one run per game is the supported
  shape. The final run's teardown is the one that proves no ruler is bound.
- `teardown release: N still held by session ...`: neither the MCP server nor
  the resumed session released it, for example after `kill -9` to
  `ck3agent`. The manifest's `resumed_release.error` says why. Quit the game.

Research use, controls and recorded variables: [research notes](research.md).
