"""Whose dynasty inherits: the primary heir's dynasty against the ruler's, and the marriages whose children
leave the ruler's dynasty. Pure functions over reads the server already makes; nothing here talks
to the bridge.

Why (a seated 1066 Lorraine game). Gottfried 34471 (dynasty 2032) was married
matrilineally to Matilda 36010 from before the 1066 start: his children were of her dynasty 2281. For five
turns the seat read an heir list naming his sister (the title's stale cache, bridge/include/heir_line.h) and
did nothing; at his death his son Goffredo 69510 (dynasty 2281) took every title and the dynasty lost them.
No read said "your heir is of another dynasty", and the turn-one succession read said "intact" with the
marriage already in place.

The rule (game rules, not a model): a child is of the father's dynasty unless the marriage is matrilineal,
when he is of the mother's (`matrilineal` on query_character's spouses rows, the engine's own reading,
query_handlers.cpp marriage_is_matrilineal). So a marriage's children leave a dynasty D when the father's
dynasty (patrilineal) or the mother's (matrilineal) is not D.

What a seat can do about an heir of another dynasty (each lever's own text says what the game requires):
disinherit needs the heir in YOUR dynasty (disinherit_interaction is_shown: scope:actor.dynasty =
scope:recipient.dynasty), so it cannot touch him; a son of a later marriage is younger and does not displace
him; vows (devoted blocks inheritance) need him 10 or older and at your court; a divorce stops further
children of that marriage, not the ones born.
"""
from __future__ import annotations

from typing import Any, Optional

#: The levers named beside the warning, each with what the game requires (00_dynast_interactions.txt,
#: 00_religious_interactions.txt, 00_marriage_interactions.txt). Proven live: disinherit, the
#: marriage send (a marriage test run), take_vows sent and answered; divorce not yet.
LEVERS_OTHER_DYNASTY = (
    "take_vows_interaction on him (target him; 10 or older, your courtier; devoted blocks inheritance; "
    "350 piety for your child; he may refuse)",
    "disinherit cannot: it needs him in your dynasty",
    "a son of a later marriage is younger: he does not displace him",
    "live past him, or have him outlived: the line is read at your death",
)


def _i(v: Any) -> Optional[int]:
    if isinstance(v, bool):
        return None
    if isinstance(v, int):
        return v
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def label(name: Any, dyn_id: Any) -> str:
    """'di Lucca (2281)', or '2281' when the name was not read."""
    d = _i(dyn_id)
    if isinstance(name, str) and name:
        return f"{name} ({d})" if d is not None else name
    return str(d) if d is not None else "unread"


def other_dynasty(seat_dyn: Any, heir_dyn: Any) -> Optional[bool]:
    """True / False when both dynasties were read, None when either was not (never a guess)."""
    a, b = _i(seat_dyn), _i(heir_dyn)
    if not a or not b:
        return None
    return a != b


def heir_warning(*, heir_id: Any, heir_name: Any, heir_dyn: Any, heir_dyn_name: Any, seat_dyn: Any,
                 seat_dyn_name: Any, what: str = "your titles", yours: bool = True) -> Optional[str]:
    """The top-of-reply line when the primary heir is of another dynasty, else None. True for any user:
    states the fact and its consequence in the game; a task's own text carries any scoring. `yours`
    False: the title is another ruler's (inspect of a title the reader does not hold)."""
    if other_dynasty(seat_dyn, heir_dyn) is not True:
        return None
    who = f"{heir_name} ({_i(heir_id)})" if heir_name else str(_i(heir_id))
    if not yours:
        return (f"the primary heir {who} is of dynasty {label(heir_dyn_name, heir_dyn)}, not the holder's "
                f"{label(seat_dyn_name, seat_dyn)}: if he inherits, the holder's dynasty no longer holds {what}")
    return (f"your primary heir {who} is of dynasty {label(heir_dyn_name, heir_dyn)}, not yours, "
            f"{label(seat_dyn_name, seat_dyn)}: if he inherits, your dynasty no longer holds {what}")


def children_dynasty(person: dict, spouse: dict, matrilineal: Optional[bool]) -> Optional[int]:
    """The dynasty of the children of `person` and `spouse` (query_character rows): the father's, or the
    mother's when the marriage is matrilineal. None when a sex, a dynasty or the marriage type is unread."""
    if matrilineal is None or not isinstance(person.get("is_female"), bool) or \
            not isinstance(spouse.get("is_female"), bool):
        return None
    father, mother = (spouse, person) if person["is_female"] else (person, spouse)
    if father is mother or father.get("is_female") is True or mother.get("is_female") is False:
        return None   # a same-sex couple: no children
    return _i((mother if matrilineal else father).get("dynasty_id"))


def marriage_rows(person: dict, spouses: dict, seat_dyn: Any, who: str) -> list:
    """Each living marriage of `person` whose children would leave `seat_dyn`: [{who, id, spouse_id, spouse,
    matrilineal, children_dynasty_id, line}]. `spouses`: spouse id -> query_character row."""
    out = []
    for s in person.get("spouses") or []:
        if not isinstance(s, dict) or s.get("alive") is False:
            continue
        sid = _i(s.get("id"))
        row = spouses.get(sid) if sid else None
        if not isinstance(row, dict):
            continue
        mat = s.get("matrilineal") if isinstance(s.get("matrilineal"), bool) else None
        kd = children_dynasty(person, row, mat)
        if other_dynasty(seat_dyn, kd) is not True:
            continue
        kname = row.get("dynasty_name") if _i(row.get("dynasty_id")) == kd else person.get("dynasty_name")
        sname = f"{row.get('name')} ({sid})" if row.get("name") else str(sid)
        kind = "matrilineal" if mat else "patrilineal"
        owner = "your" if who == "you" else f"your heir {person.get('name') or _i(person.get('id'))}'s"
        out.append({"who": who, "id": _i(person.get("id")), "spouse_id": sid, "matrilineal": mat,
                    "children_dynasty_id": kd,
                    "line": (f"{owner} marriage to {sname} is {kind}: the children are of dynasty "
                             f"{label(kname, kd)}, not yours")})
    return out
