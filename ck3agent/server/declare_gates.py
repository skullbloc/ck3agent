"""What the code can check of a war declaration beyond the game's casus-belli read.

The casus-belli read (query_cbs_for_target, the read cbs answers) runs the game's
casus-belli check, which does not ask whether the ruler can PAY for the war: live on the
1066 start (Murchad of Dublin) cbs and plan_war offered claim_cb on Ossory as valid on
1069.04.22 and 1069.07.04 while declare_war's guard (the game's can_declare_war) refused it
twice: gold was -9.69 and -2.67 (the bridge's own war-validity read names not_in_debt). On
1075.07.04 the same offer for Oriel was refused with prestige at -101.84, below the claim
war's cost. This module states those conditions on every casus-belli option the code
shapes, so cbs, discover(war_targets), plan_war, inspect(title)'s next_call and observe's
news say the same as the refusal.

The conditions, each from a read the code has:
  in_debt            query_character gold < 0 (the game refuses a declaration while in
                     debt; the bridge's pre_validate_war checks it as not_in_debt)
  prestige / piety   the casus belli's declaration cost against what the ruler holds,
                     for a casus belli in CB_COST_TABLE (the game's own script values:
                     claim_cb); any other casus belli's cost is the bridge's family
                     estimate, shown in `checked` and never a refusal
  truce              the cbs read's target_state.in_truce, laid over by truces.py (the
                     ruler's relation slots, with the truce's end)
  armies_raised      query_armies: an army of the ruler's with men in it (a refusal, the
                     game's declare_war_interaction refusing raised armies for every government
                     but a nomad's, as its own check refused the declarations sent with them)
"""
from __future__ import annotations

from typing import Any, Optional

#: The game's claim_cb declaration cost in prestige, by the tier of the claimed title
#: (common/script_values/99_casus_belli_values.txt, claim_cb_prestige_cost: 100 a county,
#: 200 a duchy, 400 a kingdom, 800 an empire; a pressed claim 25% less). The game then
#: multiplies it (culture innovations, a child on either side, same dynasty, gender
#: doctrine); the base is the least it can be before those. The bridge's cbs row says 0
#: for claim_cb (query_handlers.cpp cb_cost_for: "free to declare"), which the game's
#: file contradicts. test_declare_war_cost_gates reads this table against the file in the install.
CB_COST_TABLE: dict = {
    "claim_cb": {"currency": "prestige",
                 "by_tier": {"county": 100, "duchy": 200, "kingdom": 400, "empire": 800},
                 "pressed_factor": 0.75,
                 "source": "the game's claim_cb_prestige_cost (99_casus_belli_values.txt), before its multipliers"},
}

#: (1078.09.15: ducal_conquest_cb against 68530 refused twice "the game
#: gives no detail" with prestige at -540): ducal_conquest_cb's cost
#: (common/casus_belli_types/00_conquest.txt) is 50 prestige for a count or less, 100 above,
#: times common_cb_prestige_cost_multiplier, which can lower it (0.25, 0.5) but never to 0 or
#: below: a ruler with prestige below 0 cannot pay any of it. Only that is a refusal.
CB_POSITIVE_COST: dict = {
    "ducal_conquest_cb": {"currency": "prestige", "base": 50,
                          "source": ("the game's ducal_conquest_cb cost (00_conquest.txt): 50 prestige "
                                     "for a count, 100 above, times multipliers that can lower it")},
}

_TIER_OF_PREFIX = {"b_": "barony", "c_": "county", "d_": "duchy", "k_": "kingdom", "e_": "empire",
                   "h_": "hegemony"}


def tier_of_key(key: Any) -> Optional[str]:
    return _TIER_OF_PREFIX.get(str(key or "")[:2])


def _num(v) -> Optional[float]:
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


#: claim_cb_prestige_cost's multipliers when the attacker presses his own claim
#: (common/script_values/99_casus_belli_values.txt), each from a query_character field of the
#: two rulers. Read live in a test run: Athlone 1067 (count on count, catholic on insular_celtic)
#: and Mayo 1086 (duke on duke) each cost 122-124 against this formula's 125.
CLAIM_COST_FACTORS = (
    ("defender_child", 0.5, "an adult attacker on a child ruler"),
    ("attacker_child", 1.5, "a child attacker on an adult ruler"),
    ("same_dynasty", 0.5, "the defender is of your dynasty"),
    ("different_faith", 1.25, "the defender is of another faith"),
    ("defender_lower_tier", 1.25, "your primary title outranks the defender's"),
)
#: What the formula holds that no read here gives; each multiplies the figure.
CLAIM_COST_UNREAD = ("culture innovations (chronicle_writing, land_grants, rightful_ownership: -10% "
                     "each)", "your legitimacy level (x0.15 to x2.5 by level and tier)",
                     "the Bellum Justum perk and dynasty perks", "a gender doctrine when the defender "
                     "shares your faith and is of the other sex (x0.75 or x1.25)")
