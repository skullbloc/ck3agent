"""Which succession laws a ruler can change to now, or after one more step, by the game's own conditions.

 (a Carinthia test run, 1066.09.29: `do action_class=laws` listed 28 succession laws, the clan, mercenary, theocratic
and administrative ones among them, unchecked; `change_law partition_succession_law` was refused on the toolkit's
500-prestige line before the game's reasons, crown_authority_1 and the culture's innovation_hereditary_rule, were said).

The conditions are the game's script (common/laws/00_succession_laws.txt `potential` / `can_have` / `can_pass`;
common/scripted_triggers/00_law_triggers.txt can_have_*_succession_law_trigger; 00_legal_triggers.txt
can_change_succession_law_trigger and can_change_partition_succession_law_trigger; 00_realm_laws.txt, whose
crown_authority group is cumulative: crown_authority_1 carries can_change_partition_succession_laws,
crown_authority_2 can_change_succession_laws; tribal_authority_1 can_change_succession_laws), evaluated on reads:
the realm's laws, the culture's innovations and the ruler's government. The bridge's own trigger evaluation of a law
(CK3_LAW_TRIGGER_HARVEST, action_handlers.cpp diagnose_law_refusal) is off by default: it hung the game thread.
Modelled: feudal and tribal rulers (every seat so far). Any other government, or a government not read: every row is
kept and says it was not checked.
"""
from __future__ import annotations

from typing import Any, Iterable, Optional

SOURCE = ("the game's law conditions (00_succession_laws.txt potential/can_have/can_pass, 00_law_triggers.txt, "
          "00_legal_triggers.txt, 00_realm_laws.txt) on your realm's laws, your culture's innovations and your "
          "government")

#: the succession laws a feudal or tribal ruler can hold, with what each needs (government, innovation, the realm
#: law flag its can_pass asks for: "partition" = can_change_partition_succession_laws or can_change_succession_laws,
#: "succession" = can_change_succession_laws)
SUCC_LAWS: dict = {
    "confederate_partition_succession_law": {"gov": {"feudal", "tribal"}, "innovation": None, "pass": "partition"},
    "partition_succession_law": {"gov": {"feudal"}, "innovation": "innovation_hereditary_rule", "pass": "partition"},
    "high_partition_succession_law": {"gov": {"feudal"}, "innovation": "innovation_heraldry", "pass": "partition",
                                      "not_read": "a culture with can_enact_high_partition_succession_law needs no "
                                                  "innovation"},
    "single_heir_succession_law": {"gov": {"feudal"}, "innovation": "innovation_primogeniture", "pass": "succession"},
    "single_heir_succession_law_youngest": {"gov": {"feudal"}, "innovation": "innovation_primogeniture",
                                            "pass": "succession"},
    "single_heir_dynasty_house": {"gov": {"feudal"}, "innovation": "innovation_heraldry", "pass": "succession",
                                  "not_read": "Czech and Slovien cultures need innovation_table_of_princes instead"},
}

#: the government each key of the government read is (00_government_types.txt government_is_feudal / _tribal)
GOVERNMENTS = {"feudal_government": "feudal", "tribal_government": "tribal"}

PASS_LEVEL = {("feudal", "partition"): ("crown_authority", 1), ("feudal", "succession"): ("crown_authority", 2),
              ("tribal", "partition"): ("tribal_authority", 1), ("tribal", "succession"): ("tribal_authority", 1)}

NOT_CHECKED = ("a powerful vassal who opposes the change (no_powerful_vassal_with_negative_opinion), and the "
               "500 prestige price (change_succession_law_prestige_cost, before faith doctrines)")


def _level(laws: Iterable[str], group: str) -> Optional[int]:
    for n in laws or []:
        s = str(n)
        if s.startswith(group + "_") and s[len(group) + 1:].isdigit():
            return int(s[len(group) + 1:])
    return None


