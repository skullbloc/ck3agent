"""A faction's demand to the seat's ruler, before and after it arrives.

The game's AI answers a faction's demand for an AI-run ruler (a test run, 1079.11.13: a
populist faction's letter, `faction_demand.1001`, answered "grant independence", and its
leader took every title). A faction presses its demand by sending its target a letter event
(each type's `demand = { ... trigger_event = <event> }` in common/factions/*.txt); the AI picks
one of its options inside the same call (re/findings/re_faction_demand.md). So the seat answers
in advance, with the levers it already has: `do(intent="event_rule")` for that letter's event,
or `do(intent="faction", args={action: "respond_demand", response: "refuse"})` for every
faction's letter. Refusing starts the faction's war on the ruler.

This module reads, from the game's own files, which letter each faction type sends and its
options (key, and whether it refuses: the bridge's rule, bridge/include/faction_demand.h), and
turns the bridge's query_faction_threat read into two news rows:

* `factions_against_you`: a faction aimed at the ruler that is new, changed its threat level,
  or stands high or critical (its AI leader may press now): discontent of 100, its power
  against the ruler's, the letter it will send with the options, who answers it now, and the
  call that refuses it.
* `faction_demands_answered`: each demand letter answered since the last read (the bridge
  records every one for a controlled ruler): the answer, what the AI would have picked, who
  decided (event_rule / respond_demand / ai), and what follows.

Nothing here is guessed: an unread file or read says `unread` with the reason.
"""
from __future__ import annotations

import math
import re
from pathlib import Path
from typing import Any, Optional

import seat_events as _SE

#: the bridge's rule (faction_demand.h is_refuse_key / is_accept_key)
_PREFIX = "FACTION_DEMAND_"


def is_refuse_key(k: Any) -> bool:
    return isinstance(k, str) and k.startswith(_PREFIX) and (k.endswith("_REFUSE") or k.endswith("_REFUSE_ADMIN"))


def is_accept_key(k: Any) -> bool:
    return (isinstance(k, str) and k.startswith(_PREFIX) and not k.endswith("_OPT")
            and "_ACCEPT" in k and "_ACCEPTED" not in k)


REFUSE_MEANS = "refusing starts the faction's war on your ruler (its casus belli); accepting grants its demand"
#: the threat levels shown every read while they hold (the bridge's classification)
_LOUD = ("high", "critical")


# ── a small reader for the game's script files ─────────────────────────────────
_TOK = re.compile(r'"[^"]*"|[{}=]|[^\s{}="#]+|#[^\n]*')


def parse(text: str) -> list:
    """The file as nested [(key, value)], a value a token or a list (a block)."""
    toks = [t for t in _TOK.findall(text.lstrip("﻿")) if not t.startswith("#")]
    pos = 0

    def block() -> list:
        nonlocal pos
        out: list = []
        while pos < len(toks):
            t = toks[pos]
            if t == "}":
                pos += 1
                return out
            if pos + 1 < len(toks) and toks[pos + 1] == "=":
                pos += 2
                if pos < len(toks) and toks[pos] == "{":
                    pos += 1
                    out.append((t.strip('"'), block()))
                elif pos < len(toks):
                    out.append((t.strip('"'), toks[pos].strip('"')))
                    pos += 1
            elif t == "{":
                pos += 1
                out.append(("", block()))
            else:
                out.append(("", t.strip('"')))
                pos += 1
        return out
    return block()


def _walk(node: Any):
    if isinstance(node, list):
        for k, v in node:
            yield k, v
            yield from _walk(v)


def _name_key(opt: list) -> Optional[str]:
    """An option's name: `name = KEY`, or a dynamic name's last `desc = KEY` (its fallback:
    the bridge reads the same, faction_demand.h name_key)."""
    for k, v in opt:
        if k == "name":
            if isinstance(v, str):
                return v
            descs = [x for kk, x in _walk(v) if kk == "desc" and isinstance(x, str)]
            return descs[-1] if descs else None
    return None


