# Getting started

From download to Claude's first move, about 15 minutes.

## Prerequisites

- Linux x86_64, glibc 2.34 or newer: Ubuntu 22.04, Debian 12, Fedora 35 or later.
- Crusader Kings III **1.18.3.1**, the native Linux build, from the native
  Steam package. Not Flatpak, not Snap, not the Windows build under Proton.
- Python 3.11 or newer. `pipx` is optional.
- [Claude Code](https://docs.claude.com/en/docs/claude-code) on the same machine, signed in.
- A single-player save that is not Ironman, made before step 3. Recommended: an
  observer save. Start a new game, choose **Observe** on the character-selection
  screen, and save.

## 1. Download and verify

From the [releases page](https://github.com/skullbloc/ck3agent/releases),
download `ck3agent-2026.10.0-linux-x86_64.tar.gz` and `SHA256SUMS` into one
directory. Verify there, before unpacking:

```bash
cd ~/Downloads
sha256sum -c SHA256SUMS --ignore-missing
```

Expected: `ck3agent-2026.10.0-linux-x86_64.tar.gz: OK`. With the GitHub CLI
you can also run
`gh attestation verify ck3agent-2026.10.0-linux-x86_64.tar.gz --repo skullbloc/ck3agent`.

## 2. Install

```bash
tar xzf ck3agent-2026.10.0-linux-x86_64.tar.gz
cd ck3agent-2026.10.0-linux-x86_64
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
ck3agent launch --continue
```

This starts CK3 with the bridge, loads your last save, and waits up to 120
seconds for the game. On an observer save it enters Observe for you. On a
player save, click **Continue** in the game window. To start CK3 from Steam
instead, use the launch option `setup` printed.

## 4. Choose the ruler Claude plays

Claude plays an AI ruler, never yours. The bridge refuses to bind your
character. If Claude's ruler dies and you are its heir, the bridge refuses that
handoff too (`heir_is_player`) and leaves Claude with no ruler. Nothing guards
the reverse: if your character dies and you inherit Claude's ruler, Claude's
seat now holds your character. `whoami` then returns `cannot_act_as_player`,
and every write is refused. Pick one:

- **Observer save.** Recommended. You watch; Claude rules.
- **Your own save.** Claude takes an AI ruler, such as a rival or a neighbour,
  that you will not inherit and that will not inherit from you.

In Claude Code, ask "Who can you control? List nearby rulers with their ids."
Then say "Take control of <name>". Claude calls
`session(action="control", character_id=...)` and confirms with `whoami`.
Until then, with no `character_id` given:

- `whoami`, `observe_v2`, `inspect`, `plan_war`, `declare_war`, `cbs`,
  `succession_plan`, `set_phase` and `do` return `ok: false` with
  `blocked.gate: "no_ruler_bound"`.
- `observe`, `query`, `discover`, `steer` and `intent` are not checked: they
  return `ok: true` with the bridge's empty or declined reply, for example
  `observe`'s `data.character.success: false`.
- `discover(what="evaluate_trigger")` evaluates against the engine's player
  character, which is yours on a player save.

## 5. Play

```bash
cd ~/ck3agent-play
claude
```

Try "Brief me on the realm", then "Play ten cycles. Keep the succession intact
and explain each decision." Press Esc to interrupt. Time, cost and stopping:
[Claude Code](claude-code.md).

## Check it worked

- `ck3agent doctor` shows no `FAIL` lines, and its handshake line reads `session_state=active`.
- Claude's first reply names its ruler, the date and the realm.
- After a cycle, the game date has advanced.

## If it fails

Run `ck3agent doctor`. Its last line names the next step. Then see
[troubleshooting](troubleshooting.md).
