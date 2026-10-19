# Files, paths and environment

Where ck3agent puts things, how to install and remove it by hand, and the
environment variables the `ck3agent` command, MCP server and hook read.

## Prerequisites

None. `ck3agent config` prints the paths in use; `ck3agent doctor` checks them.

## What goes where

`DATA` is `${XDG_DATA_HOME:-~/.local/share}/ck3agent`. `RUN` is
`$XDG_RUNTIME_DIR/ck3agent`. Without `XDG_RUNTIME_DIR`, `RUN` is
`/run/user/<uid>/ck3agent` if `/run/user/<uid>` exists, else
`~/.local/state/ck3agent`. `RUN` is mode 0700 and must be owned by you.

| Piece | Location | Written by |
|-------|----------|------------|
| `libck3_bridge.so` | `DATA/lib/` | `install.sh` |
| `trigger_subclasses.csv`, the trigger catalogue | `DATA/share/`, also inside the package | `install.sh` |
| `ck3agent` command | a pipx venv, or `DATA/venv`, a link to `DATA/venv.<version>.<random>` that each install replaces; linked into `~/.local/bin/` | `install.sh` |
| `config.toml` | `${XDG_CONFIG_HOME:-~/.config}/ck3agent/`, or `CK3AGENT_CONFIG` | `ck3agent setup` |
| play directory: `.mcp.json`, `CLAUDE.md`, `.claude/settings.json`, `runs/` | `~/ck3agent-play` | `ck3agent setup`, `ck3agent run` |
| game output | `DATA/logs/ck3-<timestamp>.log` | `ck3agent launch` |
| last launch record | `DATA/state/last-launch.json` | `ck3agent launch` |
| bridge socket, mode 0600 | `RUN/agent.sock` | the bridge |
| bridge log | `RUN/bridge.log` | the bridge |
| action log | `RUN/telemetry/action_log.jsonl` | the bridge |
| default-off write list, `ck3-enabled-writes` | `RUN/` | you, by hand; see [research notes](../research.md#default-off-writes) |

A named instance (`CK3_INSTANCE_ID=<id>`) moves the socket, log and telemetry
to `RUN/inst/<id>/`. `CK3_INSTANCE_LAYOUT=legacy` restores the old shared
`/tmp` layout with a 0666 socket that any local user can reach; use it only
on a single-user lab host.

`ck3agent` never writes into the game directory. `launch --save` copies the
save into CK3's `save games/` folder and points `continue_game.json` at it,
keeping the old pointer as `continue_game.json.bak-ck3agent`.

## Install by hand

Use this when `install.sh` does not suit your system. From the unpacked release
(a later `install.sh` replaces this `venv` directory with its link and deletes the old one):

```bash
python3 -m venv ~/.local/share/ck3agent/venv
~/.local/share/ck3agent/venv/bin/pip install wheels/ck3agent-*.whl
mkdir -p ~/.local/bin && ln -sf ~/.local/share/ck3agent/venv/bin/ck3agent ~/.local/bin/ck3agent
install -D -m 0755 lib/libck3_bridge.so ~/.local/share/ck3agent/lib/libck3_bridge.so
install -D -m 0644 share/trigger_subclasses.csv ~/.local/share/ck3agent/share/trigger_subclasses.csv
ck3agent setup        # --ck3 PATH and --bridge PATH override discovery
```

## Uninstall

1. From the unpacked release, run `./uninstall.sh`. It removes the pipx
   package and all of `DATA`, including the venv, the bridge and the logs.
2. `./uninstall.sh --purge` also removes the config directory. It asks before
   deleting the play directory, and keeps it when not run interactively.
3. If you added `LD_PRELOAD=...` to the game's Steam launch options, remove it.

## Environment variables

Variables read only by the bridge are in
[research notes](../research.md#bridge-environment-variables).

| Variable | Effect |
|----------|--------|
| `CK3_BINARY` | path to the `ck3` binary; overrides Steam discovery |
| `CK3AGENT_BRIDGE` | path to `libck3_bridge.so` |
| `CK3AGENT_CONFIG` | path to `config.toml` |
| `CK3_TRIGGER_CATALOGUE_CSV` | trigger catalogue path. `launch` sets it for the bridge from the resolved catalogue. |
| `CK3_BRIDGE_SOCKET` | bridge socket path |
| `CK3_BRIDGE_LOG` | bridge log path |
| `CK3_TELE` | telemetry directory, which holds `action_log.jsonl` |
| `CK3_INSTANCE_ID` | names an instance; see [What goes where](#what-goes-where) |
| `CK3_INSTANCE_ROOT` | parent directory for named instances |
| `CK3_INSTANCE_LAYOUT` | `user`, the default, or `legacy` |
| `CK3_CONTROL_DIR` | operator control directory; research layout only |
| `CK3_AGENT_CHARACTER_ID` | the MCP server binds this character at connect, before Claude's first turn; a comma-separated list binds several, the first as the primary |
| `CK3_AGENT_PREBIND_WAIT_S` | how long that bind waits for the game to be active (a save still loading) before it is attempted anyway: seconds, default 120, polled every 2 s; `0` attempts at once |
| `CK3_TOOL_SURFACE_OUT` | the MCP server writes its registered tool list to this file |
| `CK3_RELEASE_ON_EXIT` | `run` sets it; the MCP server releases its own claims when it stops and writes the record here |
| `CK3_SESSION_OUT` | `run` sets it; the MCP server writes its session id and resume token here so `run` can finish the release if the server is killed first |
| `CK3_EPISODE_ARM` | experiment arm label; arm `b` registers the board tools |
| `CK3_BOARD_ENABLED` | overrides the arm: any value but `0` or `false` registers `board_post` and `board_read` |
| `CK3_STRICT_BUILD_ID` | `launch --allow-unsupported-build` sets it to `0` for the bridge |
| `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_RUNTIME_DIR` | base directories above; an empty value counts as unset |
| `WAYLAND_DISPLAY`, `SDL_VIDEODRIVER` | when the first is set and the second is not, `launch` runs the game under XWayland |
| `NO_COLOR` | plain CLI output |

## Check it worked

`ck3agent config` shows the paths you expect, and `ck3agent doctor` passes.

## If it fails

A path override that points nowhere fails at first use with the real error.
Unset the variable and run `ck3agent doctor` again.