_FILES: dict = {}


def _parsed(path: Path) -> list:
    if path not in _FILES:
        _FILES[path] = parse(path.read_text(encoding="utf-8-sig", errors="replace"))
    return _FILES[path]


def letter_options(event: str) -> tuple[Optional[list], str]:
    """(options, "") for a demand letter: [{index, key, refuses}] in file order (the engine's
    option order); (None, why) when the files cannot say."""
    found, where = _SE.event_in_files(event)
    root = _SE.game_dir()
    if not found or root is None:
        return None, where or f"no file under the game's events/ defines {event}"
    try:
        tree = _parsed(root / where)
    except OSError as e:
        return None, f"{where} could not be read ({type(e).__name__})"
    for k, v in tree:
        if k == event and isinstance(v, list):
            opts = [v2 for k2, v2 in v if k2 == "option" and isinstance(v2, list)]
            return [{"index": i, "key": _name_key(o), "refuses": is_refuse_key(_name_key(o))}
                    for i, o in enumerate(opts)], ""
    return None, f"{where} names {event} but its block did not parse"


def _flat(node: Any, depth: int = 0) -> str:
    """A parsed block as script text (for the conditions an option is offered under)."""
    if not isinstance(node, list):
        return str(node)
    parts = []
    for k, v in node:
        if k == "":
            parts.append(_flat(v, depth + 1))
        elif isinstance(v, list):
            parts.append(f"{k} = {{ {_flat(v, depth + 1)} }}")
        else:
            parts.append(f"{k} = {v}")
    return " ".join(parts)


#: An option's `trigger` holds while the game offers it; the AI-only part applies to the
#: seat's ruler too (he is an AI ruler in the game: is_ai = yes)
OFFER_RULE = ("the game offers an option only while its trigger holds, checked when the letter fires; "
              "your ruler is an AI ruler in the game, so its is_ai = yes conditions apply")


def option_offered(event: str, option: Any) -> dict:
    """Whether the game offers `option` of `event` to this ruler, as far as the files say:
    {"offered": "always"} with no trigger, {"offered": "unknown", "when": <its trigger>} with one (its
    conditions are the game's to check at fire time), {"offered": "unknown", "unread": why} else."""
    found, where = _SE.event_in_files(event)
    root = _SE.game_dir()
    if not found or root is None:
        return {"offered": "unknown", "unread": where or f"no file under events/ defines {event}"}
    try:
        tree = _parsed(root / where)
    except OSError as e:
        return {"offered": "unknown", "unread": f"{where} could not be read ({type(e).__name__})"}
    body = next((v for k, v in tree if k == event and isinstance(v, list)), None)
    for i, (k, v) in enumerate(x for x in (body or []) if x[0] == "option"):
        if not isinstance(v, list) or (_name_key(v) != option and option != i):
            continue
        trig = next((x for kk, x in v if kk == "trigger" and isinstance(x, list)), None)
        if trig is None:
            return {"offered": "always"}
        text = _flat(trig).replace("> =", ">=").replace("< =", "<=")
        return {"offered": "unknown", "when": text[:240] + (" ..." if len(text) > 240 else ""),
                "ai_conditions": "is_ai" in text}
    return {"offered": "unknown", "unread": f"{option} is not an option of {event} in {where}"}


_LETTERS: dict = {}


