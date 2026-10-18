# Quickstart

Ten minutes from download to Claude's first move. Linux only; see the
[supported versions](supported-versions.md) page for the exact game build.

## 1. Install

```bash
tar xzf ck3agent-2026.10.0-linux-x86_64.tar.gz
cd ck3agent-2026.10.0-linux-x86_64
sha256sum -c ../SHA256SUMS --ignore-missing   # optional but recommended
./install.sh
```

`install.sh` copies the bridge to `~/.local/share/ck3agent/lib/`, installs
the `ck3agent` command (pipx if you have it, else `pip --user`), and runs
`ck3agent setup`. Setup prints what it found:

```
ck3agent 2026.10.0 setup — supports CK3 1.18.3.1 (linux)
  [ ok ] CK3 binary — /home/you/.steam/steam/steamapps/common/Crusader Kings III/binaries/ck3
  [ ok ] CK3 build supported — 1.18.3.1, BuildID e61ff2642f07…
  [ ok ] bridge library — /home/you/.local/share/ck3agent/lib/libck3_bridge.so
  [ ok ] .mcp.json written — /home/you/ck3agent-play/.mcp.json
  [ ok ] CLAUDE.md written — the play protocol Claude Code follows in this directory
  [ ok ] live-state hook installed — each prompt gets a fresh game-state block
  [ ok ] config saved — /home/you/.config/ck3agent/config.toml
```

If the build line says **NOT supported**, stop here: this release will refuse
your game version. Check the releases page for one that matches.

## 2. Start the game with the bridge

```bash
ck3agent launch --continue --wait 120
```

That runs the `ck3` binary with `LD_PRELOAD` set to the bridge, skipping the
Paradox launcher, continuing your last save, and waits until the bridge
socket is up. You can also launch from Steam: set the game's launch options to

```
LD_PRELOAD=/home/you/.local/share/ck3agent/lib/libck3_bridge.so %command%
```

Load a save (or start a game). The bridge becomes active once a game is
loaded; `ck3agent doctor` shows `session_state=active` when it is.

## 3. Play

```bash
cd ~/ck3agent-play
claude
```

Claude Code reads `.mcp.json` (starts the `ck3` MCP server) and `CLAUDE.md`
(the play protocol). Try:

> Observe and tell me who I am and what my situation is.

then

> Play ten cycles. Keep the realm intact and tell me each decision.

Claude reads, decides, acts, narrates each cycle, and waits between
cycles. You can interrupt and redirect at any point.

## Something broke?

```bash
ck3agent doctor
```

It checks Python, the package, the config, the game binary and its build, the
bridge library, the play directory, the socket, and the bridge log, and names
the first thing that is wrong. More in [troubleshooting](troubleshooting.md).
