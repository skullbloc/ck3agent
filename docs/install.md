# Installing

## What gets installed where

| Piece | Location | Installed by |
|------|----------|--------------|
| `libck3_bridge.so` | `${XDG_DATA_HOME:-~/.local/share}/ck3agent/lib/` | `install.sh` |
| `trigger_subclasses.csv` (trigger catalogue for `discover(what="evaluate_trigger")`) | `…/ck3agent/share/` | `install.sh` |
| `ck3agent` Python package + command | pipx venv, or `~/.local/lib/python3.x/site-packages` + `~/.local/bin/ck3agent` | `install.sh` (pipx if present, else `pip --user`) |
| `config.toml` | `${XDG_CONFIG_HOME:-~/.config}/ck3agent/` | `ck3agent setup` |
| play directory (`.mcp.json`, `CLAUDE.md`, `.claude/settings.json`) | `~/ck3agent-play` by default | `ck3agent setup` |
| game output logs | `…/ck3agent/logs/ck3-<timestamp>.log` | `ck3agent launch` |
| bridge log | `/tmp/ck3_bridge.log` (override `CK3_BRIDGE_LOG`) | the bridge |
| bridge socket | `/tmp/ck3-agent.sock` (override `CK3_BRIDGE_SOCKET`) | the bridge |

Nothing is written into the game directory. Uninstall with `./uninstall.sh`
(`--purge` also removes the config).

## Requirements

* Linux x86_64 with glibc 2.35 or newer (Ubuntu 22.04, Fedora 36, Debian 12
  or later). The bridge is built on Ubuntu 22.04.
* Crusader Kings III **1.18.3.1**, the native Linux build from Steam (not the
  Windows build under Proton: that is a different binary and is refused).
* Python 3.11 or newer. `pipx` is recommended but optional.
* An MCP host: [Claude Code](https://docs.claude.com/en/docs/claude-code)
  on the same machine is the supported path. Claude Desktop works over HTTP
  from another machine, see [claude-desktop.md](claude-desktop.md).

## Installing without the installer

```bash
pip install --user ck3agent-2026.10.0-py3-none-any.whl     # or pipx install
mkdir -p ~/.local/share/ck3agent/lib ~/.local/share/ck3agent/share
cp lib/libck3_bridge.so ~/.local/share/ck3agent/lib/
cp share/trigger_subclasses.csv ~/.local/share/ck3agent/share/
ck3agent setup
```

`ck3agent setup --ck3 /path/to/ck3 --bridge /path/to/libck3_bridge.so` overrides
discovery; `CK3_BINARY` and `CK3AGENT_BRIDGE` do the same from the environment.

## Verifying a download

Each release ships `SHA256SUMS` and a GitHub build-provenance attestation:

```bash
sha256sum -c SHA256SUMS --ignore-missing
gh attestation verify ck3agent-2026.10.0-linux-x86_64.tar.gz --repo skullbloc/ck3agent
```

(Attestations exist only for releases built while the repository was public;
`SHA256SUMS` is always there.)

## Building from source

See [building.md](building.md).
