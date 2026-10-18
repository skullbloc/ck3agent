# Connecting Claude Desktop

Use Claude Desktop instead of Claude Code. Desktop reaches the MCP server on
the game machine through `mcp-remote`.

## Prerequisites

- Game machine: [Getting started](getting-started.md) done, game running.
- Desktop machine: Claude Desktop for macOS, Windows, or the Linux beta for
  Ubuntu and Debian. Node.js 18 or newer, which provides `npx`.
- Desktop on another machine: SSH access to the game machine.

## Steps

1. On the game machine, start the HTTP server:

   ```bash
   ck3agent serve --transport http --port 8000
   ```

   It binds to 127.0.0.1 and has no authentication. It prints the Desktop
   config snippet and the SSH command for step 2. Never expose the port on a
   network: a non-loopback `--host` is refused without `--allow-remote`.

2. If Desktop runs on another machine, open a tunnel from it:

   ```bash
   ssh -N -L 8000:127.0.0.1:8000 you@game-machine
   ```

3. Add the server to `claude_desktop_config.json` and restart Desktop. The
   file is under Settings, Developer, Edit Config.

   ```json
   {
     "mcpServers": {
       "ck3": { "command": "npx", "args": ["-y", "mcp-remote", "http://127.0.0.1:8000/mcp"] }
     }
   }
   ```

   With the Linux beta on the game machine itself, skip steps 1 and 2 and use
   `"command": "ck3agent", "args": ["serve"]`.

4. Create a Desktop Project and paste
   [`ck3agent/protocol/CLAUDE-desktop.md`](../ck3agent/protocol/CLAUDE-desktop.md)
   into its instructions. Desktop has no shell, so this variant plays one
   cycle per message and leaves the game at speed 1 between messages.

5. Bind a ruler: "Who can you control?", then "Take control of <name>".

## Check it worked

- The `ck3` tools appear in Desktop's tools menu.
- "Brief me on the realm" returns the ruler's name and the game date.

## If it fails

- No tools: run `npx -y mcp-remote http://127.0.0.1:8000/mcp` in a terminal
  on the Desktop machine. It prints the connection error.
- Connection refused: the tunnel or `ck3agent serve` is not running.
- Stale state: Desktop has no live-state hook. The Desktop protocol starts
  every message with `observe`.