#: Measured beside it: three claim wars on larger realms moved prestige by 335, 371 and
#: 472 in the declaring step; the formula gives 125-156 for those targets, and the game refuses a
#: declaration it cannot pay, so the rest of those steps' drops is not this price (cause unread).
CLAIM_COST_MEASURED_ABOVE = ("in the step that declares a claim war on a larger realm, prestige can fall by several "
                             "times this price from causes not read")


#: (Domnall's claim_cb priced 62.5 was refused at 78.56 held
#: and charged about 93.75 = 62.5 x 1.5 when it applied): claim_cb's cost (00_claim.txt,
#: "LEGITIMACY AFFECTS COST") multiplies by the attacker's legitimacy level's flag
#: (common/legitimacy/00_legitimacy.txt, per tier); the level's threshold is
#: LEGITIMACY_LEVEL_BASE x legitimacy_title_era_value (00_legitimacy_values.txt: the culture's
#: era 1-4 plus the tier above count, at least 2). The flags per level, read from the file
#: (test_ally_join_and_loss_price checks them against the install):
LEGITIMACY_LEVEL_BASE = (0, 30, 70, 120, 200, 300)
LEGITIMACY_FLAG_X = {"reduced": 0.5, "very_reduced": 0.25, "extra_reduced": 0.15,
                     "increased": 1.5, "very_increased": 2.0, "massively_increased": 2.5}
LEGITIMACY_CLAIM_FLAGS = {   # ruler_tier -> the claim-cost flag of levels 1..6 (None: no flag)
    2: ("increased", None, "reduced", "very_reduced", "extra_reduced", "extra_reduced"),
    3: ("very_increased", "increased", None, "reduced", "very_reduced", "extra_reduced"),
    4: ("very_increased", "increased", "increased", None, "reduced", "very_reduced"),
    5: ("massively_increased", "very_increased", "increased", "increased", None, "reduced"),
}
LEGITIMACY_ERA_UNREAD = "your culture's era (1-4) is not read: the price spans the eras"


def legitimacy_factor(legitimacy: Any, ruler_tier: Any) -> Optional[tuple]:
    """(low, high) of claim_cb's legitimacy multiplier for this legitimacy and tier across the
    four eras (equal when the era does not matter), or None when either is not read."""
    flags = LEGITIMACY_CLAIM_FLAGS.get(ruler_tier) if isinstance(ruler_tier, int) else None
    lg = _num(legitimacy)
    if flags is None or lg is None:
        return None
    xs = []
    for era in (1, 2, 3, 4):
        mult = max(2, era + max(1, ruler_tier - 1))
        level = max([i for i, b in enumerate(LEGITIMACY_LEVEL_BASE) if lg >= b * mult] or [0])
        f = flags[level]
        xs.append(LEGITIMACY_FLAG_X[f] if f else 1.0)
    return (min(xs), max(xs))


def legitimacy_detail(legitimacy: Any, ruler_tier: Any) -> Optional[dict]:
    """The legitimacy level behind claim_cb's multiplier, across the four culture eras
    (the era is not read): {legitimacy, level [low, high] (1-6), x [low, high], next_level_at
    [low, high]: the legitimacy that reaches the next level}. None when a read is missing."""
    flags = LEGITIMACY_CLAIM_FLAGS.get(ruler_tier) if isinstance(ruler_tier, int) else None
    lg = _num(legitimacy)
    if flags is None or lg is None:
        return None
    levels, nxt = [], []
    for era in (1, 2, 3, 4):
        mult = max(2, era + max(1, ruler_tier - 1))
        level = max([i for i, b in enumerate(LEGITIMACY_LEVEL_BASE) if lg >= b * mult] or [0])
        levels.append(level + 1)
        if level + 1 < len(LEGITIMACY_LEVEL_BASE):
            nxt.append(LEGITIMACY_LEVEL_BASE[level + 1] * mult)
    lf = legitimacy_factor(lg, ruler_tier)
    return {"legitimacy": round(lg, 1), "level": [min(levels), max(levels)], "x": list(lf),
            **({"next_level_at": [min(nxt), max(nxt)]} if nxt else {}),
            "source": "common/legitimacy/00_legitimacy.txt (the flag per level and tier) and "
                      "00_legitimacy_values.txt (the thresholds, by culture era and tier)"}


def _adult(c: dict) -> Optional[bool]:
    a = c.get("age")
    return (a >= 16) if isinstance(a, (int, float)) and not isinstance(a, bool) else None


#: claim_cb_prestige_cost's gender rule (99_casus_belli_values.txt) when the attacker and the
#: defender share a faith and are of the other sex: {doctrine: {attacker is female: x}}
GENDER_DOCTRINE_X = {"male_dominated": {False: 0.75, True: 1.25},
                     "female_dominated": {False: 1.25, True: 0.75},
                     "none": {False: 1.0, True: 1.0}}