def letters() -> tuple[Optional[dict], str]:
    """{faction type: demand letter event} from common/factions/*.txt: of the events a type's
    `demand` block fires, the one whose options include one refusal (a notice to the members
    is not the letter). (None, why) when the files are not readable here."""
    if "map" in _LETTERS:
        return _LETTERS["map"], _LETTERS["why"]
    root = _SE.game_dir()
    if root is None or not (root / "common" / "factions").is_dir():
        return None, "the game's faction files are not readable here (set CK3_GAME_DIR to the game/ folder)"
    out: dict = {}
    try:
        for f in sorted((root / "common" / "factions").glob("*.txt")):
            for ftype, body in _parsed(f):
                if not isinstance(body, list):
                    continue
                demand = next((v for k, v in body if k == "demand" and isinstance(v, list)), None)
                if demand is None:
                    continue
                fired = []
                for k, v in _walk(demand):
                    if k == "trigger_event" and isinstance(v, str):
                        fired.append(v)
                    elif k == "trigger_event" and isinstance(v, list):
                        fired += [x for kk, x in v if kk == "id" and isinstance(x, str)]
                for ev in fired:
                    opts, _ = letter_options(ev)
                    if opts and sum(1 for o in opts if o["refuses"]) == 1 and any(
                            is_accept_key(o["key"]) for o in opts):
                        out[ftype] = ev
                        break
    except OSError as e:
        return None, f"the game's faction files could not be read ({type(e).__name__})"
    _LETTERS.update(map=out, why="")
    return out, ""


def refuse_key(opts: Optional[list]) -> Optional[str]:
    keys = [o["key"] for o in opts or [] if o.get("refuses")]
    return keys[0] if len(keys) == 1 else None


def refuse_call(event: str, opts: Optional[list]) -> dict:
    """The call that refuses this letter from now on: an event_rule for it when the refuse
    option is known, else the standing answer for every faction's letter."""
    k = refuse_key(opts)
    if k:
        return {"tool": "do", "args": {"intent": "event_rule", "args": {
            "op": "set", "event": event, "answer": "option", "option": k}}}
    return {"tool": "do", "args": {"intent": "faction", "args": {"action": "respond_demand", "response": "refuse"}}}


# ── the game's demand clock ───────────────────────────────────────
#: common/defines/00_defines.txt NFaction and common/script_values/00_faction_values.txt +
#: common/scripted_modifiers/00_faction_modifiers.txt common_discontent_progress_modifier: a faction's
#: discontent moves every month by base_discontent_progress (3); while its power is over the power
#: threshold (DEFAULT_POWER_THRESHOLD 80 for most types; +20 with the target's hard_rule_perk, lowered
#: while other factions are strong: neither read here) it gains faction_discontent_for_extra_power more:
#: 3 x 4 x (power - threshold) / threshold, rounded, at most 10; under the threshold it loses 2 x 3.
#: At DISCONTENT_THRESHOLD 100 it can send its demand and does so within MAX_DEMAND_DELAY_DAYS 90.
DISCONTENT_THRESHOLD = 100
POWER_THRESHOLD = 80
BASE_PROGRESS = 3
EXTRA_PROGRESS_FACTOR = 4
EXTRA_PROGRESS_MAX = 10
MAX_DEMAND_DELAY_DAYS = 90
DEMAND_RULE = ("game rule: over its power threshold (80 for most factions) discontent rises 3-13 a month; at 100 "
               "the demand comes within 90 days")
_GRADES = ("low", "medium", "high", "critical", "at_war")


def faction_power(f: Any) -> Optional[float]:
    """A recorded case (faction 75 on 0887.07.17: query read power_ratio 107.38, inspect power_pct_of_liege
    51.78 "medium"; Pepin's faction read 47.8 and 53.7 "low"/"medium", then declared and won): THE faction power
    every reader reports, the number the game compares with power_threshold (80): the faction's soldiers against
    the ruler's without them, x100 (the bridge's power_ratio, CalcPowerRatio's cached +0x98; 00_factions.txt
    ai_demand_chance: "faction_power > 110" is "10% stronger than liege"). The old power_pct_of_liege,
    power_ratio / (power_ratio + 100), is a share of the two together that never reaches 80 while the faction is
    under 4x the ruler: never the game's test (the bridge sends power_pct_of_liege = power_ratio, read only
    where a reply has no power_ratio). None when the read has no number."""
    if not isinstance(f, dict):
        return None
    for k in ("power_ratio", "power_pct_of_liege"):
        v = f.get(k)
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return round(float(v), 2)
    return None


