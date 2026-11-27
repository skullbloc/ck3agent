"""Your ruler's diarch (a regent), what it blocks and how it ends.

The game made Berthold's son Gebhard his diarch; vows and disinherit on Gebhard were refused
with the game's words "You cannot take overt hostile actions against your Diarch", no news row said so, and the seat's
`end_diarchy_interaction` went through the AI path (`ai_path_no_target`).

The game's rules (CK3 1.18 files):
* `end_diarchy_interaction`, `start_diarchy_interaction` and `designate_diarch_interaction` are debug interactions
  (common/character_interactions/00_test_interactions.txt: `is_shown = { debug_only = yes }`): the game shows them to
  nobody in a normal game.
* A temporary regency starts when the ruler travels outside his realm (events/diarchy_events/diarchy_events.txt
  diarchy.0002, travel_would_cause_regency_trigger) or is imprisoned abroad (diarchy.0001); a regency when he is a
  child, incapable or isolating (diarchy.0011, regency_for_personal_reasons_trigger).
* Its end is the liege's interaction named by the diarchy type (common/diarchies/diarchy_types/00_regencies.txt
  `end_interaction`): liege_dismiss_temporary_regency_interaction (auto-accepted; the game's AI for the ruler sends
  it itself, ai_will_do 1000) or liege_dismiss_entrenched_regency_interaction (the regent answers, cooldown 1 year);
  both only while can_leave_diarchy_trigger holds: not travelling, not incapable, not a child, not isolating
  (00_diarchy_scripted_triggers.txt).
* While he is the diarch, the interactions guarded by cannot_take_overt_hostile_actions_against_diarch.tt are refused
  on him: disinherit, disinherit_children, denounce, take_vows (00_religious_triggers.txt), accuse_of_decadence,
  turn_subject_into_mandala.

The diarch is read by the game's own checks (query_interaction_check, nothing sent): the two dismiss interactions,
whose is_shown holds only for `scope:actor.diarch` under a regency that can be dismissed, and disinherit, whose
refusal carries the block's words for him.
"""
from __future__ import annotations

from typing import Any, Optional

#: the game's debug-only diarchy interactions (00_test_interactions.txt is_shown debug_only = yes) -> the real one
DEBUG_ONLY = {
    "end_diarchy_interaction": "liege_dismiss_temporary_regency_interaction",
    "start_diarchy_interaction": None,
    "designate_diarch_interaction": None,
}
DISMISS_TEMPORARY = "liege_dismiss_temporary_regency_interaction"
DISMISS_ENTRENCHED = "liege_dismiss_entrenched_regency_interaction"
#: the dismiss interaction -> the regency kind its is_shown names (00_regencies.txt end_interaction)
DISMISS = {DISMISS_TEMPORARY: "temporary_regency", DISMISS_ENTRENCHED: "entrenched_regency"}
#: the game's words for the block (localization cannot_take_overt_hostile_actions_against_diarch.tt), lower case
BLOCK_WORDS = "hostile actions against your diarch"
BLOCKS = ("disinherit, take_vows, denounce and the other overt hostile interactions on him (the game: \"You cannot "
          "take overt hostile actions against your Diarch\")")
STARTS = ("the game starts a temporary regency when your ruler travels outside his realm or is imprisoned abroad, and "
          "a regency when he is a child, incapable or isolating; the regent is the game's pick")
ENDS = ("it ends by {name} from your ruler to him once your ruler is home, capable and adult (the game's AI for your "
        "ruler sends the temporary one itself, within months)")
SOURCE = ("common/diarchies/diarchy_types/00_regencies.txt (end_interaction), events/diarchy_events/diarchy_events.txt "
          "0001/0002/0011, 00_diarchy_scripted_triggers.txt can_leave_diarchy_trigger")
DEBUG_WORDS = ("{name} is a debug interaction: the game shows it only in debug mode "
               "(00_test_interactions.txt is_shown debug_only = yes), so it can never be sent in this game; nothing was "
               "sent")


def end_call(diarch_id: Any, kind: Optional[str] = None) -> dict:
    """The call that ends the diarchy of `kind` (the temporary one when the kind was not read)."""
    name = DISMISS_ENTRENCHED if kind == "entrenched_regency" else DISMISS_TEMPORARY
    return {"tool": "do", "args": {"intent": "interact", "args": {"interaction": name, "target_id": diarch_id}}}


def debug_refusal(name: str) -> Optional[dict]:
    """The refusal of a debug-only diarchy interaction (None for any other): the game's reason and the real one."""
    if name not in DEBUG_ONLY:
        return None
    real = DEBUG_ONLY[name]
    out = {"summary": DEBUG_WORDS.format(name=name), "game_rule": "is_shown: debug_only = yes"}
    if real:
        out["instead"] = {"interaction": real, "also": DISMISS_ENTRENCHED,
                          "when": ENDS.format(name=f"{real} (or {DISMISS_ENTRENCHED} for an entrenched one)"),
                          "call": end_call("<your diarch>")}
    return out


def is_block_words(words: Any) -> bool:
    """The game's diarch block in a refusal's words."""
    return isinstance(words, str) and BLOCK_WORDS in words.lower()


def lever_note(diarch_id: Any) -> dict:
    """What a lever row refused by the diarch block says beside it."""
    return {"reason": f"{diarch_id} is your ruler's diarch (regent): the game refuses {BLOCKS}",
            "how_it_came": STARTS,
            "how_it_ends": ENDS.format(name=f"{DISMISS_TEMPORARY} (or {DISMISS_ENTRENCHED})"),
            "end_call": end_call(diarch_id)}