def claim_cost(tier: str, pressed: Optional[bool], attacker: Optional[dict],
               defender: Optional[dict]) -> dict:
    """claim_cb's prestige price for the attacker pressing his own claim on one title of
    `tier`: the base (100 a county, 200 a duchy, 400 a kingdom, 800 an empire; a pressed
    claim 25% less) times each multiplier the two rulers' reads decide; a multiplier no read
    gives is named in `unread`. `cost` is None when the base is not known."""
    row = CB_COST_TABLE["claim_cb"]
    base = row["by_tier"].get(tier)
    if base is None:
        return {"cost": None, "unread": ["the claimed title's tier"]}
    unread = list(CLAIM_COST_UNREAD)
    if pressed is None:
        # the least it can be: a refusal is never said from a price the claim may not carry
        unread.insert(0, "whether the claim is pressed: priced as pressed (25% less), the least")
    cost = base * (1.0 if pressed is False else row["pressed_factor"])
    a, d = attacker or {}, defender or {}
    applied = []
    if not a or not d:
        unread.insert(0, "the defender's character read" if a else "your character read")
        return {"cost": round(cost, 2), "applied": applied, "unread": unread}
    aa, da = _adult(a), _adult(d)
    flags = {"defender_child": aa is True and da is False,
             "attacker_child": aa is False and da is True,
             "same_dynasty": bool(a.get("dynasty_id")) and a.get("dynasty_id") == d.get("dynasty_id"),
             "different_faith": bool(a.get("faith_key") and d.get("faith_key")) and a.get("faith_key") != d.get("faith_key"),
             "defender_lower_tier": isinstance(a.get("ruler_tier"), int) and isinstance(d.get("ruler_tier"), int)
             and a["ruler_tier"] > d["ruler_tier"]}
    for name, mult, what in CLAIM_COST_FACTORS:
        if flags[name]:
            cost *= mult
            applied.append({"factor": name, "x": mult, "why": what})
    for k, label in (("faith_key", "faith"), ("ruler_tier", "primary tier"), ("age", "age")):
        if a.get(k) is None or d.get(k) is None:
            unread.append(f"{label} of either ruler")
    same_faith_other_sex = (a.get("faith_key") is not None and a.get("faith_key") == d.get("faith_key")
                            and isinstance(a.get("is_female"), bool) and isinstance(d.get("is_female"), bool)
                            and a["is_female"] != d["is_female"])
    if not same_faith_other_sex:
        unread = [u for u in unread if not u.startswith("a gender doctrine")]
    elif a.get("faith_gender") in GENDER_DOCTRINE_X:
        # The faith's gender doctrine read from the game (claim_price_facts)
        unread = [u for u in unread if not u.startswith("a gender doctrine")]
        gx = GENDER_DOCTRINE_X[a["faith_gender"]][bool(a["is_female"])]
        if gx != 1.0:
            cost *= gx
            applied.append({"factor": "gender_doctrine", "x": gx, "why": (
                f"your faith is {a['faith_gender'].replace('_', '-')}: a "
                f"{'woman' if a['is_female'] else 'man'} pressing a claim on a "
                f"{'man' if a['is_female'] else 'woman'} of it")})
    out = {}
    lx = _num(a.get("claim_legitimacy_x"))
    if lx is not None:
        # The flag the game holds, exact
        unread = [u for u in unread if not u.startswith("your legitimacy")]
        if lx != 1.0:
            cost *= lx
            applied.append({"factor": "legitimacy", "x": lx,
                            "why": "your legitimacy level's flag, read from the game (has_legitimacy_flag)"})
        return {"cost": round(cost, 2), "applied": applied, "unread": unread}
    lf = legitimacy_factor(a.get("legitimacy"), a.get("ruler_tier"))
    if lf is not None:
        unread = [u for u in unread if not u.startswith("your legitimacy")]
        lo, hi = lf
        if lo == hi:
            if lo != 1.0:
                cost *= lo
                applied.append({"factor": "legitimacy", "x": lo,
                                "why": f"your legitimacy {round(a['legitimacy'], 1)} at your tier"})
        else:
            # the least is the price a refusal may be said from; the range says the rest
            out["cost_high"] = round(cost * hi, 2)
            cost *= lo
            applied.append({"factor": "legitimacy", "x": [lo, hi],
                            "why": f"your legitimacy {round(a['legitimacy'], 1)}: " + LEGITIMACY_ERA_UNREAD})
    return {"cost": round(cost, 2), "applied": applied, "unread": unread, **out}