def discontent_per_month(power: float) -> float:
    if power < POWER_THRESHOLD:
        return float(BASE_PROGRESS - 2 * BASE_PROGRESS)
    extra = min(EXTRA_PROGRESS_MAX, round(BASE_PROGRESS * EXTRA_PROGRESS_FACTOR * (power - POWER_THRESHOLD)
                                          / POWER_THRESHOLD))
    return float(BASE_PROGRESS + extra)


def demand_clock(power: Any, discontent: Any) -> Optional[dict]:
    """{over_threshold, per_month, months_to_demand (None while under the threshold)} from the game's
    rule; None when power or discontent did not read."""
    if not isinstance(power, (int, float)) or not isinstance(discontent, (int, float)):
        return None
    rate = discontent_per_month(float(power))
    over = float(power) >= POWER_THRESHOLD
    months = (max(0, math.ceil((DISCONTENT_THRESHOLD - float(discontent)) / rate)) if over else None)
    return {"over_threshold": over, "per_month": rate, "months_to_demand": months}


def crossed_threshold(was: dict, now: dict) -> bool:
    """The faction's power read under the demand threshold before and at or over it now (one rule, two
    readers: this news row and the wake event faction_over_threshold)."""
    a, b = was.get("power_ratio"), now.get("power_ratio")
    return (isinstance(a, (int, float)) and isinstance(b, (int, float)) and a < POWER_THRESHOLD <= b
            and not now.get("at_war"))


def graded_threat(f: dict) -> str:
    """The threat graded by power as well as discontent: never below the bridge's own level; a faction
    over the power threshold is at least medium, one as strong as the ruler or with its demand within
    6 months is high."""
    bridge = f.get("threat_level")
    ours = "low"
    clock = demand_clock(f.get("power_ratio"), f.get("discontent"))
    if clock and clock["over_threshold"]:
        ours = "medium"
        if float(f["power_ratio"]) >= 100 or (clock["months_to_demand"] is not None
                                              and clock["months_to_demand"] <= 6):
            ours = "high"
    if bridge in _GRADES and _GRADES.index(bridge) >= _GRADES.index(ours):
        return bridge
    return ours


#: what losing the faction's war does (common/casus_belli_types/00_civil_war.txt on_victory, the
#: attacker's victory), for the types whose outcome is plain in the file
IF_WAR_LOST = {
    # claimant_faction_war on_victory: on_claimant_faction_war_win_common; a harness's line_lost_to_other_dynasty
    "claimant_faction": ("its claimant takes the claimed titles: another dynasty ENDS THE EPISODE AS A LOSS; "
                         "yours (a son, a brother) makes your ruler his vassal"),
    "independence_faction": "its members leave your realm, independent",
    "liberty_faction": "your crown authority drops a level",
}
#: the levers in the 13 tools that act on a faction against the ruler and are proven live
#: (a start-audit census: respond_demand 10 applied; levers.md l10_factions_liege: imprison, gift)
LEVERS = ["imprison its leader (inspect factions: levers.imprison)",
          "a gift to its members (inspect factions: levers.gift)",
          "answer the demand first: do faction respond_demand, or the event_rule in demand (refuse = its war)"]


# ── observe's news ─────────────────────────────────────────────────────────────
_ROW_KEYS = ("faction_id", "type", "leader_id", "discontent", "power_ratio", "threat_level", "ultimatum",
             "ai_press_chance_pct", "demand_cooldown_until", "at_war", "war_id", "county_count", "member_count")


def snapshot_part(threat: Any) -> dict:
    """What news diffs of the ruler's factions: each faction's row, the standing answer, the
    demand letters answered (None when query_faction_threat did not answer)."""
    if not isinstance(threat, dict) or not isinstance(threat.get("factions"), list):
        return {"factions_against": None, "demand_response": None, "demands_answered": None}
    rows = {}
    for f in threat["factions"]:
        if isinstance(f, dict) and isinstance(f.get("faction_id"), int):
            row = {k: f[k] for k in _ROW_KEYS if k in f}
            row["threat_level"] = graded_threat(f)       # Graded by power too, never below the bridge's
            rows[f["faction_id"]] = row
    ans = threat.get("demands_answered")
    return {"factions_against": rows, "demand_response": threat.get("demand_response"),
            "demands_answered": [a for a in ans if isinstance(a, dict)] if isinstance(ans, list) else None}


