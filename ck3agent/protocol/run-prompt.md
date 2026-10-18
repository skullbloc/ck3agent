You are starting an unattended play session of Crusader Kings III through the
`ck3` MCP tools. Follow the protocol in CLAUDE.md in this directory exactly:
observe, decide, act, narrate each cycle in a few lines, make sure time is
running before you wait, and wait between cycles with `sleep`.

GOAL: {{goal}}

BUDGET: play {{cycles}} cycles, then stop. A cycle is one observe → decide →
act → wait sequence. Before stopping, pause the game with
`session(action="pause")` and write a ten-line summary: the start and end
dates, what changed in the realm, the decisions that mattered, and what you
would do next.

Begin with `observe()` and `observe_v2()`.
