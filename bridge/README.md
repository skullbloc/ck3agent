# bridge/

The C++ library `libck3_bridge.so`. Loaded into CK3 with `LD_PRELOAD`, it hooks
the AI tick, reads game memory, and serves NDJSON on a Unix socket. It knows
nothing about LLMs; `ck3agent/server/` is the client.

| Path | What |
|------|------|
| `src/` | hooks, IPC server, query and action handlers |
| `include/` | headers; `action_dispatch_internal.h` is the shared handler API |
| `test/` | engine-free tests and probes ([README](test/README.md)) |
| `tools/ck3mapgen/` | optional map pre-processor ([README](tools/ck3mapgen/README.md)) |
| `third_party/` | RapidJSON, stb_image, Zydis headers, each with its license |

Offsets come from `../re/offsets/` ([re/README.md](../re/README.md)).

* **Build:** `make bridge` at the repo root. Zydis is fetched at configure time.
* **Add a handler:** one line in `src/action_registry.inc`, the handler
  function, and a pause-state class in `src/snapshot.cpp`. Dispatch and the
  registry both expand from that line.
* **Threads:** `src/ipc_server.cpp` reads on its own thread and queues anything
  that touches the game. Game memory is written only on the AI tick.
* **Rules:** CONTRIBUTING sections 5 and 7. The SIGSEGV guards are a crash
  net, not a safety boundary.