def _who_answers(event: Optional[str], snap: dict) -> str:
    if event and event in (snap.get("event_rule_events") or []):
        return f"your event_rule for {event}"
    resp = snap.get("demand_response")
    if resp in ("accept", "refuse"):
        return f"your standing answer: {resp} (do faction respond_demand)"
    if resp is None:
        return "unknown: the standing answer did not read"
    return ("the game's AI: it weighs the faction's power against yours, your debt and your ruler's "
            "boldness (the letter's ai_chance)")


def _letter(ftype: Any) -> dict:
    lmap, why = letters()
    if lmap is None:
        return {"event": None, "unread": why}
    ev = lmap.get(ftype)
    if not ev:
        return {"event": None, "unread": f"no demand letter found for {ftype} in common/factions"}
    opts, why = letter_options(ev)
    out = {"event": ev, "options": [{"index": o["index"], "key": o["key"], **({"refuses": True} if o["refuses"]
                                                                              else {}),
                                     # Which this ruler can take, as far as the file says
                                     "offered": option_offered(ev, o["key"]).get("offered")}
                                    for o in opts or []]}
    if any(o["offered"] != "always" for o in out["options"]):
        out["offered_rule"] = OFFER_RULE
    if opts is None:
        out["options_unread"] = why
    return out


def _threat_row(f: dict, snap: dict, why: str) -> dict:
    row = {"faction_id": f.get("faction_id"), "type": f.get("type"), "leader_id": f.get("leader_id"),
           "shown_because": why}
    if isinstance(f.get("discontent"), (int, float)):
        row["discontent"] = f"{round(float(f['discontent']), 1)} of 100 (at 100 its leader may press the demand)"
    if isinstance(f.get("power_ratio"), (int, float)):
        row["power_pct"] = round(float(f["power_ratio"]), 1)
        row["power_basis"] = ("the faction's soldiers against your ruler's without them, x100: 100 = as strong "
                              "as you; the AI's accept chance rises above 100")
    for k in ("threat_level", "ultimatum", "ai_press_chance_pct", "demand_cooldown_until", "at_war", "war_id"):
        if k in f and f[k] is not None:
            row[k] = f[k]
    # The strength ratio against the demand threshold, the game's clock to the demand, what losing
    # its war does and the levers that exist
    clock = demand_clock(f.get("power_ratio"), f.get("discontent"))
    if clock is not None and not f.get("at_war"):
        row["over_demand_threshold"] = clock["over_threshold"]
        m = clock["months_to_demand"]
        row["demand_in"] = (f"about {m} months, then within {MAX_DEMAND_DELAY_DAYS} days" if m is not None
                            else "not while its power stays under 80")
        row["demand_rule"] = DEMAND_RULE
    if f.get("type") in IF_WAR_LOST:
        row["if_war_lost"] = IF_WAR_LOST[f["type"]]
    letter = _letter(f.get("type"))
    row["demand"] = letter
    row["answered_now_by"] = _who_answers(letter.get("event"), snap)
    row["refusing"] = REFUSE_MEANS
    answered_by_rule = letter.get("event") and letter["event"] in (snap.get("event_rule_events") or [])
    stronger = isinstance(f.get("power_ratio"), (int, float)) and float(f["power_ratio"]) >= 100
    if not f.get("at_war"):
        row["levers"] = LEVERS
    if not answered_by_rule and snap.get("demand_response") != "refuse" and not f.get("at_war"):
        call = (refuse_call(letter["event"], letter.get("options")) if letter.get("event")
                else refuse_call("", None))
        if stronger:
            # refusing a faction stronger than the ruler is a war he is likely to lose: shrink it first
            row["next"] = {"tool": "inspect", "args": {"subject": "factions"}}
            row["refuse_call"] = call
        else:
            row["next"] = call
    return row