def cost_of(opt: dict, *, title_key: Any = None, facts: Optional[dict] = None) -> dict:
    """The declaration cost of one shaped cbs option: {prestige, piety, source}. claim_cb
    from the game's formula (claim_cost) when the claimed title's tier is known (its key or
    the option's scope), with the rulers' reads in `facts` ({attacker, defender, pressed});
    any other casus belli the bridge's row; a cost not known is None with the reason."""
    cb = str(opt.get("cb") or "")
    gp = (facts or {}).get("game_price")
    if isinstance(gp, dict) and _num(gp.get("prestige")) is not None:
        # The game's own price, its cost block evaluated for this attacker and defender (cb_prices)
        return {"prestige": round(gp["prestige"], 2), "piety": round(_num(gp.get("piety")) or 0.0, 2),
                "game": True, "source": gp.get("source") or "the game's own price",
                "basis": GAME_PRICE_BASIS}
    row = CB_COST_TABLE.get(cb)
    if row is not None:
        tier = tier_of_key(title_key) or (opt.get("scope") if opt.get("scope") in row["by_tier"] else None)
        if tier is None:
            return {"prestige": None, "piety": 0, "source": row["source"],
                    "unread": "the claimed title's tier was not read"}
        f = facts or {}
        cc = claim_cost(tier, f.get("pressed"), f.get("attacker"), f.get("defender"))
        a = f.get("attacker") or {}
        ld = legitimacy_detail(a.get("legitimacy"), a.get("ruler_tier"))
        return {"prestige": cc["cost"], "piety": 0, "source": CLAIM_COST_SOURCE,
                **({"prestige_high": cc["cost_high"]} if cc.get("cost_high") is not None else {}),
                # Each multiplier as a row, and the legitimacy level behind its factor
                "multipliers": [{"base": row["by_tier"][tier], "pressed": f.get("pressed")}] + list(cc["applied"]),
                **({"legitimacy": ld} if ld else {}),
                "basis": "the game's claim_cb price for THIS target: " + (
                    ", ".join(f"x{_x_words(x['x'])} {x['why']}" for x in cc["applied"]) or "no multiplier the reads decide")
                + f" on the base {row['by_tier'][tier]:g} ({'unpressed' if f.get('pressed') is False else 'pressed: 25% less'})",
                "unread_multipliers": cc["unread"], "measured": CLAIM_COST_MEASURED_ABOVE}
    pos = CB_POSITIVE_COST.get(cb)
    if pos is not None:
        return {pos["currency"]: pos["base"], "positive": True, "source": pos["source"],
                "basis": f"more than 0 (base {pos['base']:g}, then the game's multipliers)"}
    p, q = _num(opt.get("cost_prestige")), _num(opt.get("cost_piety"))
    return {"prestige": p, "piety": q, "estimate": True,
            "source": "the bridge's base cost for this casus belli's family (the game scales it by target)"}


def _x_words(x) -> str:
    return "-".join(f"{v:g}" for v in x) if isinstance(x, (list, tuple)) else f"{x:g}"


#: (a 1066 game: the vassalization declaration estimated 50 took 125; another, 1080.07:
#: two refused at 50.07 prestige "which no read names"): the casus belli whose declaration price the game itself
#: computes for this server, its own cost block evaluated in the war's scopes (bridge
#: script_templates.h kCbPrices, the cb_prices template; test_declaration_price_and_strength checks both lists and the files agree).
#: Any other casus belli keeps the bridge's family estimate, said `estimate` in every row.
CB_PRICED_BY_GAME = ("county_struggle_cb", "fp2_border_raid", "claim_cb", "county_conquest_cb",
                     "duchy_conquest_cb", "ducal_conquest_cb", "individual_county_de_jure_cb",
                     "individual_duchy_de_jure_cb", "vassalization_cb", "make_tributary_cb",
                     "tribal_subjugation_cb", "minor_religious_war", "religious_war", "major_religious_war")
#: those whose cost reads the war's title (target_titles) or the claimant: priced per title
CB_PRICE_NEEDS_TITLE = ("county_struggle_cb", "claim_cb", "individual_duchy_de_jure_cb")

def game_price_of(prices: Optional[dict], cb: Any, title_id: Any) -> Optional[dict]:
    """The game's price for one option from cb_game_prices' answer, or None."""
    if not prices:
        return None
    t = (int(title_id) if isinstance(title_id, int) and not isinstance(title_id, bool) else 0) if cb in CB_PRICE_NEEDS_TITLE else 0
    return prices.get((cb, t))


#: What a price the game computed is (the estimate 50 against a price of 125 a county)
GAME_PRICE_BASIS = "the price the game charges when you declare, read before sending (not an estimate)"

CLAIM_COST_SOURCE = ("the game's claim_cb_prestige_cost and its multipliers (99_casus_belli_values.txt), "
                     "from both rulers' reads")


#: The conditions this module checks, in the order a refusal names them.
CHECKS = ("in_debt", "armies_raised", "prestige", "piety", "truce")

#: Ten claim declarations from 1081.09.15 to 1085.11.27 were refused
#: by the game's can_declare_war with no words while every condition above passed; the one
#: war-validity read of those years (1082.02.08) found an army of 1,680 men raised at peace.
ARMIES_RAISED_FIX = ("your armies are raised: the game refuses a war declaration until they are "
                     "disbanded. The game's AI raises and disbands your armies; no tool disbands them")
#: (1081.12.12, 1090.12.12, 1091.06.26: the refusal said "no tool disbands them"
#: with `army` listed; the seat disbanded with it and the same declaration applied): with army orders on
ARMIES_RAISED_FIX_ON = ("your armies are raised: the game refuses a war declaration until they are "
                        "disbanded. The game's AI raises them; do army action=disband disbands one (the game "
                        "refuses it in hostile land or near enemies: move it home and hold it first), then "
                        "declare again")


def disband_call(army_id: Any = None) -> dict:
    """The ready call that disbands a raised army (army_orders, switched on)."""
    return {"tool": "do", "args": {"intent": "army", "args": {
        "action": "disband", "army_id": army_id if army_id is not None else "<army_id from inspect armies>"}}}


