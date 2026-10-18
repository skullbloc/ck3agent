# ck3agent

Let Claude play Crusader Kings III. A library loaded into the game exposes its
state to Claude through MCP, the protocol Claude uses to call tools. Claude
sets a ruler's strategy. The game's own AI carries it out.

## What you get

- Claude rules one AI character in your single-player game and explains each move.
- You watch, interrupt and redirect it in plain language.
- Unattended runs that record a transcript and a manifest.

## What you need

- Linux x86_64 with glibc 2.34 or newer: Ubuntu 22.04, Debian 12, Fedora 35 or later.
- Crusader Kings III 1.18.3.1, the native Linux build from Steam.
- Python 3.11 or newer.
- [Claude Code](https://docs.claude.com/en/docs/claude-code) and a Claude plan or API key. Every cycle uses model tokens.

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
ck3agent launch --continue                   # then: cd ~/ck3agent-play && claude
```

Claude cannot play your own character. Use an observer save, or let Claude
take an AI ruler. [Getting started](docs/getting-started.md) covers both.

## Documentation

| Page | For |
|------|-----|
| [Getting started](docs/getting-started.md) | install, launch, choose the ruler, first session |
| [Claude Code](docs/claude-code.md) | interactive play, time and cost, stopping |
| [Claude Desktop](docs/claude-desktop.md) | Desktop instead of Claude Code |
| [Scripted runs](docs/scripted-runs.md) | unattended runs with a recorded manifest |
| [Troubleshooting](docs/troubleshooting.md) | symptom, then fix |
| [All docs](docs/README.md) | reference, research, architecture, glossary |

## Status and license

Alpha, versioned by date ([CHANGELOG.md](CHANGELOG.md)). Each release supports
one game build and refuses all others. Code [Apache-2.0](LICENSE), docs
[CC BY 4.0](LICENSE-docs), third party [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md),
[SECURITY.md](SECURITY.md), [CITATION.cff](CITATION.cff). Crusader Kings III is a
trademark of Paradox Interactive AB. Not affiliated with Paradox; ships no game code or assets.
