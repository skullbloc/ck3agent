"""What the game's files say an answered event, or a decision the game's AI took,
cost the ruler in prestige or piety, so the purse names the cause with its amount instead of `partly unknown`.

a seated game's unexplained prestige, traced in the game's monthly telemetry
(the game's own telemetry log) and its files:
  * 1070.03-1070.11, Murchad, -300 in four lumps of 75: the events his AI answered on the road to
    and at a coronation (travel_danger_events.9010.b, .6000.c: add_prestige = minor_prestige_loss,
    -75 each; coronation_events.0205.d inside a condition).
  * 1087.10, Domnall, -1,000 at once: his culture went from irish to norse (the read of 1090.08.06),
    Dublin's county culture (history/provinces/k_ireland.txt: norse since 850):
    convert_to_local_culture_decision, convert_to_local_culture_base_cost 1000
    (00_basic_values.txt) times its multipliers, none of which applies between irish and norse
    (no shared ethos, heritage or language; the same martial custom): 1,000.
  * 1092.01.15, Domnall, -280 with a declaration on 14492 three days inside the truce of the white
    peace of 1087.01.19: breaking a truce, break_truce_prestige_loss -250 and a prestige level
    (00_war_effects.txt); the rest of the month's fall is the ally's -75 and the declaration.

This module reads the files only (CK3_GAME_DIR or the Steam default, seat_events.game_dir); an
amount it cannot read is said to be unread, never guessed.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Any, Optional

import faction_letters as _FL
import seat_events as _SE

#: the blocks of an option whose effects act on the ruler himself; any other block (a scope change:
#: scope:x = {...}, every_vassal = {...}, ...) acts on someone else and is not read
_SELF = {"hidden_effect", "root", "this", "show_as_tooltip"}
_COND = {"if", "else_if", "else", "random", "random_list", "switch"}
_VALUES: dict = {}


def basic_values() -> dict:
    """{name: number} of the prestige and piety scales (00_basic_values.txt *_prestige_value / *_piety_value and
    their _gain / _loss)."""
    if _VALUES:
        return _VALUES
    root = _SE.game_dir()
    p = (root / "common" / "script_values" / "00_basic_values.txt") if root else None
    if p is None or not p.is_file():
        return _VALUES
    text = p.read_text(encoding="utf-8-sig", errors="replace")
    for name, num in re.findall(r"^([a-z_]+_(?:prestige|piety)_value)\s*=\s*(-?\d+)", text, re.M):
        base = name[: -len("_value")]
        _VALUES[name] = int(num)
        _VALUES[base + "_gain"] = int(num)
        _VALUES[base + "_loss"] = -int(num)
    for name, num in re.findall(r"^([a-z_]+)\s*=\s*(-?\d+)\s*$", text, re.M):
        if ("prestige" in name or "piety" in name) and name not in _VALUES:
            _VALUES[name] = int(num)
    return _VALUES


def _amount(v: Any) -> Optional[int]:
    if isinstance(v, str):
        if re.fullmatch(r"-?\d+", v):
            return int(v)
        return basic_values().get(v)
    return None


def _walk_option(node: list, conditional: bool, out: list, effect: str = "add_prestige", _path: tuple = (),
                 _ends: Optional[set] = None) -> None:
    """The `effect` rows of an option, each {amount, value, conditional, _branch}: `_branch` the chains of if /
    else_if / else (and other conditional blocks) it sits in, as read_effects; `_ends` collects the chains that end
    in an else."""
    chain: Optional[list] = None
    for k, v in node:
        here, chain = (chain, None) if k not in ("else_if", "else") else (chain, chain)
        if k == effect:
            out.append({"amount": _amount(v) if isinstance(v, str) else None,
                        "value": v if isinstance(v, str) else "a computed value",
                        "conditional": conditional, **({"_branch": _path} if _path else {})})
        elif isinstance(v, list) and (k in _SELF):
            _walk_option(v, conditional, out, effect, _path, _ends)
        elif isinstance(v, list) and (k in _COND or re.fullmatch(r"\d+", k or "")):
            if k in ("else_if", "else") and here is not None:
                here[1] += 1
                link = here
            else:
                link = [_new_chain(), 0]
            if k == "else" and _ends is not None:
                _ends.add(link[0])
            _walk_option(v, True, out, effect, _path + ((link[0], link[1]),), _ends)
            chain = link if k in ("if", "else_if") else None


def option_prestige(event: Any, option: Any) -> Optional[dict]:
    """The prestige the ruler's own option `option` of `event` gives or takes, from the event's file:
    {"amount": n (unconditional sum) or None, "if_conditions_held": n, "file": rel} ; None when the
    option adds no prestige to the ruler (or the files are not readable: then {"unread": why})."""
    return option_amount(event, option, "prestige")


def option_piety(event: Any, option: Any) -> Optional[dict]:
    """The piety the ruler's own option `option` of `event` gives or takes (add_piety), as option_prestige."""
    return option_amount(event, option, "piety")


def option_amount(event: Any, option: Any, currency: str) -> Optional[dict]:
    """The `currency` ("prestige" or "piety": its add_<currency> effects) the ruler's own option `option` of `event`
    gives or takes, from the event's file (option_prestige)."""
    if not isinstance(event, str) or not isinstance(option, str):
        return None
    found, where = _SE.event_in_files(event)
    root = _SE.game_dir()
    if not found or root is None:
        return {"unread": where or f"no file under events/ defines {event}"} if found is None else None
    try:
        tree = _FL._parsed(root / where)
    except OSError as e:
        return {"unread": f"{where} could not be read ({type(e).__name__})"}
    body = next((v for k, v in tree if k == event and isinstance(v, list)), None)
    if body is None:
        return None
    for k, v in body:
        if k == "option" and isinstance(v, list) and _FL._name_key(v) == option:
            adds: list = []
            ends: set = set()
            _walk_option(v, False, adds, f"add_{currency}", (), ends)
            if not adds:
                return None
            sure = [a["amount"] for a in adds if not a["conditional"] and a["amount"] is not None]
            cond = [a for a in adds if a["conditional"] and a["amount"] is not None]
            out: dict = {"file": where}
            if sure:
                out["amount"] = sum(sure)
            if cond:
                # exclusive branches (if / else_if / else) are one of them, never their sum
                cp = conditional_price(cond, ends)
                out["if_conditions_held"] = cp["if_its_conditions_held"]
                for x in ("one_of", "range"):
                    if x in cp:
                        out[x] = cp[x]
            unread = [a["value"] for a in adds if a["amount"] is None]
            if unread:
                out["unread_values"] = unread
            return out
    return None


# ── Does an answered event's option move gold ────────────────────────────────────────
#: the effects that take or give gold (or a treasury's) in the game's script (1.18; counted in events/: add_gold 436,
#: pay_short_term_gold 703, remove_short_term_gold 756, pay_long_term_gold 7, remove_long_term_gold 2, add_treasury 8,
#: remove_treasury 11)
GOLD_EFFECTS = frozenset({"add_gold", "remove_short_term_gold", "remove_long_term_gold", "pay_short_term_gold",
                          "pay_long_term_gold", "add_treasury", "remove_treasury", "add_treasury_or_gold",
                          "remove_treasury_or_gold"})
_SCRIPTED_EFFECTS: dict = {}


def _scripted_effects() -> Optional[dict]:
    """{name: body} of common/scripted_effects (parsed once); None when the files are not readable."""
    if _SCRIPTED_EFFECTS:
        return _SCRIPTED_EFFECTS
    root = _SE.game_dir()
    d = (root / "common" / "scripted_effects") if root else None
    if d is None or not d.is_dir():
        return None
    try:
        for f in sorted(d.glob("*.txt")):
            for k, v in _FL._parsed(f):
                if isinstance(k, str) and isinstance(v, list):
                    _SCRIPTED_EFFECTS[k] = v
    except OSError:
        return None
    return _SCRIPTED_EFFECTS


def _moves_gold(node: list, effects: dict, depth: int, seen: set) -> bool:
    for k, v in node:
        if k in GOLD_EFFECTS:
            return True
        if k in effects and k not in seen and depth < 5:
            # a scripted effect called (`name = yes`, or with its parameters `name = { ... }`): its body counts
            if _moves_gold(effects[k], effects, depth + 1, seen | {k}):
                return True
        if isinstance(v, list) and _moves_gold(v, effects, depth, seen):
            return True
    return False


def option_gold(event: Any, option: Any) -> Optional[bool]:
    """Whether the answered option of `event` (its body, any branch or scope, the scripted effects it calls, the
    event file's own included) or the event's `after` block, which runs after the answer, can move gold; None when the
    files do not say (unreadable, the event or the option not found). The `immediate` block is not counted: it runs
    when the event fires, before the pick the purse reads the gold at."""
    if not isinstance(event, str) or not isinstance(option, str):
        return None
    found, where = _SE.event_in_files(event)
    root = _SE.game_dir()
    eff = _scripted_effects()
    if not found or root is None or eff is None:
        return None
    try:
        tree = _FL._parsed(root / where)
    except OSError:
        return None
    body = next((v for k, v in tree if k == event and isinstance(v, list)), None)
    if body is None:
        return None
    # By either reading of its name: `name = KEY`, a dynamic name's fallback, or (a name given as blocks, one per
    # variant: every stress event's options) the first block's text, as the bridge's pick records read it
    opt = next((v for k, v in body if k == "option" and isinstance(v, list)
                and option in (_FL._name_key(v), option_key(v))), None)
    if opt is None:
        return None
    # the scripted effects the event's own file defines (`scripted_effect name = {...}` at its top level)
    local = {k: v for k, v in tree if isinstance(k, str) and isinstance(v, list) and k and "." not in k}
    eff = {**eff, **local} if local else eff
    blocks = [opt] + [v for k, v in body if k == "after" and isinstance(v, list)]
    return any(_moves_gold(b, eff, 0, set()) for b in blocks)


# ── convert_to_local_culture_decision ──────────────────────────────────────────────────────
CONVERT_DECISION = "convert_to_local_culture_decision"
CONVERT_BASE = 1000
CONVERT_SOURCE = ("common/decisions/10_culture_conversion_decisions.txt convert_to_local_culture_decision: "
                  "convert_to_local_culture_base_cost 1000 (00_basic_values.txt) x0.5 to a hybrid of your "
                  "culture, x0.75 same ethos, x0.75 same heritage, x0.5 same language, x1.25 another martial "
                  "custom, x3 / x0.25 by cultural parameter, at least 25")