def armies_raised_fix(army_ids: Any = None, environ=None) -> tuple:
    """(detail, fix) for a declaration the game refused for raised armies: the disband call when the
    army lever is listed (CK3_BRIDGE_ARMY_ORDERS=1), else the armies read and that no tool disbands."""
    import army_orders  # noqa: PLC0415
    if not army_orders.orders_on(environ):
        return ARMIES_RAISED_FIX, {"tool": "inspect", "args": {"subject": "armies"}}
    ids = [a for a in (army_ids or []) if a is not None]
    return ARMIES_RAISED_FIX_ON, disband_call(ids[0] if ids else None)


def armies_raised(armies_reply: Any) -> Optional[dict]:
    """{raised, armies, men} from a query_armies reply (an army listed with men in it is a
    raised army: unraised levies and men-at-arms are not army objects), or None when the
    read did not answer."""
    if not isinstance(armies_reply, dict) or not isinstance(armies_reply.get("armies"), list):
        return None
    rows = [a for a in armies_reply["armies"] if isinstance(a, dict)
            and isinstance(a.get("troop_count"), (int, float)) and a["troop_count"] > 0]
    return {"raised": bool(rows), "armies": len(rows), "men": int(sum(a["troop_count"] for a in rows)),
            "army_ids": [a.get("army_id") for a in rows if a.get("army_id") is not None]}


def debt_words(gold: float, inc: Any) -> str:
    """One sentence for a ruler in debt: no declaration, how far, and when out at the rate read."""
    months = (-gold / inc) if isinstance(inc, (int, float)) and inc > 0 else None
    return (f"in debt: no declaration is possible (the game refuses every one while gold is below 0): "
            f"{round(-gold, 2)} gold short; "
            + (f"out of debt in {months:.1f} months at +{inc} a month (the income read, before expenses it "
               "leaves out)" if months else "no positive income read: no date out of debt"))


def failing(opt: dict, purse: Optional[dict], target_state: Optional[dict], *,
            title_key: Any = None, facts: Optional[dict] = None) -> tuple[list, dict]:
    """(failed, checked) for one shaped option. `failed`: each condition that fails with
    what is missing and the fix; `checked`: every condition with its reading (a reading
    that was not made says so, never passes)."""
    purse = purse if isinstance(purse, dict) else {}
    ts = target_state if isinstance(target_state, dict) else {}
    failed: list = []
    checked: dict = {}
    gold = _num(purse.get("gold"))
    if gold is None:
        checked["in_debt"] = "unread: no gold read"
    else:
        checked["in_debt"] = {"gold": round(gold, 2), "ok": gold >= 0}
        if gold < 0:
            inc = _num(purse.get("monthly_income"))
            months = (-gold / inc) if inc and inc > 0 else None
            failed.append({"condition": "in_debt", "gate": "insufficient_gold",
                           "have": round(gold, 2), "need": 0, "missing": round(-gold, 2),
                           "fix": debt_words(gold, inc),
                           "next": {"tool": "do", "args": {"action_class": "income_levers"}}})
    ar = purse.get("armies") if isinstance(purse.get("armies"), dict) else None
    if ar is None:
        checked["armies_raised"] = "unread: no armies read"
    else:
        # (a test run, step 2: a claim row read declarable_now true beside "your armies are raised", and the
        # declaration was refused for it): a refusal, as the declaration's own check makes it. The game's declare_war
        # interaction refuses raised armies for every government but a nomad's (00_war.txt: has_raised_armies,
        # unless government_has_flag can_start_war_with_raised_troops, 00_government_types.txt: nomad_government
        # only), and the declaration the bridge sends was refused for it by the game's own check (the template's
        # RAISED_ARMIES mark: once at 0867.06.30, three times in another game). The 1096.01.01 validity read that said
        # true with 590 men raised was a read, not a send. The government is not read here.
        checked["armies_raised"] = {"raised": ar["raised"], "men": ar["men"], "ok": not ar["raised"],
                                    **({"army_ids": ar["army_ids"]} if ar.get("army_ids") else {})}
        if ar["raised"]:
            import army_orders  # noqa: PLC0415
            on = army_orders.orders_on()
            ids = ar.get("army_ids") or []
            failed.append({"condition": "armies_raised", "gate": "armies_raised", "have": ar["men"],
                           "fix": ARMIES_RAISED_FIX_ON if on else ARMIES_RAISED_FIX,
                           "next": (disband_call(ids[0] if ids else None) if on
                                    else {"tool": "inspect", "args": {"subject": "armies"}})})
    cost = cost_of(opt, title_key=title_key, facts=facts)
    for cur in ("prestige", "piety"):
        c, have = cost.get(cur), _num(purse.get(cur))
        if c is None or have is None:
            checked[cur] = ("unread: " + ("no " + cur + " read" if have is None else
                                          (cost.get("unread") or "the cost is not known")))
            continue
        if cost.get("estimate"):
            # the bridge's family estimate is shown, never a refusal (plan_war's rows say so too)
            checked[cur] = {"cost_estimate": c, "have": round(have, 2), "gates": False}
            continue
        if cost.get("positive"):
            # the exact price is not read; a price above 0 against less than 0 always fails
            checked[cur] = {"cost_more_than": 0, "have": round(have, 2), "ok": None if have >= 0 else False}
            if have < 0:
                failed.append({"condition": cur, "gate": f"insufficient_{cur}", "cost": "more than 0",
                               "have": round(have, 2), "missing": f"more than {round(-have, 2)}",
                               "cost_source": cost["source"], "cost_basis": cost["basis"],
                               "fix": f"{cur} is {round(have, 2)}: below 0, no price of this casus belli can be paid"})
            continue
        checked[cur] = {"cost": c, "have": round(have, 2), "ok": have >= c}
        if c > 0 and have < c:
            # When it can be retried, at the monthly change (prestige; piety's is not read here)
            mo = months_to_afford(c, have, purse.get("monthly_prestige")) if cur == "prestige" else None
            failed.append({"condition": cur, "gate": f"insufficient_{cur}", "cost": c,
                           "have": round(have, 2), "missing": round(c - have, 2),
                           **({"retry_in_months": mo} if mo is not None else {}),
                           "cost_source": cost["source"],
                           **({"cost_basis": cost["basis"]} if cost.get("basis") else {}),
                           **({"measured": cost["measured"]} if cost.get("measured") else {}),
                           **({"unread_multipliers": cost["unread_multipliers"]}
                              if cost.get("unread_multipliers") else {}),
                           "fix": f"{round(c - have, 2)} {cur} short of this casus belli's cost"})
    tr = ts.get("in_truce")
    if not isinstance(tr, bool):
        checked["truce"] = "unread: the casus-belli read gave no target_state.in_truce"
    else:
        checked["truce"] = {"in_truce": tr, "ok": not tr}
        if tr:
            # Until when, against the episode's end (truces.py, the relation slots)
            import truces as _tr  # noqa: PLC0415
            t = ts.get("truce") if isinstance(ts.get("truce"), dict) else None
            if t:
                checked["truce"]["until"] = t.get("until")
            failed.append({"condition": "truce", "gate": "target_truced", "fix": _tr.fix_words(t),
                           **({"truce": t} if t else {})})
    return failed, checked


