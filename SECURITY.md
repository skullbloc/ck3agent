# Security

ck3agent loads a shared library into a game process you own and exposes that
process's memory over a local Unix socket. Please keep these in mind:

* **The socket is local and unauthenticated.** Anything that can connect to
  `/tmp/ck3-agent.sock` can read game state and steer the game. Do not expose
  it beyond your machine. `ck3agent serve --transport http` binds to
  127.0.0.1 by default; put an SSH tunnel in front of it, never a public port.
* **The bridge accepts a debug-console passthrough action** (`execute_console_command`
  on the socket, with a deny list for state-granting cheats). It is not
  exposed as an MCP tool in this release, but anything on the socket can send
  it. It is not a sandbox.
* **Scripted runs** (`ck3agent run`) hand the model an allow-list of the MCP
  tools and `sleep`; `--yolo` disables permission prompts entirely. Use it only
  in a directory you are happy for an unattended agent to work in.
* The bridge **never contacts the network** and the Python side only talks to
  the local socket and (through your MCP host) to Anthropic.

To report a vulnerability, open a
[private security advisory](https://github.com/skullbloc/ck3agent/security/advisories/new)
on GitHub. Please do not file public issues for security problems. You can
expect an acknowledgement within a week.
