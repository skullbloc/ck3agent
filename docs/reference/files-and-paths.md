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
| `ck3agent` command | a pipx venv, or `DATA/venv`, a link to `DATA/venv.<version>.<random>` that each install replaces; linked into `~/.local/bin/` | `install.sh` |
| `config.toml` | `${XDG_CONFIG_HOME:-~/.config}/ck3agent/`, or `CK3AGENT_CONFIG` | `ck3agent setup` |
| play directory: `.mcp.json`, `CLAUDE.md`, `.claude/settings.json`, `runs/` | `~/ck3agent-play` | `ck3agent setup`, `ck3agent run` |
| game output | `DATA/logs/ck3-<timestamp>.log` | `ck3agent launch` |
| the launched game's user-data directory (`-userdir=`): `save games/`, autosaves, `continue_game.json`, `logs/`, its copy of your settings | `${XDG_STATE_HOME:-~/.local/state}/ck3agent/game/` | `ck3agent launch`, the game |
| last launch record | `DATA/state/last-launch.json` | `ck3agent launch` |
| bridge socket, mode 0600 | `RUN/agent.sock` | the bridge |
| bridge log | `RUN/bridge.log` | the bridge |
| action log | `RUN/telemetry/action_log.jsonl` | the bridge |
| default-off write list, `ck3-enabled-writes` | `RUN/` | you, by hand; see [research notes](../research.md#default-off-writes) |

A named instance (`CK3_INSTANCE_ID=<id>`) moves the socket, log and telemetry
to `RUN/inst/<id>/`. `CK3_INSTANCE_LAYOUT=legacy` restores the old shared
`/tmp` layout with a 0666 socket that any local user can reach; use it only
on a single-user lab host.

`ck3agent` never writes into the game's install directory or into your own
CK3 user-data directory, `~/.local/share/Paradox Interactive/Crusader Kings
III/`. `launch` starts the game with `-userdir=` set to its own directory
(above). The first time, it copies your settings files there and links your
`mod/` folder. `--save PATH` copies the save into that directory's
`save games/` and points its `continue_game.json` at it (`title`, `desc` and
`date`, the three keys CK3 reads from that file before its main menu). The first `--continue`
does the same with the save your CK3 played last. The uninstaller leaves the
directory in place: it holds the saves Claude played. Delete it by hand.

## Install by hand

Use this when `install.sh` does not suit your system. From the unpacked release
(a later `install.sh` replaces this `venv` directory with its link and deletes the old one):

```bash
python3 -m venv ~/.local/share/ck3agent/venv
~/.local/share/ck3agent/venv/bin/pip install wheels/ck3agent-*.whl
mkdir -p ~/.local/bin && ln -sf ~/.local/share/ck3agent/venv/bin/ck3agent ~/.local/bin/ck3agent
install -D -m 0755 lib/libck3_bridge.so ~/.local/share/ck3agent/lib/libck3_bridge.so
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
| `CK3_EPISODE_ARM` | experiment arm label, stamped into the server's tool-surface record |
| `CK3_STRICT_BUILD_ID` | `launch --allow-unsupported-build` sets it to `0` for the bridge |
| `CK3_BRIDGE_ENDPOINT` | the bridge endpoint the MCP server connects to, on any OS; overrides the socket path and the Windows default |
| `CK3_BRIDGE_ARMY_ORDERS`, `CK3_BRIDGE_ARMY_HOLD` | `1` in the server's environment lists `do`'s `army` intent (orders, and holds); the bridge reads the same switches ([research notes](../research.md#bridge-environment-variables)) |
| `CK3_NOTES` | `off` at the server's start turns the notes off: `notes` leaves `discover`, the instructions carry no notes line; unset or `on` is on; any other value refuses the start |
| `CK3_NOTES_SET` | the name of the notes set the server serves (a set in `notes.json`); unset is the default set |
| `CK3_REPLY_PAGES` | `off` gives back the single-reply size rule: no paged replies and no `inspect(subject="more")` |
| `CK3_TOOL_PREFIX` | the prefix the client puts before the tool names (for example `mcp__ck3__`); the server writes it into the calls its replies suggest |
| `CK3_GOAL_TITLE` | the key of the title the run's goal names (for example `k_ireland`): the create_title veto never drops that title's creation, and a cut reply keeps its `formable_titles` row |
| `CK3_GOAL_KIND` | the goal's kind; under a goal that forbids offensive war the news leaves out the goal-title lines and the calls that offer a war |
| `CK3_START_BRIEFING`, `CK3_START_BRIEFING_SHA256` | a start briefing handed to the seat (the file and its sha256); the server checks the digest and warns `start_briefing_unread` when it cannot read it |
| `CK3_GAME_DIR` | the game's `game/` folder; the server reads event, faction and price files from it (default: the Steam path) |
| `CK3_CLOCK_OWNER` | `env`: an environment owns the game clock, so `session` does not list or serve its clock actions |
| `CK3_SESSION_ID` | the session id the server stamps into its rows; `run` sets it |
| `CK3_HOOK_STATE_OUT` | the live-state hook also appends each block it prints to this file |
| `CK3_PROPOSAL_MOD`, `CK3_SEAT_PROPOSALS_OUT` | `1` when the game runs the proposal mod (`observe` then carries the proposal block), and the file the server records the seat's answers in |
| `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_RUNTIME_DIR` | base directories above; an empty value counts as unset |
| `WAYLAND_DISPLAY`, `SDL_VIDEODRIVER` | when the first is set and the second is not, `launch` runs the game under XWayland |
| `NO_COLOR`, `TERM` | plain CLI output (`NO_COLOR` set, or `TERM=dumb`) |
| `HOME` | where the MCP server writes a debug log, `~/.ck3-mcp-debug.log`, when it is run directly as a script (`python3 ck3agent/server/mcp_server.py`); `ck3agent serve` writes none |

### Experiment seats

An experiment's launcher sets these for each seat; a seat run with none of them
behaves as a plain install. `tests/test_files_and_paths_env.py` checks that every
variable the code names is a row here or above.

| Variable | Effect |
|----------|--------|
| `CK3_EPISODE_ID`, `CK3_EPISODE_ARM` | the episode and the arm the seat runs in |
| `CK3_EPISODE_HORIZON_START`, `CK3_EPISODE_HORIZON_END`, `CK3_CYCLE_GAME_DAYS` | the episode's game-date horizon and the seat's decision cycle in game days; `session(action="wait")` waits to the next cycle boundary |
| `CK3_EPISODE_LINE_START_ID`, `CK3_EPISODE_R_OPEN`, `CK3_EPISODE_OPEN_DATE` | the goal meter's sample at the episode's open, which the goal block in `observe` reads |
| `CK3_SEAT_LABEL` | which seat this server is |
| `CK3_SEAT_DECLARED_CHARACTER_ID`, `CK3_RESERVED_FOR_OTHER_SEATS` | the seat's declared ruler, and the characters declared for the other seats (a claim of one is refused) |
| `CK3_SEAT_LINEAGE_OUT`, `CK3_SEAT_LINEAGE_TITLE_ID` | the file the seat's lineage rows go to, and the title a king's seat follows |
| `CK3_CHANNEL_TOPOLOGY`, `CK3_CHANNEL_SEATS`, `CK3_CHANNEL_QUOTA_PER_CYCLE`, `CK3_CHANNEL_MAX_TEXT_BYTES` | the seats' message channel: who may write to whom, the seats, and its per-cycle quota and message size |
| `CK3_NOTEPAD_PADS`, `CK3_NOTEPAD_PRIVATE_MAX_BYTES`, `CK3_NOTEPAD_COUNCIL_MAX_BYTES`, `CK3_NOTEPAD_MAX_WRITE_BYTES` | the seat's notepads and their size bounds |
| `CK3_GUIDE_DIR`, `CK3_GUIDE_MANIFEST_SHA256` | the arm's guide directory and its manifest digest |

## Check it worked

`ck3agent config` shows the paths you expect, and `ck3agent doctor` passes.

## If it fails

A path override that points nowhere fails at first use with the real error.
Unset the variable and run `ck3agent doctor` again.
