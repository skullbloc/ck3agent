# Architecture

How the pieces fit, for contributors and for debugging below the tools.

## Prerequisites

- [Glossary](glossary.md) for the terms used here.

## The two halves

```
CK3 process                                  agent side
  ck3 (unmodified on disk)                     ck3agent serve (MCP server, stdio or HTTP)
  libck3_bridge.so  <- LD_PRELOAD                15 tools -> bridge_client.py
    hooks the AI tick, reads memory,                    |
    applies writes on the game thread                   |
    IPC thread  <---- NDJSON over a Unix socket --------+
                                                Claude Code | Claude Desktop | ck3agent run
```

The bridge is a library: it exposes game state and accepts commands, and
knows nothing about language models. The agent side reads, reasons and
steers, and knows nothing about memory layouts. Two contracts sit between
them: `re/offsets/ck3_offsets.h`, from reverse engineering to the bridge, and
the handler table in `bridge/src/action_registry.inc`, from the bridge to the agent.

## Inside the game

1. **Load.** `LD_PRELOAD` puts the library in every process the launch
   starts. The constructor in `bridge_init.cpp` proceeds only in the `ck3`
   executable. In the Paradox launcher, `launcher_bypass.cpp` interposes
   `execve`, `execvp`, `posix_spawn` and `posix_spawnp` to start the game directly.
2. **Identify.** `runtime_resolver.cpp` compares the binary's ELF BuildID with
   the one the offsets came from. Release builds refuse a mismatch before
   hooking anything or opening the socket.
3. **Validate.** `offset_validator.cpp` smoke-tests offset groups against live
   memory. If over 30 % fail, the bridge enters degraded mode: read-only, vtable hooks off.
4. **Hook.** Vtable hooks on the AI tick and a few engine functions: war
   declaration, war score, schemes, council tasks, marriage evaluation.
5. **Serve.** The IPC thread answers bridge-internal reads at once. Game-state
   reads run only while the game is paused, when no AI workers are running.
   Writes and engine calls run on the game thread. Raw reads
   sit behind a SIGSEGV recovery point, so a bad pointer yields an error, not a crash.

## The agent side

| Module | Role |
|--------|------|
| `ck3agent/server/mcp_server.py` | registers the 15 tools with FastMCP; owns the socket and pause handling |
| `ck3agent/server/tools.py` | maps the six original tools to bridge actions: `observe`, `query`, `discover`, `steer`, `intent`, `pass_turn` |
| `ck3agent/server/redesign.py` | the nine goal-shaped tools: `observe_v2`, `inspect`, `cbs`, `plan_war`, `succession_plan`, `set_phase`, `declare_war`, `do`, `session`. Also their envelope, the `set_phase` presets and the `do` intent table. No I/O; it calls the bridge through a gateway that sets and restores pause state. |
| `ck3agent/server/bridge_client.py`, `async_bridge_client.py` | NDJSON framing, request ids, the v2 handshake in observe or control mode |
| `ck3agent/server/instance.py` | socket, log and telemetry paths, per `CK3_INSTANCE_ID` |
| `ck3agent/cli.py` | `setup`, `doctor`, `launch`, `serve`, `run`, `config`, `version`, `hook-state` |

## Response envelopes

The nine `redesign.py` tools return:

| Field | Always | Meaning |
|-------|:-----:|---------|
| `ok` | yes | false if `errors` is non-empty or `blocked` is set |
| `tool` | yes | the tool name |
| `result` | yes | the payload, always an object |
| `blocked` | yes | `null`, or why the call was refused: `{gate, class, severity, retryable, summary}`, often with `fix` and `raw_reason` |
| `warnings` | yes | list of `{code, detail}`; known limitations carry `is_known_limitation` |
| `errors` | yes | list; non-empty means a partial or failed read |
| `bridge_health` | yes | `ok`, or `hung` when the bridge is unreachable |
| `verdict` | writes | `applied` or a typed rejection |
| `applied` | writes | `verdict == "applied"` |
| `actor` | when known | the character the call acted as |
| `game` | when known | `{tick, date, paused, speed}` |
| `delta` | some reads | what changed since the last call |
| `pending` | deferred writes | what to read back to confirm |

The six `tools.py` tools return the original shape: `{ok, data, tick, game_date}`
on success, plus `resolved_character_id` when known, and
`{ok: false, error: {code, message}, tick, game_date}` on failure. They do not
manage pause state: their game-state reads return `game_unpaused` while time runs.

## Steering, not scripting

The model sets a posture: 10 personality axes and subsystem toggles that the
native AI reads every tick. It forces a specific action only after
validating it, with `declare_war` from a `plan_war` row or with `do`. Armies,
sieges and counter-offers stay with the game's AI. That keeps a cycle cheap
and keeps the game coherent when the model is slow.

## Research instrumentation

The bridge also carries experiment machinery: episode policy, a decision
budget, a speed pin, a shared board and a per-session seat declaration. It is
compiled in and inert unless its environment variables are set. See
[research notes](research.md).

## Check it worked

`ck3agent doctor` walks this chain in the order above.

## If it fails

The bridge log names the step that stopped: `REFUSED` for identify,
`DEGRADED MODE` for validate, or a hook install error for hook.