#: (1091.08.14: Breifne declared at 177 prestige, 150 for the war and 150 for the
#: ally who joined, -110 after; the white peace of 1092.12.28 left it below every war's price to the end):
#: what a war can cost in prestige on each outcome, beside what the ruler holds
#: (the file's words, kept short: the row rides on every declarable option of cbs and plan_war)
PRESTIGE_BELOW_ZERO = "below 0: the game refuses every declaration (declare_war_interaction)"
PRESTIGE_MONTHLY_UNREAD = "unread"
#: The monthly change comes from the game's own per-character cache (query_character
#: monthly_change, re_monthly_prestige.md); an older bridge sends none: "unread", never 0
PRESTIGE_MONTHLY_BASIS = "the game's monthly prestige change for your ruler (its own cache, applied once a month)"


def monthly_change_of(chr_reply: Any) -> Optional[dict]:
    """{prestige, piety} per month from a query_character reply's monthly_change, or None (not sent)."""
    mc = chr_reply.get("monthly_change") if isinstance(chr_reply, dict) else None
    if not isinstance(mc, dict):
        return None
    out = {k: _num(mc.get(k)) for k in ("prestige", "piety")}
    return out if out["prestige"] is not None else None


def months_to_afford(cost: Any, have: Any, monthly: Any) -> Any:
    """Whole months until `have` reaches `cost` at `monthly` a month: 0 when held now, "never at this
    rate" when the change is not positive, None when an input is unread."""
    c, h, m = _num(cost), _num(have), _num(monthly)
    if c is None or h is None or m is None:
        return None
    if h >= c:
        return 0
    if m <= 0:
        return "never at this rate"
    import math  # noqa: PLC0415
    return int(math.ceil(round((c - h) / m, 6)))
