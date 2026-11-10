# Getting started

From download to Claude's first move, about 15 minutes.

## Prerequisites

- Linux x86_64, glibc 2.34 or newer: Ubuntu 22.04 or later, Debian 12 or later, Fedora (tested on 44), Arch.
- Crusader Kings III **1.18.3.1**, the native Linux build, from the native
  Steam package. Not Flatpak, not Snap, not the Windows build under Proton.
- Python 3.11 or newer. `pipx` is optional.
- [Claude Code](https://docs.claude.com/en/docs/claude-code) on the same machine, signed in.
- A single-player save that is not Ironman. Two kinds work: an **observer
  save** (start a new game, choose **Observe** on the character-selection
  screen, save) if you want to watch Claude rule, or **your own campaign** if
  you want to play against Claude. `ck3agent launch` copies the save into
  its own game directory and the game saves and autosaves there, so your
  campaign file is not changed. A save Claude played is in
  `~/.local/state/ck3agent/game/save games/`.

## 1. Download and verify

From the [releases page](https://github.com/skullbloc/ck3agent/releases),
download `ck3agent-2026.12.8-linux-x86_64.tar.gz` and `SHA256SUMS` into one
directory. Verify there, before unpacking:

```bash
cd ~/Downloads
sha256sum -c SHA256SUMS --ignore-missing
```

Expected: `ck3agent-2026.12.8-linux-x86_64.tar.gz: OK`. With the GitHub CLI
you can also run
`gh attestation verify ck3agent-2026.12.8-linux-x86_64.tar.gz --repo skullbloc/ck3agent`.

## 2. Install

```bash
tar xzf ck3agent-2026.12.8-linux-x86_64.tar.gz
cd ck3agent-2026.12.8-linux-x86_64
./install.sh
```

`install.sh` installs the `ck3agent` command with pipx, or into a private venv
when pipx is absent or fails (offline, for one). It copies the bridge library, then runs `ck3agent setup`.
Setup finds the game, checks its build, and creates the play directory
`~/ck3agent-play`. If the build check fails, stop and read
[Your game updated](supported-versions.md). Paths and a manual install:
[files and paths](reference/files-and-paths.md).

## 3. Launch the game

```bash
ck3agent launch --save "$HOME/.local/share/Paradox Interactive/Crusader Kings III/save games/<name>.ck3"   # a specific save
ck3agent launch --continue                                                                                # the save CK3 played last
```

The game runs with its own user-data directory, `~/.local/state/ck3agent/game/`
(`$XDG_STATE_HOME/ck3agent/game/`): `launch` copies the save into its
`save games/`, and the game's saves, autosaves, `continue_game.json` and logs
stay there. Your own CK3 directory is only read. The first `--continue` copies
the save your CK3 played last; after that `--continue` loads the save the
launched game played last.

Both start CK3 with the bridge and wait up to 120 seconds for the game. The
first time, they wait up to 600 seconds: the game directory has no shader
cache yet, and CK3 takes several minutes to build one (about 600 MB).
Expect a few game days to pass before the pause lands: the bridge has to let
the clock run to finish loading. On an observer save it enters Observe for
you. On a player save it usually continues on its own; if the game stays on
the menu, click **Continue** in the game window. Once the game is active,
`launch` pauses it; Claude unpauses when it plays. `--no-pause` leaves the
clock running. To start CK3 from Steam instead, use the launch option `setup`
printed.

## 4. Choose the ruler Claude plays

Claude plays an AI ruler, never yours. The bridge refuses to bind your
character. If Claude's ruler dies and you are its heir, the bridge refuses that
handoff too (`heir_is_player`) and leaves Claude with no ruler. Nothing guards
the reverse: if your character dies and you inherit Claude's ruler, the ruler Claude is bound to is now your character. `whoami` then returns `cannot_act_as_player`,
and every write is refused. Pick one:

- **Observer save.** Recommended. You watch; Claude rules.
- **Your own save.** Claude takes an AI ruler, such as a rival or a neighbour,
  that you will not inherit and that will not inherit from you.

In Claude Code, ask "Who can you control? List nearby rulers with their ids."
Then say "Take control of <name>". Claude calls
`session(action="control", character_id=...)` and confirms with `whoami`.
To give Claude more than one ruler, say "Also take <name>": Claude calls
`session(action="add", character_id=...)` and keeps the first.
Until a ruler is bound, most tools refuse with `no_ruler_bound`, and the few
that answer anyway return empty data. If Claude narrates a realm before you
named a ruler, it is reading nothing: ask it to run `whoami`. The per-tool
detail is under [no_ruler_bound](troubleshooting.md#claude-says-it-controls-no-ruler-or-no_ruler_bound) in
troubleshooting.

## 5. Play

```bash
cd ~/ck3agent-play
claude
```

The first time, Claude Code asks whether you trust this directory. Accept:
until you do, the tool allow-list that `setup` wrote is ignored and every
`ck3` call prompts. (`ck3agent run` passes its allow-list on the command line
and does not need the dialog.)

Try "Brief me on the realm", then "Play ten cycles. Keep the succession intact
and explain each decision." Press Esc to interrupt. Time, cost and stopping:
[Claude Code](claude-code.md).

## Check it worked

- `ck3agent doctor` shows no `FAIL` lines, its handshake line reads `session_state=active`, and its last line says `Ready`. While the game is still loading or on the menu it says `Game not active yet` instead.
- Claude's first reply names its ruler, the date and the realm.
- After a cycle, the game date has advanced.

## If it fails

Run `ck3agent doctor`. Its last line names the next step. Then see
[troubleshooting](troubleshooting.md).
