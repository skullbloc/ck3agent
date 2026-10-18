# Architecture

```
┌──────────────────────── CK3 process ────────────────────────┐   ┌──────── agent side ────────┐
│ ck3 (Paradox's binary, unmodified on disk)                   │   │ ck3agent serve (MCP server) │
│   libck3_bridge.so   ← LD_PRELOAD                            │   │   15 tools over stdio/HTTP  │
│   · constructor fires, verifies /proc/self/exe is `ck3`      │   │   bridge_client.py          │
│   · reads the ELF BuildID, compares to offset_version.h      │   └──────────┬──────────────────┘
│   · validates offset groups against live memory              │              │ NDJSON
│   · installs vtable hooks on the AI tick and a few handlers  │   /tmp/ck3-agent.sock (AF_UNIX)
│   · IPC server thread: NDJSON request/response + pushes  ────┼──────────────┘
│   · dispatch_router.cpp: ~hundreds of query/action handlers  │   Claude Code │ Claude Desktop │ ck3agent run
└──────────────────────────────────────────────────────────────┘
```

## The library / consumer split

The bridge is a **library**: it exposes game state and accepts commands over
IPC and knows nothing about language models. The agent side is a
**consumer**: it reads state, reasons, and sends steering commands and knows
nothing about memory layouts. The contract between reverse-engineering work
and bridge code is `re/offsets/ck3_offsets.h`; the contract between bridge
and consumer is the handler table in `bridge/src/dispatch_router.cpp`, which
`docs/reference/tool-reference.md` is generated from.

## Inside the game

* **Loading.** `LD_PRELOAD` puts the library in every process the launch
  chain starts. The constructor (`bridge_init.cpp`) only proceeds in a process
  whose executable is named `ck3`; in the Paradox launcher it instead
  interposes `execve`/`posix_spawn` (`launcher_bypass.cpp`) so the game is
  started directly with the library re-injected.
* **Identity.** `runtime_resolver.cpp` reads the `NT_GNU_BUILD_ID` note of the
  loaded binary and compares it to the one the offsets were derived from.
  Release builds are strict: a mismatch logs a refusal and stops before any
  memory is touched through offsets.
* **Validation.** `offset_validator.cpp` smoke-tests groups of offsets against
  live memory before hooks are installed; more than 30 % failing groups puts
  the bridge in a read-only degraded mode.
* **Hooks.** Vtable hooks on the AI manager's tick and on a handful of engine
  functions (war declaration, war score, scheme creation, council tasks,
  marriage evaluation). Steering is applied *inside* the AI tick, on the game
  thread, from a queue the IPC thread fills — the bridge never calls engine
  code from its own thread.
* **Reads.** Memory reads are guarded (a SIGSEGV recovery point around every
  raw access); a bad pointer produces an error envelope, not a crash.
* **Writes.** Personality axes and subsystem toggles are written into the AI's
  own data structures; explicit actions go through the engine's interaction
  and war-creation paths so the game's validation runs.
* **Signatures.** `re/offsets/signatures.h` holds 34 byte patterns resolved at
  load with the Zydis decoder for functions whose addresses must not be
  hard-coded.

## The agent side

* **`ck3agent/server/mcp_server.py`** registers the 15 tools with FastMCP and
  shapes every result into `{ok, data, error, tick, game_date}`.
* **`tools.py`** and **`redesign.py`** map tool calls to bridge handlers
  (domains for `query`, `what` for `discover`, subjects for `inspect`, intents
  for `do`) and own the pause/unpause choreography around reads and writes.
* **`bridge_client.py` / `async_bridge_client.py`** speak the NDJSON protocol
  (framing, request ids, the v2 handshake with observe/control modes).
* **`instance.py`** resolves socket/log/telemetry paths, with
  `CK3_INSTANCE_ID` namespacing for several games on one host.
* **`cli.py`** is the product shell: setup, doctor, launch, serve, run.

## Steering, not scripting

The model does not play the game click by click. It asserts a posture
(`set_phase`) that sets the 10 personality axes (boldness, greed, honour, …)
and the subsystem toggles (declare war, marriages, schemes, …) the native AI
reads every tick, and occasionally forces one specific action after
validating it (`declare_war`, `do`). Execution — raising armies,
sieges, counter-offers — stays with the game's AI. This is why a cycle is
cheap (a handful of tool calls) and why the game stays coherent when the
model is slow or absent.

## Research instrumentation

The bridge also carries the authors' experiment machinery: an episode policy,
a decision-budget ledger, a game-speed pin, a shared append-only board, pause
and clock witnesses, and a per-session "seat" declaration in the handshake.
All of it is compiled in, gated by `CK3_EPISODE_*` / `CK3_BOARD_*` /
`CK3_BUDGET_*` environment variables, and inert when they are unset. See
[research.md](research.md).