_PILLARS: dict = {}


def culture_pillars(key: Any) -> Optional[dict]:
    """{ethos, heritage, language, martial_custom} of a culture in common/culture/cultures, or None
    (a culture made in play, a hybrid or divergence, is in no file)."""
    if not isinstance(key, str) or not key:
        return None
    if key in _PILLARS:
        return _PILLARS[key]
    root = _SE.game_dir()
    d = root / "common" / "culture" / "cultures" if root else None
    found = None
    if d is not None and d.is_dir():
        pat = re.compile(r"^" + re.escape(key) + r"\s*=\s*\{", re.M)
        for f in sorted(d.glob("*.txt")):
            text = f.read_text(encoding="utf-8-sig", errors="replace")
            if pat.search(text):
                for k, v in _FL._parsed(f):
                    if k == key and isinstance(v, list):
                        found = {p: next((x for kk, x in v if kk == p and isinstance(x, str)), None)
                                 for p in ("ethos", "heritage", "language", "martial_custom")}
                        found["parents"] = [x for kk, x in _FL._walk(v) if kk == "parents"]
                break
    _PILLARS[key] = found
    return found


def convert_price(old: Any, new: Any) -> dict:
    """The prestige convert_to_local_culture_decision costs going from culture `old` to `new` (the
    capital's), from the decision's cost block and the two cultures' pillars in the files."""
    a, b = culture_pillars(old), culture_pillars(new)
    if a is None or b is None:
        return {"prestige": f"-{CONVERT_BASE} x multipliers (a culture not in the game's files: its pillars unread)",
                "basis": CONVERT_SOURCE}
    x = 1.0
    applied = []
    for p, f in (("ethos", 0.75), ("heritage", 0.75), ("language", 0.5)):
        if a.get(p) and a.get(p) == b.get(p):
            x *= f
            applied.append(f"same {p} x{f}")
    if a.get("martial_custom") and b.get("martial_custom") and a["martial_custom"] != b["martial_custom"]:
        x *= 1.25
        applied.append("another martial custom x1.25")
    return {"prestige": -max(25, round(CONVERT_BASE * x)), "multipliers": applied or ["none apply"],
            "unread": "cultural parameters (expensive x3, cheaper x0.25) and a struggle phase (x0.25)",
            "basis": CONVERT_SOURCE}


# ── A decision's price from its cost block ─────────────────────────────────────────
#: the currencies a decision's cost block names that the seat reads in its purse
DECISION_CURRENCIES = ("gold", "prestige", "piety")
_DECISION_FILE: dict = {}
#: the seat's ruler id -> his capital county's culture, as news.culture_conversion_ahead read it (the
#: map graph): prices a convert_to_local_culture_decision row the veto records
CAPITAL_CULTURE: dict = {}
_SCRIPT_VALUES: dict = {}


def decision_file(key: Any) -> Optional[Path]:
    """The common/decisions file that defines `key = {`, or None (not found or unreadable)."""
    if not isinstance(key, str) or not re.fullmatch(r"[a-z0-9_]+", key):
        return None
    if key in _DECISION_FILE:
        return _DECISION_FILE[key]
    root = _SE.game_dir()
    d = root / "common" / "decisions" if root else None
    found = None
    if d is not None and d.is_dir():
        pat = re.compile(r"^" + re.escape(key) + r"\s*=\s*\{", re.M)
        for f in sorted(d.rglob("*.txt")):
            if pat.search(f.read_text(encoding="utf-8-sig", errors="replace")):
                found = f
                break
    _DECISION_FILE[key] = found
    return found


_DECISION_KEYS: list = []


def decision_keys() -> Optional[frozenset]:
    """Every decision key the game's common/decisions files define (a top-level `key = {`), or None when
    the files are not found (the caller then cannot check a key and says so)."""
    if _DECISION_KEYS:
        return _DECISION_KEYS[0]
    root = _SE.game_dir()
    d = root / "common" / "decisions" if root else None
    if d is None or not d.is_dir():
        return None
    pat = re.compile(r"^([a-z0-9_]+)\s*=\s*\{", re.M)
    keys = frozenset(m.group(1) for f in sorted(d.rglob("*.txt"))
                     for m in pat.finditer(f.read_text(encoding="utf-8-sig", errors="replace")))
    if not keys:
        return None
    _DECISION_KEYS.append(keys)
    return keys


def script_value(name: str) -> Optional[float]:
    """A script value defined as a plain number in common/script_values (`name = 100`), else None."""
    if not _SCRIPT_VALUES:
        root = _SE.game_dir()
        d = root / "common" / "script_values" if root else None
        if d is not None and d.is_dir():
            for f in sorted(d.glob("*.txt")):
                for n, v in re.findall(r"^([a-z0-9_]+)\s*=\s*(-?\d+(?:\.\d+)?)\s*(?:#.*)?$",
                                       f.read_text(encoding="utf-8-sig", errors="replace"), re.M):
                    _SCRIPT_VALUES.setdefault(n, float(v))
        _SCRIPT_VALUES.setdefault("__read__", 0.0)
    return _SCRIPT_VALUES.get(name)


def _plain(v: Any) -> tuple[Optional[float], str]:
    """(amount, how) for one currency's value: a number, a script value that is a number, or a block
    holding only `value = <number or such a name>`; (None, why) for anything computed."""
    if isinstance(v, str):
        if re.fullmatch(r"-?\d+(?:\.\d+)?", v):
            return float(v), "a number"
        x = script_value(v)
        return (x, f"the script value {v}") if x is not None else (None, f"{v}: a script value computed in play")
    if isinstance(v, list) and len(v) == 1 and v[0][0] == "value" and isinstance(v[0][1], str):
        return _plain(v[0][1])
    return None, "a value computed in play (conditions or multipliers in its cost block)"


_ON_ACTIONS: dict = {}


def on_action_events(name: Any) -> Optional[list]:
    """The events an on_action of common/on_action lists (its `events`, `random_events` and `first_valid`
    blocks), in file order; None when the files are not readable or no file defines it."""
    if not isinstance(name, str) or not name:
        return None
    if not _ON_ACTIONS:
        root = _SE.game_dir()
        d = (root / "common" / "on_action") if root else None
        if d is None or not d.is_dir():
            return None
        try:
            for f in sorted(d.rglob("*.txt")):
                for k, v in _FL._parsed(f):
                    if isinstance(k, str) and isinstance(v, list):
                        _ON_ACTIONS.setdefault(k, []).append(v)
        except OSError:
            _ON_ACTIONS.clear()
            return None
        _ON_ACTIONS.setdefault("__read__", [])
    blocks = _ON_ACTIONS.get(name)
    if blocks is None:
        return None
    out: list = []
    for b in blocks:
        for k, v in b:
            if k in ("events", "random_events", "first_valid") and isinstance(v, list):
                for _w, ev in v:
                    if isinstance(ev, str) and "." in ev and ev not in out:
                        out.append(ev)
    return out


def fired_events(body: list) -> list:
    """The events a decision's `effect` block fires directly (trigger_event = KEY,
    trigger_event = { id = KEY ... }, or trigger_event = { on_action = NAME }: the events that on_action lists),
    first level only (an event the fired event fires is not followed): [{event, conditional[, on_action]}] in file
    order, one row per key."""
    eff = next((v for k, v in body if k == "effect" and isinstance(v, list)), None)
    out: dict = {}

    def add(ev: str, cond: bool, via: Optional[str] = None) -> None:
        if ev not in out:
            out[ev] = {"event": ev, "conditional": cond, **({"on_action": via} if via else {})}
        else:
            out[ev]["conditional"] = out[ev]["conditional"] and cond

    def walk(node: list, cond: bool) -> None:
        for k, v in node:
            if k == "trigger_event":
                ev = v if isinstance(v, str) else next((x for kk, x in v if kk == "id" and isinstance(x, str)),
                                                       None) if isinstance(v, list) else None
                if ev:
                    add(ev, cond)
                oa = next((x for kk, x in v if kk == "on_action" and isinstance(x, str)), None) \
                    if isinstance(v, list) else None
                for e2 in on_action_events(oa) or []:
                    # (stress_loss_improvident_decision: no cost block; its effect fires the on_action
                    # stress_loss_improvident, whose one event takes medium_gold_value): an on_action's events count
                    add(e2, cond, oa)
            elif isinstance(v, list):
                walk(v, cond or k in _COND or k == "limit")
    if eff:
        walk(eff, False)
    return list(out.values())


# ── An option's gold, by the script value it names ────────────────────────────────────────
#: The gold scale of the game's events (01_dynamic_values.txt): `<tier>_gold_value` is the ruler's monthly income
#: times <tier>_income_multiplier_value, at least <tier>_gold_min_value, at most its maximum, rounded up to 5
GOLD_TIERS = ("tiny", "minor", "medium", "major", "massive", "monumental")
_GOLD_VALUE_RE = re.compile(r"^(" + "|".join(GOLD_TIERS) + r")_gold_value(?:_static_max)?$")
GOLD_VALUE_UNREAD = ("a vassal with a treasury (an administrative realm, not the top liege) pays a quarter; the era "
                     "multiplies the maximum (x0.75 to x3): neither is read")


