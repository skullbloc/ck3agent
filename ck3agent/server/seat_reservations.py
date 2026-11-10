"""seat_reservations.py — THE reader of the cell's seat reservations (toolkit 2026.12.7).

WHY (v3pilot5, 2026-11-10). In weak_orders and weak_intent the king seat called
`session(action="add")` on all five dukes' declared characters within its first
minute and held them to the horizon; the duke seats were refused
`owned_by_other` (weak_intent D4: 43 times, continuations exhausted). Nothing
stopped one seat claiming another declared seat's ruler before that seat bound.
Owner ruling 2026-11-10, "Toolkit 2026.12.7 (Recommended)": the server refuses
the claim (`reserved_for_another_seat`) and the cell passes each seat its
reserved ids.

THE DECLARATION. The cell (`scripts/dry_cell.py build_plan`, which the cell
runner `ck3bench/runner/cell.py` uses for every seat's environment) sets
`CK3_RESERVED_FOR_OTHER_SEATS` on each seat to the declared characters of the
OTHER seats, comma-separated ("12137,14445"). It never names the seat's own
ruler. This module is the one place the value is read and parsed; the cell
imports the spelling from here.

WHAT IS RESERVED is the declared ids and nothing else. An heir who becomes a
seat's ruler after a succession is reserved for nobody unless he is himself
another seat's declared character, so the seat's own rebind is never blocked
by this, and neither is the owning seat's claim of its own ruler.

ABSENT OR EMPTY MEANS NO RESERVATIONS: every non-cell user (interactive play,
`ck3agent run` outside a cell, the driver's arms) behaves byte for byte as
before. A value that is set and cannot be read (a non-integer, an id <= 0, no
`CK3_SEAT_DECLARED_CHARACTER_ID` beside it, or one that reserves that id) is NOT
read as "no reservations": every claim is refused `seat_reservations_unreadable`
until it is fixed, because a reservation the server cannot establish is not one
it may default away.

THE SELF CHECK IS AGAINST THE SEAT'S ORIGINAL DECLARED ID (witness r1 F1 on
1eaab8ac), `CK3_SEAT_DECLARED_CHARACTER_ID`, which the cell plan sets once and
nothing rewrites. NOT `CK3_AGENT_CHARACTER_ID`: `ck3agent run` rewrites that to
the rulers the seat CARRIES into a continuation (cli.py, the carry after a
hand-over), and after a succession a carried heir may be another seat's
declared ruler (the lineage rebind, the bridge's heir handoff). Comparing
against it read that as a self-reservation and refused every later claim.
Coming to hold another seat's declared ruler by those routes is not refused
here (they do not pass through session add/control); it is WITNESSED: the
lineage step writes a `holds_reserved` note (seat_lineage.KIND_HOLDS_RESERVED).
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Mapping, Optional

#: THE spelling. scripts/dry_cell.py sets it; scripts/test_dry_cell.py checks the mirror.
RESERVED_ENV = "CK3_RESERVED_FOR_OTHER_SEATS"
#: The seat's ORIGINAL declared ruler, as the cell plan records it (the plan's
#: seat character_id). Set beside RESERVED_ENV; never rewritten by a continuation.
DECLARED_ENV = "CK3_SEAT_DECLARED_CHARACTER_ID"
#: The rulers the seat binds at connect (mcp_server.AGENT_CHARACTER_ENV, the same
#: spelling). NOT the self check's input: a continuation rewrites it.
AGENT_CHARACTER_ENV = "CK3_AGENT_CHARACTER_ID"

#: A claim of a character the cell declared for another seat.
R_RESERVED = "reserved_for_another_seat"
#: The declaration is set and cannot be read: nothing is claimed while it stands.
R_UNREADABLE = "seat_reservations_unreadable"
#: The refusals this module decides, in the order they are checked.
REASONS = (R_UNREADABLE, R_RESERVED)

#: The session actions that claim a character and so are checked: `add` (the
#: v3pilot5 route) and `control`, which SWITCHES the seat onto the character and
#: takes it from its seat just the same. `release` claims nothing.
CLAIM_ACTIONS = ("add", "control")


@dataclass(frozen=True)
class Reservations:
    """`ids` the reserved characters (empty: none); `error` why the declaration
    could not be read (None when it could, or when it is absent)."""
    ids: frozenset = frozenset()
    error: Optional[str] = None
    declared: bool = False


def parse(raw: Optional[str], declared: Optional[str] = None) -> Reservations:
    """The ONE parser. `raw` the RESERVED_ENV value, `declared` the seat's
    original declared id (DECLARED_ENV), one positive integer."""
    if raw is None or not str(raw).strip():
        return Reservations()
    ids = set()
    for part in str(raw).split(","):
        p = part.strip()
        try:
            v = int(p)
        except ValueError:
            return Reservations(error=f"{RESERVED_ENV}={raw!r}: {p!r} is not an integer", declared=True)
        if v <= 0:
            return Reservations(error=f"{RESERVED_ENV}={raw!r}: {v} is not a character id", declared=True)
        ids.add(v)
    try:
        own = int(str(declared).strip()) if declared is not None else 0
    except ValueError:
        own = 0
    if own <= 0:
        return Reservations(error=(f"{RESERVED_ENV} is set and {DECLARED_ENV}={declared!r} is not "
                                   "the seat's declared character id"), declared=True)
    if own in ids:
        return Reservations(error=(f"{RESERVED_ENV}={raw!r} reserves {own}, which this seat itself "
                                   f"declares ({DECLARED_ENV}): a seat is never reserved against "
                                   "itself"), declared=True)
    return Reservations(ids=frozenset(ids), declared=True)


def read(environ: Optional[Mapping[str, str]] = None) -> Reservations:
    """The reservations as this process's environment declares them."""
    env = os.environ if environ is None else environ
    return parse(env.get(RESERVED_ENV), env.get(DECLARED_ENV))


def refusal(character_id: int, environ: Optional[Mapping[str, str]] = None) -> Optional[str]:
    """The refusal for claiming `character_id`, or None when the claim may go to the bridge."""
    res = read(environ)
    if res.error is not None:
        return R_UNREADABLE
    try:
        cid = int(character_id)
    except (TypeError, ValueError):
        return None
    return R_RESERVED if cid in res.ids else None


def render(ids) -> str:
    """The env value for `ids` (the cell's side of the one spelling): sorted, comma-separated."""
    return ",".join(str(int(i)) for i in sorted({int(i) for i in ids}))