def government_of(key: Any, laws: Iterable[str] = ()) -> Optional[str]:
    """'feudal' / 'tribal' from the government read; when it did not read, from the realm's laws: a crown_authority
    law with no clan succession law is feudal (clan realms keep crown authority too), a tribal_authority law
    tribal; a realm with laws only feudal and tribal realms share and no tribal_authority law, feudal; None for
    another government's law or no laws read (not modelled)."""
    if key:
        return GOVERNMENTS.get(str(key))
    names = [str(n) for n in laws or []]
    if any(n.startswith("tribal_authority_") for n in names):
        return "tribal"
    if not names or any(n.startswith(OTHER_GOVERNMENT_LAWS) for n in names):
        return None
    return "feudal"


#: realm laws only another government than feudal or tribal holds (00_realm_laws.txt, 00_succession_laws.txt)
OTHER_GOVERNMENT_LAWS = ("clan_", "imperial_bureaucracy_", "celestial_", "meritocratic_", "japanese_", "mandala_",
                         "nomadic_authority_", "camp_purpose_", "herder_", "city_succession", "holy_order_",
                         "mercenary_company_", "bishop_theocratic_")


def reach(law: str, *, government: Optional[str], laws: Iterable[str], innovations: Optional[set]) -> dict:
    """One succession law for this ruler: {law, possible, missing: [...], steps} or {law, other_government} or
    {law, not_checked}. `government` is 'feudal' / 'tribal' (government_of), None when not read or not modelled;
    `innovations` the culture's known innovations, None when not read."""
    if government is None:
        return {"law": law, "not_checked": "your government was not read or is not modelled here"}
    row = SUCC_LAWS.get(law)
    if row is None or government not in row["gov"]:
        return {"law": law, "other_government": True}
    missing, steps = [], 0
    inn = row["innovation"]
    if inn:
        if innovations is None:
            # not read is not missing: the row is kept (a reading that hides it would be the greener one)
            missing.append(f"your culture's {inn} (unread)")
        elif inn not in innovations:
            missing.append(f"your culture's {inn}")
            steps += 1
    group, need = PASS_LEVEL[(government, row["pass"])]
    have = _level(laws, group)
    if have is None:
        missing.append(f"{group}_{need} (your {group} law was not read)")
    elif have < need:
        missing.append(f"{group}_{need} (you have {group}_{have}; do change_law law={group}_{have + 1}"
                       + (" and on" if need - have > 1 else "") + ")")
        steps += need - have
    unread = any("(unread)" in m or "was not read" in m for m in missing)
    out = {"law": law, "possible": (None if unread and steps == 0 else not missing), "steps": steps}
    if missing:
        out["missing"] = missing
    if row.get("not_read"):
        out["not_read"] = row["not_read"]
    return out


def listing(options: Iterable[str], *, current: Optional[str], government: Optional[str], laws: Iterable[str],
            innovations: Optional[set]) -> tuple[list, dict]:
    """(rows kept, left-out summary) for the succession_order_laws group: a law the game would allow now or one
    step away is kept with what is missing; another government's law, or one two or more steps away, is left out
    and counted. Not modelled or not read: every row kept as not checked."""
    kept, other, far = [], 0, []
    for law in options:
        if law == current:
            continue
        r = reach(law, government=government, laws=laws, innovations=innovations)
        if r.get("not_checked") or r.get("possible") or r.get("steps", 99) <= 1:
            kept.append(r)
        elif r.get("other_government"):
            other += 1
        else:
            far.append({"law": law, "missing": r.get("missing")})
    left = {}
    if other:
        left["other_governments"] = other
    if far:
        left["two_or_more_steps"] = far
    return kept, left


def refusal_reasons(law: str, *, government: Optional[str], laws: Iterable[str],
                    innovations: Optional[set]) -> Optional[list]:
    """The game's conditions this code still fails, in its own terms, or None when none is known to fail."""
    r = reach(law, government=government, laws=laws, innovations=innovations)
    if r.get("other_government"):
        return [f"{law} is another government's law: the game never offers it to your realm"]
    return r.get("missing") or None
