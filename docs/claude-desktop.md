# Connecting Claude Desktop

Claude Desktop has no Linux build, and the bridge only runs on Linux, so
Desktop connects from another machine (Mac or Windows) to the MCP server on
the game machine.

## 1. Serve over HTTP on the game machine

```bash
ck3agent serve --transport http --port 8000       # binds 127.0.0.1 only
```

The endpoint is `http://127.0.0.1:8000/mcp` (streamable HTTP).

## 2. Reach it from your desktop

The server is unauthenticated, so do not open the port. Tunnel it:

```bash
# on the desktop machine
ssh -N -L 8000:127.0.0.1:8000 you@game-machine
```

Now `http://127.0.0.1:8000/mcp` on the desktop reaches the game machine.

## 3. Add it to Claude Desktop

Either **Settings → Connectors → Add custom connector** with the URL above
(works where your Desktop build accepts plain-HTTP localhost connectors), or
the stdio shim in `claude_desktop_config.json`, which always works:

```json
{
  "mcpServers": {
    "ck3": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "http://127.0.0.1:8000/mcp"]
    }
  }
}
```

Restart Desktop; the `ck3` tools appear in the tools menu.

## 4. Give it the protocol

Desktop has no `CLAUDE.md`. Paste the contents of the play directory's
`CLAUDE.md` into a Project's instructions (or into the first message). The
pause discipline matters most: reads need the game paused, writes need it
unpaused.

## Notes

* This path was designed on Linux and tested against the HTTP transport with
  a generic MCP client; it has not been exercised from a Desktop build by the
  maintainers in this release. Issues welcome.
* The live-state hook is a Claude Code feature; in Desktop, start each turn
  with `observe`.