def news_part(prev: dict, snap: dict) -> dict:
    out: dict = {}
    old, new = prev.get("factions_against"), snap.get("factions_against")
    if new is None:
        if prev.get("factions_against") is not None:
            out["factions_unread"] = ("query_faction_threat did not answer: the factions against your ruler "
                                      "and the demands answered are unknown, not none")
        return out
    if prev.get("actor_id") != snap.get("actor_id"):
        old = None                     # a new ruler: every faction against him is news
    rows = []
    for fid, f in sorted(new.items()):
        was = (old or {}).get(fid)
        if old is None or was is None:
            why = "new"
        elif crossed_threshold(was, f):
            why = f"crossed the demand power threshold ({POWER_THRESHOLD}): its discontent now climbs to the demand"
        elif was.get("threat_level") != f.get("threat_level"):
            why = f"threat {was.get('threat_level')} -> {f.get('threat_level')}"
        elif f.get("threat_level") in _LOUD:
            why = f"threat {f.get('threat_level')}"
        else:
            continue
        rows.append(_threat_row(f, snap, why))
    if rows:
        out["factions_against_you"] = rows
    ans = snap.get("demands_answered")
    if isinstance(ans, list):
        last = max([a.get("seq") or 0 for a in prev.get("demands_answered") or [] if isinstance(a, dict)] or [0])
        done = []
        for a in ans:
            if (a.get("seq") or 0) <= last:
                continue
            row = {k: a.get(k) for k in ("date", "event", "answered", "ai_would_have", "by", "refused")}
            row["options"] = a.get("options")
            # A rule whose option the game did not offer: which was wanted and that the
            # AI's answer was applied (the bridge's record says by=ai; the pick record says the rest)
            pk = next((p for p in snap.get("event_picks") or [] if isinstance(p, dict)
                       and p.get("event") == a.get("event") and p.get("date") == a.get("date")
                       and p.get("answered_by") in ("ai", "event_rule_else")), None)
            if pk is not None:
                w = pk.get("wanted")
                row["rule_wanted"] = (w.get("key") or w.get("index")) if isinstance(w, dict) else w
                row["rule_why"] = ("the game did not offer it to your ruler" if pk.get("why") == "option_not_available"
                                   else pk.get("why"))
                row["by"] = "ai" if pk.get("answered_by") == "ai" else "event_rule_else"
            if a.get("refused"):
                # run f2 (1070.10.28): 32110's peasant war started and ended between two reads, so
                # wars_started never listed it; the row says what follows, not where it shows
                row["then"] = ("refused: the faction declared its war on your ruler at once (its casus belli); "
                               "a war that started and ended between two reads is in no wars_started row")
            else:
                row["then"] = f"granted: {a.get('answered')}"
            if a.get("by") == "ai" and a.get("event"):
                opts, _ = letter_options(a["event"])
                row["next"] = refuse_call(a["event"], opts or [
                    {"index": i, "key": k, "refuses": is_refuse_key(k)} for i, k in enumerate(a.get("options") or [])])
            done.append(row)
        if done:
            out["faction_demands_answered"] = done
    return out


# ── the factions a VASSAL ruler is in ───────────────────────────────────────────────────
#: The game's AI may put a vassal ruler into a faction against his liege, and a faction he LEADS fights its war,
#: when its demand is refused, with him as its attacker (common/scripted_effects/00_faction_effects.txt
#: faction_start_war): a war he declared. Read from the bridge's query_factions on the liege (the member ids the
#: game keeps; factions.py confirms its writes with the same read). Built, not proven live on a vassal's observe.
#: An independent ruler (liege 0) is in no faction: the caller passes NO_LIEGE and nothing is read.
NO_LIEGE: dict = {"factions": []}
_MEMBER_KEYS = ("faction_id", "type", "leader_character_id", "discontent_pct", "ultimatum", "at_war", "war_id",
                "member_count")
