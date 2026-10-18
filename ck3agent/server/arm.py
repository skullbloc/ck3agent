"""The experimental ARM, and the one question it decides: is there a board?

ck-6kvq.12 (R4), decision G5 — RULED 2026-09-10 by the progenitor as option
**A2, ABSENT**: in arm A (and C, and E0, and free play) the two board tools are
NOT REGISTERED, and the system prompt never mentions a board. Not
present-and-refused: a channel that exists and fails is a third condition an
agent spends decision budget discovering and may narrate around, which is a
worse confound than the one it avoids. The tool-surface delta is named in the
pre-registration as part of the manipulation, amending the paper's §2.3
held-constant list.

THE BRIDGE IS THE AUTHORITY, NOT THIS FILE. `bridge/src/board.cpp`'s
`board_init_from_env()` reads the same two variables and gates
`dispatch_action()` *and* `is_registered_action()` on the answer, so in arm A
`board_post` is `unknown_action` on the wire no matter what any Python process
believes. What this module buys is that the agent never SEES a tool it would
then be refused — the surface and the wire agree, and they agree because both
read the same episode config.

Environment (set by the episode harness when it launches CK3, and inherited by
the MCP server it launches alongside):

    CK3_EPISODE_ARM     e0 | a | b | c   — the board exists iff this is `b`
    CK3_BOARD_ENABLED   0 | 1            — explicit override, for host tests
"""

from __future__ import annotations

import os

#: The arm that carries the shared board. Exactly one.
BOARD_ARM = "b"


def episode_arm() -> str:
    """The declared arm, lower-cased. ``""`` when none is declared (free play).

    An unrecognised value is returned verbatim and is therefore not ``b``, so a
    typo produces an arm-A-shaped surface. That is the conservative direction:
    a missing channel is a visible null result, while a channel that should not
    be there silently contaminates the manipulation.
    """
    return (os.environ.get("CK3_EPISODE_ARM") or "").strip().lower()


def board_enabled() -> bool:
    """True iff this episode has a shared board.

    Mirrors ``bridge::board_init_from_env`` exactly, including the override's
    precedence. ``scripts/test_board_arm_gate.py`` pins the two against each
    other by re-reading the C++ so they cannot drift.
    """
    override = os.environ.get("CK3_BOARD_ENABLED")
    if override:
        return override not in ("0", "false")
    return episode_arm() == BOARD_ARM