def candidates(chr_sect: dict, council_rows: Any, me: Any) -> list[int]:
    """Who may be the diarch: the ruler's children and spouses, and his councillors (at most 64)."""
    ids: list[int] = []

    def add(x):
        v = x.get("id") if isinstance(x, dict) else x
        v = v.get("character_id") if isinstance(v, dict) else v
        if isinstance(v, int) and not isinstance(v, bool) and v > 0 and v != me and v not in ids:
            ids.append(v & 0xFFFFFF)
    for k in ("children", "spouses"):
        for x in chr_sect.get(k) or [] if isinstance(chr_sect, dict) else []:
            add(x.get("character_id") if isinstance(x, dict) and "character_id" in x else x)
    if isinstance(chr_sect, dict):
        add(chr_sect.get("spouse_id"))
    for c in council_rows or [] if isinstance(council_rows, list) else []:
        if isinstance(c, dict) and not c.get("vacant"):
            add(c.get("holder_id"))
    return ids[:64]


#: the validity stages the bridge names that come AFTER is_shown (ai_eval_hook.cpp validity_failed_stage): a refusal
#: there means is_shown held. `actor_or_recipient_unavailable` comes BEFORE it and says nothing (live d1073: a
#: councillor and the son read so; it is not the diarch)
PAST_SHOWN = frozenset({"hardcoded", "is_valid_showing_failures_only", "is_valid", "unattributed"})
#: the check that carries the game's block words for the diarch (is_valid custom_tooltip, 00_dynast_interactions.txt)
WORDS_CHECK = "disinherit_interaction"


def diarch_of(checks: dict) -> Optional[dict]:
    """The diarch from the game's checks ({interaction: reply}, nothing sent): {id, kind, can_end_now, why_not?}
    from a dismiss interaction whose is_shown held for him (its own check of `scope:actor.diarch`), or {id, kind:
    "not_read", source} from the game's block words on his disinherit check; {"id": None} when both dismiss checks
    answered and neither found him; None when a dismiss check did not answer (nobody looked is not "no diarch").
    The game's other diarchy types (a regency whose swing gives no dismissal parameter, a co-rulership) pass
    neither dismiss is_shown: only the words find those."""
    for name, kind in DISMISS.items():
        rep = checks.get(name)
        rows = ((rep.get("result") or {}).get("results") if isinstance(rep, dict) else None)
        for r in rows if isinstance(rows, list) else []:
            if isinstance(r, dict) and (r.get("sendable") is True or r.get("failed_check") in PAST_SHOWN):
                out = {"id": r.get("target_id"), "kind": kind, "can_end_now": r.get("sendable") is True}
                if r.get("sendable") is not True:
                    out["why_not"] = r.get("engine_reason") or r.get("failed_check")
                return out
    rep = checks.get(WORDS_CHECK)
    rows = ((rep.get("result") or {}).get("results") if isinstance(rep, dict) else None)
    for r in rows if isinstance(rows, list) else []:
        if isinstance(r, dict) and is_block_words(r.get("engine_reason")):
            out = {"id": r.get("target_id"), "kind": "not_read", "can_end_now": False,
                   "source": "the game's words on his disinherit check"}
            # live d1072 (1072.01.01): both dismiss checks refused him with the game's words "Duke Berthold is
            # traveling": why it cannot end now
            why = next((x.get("engine_reason") for n in DISMISS for x in
                        (((checks.get(n) or {}).get("result") or {}).get("results") or []
                         if isinstance(checks.get(n), dict) else [])
                        if isinstance(x, dict) and x.get("target_id") == r.get("target_id") and x.get("engine_reason")),
                       None)
            if why:
                out["why_not"] = why
            return out
    answered = all(isinstance(((checks.get(n) or {}).get("result") or {}).get("results")
                              if isinstance(checks.get(n), dict) else None, list) for n in DISMISS)
    return {"id": None} if answered else None


async def read(gw, me: int, chr_sect: dict, council_rows: Any) -> Optional[dict]:
    """The ruler's diarch, by the game's checks (nothing sent); None when unread."""
    ids = candidates(chr_sect, council_rows, me)
    if not me or not ids:
        return {"id": None} if me else None
    checks = {}
    for name in (*DISMISS, WORDS_CHECK):
        checks[name] = await gw.read("query_interaction_check", {"actor_id": me, "interaction_type": name,
                                                                 "candidates": ids})
    return diarch_of(checks)


def news_row(prev: Any, now: Any) -> Optional[dict]:
    """news diarch_changed: a diarch appointed, changed or ended for the ruler (both reads made), else None."""
    if not isinstance(prev, dict) or not isinstance(now, dict):
        return None
    a, b = prev.get("id"), now.get("id")
    if a == b and (a is None or prev.get("kind") == now.get("kind")):
        return None
    row: dict = {"from": a, "to": b}
    if b is not None:
        row.update(kind=now.get("kind"), can_end_now=now.get("can_end_now"),
                   blocks=f"while {b} is diarch the game refuses {BLOCKS}", how_it_came=STARTS,
                   how_it_ends=ENDS.format(name=DISMISS_ENTRENCHED if now.get("kind") == "entrenched_regency"
                                           else DISMISS_TEMPORARY))
        if now.get("why_not"):
            row["why_not_now"] = now["why_not"]
        if now.get("can_end_now"):
            row["next"] = end_call(b, now.get("kind"))
    else:
        row["ended"] = f"{a} is no longer your ruler's diarch: disinherit and vows on him are no longer blocked by it"
    return row
