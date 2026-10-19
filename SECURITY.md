# Security

ck3agent loads a library into a game you own and exposes that process's
memory to a local client. This page states what it protects, what it does
not, and how to report a problem.

## Supported versions

| Version | Supported |
|---------|-----------|
| the latest release (2026.10.x) | yes |
| anything older | no: upgrade |

Each release supports one CK3 build ([docs/supported-versions.md](docs/supported-versions.md));
fixes ship as a new release, never as a patch to an old one. In scope:
`libck3_bridge.so`, the `ck3agent` CLI and MCP server, the installer scripts,
the release workflow and its artifacts.

## Threat model

- **Single user.** Any process running as you can read and steer the game.
  Do not share the login.
- **The socket.** The bridge socket is in your private runtime directory
  (`$XDG_RUNTIME_DIR/ck3agent`), mode 0600. The bridge refuses to bind unless
  that directory and every parent above it belong to you or root. It rejects
  peers whose uid is not the game's. The MCP server and the live-state hook
  check the socket's owner and the peer's uid before they send anything. If
  the instance settings are invalid, neither side picks a fallback path: the
  bridge does not bind and the hook reports the bridge offline. Both sides
  strip the same surrounding whitespace from `CK3_INSTANCE_LAYOUT` and
  `CK3_INSTANCE_ID`, so they accept and refuse the same values.
  `CK3_INSTANCE_LAYOUT=legacy` turns all of this off: a 0666 socket in `/tmp`
  that any local user can reach.
- **Bridge files.** Every file the bridge writes (logs and their archives,
  flag and lock files, telemetry, the experiment's board and session files)
  is created 0600 (0644 in the legacy layout), tightened if it already exists,
  and opened without following a symlink at its own name. They live in the private directory,
  except where you point a `CK3_*` path variable elsewhere. A boot log is
  written only with `CK3_BRIDGE_BOOT_LOG=1`; it goes to `/tmp` only with
  `CK3_INSTANCE_LAYOUT=legacy` as well.
- **HTTP has no authentication.** `ck3agent serve --transport http` binds to
  127.0.0.1 and refuses another host without `--allow-remote`. Reach it
  through an SSH tunnel, never an open port.
- **Game text is untrusted.** Names, titles and event text come from saves and
  mods and reach the model. The play protocol and the live-state hook mark
  them as data. A crafted save or mod can still try to steer the model.
- **Unattended runs.** `--dangerously-skip-permissions` lets the model run any
  tool without asking. Combined with untrusted game text, that is code
  execution as you. Use it only in a disposable VM.
- **Debug verbs.** `memory_probe` and `resolve_global` read raw memory.
  `inject_mouse_click` and `select_observe_default` send synthetic mouse
  clicks. `execute_console_command` passes commands to the game console.
  `set_cb_selector_override` hands client-supplied pointers to the engine.
  Release builds leave out their registry entries and handler code, so these
  verbs do not exist in a release `.so`. Calling one returns
  `error: unavailable_in_release`. They are compiled in only when the bridge
  is built with `CK3_BRIDGE_DEBUG_VERBS=ON`, and then anything that can reach
  the socket can use them. Release replies also carry no process memory
  addresses; debug builds add some to diagnostic fields.
- **Network.** The bridge never opens a network connection. The Python side
  talks to the local socket and, through your MCP host, to Anthropic.
- **Single-player only.** Do not use ck3agent in multiplayer, Ironman or
  achievement runs.

**Out of scope:** attacks that already need your user account, the game's
own code, Steam, and your MCP host.

## Reporting a vulnerability

Open a [private security advisory](https://github.com/skullbloc/ck3agent/security/advisories/new).
If that form is unavailable, send a private message to the maintainer
([@stebbins](https://github.com/stebbins)) on GitHub with "ck3agent security"
in the first line. Do not file public issues for security problems. We
acknowledge within 7 days and aim to fix within 90 days.