ALLY_JOIN_PRESTIGE = "-75 county, -150 duchy, -350 kingdom (call_ally)"
_OUTCOME_PRESTIGE = {
    "claim": ("-5 x the war's factor (unread)", "-10 x the war's factor (unread), and legitimacy"),
    "de_jure": ("-5 x the war's factor (unread)", "-10 x the war's factor (unread)"),
}
#: What each ending costs or gives the attacker in prestige, before he pays it, from the casus belli's own block
#: for that ending (event_prices.cb_end_prestige) when the game's files are readable; this table where they are not.
#: claim_cb (00_claim.txt): a defeat is -10 x the war's cb_prestige_factor (modify_all_participants_fame_values,
#: LOSER_FAME_SCALE -10, at least -1000), a white peace -5 x it; the factor is set by the engine for each war
#: (setup_claim_cb) and no file or read gives it, so no number stands for it. A win gives prestige experience only
#: (the fame effect's winning attacker), not prestige. The conquest casus belli carry fixed amounts (00_conquest.txt).
#: (win, white_peace, defeat): numbers where the file gives one, else the rule in words.
CLAIM_FACTOR_RULE = "the war's prestige factor (unread)"
OUTCOME_PRICE: dict = {
    "claim_cb": {"win": 0, "white_peace": "-5 x " + CLAIM_FACTOR_RULE,
                 "defeat": "-10 x " + CLAIM_FACTOR_RULE + ", at least -1000",
                 "basis": ("00_claim.txt: a win is prestige experience; 5 / 10 x the war's factor; a defeat also "
                           "costs legitimacy, the claim, gold")},
    "ducal_conquest_cb": {"win": 0, "white_peace": -75, "defeat": -150,
                          "basis": ("00_conquest.txt: a win gives 75 prestige experience per title taken (not "
                                    "prestige); a defeat also costs legitimacy and 2 years of income in gold")},
    "duchy_conquest_cb": {"win": 0, "white_peace": -75, "defeat": -350,
                          "basis": ("00_conquest.txt: a win gives 150 prestige experience per title taken (not "
                                    "prestige); a defeat also costs legitimacy and 3 years of income in gold")},
    #
    # a win gives prestige EXPERIENCE (add_prestige_experience), not prestige to spend
    "vassalization_cb": {"win": 0, "white_peace": -75, "defeat": -350,
                         "basis": ("00_vassalization.txt: a win gives 150 prestige experience (not prestige); a "
                                   "defeat also costs 3 years of income in gold")},
    "make_tributary_cb": {"win": 0, "white_peace": -75, "defeat": -350,
                          "basis": ("00_tributarize.txt: a win gives 150 prestige experience (not prestige); a "
                                    "defeat costs no gold")},
    # A holy war moves piety, not prestige (00_religious_war.txt minor_religious_war)
    "minor_religious_war": {"win": 0, "white_peace": 0, "defeat": 0,
                            "basis": ("00_religious_war.txt: no prestige on any ending; a white peace costs 50 "
                                      "piety, a defeat 2 years of income in gold and legitimacy")},
}
#: The families whose prices are read from the files though the table has no row of their own
OUTCOME_FILE_FAMILIES = ("claim", "de_jure")
_OUTCOME_ENDINGS = (("win", "attacker_victory"), ("white_peace", "white_peace"), ("defeat", "defender_victory"))


def outcome_from_files(cb: Any) -> Optional[dict]:
    """{win, white_peace, defeat, basis[, not_read]} for the attacker of a war of `cb`, each read from the casus
    belli's block for that ending (event_prices.cb_end_prestige): a number where every part is one, else the rule in
    words. None when the files do not say."""
    import event_prices as _ep  # noqa: PLC0415
    out: dict = {}
    files: list = []
    for term, ending in _OUTCOME_ENDINGS:
        r = _ep.cb_end_prestige(cb, ending, "attacker")
        if r is None:
            return None
        if isinstance(r.get("prestige"), (int, float)):
            out[term] = r["prestige"]
        elif r.get("rule"):
            out[term] = r["rule"].replace("the war's prestige factor", CLAIM_FACTOR_RULE)
        else:
            out[term] = "a value computed in play"
        files.append(r["file"].split(" ")[0])
    row = OUTCOME_PRICE.get(str(cb or "")) or {}
    out["basis"] = row.get("basis") or (files[0] if files else "")
    return out


def outcome_price(cb: Any) -> Optional[dict]:
    """{win, white_peace, defeat, basis} in prestige for the attacker of a war of `cb`: from the casus belli's file
    when it is readable (outcome_from_files), else the table."""
    c = str(cb or "")
    if c in OUTCOME_PRICE or any(f in c for f in OUTCOME_FILE_FAMILIES):
        got = outcome_from_files(c)
        if got is not None:
            return got
    return OUTCOME_PRICE.get(c)


def prestige_outcomes(opt: dict, have: Any, monthly: Any = None) -> Optional[dict]:
    """The war's prestige on each outcome against the prestige held: the declaration's price
    (read), each joining ally's (by his tier), a white peace's and a defeat's (the casus belli's file: a
    multiple of a factor no read gives), the floor below which no war can be declared, and the monthly
    change with the months until the declaration is affordable ("unread" from a bridge that does
    not send it). None when the prestige held was not read."""
    h = _num(have)
    if h is None:
        return None
    chk = (opt.get("checked") or {}).get("prestige")
    c = chk.get("cost", chk.get("cost_estimate")) if isinstance(chk, dict) else None
    fam = "claim" if "claim" in str(opt.get("cb") or "") else ("de_jure" if "de_jure" in str(opt.get("cb") or "")
                                                                else None)
    out: dict = {"have": round(h, 2)}
    if isinstance(c, (int, float)):
        out["declare"] = -round(float(c), 2)
        out["after_declare"] = round(h - float(c), 2)
    else:
        out["declare"] = "unread"
    out["each_ally_joining"] = ALLY_JOIN_PRESTIGE
    op = outcome_price(opt.get("cb"))
    if op is not None:
        out["win"], out["white_peace"], out["defeat"] = op["win"], op["white_peace"], op["defeat"]
        out["outcome_basis"] = op["basis"]
    elif fam:
        out["white_peace"], out["defeat"] = _OUTCOME_PRESTIGE[fam]
    else:
        out["white_peace"] = out["defeat"] = "unread (no outcome table for this casus belli)"
    out["floor"] = PRESTIGE_BELOW_ZERO
    m = _num(monthly)
    if m is None:
        out["monthly"] = PRESTIGE_MONTHLY_UNREAD
    else:
        out["monthly"] = round(m, 2)
        if isinstance(c, (int, float)):
            out["months_to_afford"] = months_to_afford(c, h, m)
    return out


