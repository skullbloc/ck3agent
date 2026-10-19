# Troubleshooting

Find your symptom, apply the fix. Start with `ck3agent doctor`: it checks
each link in order and names the first broken one.

## Prerequisites

- `ck3agent` installed. `ck3agent config` prints the paths in use.

## Logs

| Log | Default path |
|-----|------|
| Bridge | `$XDG_RUNTIME_DIR/ck3agent/bridge.log` |
| Bridge action log | `$XDG_RUNTIME_DIR/ck3agent/telemetry/action_log.jsonl` |
| Game output from `ck3agent launch` | `~/.local/share/ck3agent/logs/ck3-<timestamp>.log` |
| Game's own logs | `~/.local/share/Paradox Interactive/Crusader Kings III/logs/` (`debug.log`, `error.log`) |

`$XDG_RUNTIME_DIR` is usually `/run/user/<uid>` and is cleared at logout.
Copy the logs before you reboot if you plan to report a bug. Fallbacks are in
[files and paths](reference/files-and-paths.md#what-goes-where).

## Symptoms

### Setup or doctor says the build is not supported

Steam updated your game past 1.18.3.1. See [Your game updated](supported-versions.md).

### The game stays `dormant` after `ck3agent launch --continue`

The bridge is loaded but no game is running yet. On a player save, click
**Continue** in the game window. On an observer save, click **Observe** if
`launch` timed out. `doctor` shows `session_state=active` once the map loads.

### The game never finishes loading (Wayland)

The game can stall on load under Wayland; `debug.log` stops a few seconds in.
`ck3agent launch` runs it under XWayland by default. From Steam, put
`SDL_VIDEODRIVER=x11` before the launch option `setup` printed.

### `bridge socket not listening`

1. The game is still starting. `launch --continue` waits up to 120 seconds.
2. The bridge refused the build: the bridge log says `REFUSED: unsupported CK3 build`.
3. The game started without the bridge. Use `ck3agent launch`, or the Steam
   launch option `setup` printed.

### Claude Code does not list the `ck3` tools

Start `claude` in the play directory, where `.mcp.json` is. Check that
`~/.local/bin` is on `PATH` and that `ck3agent serve` starts by hand. Ctrl-C quits it.

### Claude says it controls no ruler, or `no_ruler_bound`

Nothing is bound to Claude's session. `whoami`'s
`blocked.measured.unbound_reason` says which case: `heir_is_player` means
Claude's ruler died with you as heir, and the bridge unbound rather than hand
Claude your character. Empty means the bridge recorded no reason: Claude never
took a ruler, the bridge refused your character, or Claude Code restarted or
reconnected the MCP server (a `heir_is_player` reason does not survive it),
which starts a new session bound only to
`CK3_AGENT_CHARACTER_ID`, if set. Ask Claude to take an
AI ruler: see [choose the ruler](getting-started.md#4-choose-the-ruler-claude-plays).
`actor_unresolved` is different: the read failed. Check `session(action="health")`.
`cannot_act_as_player` from `whoami` means you inherited Claude's ruler.

With no ruler bound and no `character_id` given, `whoami`, `observe_v2`,
`inspect`, `plan_war`, `declare_war`, `cbs`, `succession_plan`, `set_phase`
and `do` return `ok: false` with `blocked.gate: "no_ruler_bound"`. The tools
`observe`, `query`, `discover`, `steer` and `intent` are not checked and
return `ok: true` with the bridge's empty or declined reply, for example
`observe`'s `data.character.success: false`. `discover(what="evaluate_trigger")`
evaluates against the engine's player character, which is yours on a player
save.

### `game_paused`, `game_unpaused` or `cb_query_timeout`

`observe`, `query` and `discover` need the game paused; the other tools set
pause state themselves. Ask Claude to pause and retry once. If it repeats,
`session(action="health")` shows whether the bridge is hung.

### `experimental_write_disabled`

Two actions, creating or joining a faction and breaking an alliance, are off
by default because the engine code they call has not been exercised
crash-free. Claude cannot turn them on. If you want them in your game, add the
action name to the file named in the reply's `blocked.fix` (one name per line,
no relaunch needed): [default-off writes](research.md#default-off-writes).

### Flatpak or Snap Steam

Unsupported. The sandbox gives the game its own home and runtime directories,
so it cannot load the bridge from `~/.local/share` and the MCP server cannot
reach the bridge socket. Install Steam from your distribution's native package.

### Steam Deck

Untested. Reports are welcome.

### The game crashed

Open an issue with the logs above, the `ck3agent doctor` output and what
Claude was doing. Each release attaches bridge debug symbols as
`libck3_bridge-<version>.so.debug`.

### Two games on one machine

Launch each game with `CK3_INSTANCE_ID=<name> ck3agent launch ...`, and give each its own play directory (`ck3agent setup --play-dir DIR`) whose `.mcp.json` sets the same `CK3_INSTANCE_ID` in its `env` block. Each
instance gets its own socket, logs and telemetry.

## If it fails

Open an issue at <https://github.com/skullbloc/ck3agent/issues> with the full
`ck3agent doctor` output and the bridge log.
