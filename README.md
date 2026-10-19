# ck3agent

[![CI](https://github.com/skullbloc/ck3agent/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/skullbloc/ck3agent/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/skullbloc/ck3agent?include_prereleases)](https://github.com/skullbloc/ck3agent/releases)
[![Code: Apache-2.0](https://img.shields.io/badge/code-Apache--2.0-blue.svg)](LICENSE)
[![Docs: CC BY 4.0](https://img.shields.io/badge/docs-CC_BY_4.0-lightgrey.svg)](LICENSE-docs)

Let Claude play Crusader Kings III. A library loaded into the game exposes its
state to Claude through MCP, the protocol Claude uses to call tools. Claude
sets a ruler's strategy. The game's own AI carries it out.

## What you get

- **Watch Claude rule.** Load an observer save; Claude takes a ruler, plays it and explains each move. You interrupt and redirect it in plain language.
- **Play against Claude.** Load your own campaign; Claude takes a rival or a neighbour while you keep playing your character. Claude can never take your character.
- **Run it unattended.** `ck3agent run` plays a goal for N cycles and records a transcript and a manifest.
- **Several rulers at once.** One Claude, one session, one posture per ruler; the same game clock.

## What you need

- Linux x86_64 with glibc 2.34 or newer: Ubuntu 22.04 or later, Debian 12 or later, Fedora (tested on 44), Arch.
- Crusader Kings III 1.18.3.1, the native Linux build from Steam.
- Python 3.11 or newer.
- [Claude Code](https://docs.claude.com/en/docs/claude-code) and a Claude plan or API key. Every round of play (a cycle: observe, decide, act, let a game month pass) is a paid model call, about $0.20 to $2 at API prices depending on how much Claude reads; an evening of play is a few dollars, and a subscription plan counts it against your usage limit.

## Will it work for me?

| You have | Works? |
|----------|--------|
| Linux, native Steam package, CK3 1.18.3.1 | Yes |
| CK3 updated past 1.18.3.1 | Not until a matching release. See [Your game updated](docs/supported-versions.md). |
| Flatpak or Snap Steam | No. See [troubleshooting](docs/troubleshooting.md#flatpak-or-snap-steam). |
| Steam Deck | Untested. |
| Windows or macOS | Not yet. See the [port plan](docs/adr/0001-windows-macos-port.md). |
| Ironman, achievements, multiplayer | No. Single-player, non-Ironman saves only. |

## Install

```bash
sha256sum -c SHA256SUMS --ignore-missing     # in the download directory
tar xzf ck3agent-2026.10.0-linux-x86_64.tar.gz && cd ck3agent-2026.10.0-linux-x86_64
./install.sh                                 # installs the bridge and the ck3agent command
```

Before you launch: have a non-Ironman single-player save ready, and copy any
campaign you care about out of `~/.local/share/Paradox Interactive/Crusader
Kings III/save games/`. ck3agent never writes into the game folder, but once
the clock runs the game's own autosave rotation does. Claude cannot play your
character: use an observer save (start a new game, choose **Observe**, save),
or let Claude take an AI ruler in your own campaign.
[Getting started](docs/getting-started.md) covers both.

```bash
ck3agent launch --continue      # loads the save CK3 played last; --save PATH loads a specific one
cd ~/ck3agent-play && claude
```

## Documentation

| Page | For |
|------|-----|
| [Getting started](docs/getting-started.md) | install, launch, choose the ruler, first session |
| [Claude Code](docs/claude-code.md) | interactive play, time and cost, stopping |
| [Claude Desktop](docs/claude-desktop.md) | Desktop instead of Claude Code |
| [Scripted runs](docs/scripted-runs.md) | unattended runs with a recorded manifest |
| [Troubleshooting](docs/troubleshooting.md) | symptom, then fix |
| [All docs](docs/README.md) | reference, research, architecture, glossary |

## Help and contributing

Something broke: [troubleshooting](docs/troubleshooting.md), then an
[issue](https://github.com/skullbloc/ck3agent/issues/new/choose); the forms ask
for `ck3agent doctor` output. Questions and show-and-tell:
[Discussions](https://github.com/skullbloc/ck3agent/discussions). Security
problems: [SECURITY.md](SECURITY.md), never a public issue.

Contributions are welcome under the DCO: [CONTRIBUTING.md](CONTRIBUTING.md),
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Windows and macOS:
[port plan](docs/adr/0001-windows-macos-port.md). Built on Zydis, RapidJSON,
stb_image and the MCP Python SDK ([THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)).

## Status and license

Alpha, versioned by date ([CHANGELOG.md](CHANGELOG.md)). Each release supports
one game build and refuses all others. Code [Apache-2.0](LICENSE), docs
[CC BY 4.0](LICENSE-docs), third party [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md),
[SECURITY.md](SECURITY.md), [CITATION.cff](CITATION.cff).

Crusader Kings® and Paradox Interactive® are registered trademarks of Paradox
Interactive AB; Claude is a trademark of Anthropic, PBC. ck3agent is not
affiliated with or endorsed by either. It ships no game assets and no game
code beyond the short byte patterns in `re/offsets/signatures.h` that locate
functions at load; it works by reading and writing the memory of a copy of the
game you own, with offsets found by reverse engineering its Linux binary.
Paradox's EULA and Steam's terms are between you and them: single-player
only, never Ironman, achievement or multiplayer games.
