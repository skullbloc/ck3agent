# Troubleshooting

Start with `ck3agent doctor`; it checks in the order things can fail and
prints the first broken link. The usual suspects:

**`CK3 build NOT supported`.** Your game is not 1.18.3.1 Linux native. See
[supported versions](supported-versions.md).

**`bridge socket not listening`** after launching. Either the game has not
finished starting (give it a minute; `--wait 120` on launch), or the bridge
refused the build (doctor then shows the refusal from `/tmp/ck3_bridge.log`),
or the game was launched without `LD_PRELOAD` (from Steam without the launch
option). The bridge writes `/tmp/ck3_bridge_boot.log` the moment it is loaded;
if that file does not exist, the library was never loaded.

**Launched via Steam, nothing happens.** The Paradox launcher starts first;
the bridge interposes its `exec` of the game and re-injects itself, which is
logged in `/tmp/ck3_bridge_boot.log`. If you see the launcher but the log is
empty, the launch option is wrong — it must be exactly
`LD_PRELOAD=/full/path/libck3_bridge.so %command%`.

**`session_state` is not `active`.** The bridge is loaded but no game is
loaded yet. Load a save or start a game.

**Claude says `game_unpaused` / `game_paused`.** Some reads need the game
paused and every write needs it unpaused. The redesigned tools (`observe`,
`plan_war`, `do`, `declare_war`) manage that themselves; for a raw `query` or
`discover`, tell Claude to `session(action="pause")` (or unpause) and retry.

**`cb_query_timeout` on war checks.** Deferred validity queries drain on the
game tick; they only answer while unpaused. Use `plan_war` / `cbs`, which own
the unpause, or unpause and ask again.

**Claude Code does not list the `ck3` tools.** Check that `claude` was started
in the play directory (where `.mcp.json` is), that `ck3agent serve` works when
run by hand (it waits on stdin; Ctrl-C to quit), and that `ck3agent` is on
`PATH` for the shell Claude Code was started from (`install.sh` prints a PATH
note if `~/.local/bin` is missing).

**The hook makes every prompt slow.** It has a 2.5 s budget; if the bridge is
busy it prints a degrade line instead. Remove it with
`ck3agent setup --no-hook --force` if you prefer.

**The game stays `dormant` after `ck3agent launch --continue`.** CK3's
`--continuelastsave` lands on a confirmation screen; for an observer save it
has an **Observe** button. `launch --wait` sends the bridge's
`enter_observe_mode` once the lobby appears, which dismisses it. If your save
is a normal player save, click **Continue** in the game window; the bridge
turns `active` once the map is loaded.

**Launching from a terminal on Wayland.** If the game starts but never gets
past loading (its `debug.log` stops seconds after launch), force X11:
`env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 ck3agent launch --continue`.
Launching from Steam avoids this.

**Crash in the game.** Collect `/tmp/ck3_bridge.log`, `/tmp/ck3_bridge_boot.log`,
the game's `~/.local/share/Paradox Interactive/Crusader Kings III/logs/`, and
the `ck3-<timestamp>.log` under `~/.local/share/ck3agent/logs/`, and open an
issue with the game build and what the agent was doing. Debug symbols for the
bridge are attached to each release (`libck3_bridge-<version>.so.debug`).

**Two games, one machine.** Set `CK3_INSTANCE_ID=<name>` for both the game and
the MCP server; the socket, logs and telemetry move under
`/tmp/ck3-inst/<name>/`.
