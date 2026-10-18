# ck3agent

**Let Claude play Crusader Kings III.** ck3agent hooks the game's AI subsystem
from inside the process and exposes it as an [MCP](https://modelcontextprotocol.io)
server, so a language model can take over the *strategic* decisions of a ruler
while the game's own AI keeps executing them. It is a research tool with the
ergonomics of a product: install, run `ck3agent setup`, start the game, open
Claude Code, and play.

```
[CK3 process]                                  [your terminal]
  libck3_bridge.so  (loaded with LD_PRELOAD)      ck3agent serve  (MCP server, stdio)
  · hooks the AI tick                             · 15 tools: observe, observe_v2, inspect,
  · reads game state from memory                    query, discover, cbs, plan_war,
  · writes personality weights + toggles            succession_plan, set_phase, steer,
  · NDJSON over /tmp/ck3-agent.sock                 declare_war, do, intent, session, pass_turn
            └──────────────── Unix socket ────────────────┘
                                                  Claude Code / Claude Desktop / `ck3agent run`
```

The model **steers** rather than micromanages: it asserts a posture
(`set_phase`) that sets the AI's 10 personality axes and subsystem toggles,
validates and issues the occasional explicit action (`declare_war` from a
`plan_war` row, a council task or decision through `do`), and lets time run.
The native AI handles armies, sieges and the thousand small choices.

## Supported game build

| | |
|---|---|
| Game | Crusader Kings III **1.18.3.1** (Crane), **Linux** native build (Steam) |
| BuildID | `e61ff2642f07094defd140fcc98e078d42541275` |
| Platforms | Linux x86_64 only in this release. Windows and macOS: see [the port design](docs/adr/0001-windows-macos-port.md). |

The bridge reads game memory at fixed offsets that are valid for exactly one
binary. **Any other CK3 build is refused at load time** with a message in the
bridge log; `ck3agent doctor` tells you before you launch. Each game patch
needs a new ck3agent release (see [supported versions](docs/supported-versions.md)).

## Install (Linux)

1. Download `ck3agent-<version>-linux-x86_64.tar.gz` from the
   [releases page](https://github.com/skullbloc/ck3agent/releases), verify it
   against `SHA256SUMS`, unpack it.
2. `./install.sh` — installs the bridge under `~/.local/share/ck3agent`, the
   `ck3agent` command for your user (pipx if present, else `pip --user`), then
   runs `ck3agent setup`, which finds your Steam install, checks the game
   build, and creates a play directory (`~/ck3agent-play`) with the MCP config
   and the play protocol Claude follows.
3. `ck3agent launch --continue` — starts CK3 with the bridge loaded (or add
   `LD_PRELOAD=~/.local/share/ck3agent/lib/libck3_bridge.so %command%` to the
   game's Steam launch options and launch from Steam).
4. In another terminal: `cd ~/ck3agent-play && claude`. Claude Code picks up
   `.mcp.json` and `CLAUDE.md` and starts playing when you ask it to.

`ck3agent doctor` checks every link in that chain and names the broken one.
Full details: [docs/quickstart.md](docs/quickstart.md), [docs/install.md](docs/install.md).

Requirements: Linux x86_64, CK3 1.18.3.1 (Steam, native Linux build), Python
3.11+, and an MCP host — [Claude Code](https://docs.claude.com/en/docs/claude-code)
on the same machine, or Claude Desktop on another machine over HTTP
([docs/claude-desktop.md](docs/claude-desktop.md)).

## Ways to play

* **Interactive (Claude Code).** The default. You talk to Claude in the play
  directory; it observes, reasons, acts and narrates in cycles, and you can
  step in at any time. Each prompt gets a fresh game-state block from the
  installed hook. [docs/claude-code.md](docs/claude-code.md)
* **Claude Desktop.** Run `ck3agent serve --transport http` on the game machine
  and connect Desktop to it. [docs/claude-desktop.md](docs/claude-desktop.md)
* **Scripted runs (research).** `ck3agent run --goal "..." --cycles 100`
  launches an unattended Claude Code session with a starting prompt and a
  cycle budget, logging everything under the play directory.
  [docs/scripted-runs.md](docs/scripted-runs.md)

## Documentation

| | |
|---|---|
| [Quickstart](docs/quickstart.md) · [Install](docs/install.md) · [Troubleshooting](docs/troubleshooting.md) | getting it running |
| [Claude Code](docs/claude-code.md) · [Claude Desktop](docs/claude-desktop.md) · [Scripted runs](docs/scripted-runs.md) | the three ways to drive it |
| [Tool reference](docs/reference/tool-reference.md) · [Agent–character contract](docs/reference/agent-character-contract.md) · [Personality axes](docs/reference/personality-weights.md) | what the model can see and do |
| [Architecture](docs/architecture.md) · [Building from source](docs/building.md) · [Supported versions](docs/supported-versions.md) | how it works and how to rebuild it |
| [Research notes](docs/research.md) · [CITATION.cff](CITATION.cff) | using it in a paper |
| [Windows/macOS port design](docs/adr/0001-windows-macos-port.md) | where the other platforms stand |

## Status

Alpha, versioned by calendar (`2026.10.0`). The tool surface is stable in
shape (15 tools, enveloped responses) but sub-operations still change between
releases; read [CHANGELOG.md](CHANGELOG.md). The bridge contains instrumentation
used by the authors' experiments (episode ledgers, budget counters, a shared
board). It is compiled in, off by default, and documented in
[docs/research.md](docs/research.md).

## License

Code: [Apache-2.0](LICENSE). Documentation: [CC BY 4.0](LICENSE-docs).
Third-party components: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
Crusader Kings III is a trademark of Paradox Interactive AB; this project is
not affiliated with Paradox and ships no game assets or game code.

If you use ck3agent in research, please cite it — GitHub's "Cite this
repository" button reads [CITATION.cff](CITATION.cff).