def gold_value(name: Any, monthly_income: Any) -> Optional[dict]:
    """{gold, basis} for a gold script value at a monthly income: `<tier>_gold_value` by its formula, a plain
    number (`major_gold_value_check = 300`) as it is; {unknown: why} when the formula's maximum may bind (the era's
    multiplier is not read) or the income is unread; None for a name that is not a gold value the files define."""
    if not isinstance(name, str):
        return None
    plain = script_value(name)
    if plain is not None:
        return {"gold": plain, "basis": f"the script value {name} = {plain:g}"}
    m = _GOLD_VALUE_RE.match(name)
    if not m:
        return None
    t = m.group(1)
    mult, lo, hi = (script_value(f"{t}_income_multiplier_value"), script_value(f"{t}_gold_min_value"),
                    script_value(f"{t}_gold_max_value_static"))
    if mult is None or lo is None or hi is None:
        return None
    rule = (f"{name}: {mult:g} months of income, at least {lo:g}, at most {hi:g} (the early era's maximum), "
            "rounded up to 5 (01_dynamic_values.txt)")
    if not isinstance(monthly_income, (int, float)) or isinstance(monthly_income, bool):
        return {"unknown": "your monthly income was not read", "rule": rule}
    raw = monthly_income * mult
    if raw > 0.75 * hi:
        return {"unknown": f"{mult:g} months of income is {raw:.0f}, near the maximum the era sets (not read)",
                "rule": rule}
    gold = float(-(-max(lo, raw) // 5) * 5)
    return {"gold": gold, "basis": f"{rule}; at income {monthly_income:g} a month: {gold:g}",
            "not_read": GOLD_VALUE_UNREAD}


#: the effects that take gold from the ruler, and how their amount reads
_TAKES_GOLD = ("remove_short_term_gold", "remove_long_term_gold", "pay_short_term_gold", "pay_long_term_gold")


def option_gold_taken(event: Any, option: Any, monthly_income: Any = None) -> Optional[dict]:
    """The gold `option` of `event` takes unconditionally from the ruler, by the script value it names
    ({gold: -n, basis} or {unknown, rule}); {conditional: true, ...} when it takes it only inside a condition; None
    when the files do not say or the option takes no gold by these effects."""
    if not isinstance(event, str) or not isinstance(option, (str, int)):
        return None
    found, where = _SE.event_in_files(event)
    root = _SE.game_dir()
    if not found or root is None:
        return None
    try:
        tree = _FL._parsed(root / where)
    except OSError:
        return None
    body = next((v for k, v in tree if k == event and isinstance(v, list)), None)
    opts = [v for k, v in body or [] if k == "option" and isinstance(v, list)]
    if isinstance(option, int):
        opt = opts[option] if 0 <= option < len(opts) else None
    else:
        opt = next((o for o in opts if option_key(o) == option or _FL._name_key(o) == option), None)
    if opt is None:
        return None
    hits: list = []

    def walk(node: list, cond: bool) -> None:
        for k, v in node:
            if k in ("trigger", "ai_chance", "name", "limit"):
                continue
            if k in _TAKES_GOLD and isinstance(v, str):
                hits.append((v, cond))
            elif isinstance(v, list):
                walk(v, cond or k in _COND)
    walk(opt, False)
    if not hits:
        return None
    value, cond = hits[0]
    gv = gold_value(value, monthly_income)
    if gv is None:
        return {"unknown": f"{value}: a value computed in play", **({"conditional": True} if cond else {})}
    out = dict(gv)
    if "gold" in out:
        out["gold"] = -abs(out["gold"])
    if cond:
        out["conditional"] = True
    return out


#: the effects that give the ruler gold, and the script values a ruler without a treasury (feudal, clan, tribal) is
#: paid by: `<tier>_treasury_or_gold_value` is `<tier>_gold_value` for him (01_dynamic_values.txt: has_treasury = no)
_GIVES_GOLD = ("add_gold", "add_treasury_or_gold")
TREASURY_OR_GOLD_NOTE = ("{name} pays a ruler without a treasury {gold_name} (01_dynamic_values.txt); a treasury "
                         "(an administrative realm) is paid its treasury value instead: not read")


def option_gold_given(event: Any, option: Any, monthly_income: Any = None) -> Optional[dict]:
    """The gold `option` of `event` gives the ruler unconditionally, by the script value it names ({gold: n, basis}
    or {unknown, rule}); {conditional: true, ...} when it gives it only inside a condition; None when the files do not
    say or the option gives no gold by these effects."""
    if not isinstance(event, str) or not isinstance(option, (str, int)) or isinstance(option, bool):
        return None
    found, where = _SE.event_in_files(event)
    root = _SE.game_dir()
    if not found or root is None:
        return None
    try:
        tree = _FL._parsed(root / where)
    except OSError:
        return None
    body = next((v for k, v in tree if k == event and isinstance(v, list)), None)
    opts = [v for k, v in body or [] if k == "option" and isinstance(v, list)]
    if isinstance(option, int):
        opt = opts[option] if 0 <= option < len(opts) else None
    else:
        opt = next((o for o in opts if option_key(o) == option or _FL._name_key(o) == option), None)
    if opt is None:
        return None
    hits: list = []

    def walk(node: list, cond: bool) -> None:
        for k, v in node:
            if k in ("trigger", "ai_chance", "name", "limit", "show_as_tooltip"):
                continue
            if k in _GIVES_GOLD and isinstance(v, str):
                hits.append((v, cond))
            elif isinstance(v, list) and (k in _SELF or k in _COND or k in EFFECT_COND):
                walk(v, cond or k in _COND or k in EFFECT_COND)
    walk(opt, False)
    if not hits:
        return None
    value, cond = hits[0]
    if re.fullmatch(r"-?\d+(?:\.\d+)?", value):
        out: dict = {"gold": float(value), "basis": f"{value} gold in that option ({where})"}
    else:
        name = value.replace("_treasury_or_gold_value", "_gold_value")
        gv = gold_value(name, monthly_income)
        if gv is None:
            return {"unknown": f"{value}: a value computed in play", **({"conditional": True} if cond else {})}
        out = dict(gv)
        if name != value:
            out["basis"] = (out.get("basis") or out.get("rule") or name) + "; " + TREASURY_OR_GOLD_NOTE.format(
                name=value, gold_name=name)
    if cond:
        out["conditional"] = True
    return out


def option_key(opt: list) -> Optional[str]:
    """An option's key as the bridge's pick records read it: `name = KEY`; for a name given as blocks
    (`name = { trigger = {...} text = KEY }`, one per variant), the first block's text; else the dynamic name's
    fallback (faction_letters._name_key)."""
    for k, v in opt:
        if k == "name":
            if isinstance(v, str):
                return v
            text = next((x for kk, x in v if kk == "text" and isinstance(x, str)), None) if isinstance(v, list) \
                else None
            return text if text else _FL._name_key(opt)
    return None


def price_fired(fired: list, monthly_income: Any = None) -> dict:
    """The prestige of the events a decision fires: each event's options with the prestige each takes from
    the ruler (option_prestige), `prestige` the worst option of the worst event when every option read
    (a negative number), else "unknown: why". The gold an option takes is read too (option_gold_taken):
    `gold` the dearest fired event's cheapest option, when one takes gold."""
    rows, worst, unread = [], 0.0, []
    gold_rows: list = []
    for fe in fired:
        opts, why = _FL.letter_options(fe["event"])
        r: dict = {"event": fe["event"], "conditional": fe["conditional"],
                   **({"on_action": fe["on_action"]} if fe.get("on_action") else {})}
        if opts is None:
            r["options"] = f"unknown: {why}"
            unread.append(fe["event"])
            rows.append(r)
            continue
        po = []
        golds = []
        for o in opts:
            p = option_prestige(fe["event"], o["key"]) or {}
            amt = p.get("amount", 0 if not p.get("unread") else None)
            if p.get("unread") or p.get("unread_values"):
                unread.append(f"{fe['event']}:{o['key']}")
            row = {"option": o["key"], "prestige": amt if amt is not None else "unknown"}
            g = option_gold_taken(fe["event"], o["index"], monthly_income)
            if g is not None:
                row["gold"] = g.get("gold", "unknown: " + str(g.get("unknown")))
                if g.get("conditional"):
                    row["gold_if_its_conditions_held"] = row.pop("gold")
                golds.append(g)
            po.append(row)
        nums = [x["prestige"] for x in po if isinstance(x["prestige"], (int, float))]
        if nums and len(nums) == len(po):
            # one option: its price; several: the AI picks one, so at least the cheapest
            worst = min(worst, max(nums))
        if golds and len(golds) == len(po) and not any(g.get("conditional") for g in golds):
            gold_rows.append(golds)
        r["options"] = po
        rows.append(r)
    out: dict = {"fires": rows}
    if unread:
        out["prestige"] = f"unknown: not every option read ({', '.join(unread[:3])})"
    elif worst < 0:
        out["prestige"] = worst
    else:
        out["prestige"] = 0
        out["note"] = "the events it fires take no prestige"
    if gold_rows:
        # every option of a fired event takes gold: the cheapest of them is paid at least
        amounts = [[g["gold"] for g in gs if isinstance(g.get("gold"), (int, float))] for gs in gold_rows]
        known = [max(a) for a, gs in zip(amounts, gold_rows) if len(a) == len(gs)]
        if known and len(known) == len(gold_rows):
            out["gold"] = min(known)
            out["gold_basis"] = next(g.get("basis") for gs in gold_rows for g in gs if g.get("basis"))
        else:
            out["gold"] = "unknown: " + next((str(g.get("unknown")) for gs in gold_rows for g in gs
                                              if "unknown" in g), "a value computed in play")
            rule = next((g.get("rule") for gs in gold_rows for g in gs if g.get("rule")), None)
            if rule:
                out["gold_basis"] = rule
    return out


def decision_ai_potential(key: Any) -> Optional[list]:
    """The conditions of a decision's `ai_potential` block in common/decisions, one string each (the check the
    seat's own take meets: AI_TRY_DECISION_CORE refuses "Does not fulfill ai_potential"); [] when it has none, None
    when the file was not read."""
    f = decision_file(key)
    if f is None:
        return None
    try:
        body = next((v for k, v in _FL._parsed(f) if k == key and isinstance(v, list)), None)
    except OSError:
        return None
    pot = next((v for k, v in body or [] if k == "ai_potential" and isinstance(v, list)), None)
    out: list = []
    for k, v in pot or []:
        txt = _FL._flat(v) if isinstance(v, list) else str(v)
        if not k and out and (txt in _COMPARE or out[-1].endswith(tuple(" " + c for c in _COMPARE))):
            out[-1] += " " + txt          # `a > b` reads as three bare tokens (the reader splits on `=` only)
        else:
            out.append(f"{k} = {txt}" if k else txt)
    return out


_COMPARE = ("<", ">", "<=", ">=", "!=")


def decision_price(key: Any, culture: Any = None, capital_culture: Any = None, monthly_income: Any = None) -> dict:
    """What the decision `key` costs, from its cost block in common/decisions: {"prestige": -n, ...}
    for each currency the file states as a number or a plain script value, "unknown: why" for one it
    computes in play, {"free": true} when it has no cost block; convert_to_local_culture_decision from both
    cultures' pillars when they are known. {"unread": why} when the file is not found."""
    if key == CONVERT_DECISION and culture and capital_culture:
        p = convert_price(culture, capital_culture)
        out = {"prestige": p["prestige"], "basis": p["basis"]}
        if "multipliers" in p:
            out["multipliers"] = p["multipliers"]
        return out
    f = decision_file(key)
    if f is None:
        return {"unread": f"no file under common/decisions defines {key}" if _SE.game_dir() else
                "the game's files were not found (CK3_GAME_DIR)"}
    try:
        body = next((v for k, v in _FL._parsed(f) if k == key and isinstance(v, list)), None)
    except OSError as e:
        return {"unread": f"{f.name} could not be read ({type(e).__name__})"}
    rel = f"common/decisions/{f.name}"
    cost = next((v for k, v in body or [] if k == "cost" and isinstance(v, list)), None)
    fired = fired_events(body or [])
    if not cost:
        if fired:
            # (1086.02.08, 1087.11.05: isolate_family_decision priced "free (no cost
            # block)"; its effect fires epidemic_events.0100, whose only option is -350 prestige)
            return dict(price_fired(fired, monthly_income), basis=f"{key} has no cost block; its effect fires events ({rel})")
        return {"free": True, "basis": f"{key} has no cost block and its effect fires no event ({rel})"}
    out: dict = {}
    if fired:
        out["fires"] = price_fired(fired, monthly_income)["fires"]
    # a file's own constants (`@name = 250` at its top: a test run's sale_of_titles_decision)
    consts = dict(re.findall(r"^(@[a-z0-9_]+)\s*=\s*(-?\d+(?:\.\d+)?)", f.read_text(encoding="utf-8-sig",
                                                                               errors="replace"), re.M))
    for cur, v in cost:
        if isinstance(v, str) and v in consts:
            v = consts[v]
        if cur not in DECISION_CURRENCIES and cur not in ("treasury", "treasury_or_gold", "influence", "herd"):
            continue
        amount, how = _plain(v)
        out[cur] = -round(amount, 2) if amount is not None else f"unknown: {how}"
    if key == CONVERT_DECISION and isinstance(out.get("prestige"), str):
        out["prestige"] = (f"unknown: -{CONVERT_BASE} x multipliers by your culture and your capital's "
                           "(the capital's culture was not read)")
    out["basis"] = f"the cost block of {key} ({rel})"
    return out


# ── A title's creation price and where its discount comes from ─────────────────────────────
#: k_ireland read 325 gold under Murchad and 425 under his son Brian. The game's files: the base is
#: 00_defines.txt CREATE_TITLE_GOLD_COST by tier (kingdom 500); the discount is the sum of the creator's
#: title_creation_cost_mult modifiers. Murchad: ethos_courtly (irish culture, -0.15) and ducal_conquest_perk (-0.20,
#: 00_diplomacy_1_foreign_affairs_tree_perks.txt, for a government without its own perks) = 500 x 0.65 = 325;
#: Brian (the final save's perk list has no ducal_conquest_perk): 500 x 0.85 = 425.
CREATION_MULT = "title_creation_cost_mult"
_TITLE_COST_BASE: dict = {}
_MULT_BY_KEY: dict = {}


def title_cost_base() -> Optional[dict]:
    """{tier: gold} from 00_defines.txt CREATE_TITLE_GOLD_COST (tiers 0..6), and the floor factor; None unread."""
    if _TITLE_COST_BASE:
        return _TITLE_COST_BASE
    root = _SE.game_dir()
    p = root / "common" / "defines" / "00_defines.txt" if root else None
    if p is None or not p.is_file():
        return None
    text = p.read_text(encoding="utf-8-sig", errors="replace")
    m = re.search(r"CREATE_TITLE_GOLD_COST\s*=\s*\{([^}]*)\}", text)
    f = re.search(r"CREATE_TITLE_MIN_COST_FACTOR\s*=\s*([\d.]+)", text)
    if not m:
        return None
    vals = [float(x) for x in m.group(1).split()]
    _TITLE_COST_BASE.update({"by_tier": dict(enumerate(vals)), "min_factor": float(f.group(1)) if f else None})
    return _TITLE_COST_BASE


_OR_TIER: dict = {}
_OR_TIER_KEYS = {3: "DUCHY", 4: "KINGDOM", 5: "EMPIRE", 6: "HEGEMONY"}


def title_or_tier() -> Optional[dict]:
    """{target tier: [titles of each tier 0..6 that suffice, -1 none]} from 00_defines.txt
    CREATE_TITLE_OR_TIER_<TIER> (1.18.3.1: kingdom {-1 -1 -1 2 1 1 1}, empire {-1 -1 -1 -1 2 1 1}); None unread.
    The game's rule for creating a title of that tier: hold that many titles of any one tier."""
    if _OR_TIER:
        return _OR_TIER
    root = _SE.game_dir()
    p = root / "common" / "defines" / "00_defines.txt" if root else None
    if p is None or not p.is_file():
        return None
    text = p.read_text(encoding="utf-8-sig", errors="replace")
    for tier, word in _OR_TIER_KEYS.items():
        m = re.search(rf"CREATE_TITLE_OR_TIER_{word}\s*=\s*\{{([^}}]*)\}}", text)
        if m:
            _OR_TIER[tier] = [int(x) for x in m.group(1).split()]
    return _OR_TIER or None


def _mults_in(sub: str, field: str = CREATION_MULT) -> dict:
    """{block key: its `field` (title_creation_cost_mult by default)} over the top-level blocks of every file in
    common/<sub>."""
    root = _SE.game_dir()
    ck = sub if field == CREATION_MULT else (sub, field, str(root))
    if ck in _MULT_BY_KEY:
        return _MULT_BY_KEY[ck]
    out: dict = {}
    d = root / "common" / sub if root else None
    if d is not None and d.is_dir():
        for f in sorted(d.glob("*.txt")):
            if field not in f.read_text(encoding="utf-8-sig", errors="replace"):
                continue
            for k, v in _FL._parsed(f):
                if not isinstance(v, list):
                    continue
                got = [x for kk, x in _FL._walk(v) if kk == field]
                nums = [float(x) for x in got if isinstance(x, str) and re.fullmatch(r"-?\d+(\.\d+)?", x)]
                if nums:
                    out[k] = sum(nums)
    _MULT_BY_KEY[ck] = out
    return out


#: The modifier that scales a mercenary company's hire price for its hirer (00_basic_modifiers.txt: the rank's)
MERC_HIRE_MULT = "mercenary_hire_cost_mult"


def personal_merc_mults(perks: Any, traits: Any) -> list:
    """The hirer's own mercenary price modifiers: [{source, key, mult}] for his lifestyle perks and traits with a
    mercenary_hire_cost_mult in the game's files (soldiers_of_lesser_fortune_perk -0.3 for a landed ruler)."""
    out = []
    pm, tm = _mults_in("lifestyle_perks", MERC_HIRE_MULT), _mults_in("traits", MERC_HIRE_MULT)
    for k in perks if isinstance(perks, list) else []:
        if k in pm:
            out.append({"source": "perk", "key": k, "mult": pm[k]})
    for k in traits if isinstance(traits, list) else []:
        if k in tm:
            out.append({"source": "trait", "key": k, "mult": tm[k]})
    return out


def personal_creation_mults(perks: Any, traits: Any) -> list:
    """The creator's own discounts: [{source, key, mult}] for his lifestyle perks and traits with a
    title_creation_cost_mult in the game's files (they pass to nobody at his death)."""
    out = []
    pm, tm = _mults_in("lifestyle_perks"), _mults_in("traits")
    for k in perks if isinstance(perks, list) else []:
        if k in pm:
            out.append({"source": "perk", "key": k, "mult": pm[k]})
    for k in traits if isinstance(traits, list) else []:
        if k in tm:
            out.append({"source": "trait", "key": k, "mult": tm[k]})
    return out


def culture_creation_mult(culture: Any) -> Optional[dict]:
    """The creator's culture's ethos discount {source, key, mult} (common/culture/pillars), None when the culture
    or its ethos has none or is not in the files."""
    p = culture_pillars(culture)
    eth = (p or {}).get("ethos")
    m = _mults_in("culture/pillars").get(eth) if eth else None
    return {"source": "culture ethos", "key": f"{eth} ({culture})", "mult": m} if m else None


# ── The prestige price of raising crown authority ─────────────────────────────────────────────
#: a pre-flight run: with no veto the AI passed crown_authority_1 for 143 prestige at Munster 1066 and for 792 at West
#: Francia 867 (prestige 906 -> -375). The game's files (1.18.3.1): each crown_authority_N's pass_cost is
#: common/script_values/00_law_values.txt increase_crown_authority_prestige_cost: increase_crown_authority_prestige_base
#: (100) plus subrealm_size_modifier (10 a county of the ruler's realm, vassals' too, at most 900), that sum times
#: (1 + (8 - learning) / 50, at least 0.5). Both live prices: (100 + 30) x 1.10 = 143 (3 counties, learning 3) and
#: (100 + 890) x 0.80 = 792 (89 counties, learning 18). test_empire_land_rule checks the numbers against the install's file.
CROWN_AUTHORITY_BASE = 100
CROWN_AUTHORITY_PER_COUNTY = 10
CROWN_AUTHORITY_REALM_MAX = 900
LAW_LEARNING_PIVOT = 8
LAW_LEARNING_DIV = 50
LAW_LEARNING_FLOOR = -0.5
CROWN_AUTHORITY_NOT_READ = ("culture parameter cheaper_to_change_laws (halves the realm part)",
                            "faith doctrines legalism, natural primitivism (x1.5), legal pronouncements (x0.9)",
                            "a vassal's forced-partition contract (x1.25)", "breaking the 20-year cooldown",
                            "character modifiers (x0.9 / x1.1)")


def crown_authority_price(realm_counties: Any, learning: Any) -> Optional[dict]:
    """The prestige a crown-authority raise costs this ruler by the game's rule (above), with what is not read; None
    when the realm's counties or his learning is not a number."""
    ok = all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in (realm_counties, learning))
    if not ok:
        return None
    realm = min(CROWN_AUTHORITY_PER_COUNTY * int(realm_counties), CROWN_AUTHORITY_REALM_MAX)
    mod = max((LAW_LEARNING_PIVOT - learning) / LAW_LEARNING_DIV, LAW_LEARNING_FLOOR)
    return {"prestige": round((CROWN_AUTHORITY_BASE + realm) * (1 + mod), 2),
            "basis": (f"(100 + 10 x {int(realm_counties)} realm counties, at most 900) x (1 + (8 - learning "
                      f"{learning}) / 50): common/script_values/00_law_values.txt"),
            "not_read": list(CROWN_AUTHORITY_NOT_READ)}


# ── What a block of script does: the effects a reply can name ────────────────────────────────────────
#: The effects read and what each moves. A value is a number, a named script value, or a block
#: {value = X multiply = -1}; anything else is "a value computed in play".
EFFECT_CURRENCY = {"add_gold": "gold", "add_treasury_or_gold": "gold", "remove_treasury_or_gold": "gold",
                   "remove_short_term_gold": "gold", "remove_long_term_gold": "gold",
                   "pay_short_term_gold": "gold", "pay_long_term_gold": "gold", "add_prestige": "prestige",
                   "add_piety": "piety", "add_legitimacy": "legitimacy", "change_county_control": "county control",
                   "add_prestige_experience": "prestige experience", "add_piety_experience": "piety experience",
                   "add_prestige_level": "prestige level", "add_piety_level": "piety level",
                   "add_dynasty_prestige": "renown", "add_stress": "stress", "change_development_progress":
                   "development progress", "add_dread": "dread", "add_tyranny": "tyranny"}
#: The effects that take gold from the scope they run in (their value is the gold taken, a positive number)
GOLD_TAKEN = frozenset({"remove_short_term_gold", "remove_long_term_gold", "pay_short_term_gold",
                        "pay_long_term_gold", "remove_treasury_or_gold"})
#: The effects that add a modifier for a time ({modifier, years | months | days})
MODIFIER_EFFECTS = frozenset({"add_county_modifier", "add_character_modifier", "add_realm_modifier",
                              "add_dynasty_modifier", "add_house_modifier", "add_province_modifier"})
#: The effects that move titles or trait, named as they stand
NAMED_EFFECTS = frozenset({"add_trait", "remove_trait", "destroy_title", "change_title_holder",
                           "change_title_holder_include_vassals", "create_title_and_vassal_change",
                           "resolve_title_and_vassal_change", "becomes_independent", "change_liege",
                           "remove_claim", "add_unpressed_claim", "add_pressed_claim", "add_realm_law",
                           "add_realm_law_skip_effects", "imprison", "release_from_prison", "death",
                           "set_character_faith", "set_character_faith_with_conversion", "excommunicate_character",
                           "faction_start_war", "destroy_faction", "start_war", "end_war", "use_hook", "add_hook",
                           "stress_impact", "add_opinion", "reverse_add_opinion"})
#: Blocks that run their effects where they stand (not a scope change); `show_as_tooltip` only shows them
EFFECT_SAME_SCOPE = frozenset({"hidden_effect", "custom_tooltip", "custom_description", "custom_label"})
#: Blocks read as conditions or text, never as effects
EFFECT_SKIP = frozenset({"limit", "trigger", "ai_chance", "name", "show_as_tooltip", "alternative_limit",
                         "desc", "text", "flavor", "is_valid", "is_shown", "weight", "modifier", "set_variable",
                         "change_variable", "remove_variable", "clamp_variable", "add_to_list", "remove_from_list",
                         "add_to_variable_list", "remove_list_variable", "add_character_flag", "remove_character_flag",
                         "debug_log", "debug_log_scopes", "play_music_cue", "send_interface_toast"})
#: Blocks whose effects run only when a condition holds (or by chance)
EFFECT_COND = frozenset({"if", "else_if", "else", "random", "random_list", "switch", "trigger_if",
                         "trigger_else_if", "trigger_else", "while"})
#: The scope words that are the block's own subject
SELF_SCOPES = frozenset({"this", "root"})
EFFECTS_MAX_ROWS = 24
EFFECTS_MAX_DEPTH = 6
_MODIFIERS: dict = {}


def modifier_does(name: Any) -> Optional[dict]:
    """{field: value} of a modifier's block in common/modifiers (its numbers as they stand), None unread."""
    if not isinstance(name, str) or not name:
        return None
    if not _MODIFIERS:
        root = _SE.game_dir()
        d = (root / "common" / "modifiers") if root else None
        if d is None or not d.is_dir():
            return None
        try:
            for f in sorted(d.rglob("*.txt")):
                for k, v in _FL._parsed(f):
                    if isinstance(k, str) and isinstance(v, list):
                        _MODIFIERS.setdefault(k, v)
        except OSError:
            _MODIFIERS.clear()
            return None
        _MODIFIERS.setdefault("__read__", [])
    body = _MODIFIERS.get(name)
    if body is None:
        return None
    return {k: v for k, v in body if isinstance(v, str) and k not in ("icon", "stacking", "scale")}


def effect_value(v: Any) -> tuple:
    """(number or None, the value as written) of an effect's value: a number, a named script value (the prestige
    and piety scales, then common/script_values), or {value = X} with multiply / subtract / add of numbers. A value
    the files give as an expression of names (`{ value = scope:cost multiply = -1 }`) is written out as it stands."""
    if isinstance(v, str):
        if re.fullmatch(r"-?\d+(?:\.\d+)?", v):
            return float(v), v
        a = basic_values().get(v)
        if a is None:
            a = script_value(v)
        return (float(a) if a is not None else None), v
    if isinstance(v, list):
        base = next((x for k, x in v if k == "value"), None)
        if base == "scope:cb_prestige_factor":
            # The engine's own factor for the war (no file gives it): its multiple, in words
            m = next((x for k, x in v if k == "multiply" and isinstance(x, str)), None)
            mn = effect_value(m)[0] if m is not None else 1.0
            return None, (f"{mn:g} x the war's prestige factor" if mn is not None else "the war's prestige factor")
        n, text = effect_value(base) if base is not None else (None, "a computed value")
        branch = False
        words = [str(base)] if isinstance(base, str) else []
        for k, x in v:
            if k in ("value", "desc", "max", "min"):
                continue
            if k in ("if", "else_if", "else"):
                branch = True                 # a step taken only when a condition holds: the plain value stands
                continue
            if isinstance(x, str) and k in _VALUE_OPS:
                words.append(f"{_VALUE_OPS[k]} {x}")
            else:
                words = []                    # a block inside: not written out
            if n is None:
                continue
            m, _t = effect_value(x) if isinstance(x, str) else (None, "")
            if m is None:
                n = None
                continue
            if k == "multiply":
                n *= m
            elif k == "subtract":
                n -= m
            elif k == "add":
                n += m
            elif k == "divide" and m:
                n /= m
            else:
                n = None
        if n is not None and branch:
            return n, f"{text} (more or less where a condition in its value holds)"
        if n is not None:
            return n, text
        if words and isinstance(base, str):
            return None, " ".join(words) + (" (more or less where a condition in its value holds)" if branch else "")
        return None, "a value computed in play"
    return None, "a value computed in play"


#: The steps of a value block, as its expression is written out
_VALUE_OPS = {"multiply": "x", "add": "+", "subtract": "-", "divide": "/"}


def _subst(node: Any, params: dict) -> Any:
    """A scripted effect's body with its $PARAMETERS$ given."""
    if not params:
        return node
    if isinstance(node, str):
        for p, x in params.items():
            node = node.replace(f"${p}$", x)
        return node
    if isinstance(node, list):
        return [(_subst(k, params), _subst(v, params)) for k, v in node]
    return node


#: The most characters a branch's condition is written in (cut at a word, never inside one)
COND_WORDS_MAX = 240


def _cond_words(lim: list) -> str:
    """A condition block as script text, its comparison signs whole (the tokenizer splits `!=`), each condition of
    the block (all of which must hold) joined by AND; cut at a word, with "...", past COND_WORDS_MAX."""
    groups: list = []
    for k, v in lim:
        # the tokenizer reads `a != b` as ('', a), ('!', b): the sign's pair joins the one before it
        if k in ("!", "?", ">", "<") and groups:
            groups[-1].append((k, v))
        else:
            groups.append([(k, v)])
    parts = []
    for g in groups:
        t = _FL._flat(g)
        for a, b in (("! =", "!="), ("? =", "?="), ("> =", ">="), ("< =", "<=")):
            t = t.replace(a, b)
        parts.append(t)
    t = " AND ".join(parts)
    if len(t) <= COND_WORDS_MAX:
        return t
    cut = t[:COND_WORDS_MAX].rsplit(" ", 1)[0]
    return cut + " ..."


#: The effects that set a truce, and what they name
TRUCE_EFFECTS = frozenset({"add_truce_both_ways", "add_truce_one_way"})
#: The rows an effects read names when an on_action lists nothing the files hold (the engine handles it)
ON_ACTION_ENGINE = "lists no event or effect in common/on_action: the engine's own handling of it"
_CHAIN_IDS = [0]


def _new_chain() -> int:
    _CHAIN_IDS[0] += 1
    return _CHAIN_IDS[0]


def read_effects(node: list, where: str = "you", conditional: bool = False, ran: Optional[set] = None,
                 _out: Optional[dict] = None, _depth: int = 0, _via: Optional[str] = None,
                 _cond_text: Optional[str] = None, effects: Optional[dict] = None, _path: tuple = (),
                 _tooltip: bool = False) -> dict:
    """What a block of effects does, read from the script itself: {"rows": [{effect, where, amount | value,
    conditional, when?, via?, modifier?, for?, modifier_does?}], "not_read": [effects it could not read]}.
    `where` is the subject the block runs on ("you" for an event's option); a scope change (scope:x = {...},
    every_x = {...}) is named in the rows it holds. The scripted effects a block calls (`name = yes`, or with
    parameters) are read as part of it. `ran` holds the ids of the conditional blocks known to have run (a letter
    the block sent was received), whose rows are therefore not conditional. Each conditional row carries `_branch`:
    the chains of if / else_if / else (one of them runs) and other conditional blocks it sits in, as (chain, branch)
    pairs, and `_chains` the chains that end in an `else` (branch_totals reads them). An event or an on_action the
    block fires is a row (trigger_event); what the option's tooltip shows done another way (a faction's war, a title
    changed) is a row with `shown_in_tooltip`."""
    out = _out if _out is not None else {"rows": [], "not_read": [], "_chains": set()}
    out.setdefault("_chains", set())
    eff = effects if effects is not None else (_scripted_effects() or {})
    prev_limit: Optional[str] = None
    chain: Optional[list] = None                 # [chain id, branch index] of the if / else_if chain being read

    def add(row: dict) -> None:
        if conditional and _cond_text:
            row["when"] = _cond_text
        if _via:
            row["via"] = _via
        if _path:
            row["_branch"] = _path
        if _tooltip:
            row["shown_in_tooltip"] = True
        out["rows"].append(row)

    for k, v in node:
        if len(out["rows"]) >= EFFECTS_MAX_ROWS:
            break
        if not isinstance(k, str) or k in EFFECT_SKIP and k != "show_as_tooltip":
            if k not in ("else_if", "else"):
                chain = None if k not in ("limit", "trigger") else chain
            continue
        if k not in ("else_if", "else"):
            chain_here = chain
            chain = None
        else:
            chain_here = chain
        if k in EFFECT_CURRENCY and not _tooltip:
            n, text = effect_value(v if not isinstance(v, list) or k not in GOLD_TAKEN
                                   else next((x for kk, x in v if kk == "gold"), v))
            if k in GOLD_TAKEN and n is not None:
                n = -abs(n)
            row = {"effect": k, "moves": EFFECT_CURRENCY[k], "where": where,
                   **({"amount": round(n, 2)} if n is not None else {}),
                   **({"value": text} if n is None or not re.fullmatch(r"-?\d+(?:\.\d+)?", str(text)) else {}),
                   "conditional": conditional}
            if k in GOLD_TAKEN and isinstance(v, list):
                tgt = next((x for kk, x in v if kk == "target" and isinstance(x, str)), None)
                if tgt:
                    row["paid_to"] = tgt
            add(row)
        elif k in MODIFIER_EFFECTS and not _tooltip:
            name = v if isinstance(v, str) else next((x for kk, x in v if kk == "modifier"), None) \
                if isinstance(v, list) else None
            span = next((f"{x} {kk}" for kk, x in (v if isinstance(v, list) else [])
                         if kk in ("years", "months", "days") and isinstance(x, str)), None)
            row = {"effect": k, "where": where, "modifier": name, "for": span or "until removed",
                   "conditional": conditional}
            does = modifier_does(name)
            if does is not None:
                row["modifier_does"] = does
            elif isinstance(name, str):
                row["modifier_does"] = "unread: not found in common/modifiers"
            add(row)
        elif k in TRUCE_EFFECTS:
            blk = {kk: x for kk, x in v if isinstance(kk, str) and isinstance(x, str)} if isinstance(v, list) else {}
            span = next((f"{blk[u]} {u}" for u in ("years", "months", "days") if u in blk), None)
            add({"effect": k, "where": where, "conditional": conditional,
                 **({"with": blk["character"]} if "character" in blk else {}),
                 "for": span or "unread: no years, months or days in the block"})
        elif k in NAMED_EFFECTS:
            add({"effect": k, "where": where, "conditional": conditional,
                 **({"value": v} if isinstance(v, str) else {})})
        elif k == "show_as_tooltip" and isinstance(v, list):
            # The tooltip names what the game does another way (its on_action, the engine): named, never priced
            read_effects(v, where, conditional, ran, out, _depth, _via, _cond_text, eff, _path, True)
        elif k in EFFECT_SAME_SCOPE and isinstance(v, list):
            read_effects(v, where, conditional, ran, out, _depth, _via, _cond_text, eff, _path, _tooltip)
        elif k in EFFECT_COND and isinstance(v, list):
            lim = next((x for kk, x in v if kk == "limit" and isinstance(x, list)), None)
            if k == "else":
                text = f"otherwise (not: {prev_limit})" if prev_limit else "otherwise"
            else:
                text = _cond_words(lim) if lim is not None else k
            sure = ran is not None and id(v) in ran
            if k in ("else_if", "else") and chain_here is not None:
                chain_here[1] += 1
                link = chain_here
            else:
                link = [_new_chain(), 0]
            if k == "else":
                out["_chains"].add(link[0])
            read_effects(v, where, conditional or not sure, ran, out, _depth, _via,
                         _cond_text if sure else text, eff, _path if sure else _path + ((link[0], link[1]),),
                         _tooltip)
            prev_limit = text if k in ("if", "else_if") else None
            chain = link if k in ("if", "else_if") else None
            continue
        elif k == "trigger_event":
            ev = v if isinstance(v, str) else next((x for kk, x in v if kk == "id" and isinstance(x, str)), None) \
                if isinstance(v, list) else None
            oa = next((x for kk, x in v if kk == "on_action" and isinstance(x, str)), None) \
                if isinstance(v, list) else None
            if ev or oa:
                row = {"effect": "trigger_event", "where": where, "conditional": conditional,
                       "value": ev if ev else f"on_action {oa}"}
                if oa and not ev:
                    listed = on_action_events(oa)
                    if listed == [] and not _on_action_effect(oa):
                        row["on_action"] = f"{oa} {ON_ACTION_ENGINE}"
                    elif listed:
                        row["on_action_events"] = listed[:6]
                add(row)
        elif k == "send_interface_message" or k.startswith("save_"):
            if k == "send_interface_message" and isinstance(v, list):
                read_effects(v, where, conditional, ran, out, _depth, _via, _cond_text, eff, _path, _tooltip)
        elif k in eff and _depth < EFFECTS_MAX_DEPTH and (v == "yes" or isinstance(v, list)):
            params = {kk: x for kk, x in v if isinstance(kk, str) and isinstance(x, str)} \
                if isinstance(v, list) else {}
            read_effects(_subst(eff[k], params), where, conditional, ran, out, _depth + 1, _via or k, _cond_text,
                         eff, _path, _tooltip)
        elif isinstance(v, list):
            # A scope change: the block's effects run on another subject
            sub = (where if k in SELF_SCOPES else k if where == "you" or k.startswith("scope:")
                   else f"{where} > {k}")
            read_effects(v, sub, conditional, ran, out, _depth, _via, _cond_text, eff, _path, _tooltip)
        elif isinstance(v, str) and v == "yes" and _depth < EFFECTS_MAX_DEPTH and len(out["not_read"]) < 12 \
                and k not in out["not_read"] and not _tooltip:
            out["not_read"].append(k)
        prev_limit = None
    return out


def _on_action_effect(name: str) -> bool:
    """An on_action of common/on_action has an `effect` block (it does something itself)."""
    on_action_events(name)
    return any(k == "effect" for b in _ON_ACTIONS.get(name) or [] for k, _v in b)


#: The most totals a set of branches is listed by; past it only the range is given
BRANCH_TOTALS_MAX = 16


def branch_totals(rows: list, chains: Any) -> Optional[list]:
    """The totals the conditional rows can come to, read as the script runs them: of an if / else_if / else chain
    one branch runs (none, when it has no `else`), and separate conditional blocks may each run or not. A sorted
    list of the possible sums of the rows' `amount`s (0 among them when nothing may run); None past
    BRANCH_TOTALS_MAX (branch_range gives the range)."""
    ends = set(chains or ())
    nums = [r for r in rows if isinstance(r.get("amount"), (int, float))]

    def at(rs: list, depth: int) -> Optional[set]:
        plain = round(sum(r["amount"] for r in rs if len(r.get("_branch") or ()) == depth), 2)
        groups: dict = {}
        for r in rs:
            p = r.get("_branch") or ()
            if len(p) > depth:
                groups.setdefault(p[depth][0], {}).setdefault(p[depth][1], []).append(r)
        acc = {plain}
        for cid, branches in groups.items():
            opts: set = set() if cid in ends else {0.0}
            for brs in branches.values():
                got = at(brs, depth + 1)
                if got is None:
                    return None
                opts |= got
            acc = {round(a + b, 2) for a in acc for b in opts}
            if len(acc) > BRANCH_TOTALS_MAX:
                return None
        return acc
    got = at(nums, 0)
    return sorted(got) if got is not None else None


def branch_range(rows: list, chains: Any) -> tuple:
    """(least, most) of the totals branch_totals reads, also past its cap (each chain's branch bounds summed)."""
    ends = set(chains or ())
    nums = [r for r in rows if isinstance(r.get("amount"), (int, float))]

    def at(rs: list, depth: int) -> tuple:
        lo = hi = round(sum(r["amount"] for r in rs if len(r.get("_branch") or ()) == depth), 2)
        groups: dict = {}
        for r in rs:
            p = r.get("_branch") or ()
            if len(p) > depth:
                groups.setdefault(p[depth][0], {}).setdefault(p[depth][1], []).append(r)
        for cid, branches in groups.items():
            got = [at(brs, depth + 1) for brs in branches.values()]
            los = [g[0] for g in got] + ([] if cid in ends else [0.0])
            his = [g[1] for g in got] + ([] if cid in ends else [0.0])
            lo, hi = round(lo + min(los), 2), round(hi + max(his), 2)
        return lo, hi
    return at(nums, 0)


def conditional_price(rows: list, chains: Any) -> dict:
    """The conditional rows' price for a reply: {"if_its_conditions_held": the largest charge (or grant) the branches
    can come to} and, when the rows are exclusive branches whose plain sum no run can reach, {"one_of": the possible
    totals} (or {"range": [least, most]} past BRANCH_TOTALS_MAX)."""
    nums = [r for r in rows if isinstance(r.get("amount"), (int, float))]
    if not nums:
        return {}
    naive = round(sum(r["amount"] for r in nums), 2)
    tot = branch_totals(nums, chains)
    if tot is not None and naive in tot:
        return {"if_its_conditions_held": naive}
    lo, hi = branch_range(nums, chains)
    big = lo if abs(lo) >= abs(hi) else hi
    out: dict = {"if_its_conditions_held": big}
    if tot is not None:
        out["one_of"] = tot
    else:
        out["range"] = [lo, hi]
    return out


def option_effects_read(event: Any, option: Any) -> Optional[dict]:
    """What `option` of `event` does to the ruler who answers it and to whom else, from the event's file and the
    scripted effects it calls (read_effects); None when the files do not say (the event or the option not found)."""
    if not isinstance(event, str) or not isinstance(option, (str, int)) or isinstance(option, bool):
        return None
    found, where = _SE.event_in_files(event)
    root = _SE.game_dir()
    if not found or root is None:
        return None
    try:
        tree = _FL._parsed(root / where)
    except OSError:
        return None
    body = next((v for k, v in tree if k == event and isinstance(v, list)), None)
    opts = [v for k, v in body or [] if k == "option" and isinstance(v, list)]
    opt = (opts[option] if isinstance(option, int) and 0 <= option < len(opts) else
           next((o for o in opts if option in (option_key(o), _FL._name_key(o))), None)
           if not isinstance(option, int) else None)
    if opt is None:
        return None
    # the scripted effects the event's own file defines (`scripted_effect name = {...}` at its top level)
    local = {k: v for k, v in tree if isinstance(k, str) and isinstance(v, list) and k and "." not in k}
    eff = _scripted_effects() or {}
    got = read_effects(opt, effects={**eff, **local} if local else eff)
    return {"file": where, **got}


# ── The letter an interaction sends: which interaction ran, and what it charged ─────────────────────────────
#: The blocks of an interaction that send a letter when they run
INTERACTION_BLOCKS = ("on_accept", "on_decline", "on_send", "on_auto_accept")
_LETTERS: dict = {}
#: game folder -> {event: [the files outside common/character_interactions whose script fires it]}
_FIRED_ELSEWHERE: dict = {}


def _path_to(node: list, event: str, path: list, effects: Optional[dict] = None, depth: int = 0) -> Optional[list]:
    """The blocks from `node` down to the one holding `trigger_event = event` (or `= { id = event }`), with the
    key each was entered by, through the scripted effects the block calls (`name = yes`); None when the block does
    not send it."""
    for k, v in node:
        if k == "trigger_event":
            ev = v if isinstance(v, str) else next((x for kk, x in v if kk == "id" and isinstance(x, str)), None) \
                if isinstance(v, list) else None
            if ev == event:
                return path
        elif isinstance(v, list) and k not in EFFECT_SKIP:
            got = _path_to(v, event, path + [(k, v)], effects, depth)
            if got is not None:
                return got
        elif v == "yes" and effects and k in effects and depth < EFFECTS_MAX_DEPTH:
            got = _path_to(effects[k], event, path + [(k, effects[k])], effects, depth + 1)
            if got is not None:
                return got
    return None


def _fired_elsewhere(event: str) -> list:
    """The files outside common/character_interactions whose script fires `event` (`trigger_event = event`, or
    `= { id = event }`), each relative to the game folder; the scripted effects an interaction calls to send it
    are among them (letter_senders sets those apart)."""
    root = _SE.game_dir()
    if root is None:
        return []
    idx = _FIRED_ELSEWHERE.get(str(root))
    if idx is None:
        idx = {}
        pat = re.compile(r"trigger_event\s*=\s*(?:\{[^{}]*?\bid\s*=\s*)?([A-Za-z0-9_]+\.[0-9]+)\b")
        for sub in ("events", "common"):
            d = root / sub
            if not d.is_dir():
                continue
            for f in sorted(d.rglob("*.txt")):
                rel = str(f.relative_to(root))
                if rel.startswith("common/character_interactions"):
                    continue
                try:
                    text = f.read_text(encoding="utf-8-sig", errors="replace")
                except OSError:
                    continue
                for m in pat.finditer(text):
                    idx.setdefault(m.group(1), [])
                    if rel not in idx[m.group(1)]:
                        idx[m.group(1)].append(rel)
        _FIRED_ELSEWHERE[str(root)] = idx
    return list(idx.get(event) or [])


def letter_senders(event: Any) -> Optional[dict]:
    """Every interaction block that sends `event` ({interaction, block, to, file, _body, _ran, _via}), directly or
    through the scripted effects it calls, and the other places the game's script fires it from
    ({"senders": [...], "elsewhere": [files]}); None when the files are not readable."""
    if not isinstance(event, str) or not event:
        return None
    root = _SE.game_dir()
    d = (root / "common" / "character_interactions") if root else None
    if d is None or not d.is_dir():
        return None
    eff = _scripted_effects() or {}
    firing = _effects_firing(event, eff)
    senders: list = []
    via: set = set()
    try:
        for f in sorted(d.glob("*.txt")):
            text = f.read_text(encoding="utf-8-sig", errors="replace")
            if event not in text and not any(n in text for n in firing):
                continue
            for name, body in _FL._parsed(f):
                if not isinstance(body, list):
                    continue
                for blk, v in body:
                    if blk not in INTERACTION_BLOCKS or not isinstance(v, list):
                        continue
                    path = _path_to(v, event, [], eff)
                    if path is None:
                        continue
                    scopes = [k for k, _b in path if k.startswith("scope:")]
                    to = scopes[-1] if scopes else None
                    if to is None:
                        continue
                    through = [k for k, b in path if k in eff and b is eff.get(k)]
                    via.update(through)
                    senders.append({"interaction": name, "block": blk, "to": to, "file": str(f.relative_to(root)),
                                    "_body": v, "_ran": {id(b) for _k, b in path},
                                    **({"_via": through[0]} if through else {})})
    except OSError:
        return None
    elsewhere = _fired_elsewhere(event)
    if via:
        # the scripted effect an interaction sends it through is that interaction's, not another place
        homes = set()
        for f in sorted((root / "common" / "scripted_effects").glob("*.txt")):
            if any(k in via for k, _v in _FL._parsed(f)):
                homes.add(str(f.relative_to(root)))
        elsewhere = [e for e in elsewhere if e not in homes]
    return {"senders": senders, "elsewhere": elsewhere}


#: game folder -> {scripted effect: its body as text}, for the effects that may fire a letter
_EFFECT_TEXT: dict = {}


def _effects_firing(event: str, eff: dict) -> set:
    """The scripted effects whose body fires `event`, and those that call one of them (to EFFECTS_MAX_DEPTH)."""
    root = _SE.game_dir()
    texts = _EFFECT_TEXT.get(str(root))
    if texts is None:
        texts = _EFFECT_TEXT[str(root)] = {n: _FL._flat(b) for n, b in eff.items()}
    found = {n for n, t in texts.items() if event in t}
    for _ in range(EFFECTS_MAX_DEPTH):
        more = {n for n, t in texts.items() if n not in found and any(x in t for x in found)}
        if not more:
            break
        found |= more
    return found


def interaction_letter(event: Any) -> Optional[dict]:
    """The interaction whose `on_accept` / `on_decline` (INTERACTION_BLOCKS) sends `event` to a character in it,
    and what that block does to him, from common/character_interactions and the scripted effects it calls:
    {interaction, block, to (the scope the letter goes to), file, rows (read_effects rows for that character; a
    branch the letter's own branch holds counts as run), not_read}; when more than one interaction block sends it,
    or the game's script fires it from other places too (an illness event, an effect), `one_of` names them all and
    `elsewhere` the other files: the letter then does not show which ran. None when no interaction sends it or the
    files are not readable. The receipt of the letter is the record that the interaction ran (when only one sends
    it): the letter goes out in the same block as its charges."""
    if not isinstance(event, str) or not event:
        return None
    if event in _LETTERS:
        return _LETTERS[event]
    got_s = letter_senders(event)
    if got_s is None:
        return None
    if not got_s["senders"]:
        _LETTERS[event] = None
        return None
    rows_by: list = []
    for snd in got_s["senders"]:
        got = read_effects(snd["_body"], where="the interaction", ran=snd["_ran"])
        rows_by.append({"interaction": snd["interaction"], "block": snd["block"], "to": snd["to"],
                        "file": snd["file"], **({"via": snd["_via"]} if snd.get("_via") else {}),
                        "rows": [r for r in got["rows"] if r.get("where") == snd["to"]],
                        "_chains": got["_chains"], "not_read": got["not_read"]})
    found = dict(rows_by[0])
    if len(rows_by) > 1 or got_s["elsewhere"]:
        found["one_of"] = [f"{x['interaction']} {x['block']}" for x in rows_by]
        if got_s["elsewhere"]:
            found["elsewhere"] = got_s["elsewhere"][:6]
            found["elsewhere_count"] = len(got_s["elsewhere"])
        found["senders"] = rows_by
    _LETTERS[event] = found
    return found


def _charge_of(li: dict, currency: str, event: str) -> Optional[dict]:
    rows = [r for r in li["rows"] if r.get("moves") == currency]
    if not rows:
        return None
    sure = [r["amount"] for r in rows if not r["conditional"] and isinstance(r.get("amount"), (int, float))]
    cond = [r for r in rows if r["conditional"] and isinstance(r.get("amount"), (int, float))]
    out = {"interaction": li["interaction"], "block": li["block"], "file": li["file"],
           "basis": (f"{li['interaction']} {li['block']} ({li['file']}): "
                     + ", ".join(f"{r['effect']} = " + (f"{r['value']} ({r['amount']:g})" if "value" in r and "amount"
                                                         in r else str(r.get("amount", r.get("value"))))
                                 + (f" (in {r['via']})" if r.get("via") else "")
                                 + (" if its condition held" if r["conditional"] else "") for r in rows)
                     + f"; the same block sends {event}")}
    if sure:
        out["amount"] = round(sum(sure), 2)
    if cond:
        cp = conditional_price(cond, li.get("_chains"))
        out.update(cp)
        if "one_of" in cp or "range" in cp:
            out["basis"] += ("; the conditional ones are branches of one if / else_if chain (exactly one of them "
                             "runs): "
                             + ("one of " + ", ".join(f"{x:g}" for x in cp["one_of"]) if "one_of" in cp else
                                f"from {cp['range'][0]:g} to {cp['range'][1]:g}"))
    unread = [r.get("value") for r in rows if "amount" not in r]
    if unread:
        out["unread_values"] = unread
    return out


def letter_charge(event: Any, currency: str, interaction: Optional[str] = None) -> Optional[dict]:
    """{interaction, block, amount (the sum of the sure rows), if_its_conditions_held (the conditional ones: the
    largest charge their branches can come to), one_of (the totals exclusive branches can come to), basis} of
    `currency` the letter's interaction charged the character it was sent to; None when no interaction sends the
    letter or its block moves none of that currency on him. When several places send the letter (`one_of`), the
    charge is that of `interaction` when the caller names the one that ran (an act of the seat's), else no amount
    stands: `one_of` names the senders and `elsewhere` the other files that fire it."""
    li = interaction_letter(event)
    if not li:
        return None
    if li.get("one_of"):
        pick = next((x for x in li.get("senders") or [] if interaction and x["interaction"] == interaction), None)
        if pick is not None:
            return _charge_of(pick, currency, event)
        charges = [c for c in (_charge_of(x, currency, event) for x in li.get("senders") or []) if c]
        if not charges:
            return None
        out = dict(charges[0])
        out.pop("amount", None)
        out.pop("if_its_conditions_held", None)
        out.pop("one_of", None)
        out.pop("range", None)
        out["senders"] = li["one_of"]
        if li.get("elsewhere"):
            out["elsewhere"] = li["elsewhere"]
            out["elsewhere_count"] = li.get("elsewhere_count")
        out["basis"] = (f"{event} is sent by one of: {', '.join(li['one_of'])}"
                        + (f"; the game's script also fires it from {li.get('elsewhere_count')} other files "
                           f"({', '.join(li['elsewhere'][:3])})" if li.get("elsewhere") else "")
                        + ": which one ran is not read, so no charge is counted; " + charges[0]["basis"])
        return out
    return _charge_of(li, currency, event)


# ── A realm law's pass cost ─────────────────────────────────────────────────────────────────────
_LAWS: dict = {}


def law_pass_cost(law: Any) -> Optional[dict]:
    """What passing realm law `law` costs, from its block in common/laws: {"prestige": -n (and other currencies)
    for amounts the file states, "unknown": {currency: why} for a computed one, "basis"}; {"none": true, "basis"}
    when the law has no pass_cost block (it costs nothing to pass: the game also switches some laws itself, by their
    can_keep / should_start_with). None when no file defines it or the files are not readable."""
    if not isinstance(law, str) or not law:
        return None
    root = _SE.game_dir()
    d = (root / "common" / "laws") if root else None
    if d is None or not d.is_dir():
        return None
    idx = _LAWS.get(str(root))
    if idx is None:
        idx = {}
        try:
            for f in sorted(d.glob("*.txt")):
                for gk, gv in _FL._parsed(f):
                    if not isinstance(gv, list):
                        continue
                    for lk, lv in gv:
                        if isinstance(lk, str) and isinstance(lv, list) and lk not in ("default", "flag"):
                            idx.setdefault(lk, (lv, gk, str(f.relative_to(root))))
        except OSError:
            return None
        _LAWS[str(root)] = idx
    got = idx.get(law)
    if got is None:
        return None
    body, group, rel = got
    pc = next((v for k, v in body if k == "pass_cost" and isinstance(v, list)), None)
    if pc is None:
        return {"none": True, "basis": f"{law} ({group}, {rel}) has no pass_cost block: passing it costs nothing"}
    out: dict = {"basis": f"the pass_cost block of {law} ({group}, {rel})"}
    for cur, v in pc:
        if cur not in ("prestige", "piety", "gold", "influence"):
            continue
        blk = v
        if isinstance(v, list) and not any(k == "value" for k, _x in v):
            # `prestige = { add = X }`: its steps from 0
            adds = [x for k, x in v if k == "add" and isinstance(x, str)]
            blk = adds[0] if len(adds) == 1 and len(v) == 1 else v
        n, text = effect_value(blk)
        if n is not None:
            out[cur] = -round(abs(n), 2)
        else:
            out.setdefault("unknown", {})[cur] = text if text != "a value computed in play" else (
                "a value computed in play (" + _FL._flat(v)[:120] + ")")
    return out


# ── A war's end in prestige, by its casus belli's file ───────────────────────────────────────────
#: The block of a casus belli that runs at each ending
CB_END_BLOCK = {"attacker_victory": "on_victory", "defender_victory": "on_defeat", "white_peace": "on_white_peace"}
#: The scripted effects that move the two leaders' prestige or piety by a base and a scale (00_casus_belli_effects.txt):
#: the loser loses base x LOSER_FAME_SCALE (at least -1000); a winning defender gains base x WINNER_FAME_SCALE (at most
#: 1000); a winning attacker gains it as prestige EXPERIENCE only, not prestige to spend
CB_FAME_EFFECTS = ("modify_all_participants_fame_values", "modify_all_participants_fame_values_crusading_claim")
CB_FAME_LIMIT = 1000
#: The base some casus belli take from the engine (setup_claim_cb, setup_de_jure_cb), which no file gives
CB_ENGINE_FACTOR = "scope:cb_prestige_factor"
CB_FACTOR_UNREAD = ("cb_prestige_factor: the engine's setup_claim_cb / setup_de_jure_cb sets it for each war (no file "
                    "gives it, and no read of this bridge does)")
_CB_BODIES: dict = {}


def cb_body(cb: Any) -> Optional[tuple]:
    """(body, file) of a casus belli in common/casus_belli_types; None when not found or not readable."""
    if not isinstance(cb, str) or not cb:
        return None
    if not _CB_BODIES:
        root = _SE.game_dir()
        d = (root / "common" / "casus_belli_types") if root else None
        if d is None or not d.is_dir():
            return None
        try:
            for f in sorted(d.glob("*.txt")):
                for k, v in _FL._parsed(f):
                    if isinstance(k, str) and isinstance(v, list):
                        _CB_BODIES.setdefault(k, (v, str(f.relative_to(root))))
        except OSError:
            _CB_BODIES.clear()
            return None
        _CB_BODIES.setdefault("__read__", ([], ""))
    return _CB_BODIES.get(cb)


def cb_end_prestige(cb: Any, outcome: Any, side: Any) -> Optional[dict]:
    """What a war of casus belli `cb` ending in `outcome` (attacker_victory | defender_victory | white_peace) does to
    the prestige of its leader on `side` (attacker | defender), read from the casus belli's block for that ending:
    {"prestige": the sum when every part is a number, "parts": [...], "rule": the words when a part is the engine's
    factor, "not_read": [...], "file"}. A winning attacker's share of the fame effects is prestige experience and
    counts 0. None when the file or the block is not found."""
    got = cb_body(cb)
    blk_name = CB_END_BLOCK.get(str(outcome or ""))
    if got is None or blk_name is None or side not in ("attacker", "defender"):
        return None
    body, rel = got
    blk = next((v for k, v in body if k == blk_name and isinstance(v, list)), None)
    if blk is None:
        return None
    me = f"scope:{side}"
    parts: list = []
    rule: list = []
    for k, v in blk:
        if k not in CB_FAME_EFFECTS or not isinstance(v, list):
            continue
        p = {kk: x for kk, x in v if isinstance(kk, str) and isinstance(x, str)}
        piety = p.get("IS_RELIGIOUS_WAR") == "yes"
        if p.get("LOSER") == me:
            scale, sign, kind = p.get("LOSER_FAME_SCALE"), -1, "piety" if piety else "prestige"
        elif p.get("WINNER") == me:
            scale, sign = p.get("WINNER_FAME_SCALE"), 1
            kind = (("piety" if piety else "prestige") + " experience") if side == "attacker" else (
                "piety" if piety else "prestige")
        else:
            continue
        sn, st = effect_value(scale)
        part = {"via": k, "moves": kind, "scale": sn if sn is not None else st, "base": p.get("FAME_BASE")}
        if p.get("FAME_BASE") == CB_ENGINE_FACTOR:
            part["amount"] = f"{sn:g} x the war's prestige factor" if sn is not None else "the war's prestige factor"
            if kind == "prestige":
                rule.append(f"{sn:g} x the war's prestige factor, {'at least' if sign < 0 else 'at most'} "
                            f"{sign * CB_FAME_LIMIT}" if sn is not None else "a multiple of the war's prestige factor")
        else:
            bn, _bt = effect_value(p.get("FAME_BASE"))
            if bn is not None and sn is not None:
                n = bn * sn
                part["amount"] = round(max(n, -CB_FAME_LIMIT) if sign < 0 else min(n, CB_FAME_LIMIT), 2)
            else:
                part["amount"] = "a value computed in play"
        parts.append(part)
    rest = [(k, v) for k, v in blk if k not in CB_FAME_EFFECTS and not str(k).startswith("modify_allies")]
    got_rows = read_effects(rest, where="the war")
    for r in got_rows["rows"]:
        if r.get("where") == me and r.get("moves") == "prestige" and "prestige factor" in str(r.get("value")) \
                and not r["conditional"]:
            rule.append(r["value"])
        if r.get("where") == me and r.get("moves") in ("prestige", "piety"):
            parts.append({"via": r.get("via") or r["effect"], "moves": r["moves"],
                          "amount": r.get("amount", r.get("value")), "conditional": r["conditional"],
                          **({"when": r["when"]} if r.get("when") else {})})
    out: dict = {"file": f"{rel} {cb} {blk_name}", "parts": parts}
    sure = [x["amount"] for x in parts if x["moves"] == "prestige" and not x.get("conditional")]
    if sure and all(isinstance(a, (int, float)) for a in sure):
        out["prestige"] = round(sum(sure), 2)
    elif not sure:
        out["prestige"] = 0
    if rule:
        out["rule"] = "; ".join(rule)
        out["not_read"] = [CB_FACTOR_UNREAD]
    return out


#: The rows an option's effects block names in a reply, at most
OPTION_EFFECT_ROWS = 10
OPTION_EFFECTS_NOTE = ("each effect as the option's script and the scripted effects it calls set it: on you, or on "
                       "the scope named (each county of a faction, the faction); `when` is the condition of a branch "
                       "(it applies only when that holds); `not_read` names the effects whose result the files do "
                       "not give (they run in the engine)")


#: What a row read from the option's tooltip means
TOOLTIP_NOTE = ("shown_in_tooltip: the option's tooltip (show_as_tooltip) names this effect and the game does it "
                "another way (an on_action the option fires, the engine): it happens, it is not run by this block")


def option_effects_brief(event: Any, option: Any) -> dict:
    """The effects of `option` of `event` for a reply: {"effects": [compact rows], "not_read": [...], "file"}, or
    {"unread": why} when the files do not say. Fact only: what the option does, read from the files."""
    got = option_effects_read(event, option)
    if got is None:
        found, where = _SE.event_in_files(event) if isinstance(event, str) else (None, "")
        return {"unread": (where or "the game's event files are not readable here") if found is None else
                f"{option} is not an option of {event} in its file" if found else f"no file defines {event}"}
    rows = []
    # what the option does first; the events it fires (a letter to another, its on_action) after
    ordered = ([r for r in got["rows"] if r["effect"] != "trigger_event"]
               + [r for r in got["rows"] if r["effect"] == "trigger_event"])
    for r in ordered[:OPTION_EFFECT_ROWS]:
        x = {"effect": r["effect"], "on": r["where"]}
        for k in ("amount", "value", "with", "modifier", "for", "modifier_does", "paid_to", "on_action",
                  "on_action_events", "shown_in_tooltip", "when"):
            if k in r:
                x[k] = r[k]
        if r.get("conditional") and "when" not in x:
            x["when"] = "inside a branch or a chance"
        rows.append(x)
    out = {"file": got["file"], "effects": rows}
    if any(r.get("shown_in_tooltip") for r in rows):
        out["shown_in_tooltip"] = TOOLTIP_NOTE
    if len(got["rows"]) > OPTION_EFFECT_ROWS:
        out["effects_cut"] = f"{len(got['rows']) - OPTION_EFFECT_ROWS} more effect rows not shown"
    if got["not_read"]:
        out["not_read"] = got["not_read"]
    if not rows and not got["not_read"]:
        out["effects"] = "none: the option changes nothing the files name"
    return out
