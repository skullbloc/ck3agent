# Using ck3agent in research

ck3agent was built for a pre-registered study of language-model steering in a
grand-strategy game. This page says what in the release is meant for research
use, what is not, and how to cite it.

## Citing

`CITATION.cff` at the repository root is read by GitHub's "Cite this
repository" button. Cite the exact version you ran (`ck3agent version` prints
it together with the supported game BuildID), and record the game build and
save file alongside.

## Running conditions

* **Treatment** (model steering): `ck3agent run` with a starting prompt, or an
  interactive Claude Code session. One play directory per condition keeps the
  `CLAUDE.md` (persona, goal, house rules) with the condition.
* **Control** (native AI): the game with the bridge attached and no client.
  The bridge steers nothing on its own; its presence changes nothing about the
  AI's decisions except the hooks' CPU time.
* **Observation only**: connect with `mode="observe"` (the handshake in
  `bridge_client.py`) to read state without being granted control. `ck3agent
  doctor` does this.

What is recorded: the bridge's action log (`/tmp/ck3-telemetry/action_log.jsonl`,
every write with the game date and the requesting session), the run prompt
and transcript under `runs/`, and the game's own save. Nothing is sealed,
hashed or signed in this release; that machinery stayed in the authors'
private tree because it is specific to their study design.

## The instrumentation that ships inside the bridge

These are compiled into `libck3_bridge.so`, inert unless their environment
variables are set, and reported in the handshake ack so a reader can see which
were in force:

| Variables | What it does when set |
|-----------|-----------------------|
| `CK3_EPISODE_MODE`, `CK3_EPISODE_ID`, `CK3_EPISODE_ARM`, `CK3_EPISODE_GAME_SPEED`, `CK3_EPISODE_DIR` | declares an "episode": a fixed game speed the bridge restores after every pause, an id stamped into logs, and (if `CK3_EPISODE_DIR` is set) a session ledger written there |
| `CK3_BUDGET_PER_AGENT_PER_YEAR`, `CK3_BUDGET_ENFORCE` | a per-session decision budget; refusals are logged, and enforced only with the second variable |
| `CK3_BOARD_ENABLED`, `CK3_BOARD_ADMIN_TOKEN`, `CK3_BOARD_CARRIER_PATH` | a shared append-only message board between several agent sessions (`board_post` / `board_read` MCP tools appear only when enabled) |
| `CK3_PIPELINE_MODE` | how queued writes are flushed onto the game tick |
| `CK3_STRICT_BUILD_ID` | the BuildID refusal (see [supported-versions](supported-versions.md)) |
| `CK3_INSTANCE_ID`, `CK3_INSTANCE_ROOT`, `CK3_BRIDGE_SOCKET`, `CK3_TELE`, `CK3_BRIDGE_LOG` | where the socket, telemetry and logs go; several games per host |
| `CK3_AI_WATCH`, `CK3_BRIDGE_TRACE`, `CK3_BRIDGE_STRICT` | diagnostics: AI-flag watcher, verbose tracing, abort instead of recover on a caught fault |

The Python side reads `CK3_EPISODE_ARM` / `CK3_BOARD_ENABLED` (board tools),
`CK3_AGENT_CHARACTER_ID` (pre-bind a character at handshake) and
`CK3_TOOL_SURFACE_OUT` (write the registered tool list to a file).

## What a run cannot tell you

* The model's chain of reasoning is in the transcript only as far as the host
  prints it; `claude -p` prints the final text and tool calls, not hidden
  reasoning.
* The bridge sees the game's state, not its RNG. Two runs from the same save
  diverge; use many seeds (saves) rather than reruns of one.
* Game speed and wall-clock interact: a slow model at a high game speed
  misses events between cycles. Fix the speed per condition (the episode
  variables do this) and report it.