JOINED = "joined since your last read"
FIRST = "a member at your first read"
LEADS = "your ruler leads it"
NOW_LEADS = "your ruler leads it now"
IF_REFUSED_LEADER = ("refused, its demand becomes a war on your liege with its leader as attacker: led by your "
                     "ruler, that war is one he declared")
IF_REFUSED_MEMBER = ("refused, its demand becomes a war on your liege led by {leader}; you fight in it on his side, "
                     "and if he leaves or dies a member may come to lead it")


def member_part(liege_factions: Any, actor_id: Any) -> dict:
    """{factions_in: {faction_id: row} of the factions aimed at the liege whose member ids hold the actor (None when
    the read did not answer), factions_in_unread: [faction_id] whose member list did not read}."""
    if not isinstance(liege_factions, dict) or not isinstance(liege_factions.get("factions"), list) or not actor_id:
        return {"factions_in": None, "factions_in_unread": None}
    rows, unread = {}, []
    for f in liege_factions["factions"]:
        if not isinstance(f, dict) or not isinstance(f.get("faction_id"), int):
            continue
        mem = f.get("members")
        leads = f.get("leader_character_id") == actor_id
        if isinstance(mem, list):
            inside = actor_id in mem or leads
        elif leads:
            inside = True
        else:
            if (f.get("member_count") or 0) > 0:
                unread.append(f["faction_id"])
            continue
        if inside:
            rows[f["faction_id"]] = {k: f[k] for k in _MEMBER_KEYS if k in f}
    return {"factions_in": rows, "factions_in_unread": unread}


def _member_row(f: dict, me: Any, why: str) -> dict:
    leads = f.get("leader_character_id") == me
    row = {"faction_id": f.get("faction_id"), "type": f.get("type"), "you_lead": leads, "why": why}
    if not leads:
        row["leader_id"] = f.get("leader_character_id")
    if isinstance(f.get("discontent_pct"), (int, float)):
        row["discontent"] = f"{round(float(f['discontent_pct']), 1)} of 100 (at 100 its leader may press the demand)"
    for k in ("ultimatum", "at_war", "war_id"):
        if f.get(k) is not None:
            row[k] = f[k]
    row["if_refused"] = (IF_REFUSED_LEADER if leads
                         else IF_REFUSED_MEMBER.format(leader=f.get("leader_character_id")))
    row["next"] = {"tool": "do", "args": {"intent": "faction", "args": {"action": "leave",
                                                                         "faction_id": f.get("faction_id")}}}
    return row


def member_news(prev: dict, snap: dict) -> dict:
    """`factions_you_are_in`: a membership new since the last read, and every faction the ruler leads at every read;
    `factions_left`: the ids he is no longer in; an unread membership is said, never read as none."""
    out: dict = {}
    new, me = snap.get("factions_in"), snap.get("actor_id")
    if snap.get("factions_in_unread"):
        out["factions_you_are_in_unread"] = ("the member lists of factions " + ", ".join(
            str(x) for x in snap["factions_in_unread"]) + " did not read: whether your ruler is in them is unknown")
    if new is None:
        if prev.get("factions_in") is not None:
            out["factions_you_are_in_unread"] = ("query_factions on your liege did not answer: the factions your "
                                                 "ruler is in are unknown, not none")
        return out
    old = prev.get("factions_in") if prev.get("actor_id") == me else None
    rows = []
    for fid, f in sorted(new.items()):
        was = (old or {}).get(fid)
        leads = f.get("leader_character_id") == me
        if was is None:
            why = FIRST if old is None else JOINED
        elif leads:
            why = LEADS if was.get("leader_character_id") == me else NOW_LEADS
        else:
            continue
        rows.append(_member_row(f, me, why))
    if rows:
        out["factions_you_are_in"] = rows
    left = sorted(fid for fid in (old or {}) if fid not in new)
    if left:
        out["factions_left"] = left
    return out