def facts_for(opt: dict, purse: Optional[dict], defender: Optional[dict], prices: Optional[dict] = None) -> dict:
    """The reads claim_cost takes for one option: the attacker (purse['me']), the defender's
    query_character, and whether the claim on the option's title is pressed (purse['claims']);
    And the game's own price of this option when it was read (redesign.cb_game_prices)."""
    p = purse if isinstance(purse, dict) else {}
    typ = (p.get("claims") or {}).get(opt.get("cb_title_id")) if isinstance(p.get("claims"), dict) else None
    gp = game_price_of(prices, opt.get("cb"), opt.get("cb_title_id"))
    return {"attacker": p.get("me"), "defender": defender,
            "pressed": (typ == "pressed") if typ in ("pressed", "unpressed") else None,
            **({"game_price": gp} if gp else {})}


def annotate(opts: list, purse: Optional[dict], target_state: Optional[dict],
             title_keys: Optional[dict] = None, defender: Optional[dict] = None,
             prices: Optional[dict] = None) -> None:
    """Mark each option the game's check accepts with what else would refuse it now:
    `blocked_by` (the failing conditions, first one first) and `checked`."""
    for o in opts or []:
        if not (isinstance(o, dict) and o.get("valid")):
            continue
        tk = (title_keys or {}).get(o.get("cb_title_id"))
        bad, chk = failing(o, purse, target_state, title_key=tk, facts=facts_for(o, purse, defender, prices))
        o["checked"] = chk
        o.pop("may_refuse", None)                 # Raised armies are a blocked_by condition (failing)
        if bad:
            o["blocked_by"] = bad
        else:
            o.pop("blocked_by", None)
        po = prestige_outcomes(o, (purse or {}).get("prestige") if isinstance(purse, dict) else None,
                               (purse or {}).get("monthly_prestige") if isinstance(purse, dict) else None)
        if po is not None:
            o["prestige_if"] = po


def purse_of(chr_reply: Any) -> Optional[dict]:
    """{gold, prestige, piety, monthly_income} from a query_character reply, or None."""
    if not isinstance(chr_reply, dict) or not chr_reply:
        return None
    bd = chr_reply.get("income_breakdown") if isinstance(chr_reply.get("income_breakdown"), dict) else {}
    out = {k: _num(chr_reply.get(k)) for k in ("gold", "prestige", "piety")}
    out["monthly_income"] = _num(bd.get("total_monthly"))
    if not any(v is not None for v in out.values()):
        return None
    mc = monthly_change_of(chr_reply)
    out["monthly_prestige"] = mc["prestige"] if mc else None
    out["me"] = ruler_facts(chr_reply)
    return out


def ruler_facts(chr_reply: Any) -> Optional[dict]:
    """The query_character fields claim_cost reads, or None."""
    if not isinstance(chr_reply, dict) or not chr_reply:
        return None
    return {k: chr_reply.get(k) for k in ("id", "age", "is_female", "faith_key", "dynasty_id", "ruler_tier")}


def loss_after(prestige_if: Any, cb: Any = None) -> Optional[dict]:
    """A recorded case (1072-1074: two wars declared at 526 vs 5748 and lost; prestige -132 and
    eight years below every war's price): beside an outnumbered verdict, what losing this war leaves
    and how long until the same declaration is affordable again at the monthly change. None when
    the prestige held, the declaration's price or the defeat's price did not read."""
    p = prestige_if if isinstance(prestige_if, dict) else {}
    have, dec, lose = _num(p.get("have")), _num(p.get("declare")), _num(p.get("defeat"))
    if have is not None and dec is not None and lose is None and isinstance(p.get("defeat"), str) \
            and "factor" in p["defeat"]:
        # The defeat's prestige is a multiple of the war's unread factor: what is left after the declaration, and the
        # defeat's rule beside it, never a number for the factor
        out = {"prestige": p["defeat"], "prestige_after_declare": round(have + dec, 2),
               "after_loss": "lower than that by the defeat's share"}
        if "claim" in str(cb or ""):
            out["claim"] = "lost"
        return out
    if have is None or dec is None or lose is None:
        return None
    after = round(have + dec + lose, 2)
    out = {"prestige": lose, "prestige_after_loss": after,
           "months_to_afford_again": months_to_afford(-dec, after, p.get("monthly"))}
    if "claim" in str(cb or ""):
        out["claim"] = "lost"
    if after < 0:
        out["below_zero"] = "no declaration at all until it is above 0 again"
    return out

