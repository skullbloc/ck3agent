"""The activities a seat's ruler may host (feast, hunt, pilgrimage), put to the game's own AI.

How the game hosts an activity for a ruler it runs (re/findings/re_activities.md): a monthly AI pass
(AI.ToggleHostActivities; image 0x04da9f40) scores every activity type the ruler may host (the type's
is_shown / can_start, then its ai_will_do of at least ACTIVITY_SCORE_THRESHOLD 20), takes the best, and
hosts it with a chance of that score in 100; the AI then picks the options, the place and the guests,
and pays the price. The game's own effect `ai_attempt_to_host_activity = <type>` puts one type into
the ruler's forced set, so the next pass weighs it whatever its check interval and cooldown
(image 0x04ec0960; it refuses a character the game's AI does not run). That is the path here: the
bridge's `activity` template (bridge/include/script_templates.h kActivity) runs, in the ruler's own
scope, each AI condition of the three activity files (word for word) and the game's own
`can_host_activity`, and, for a host call, the effect, guarded by can_host_activity. Nothing here
creates an activity: the game's AI does, or does not, at its next pass.
"""
from __future__ import annotations

from typing import Any, Optional

#: The seat's name -> the game's activity type (THE set: script_templates.h kActivityTypes).
ACTIVITIES = {"feast": "activity_feast", "hunt": "activity_hunt", "pilgrimage": "activity_pilgrimage"}
_WORD = {"feast": "FEAST", "hunt": "HUNT", "pilgrimage": "PILGRIMAGE"}
#: The AI value a COUNT must hold at 100 for the game's AI to host it (the type's "Throttle Counts").
AXIS = {"feast": "sociability", "hunt": "boldness", "pilgrimage": "zeal"}

#: What each activity gives and costs as its file states it (1.18.3.1). Amounts named, never summed.
FACTS = {
    "feast": {
        "gives": ("prestige to you as host by the food the AI picks: 35 (bad), 115 (normal) or 225 (good), "
                  "more with a brewmaster, a refined-feasts culture or a royal court's food; opinion of the guests "
                  "(disburse_feast_activity_rewards, common/scripted_effects/00_activity_effects.txt)"),
        "cooldown": ("5 years between feasts (standard_feast_cooldown_time, 00_activity_values.txt; halved "
                     "for a culture with more_frequent_feasts)"),
        "ai_score": ("30 - greed/4 + sociability/2 (+ energy when below 0, traits; feast.txt ai_will_do)"),
    },
    "hunt": {
        "gives": ("prestige: 75 x the quarry (x2 hare or fox, x6 boar, hart, elk, bison, aurochs, x8 bear, lion, "
                  "tiger, leopard or an exotic bird, x4 any other), among other modifiers: halved for a failed hunt "
                  "or a female quarry, x1.25 or x1.5 by the party's size, x2 for a legendary hunt or falconry "
                  "(hunt_prestige_reward_value, 04_ep2_hunt_values.txt); half of it as piety for a culture "
                  "with sacred hunts"),
        "cooldown": ("2 years between hunts (standard_hunting_cooldown_time, 04_ep2_hunt_values.txt; halved for a "
                     "culture with more_frequent_hunts)"),
        "ai_score": ("-10 (-30 for a count) - greed/2 + boldness + sociability/2 (+ energy when below 0, "
                     "traits; hunt.txt ai_will_do)"),
    },
    "pilgrimage": {
        "gives": ("piety and the pilgrim trait (give_pilgrimage_reward_effect, 00_pilgrimage_effects.txt; "
                  "its amount is not one number)"),
        "cooldown": ("no cooldown key: each holy site waits 5 years after a pilgrimage to it "
                     "(pilgrimage.txt can_start)"),
        "ai_score": "pilgrimage.txt ai_will_do (zeal and traits; not summed here)",
    },
}

#: Each mark of the template's listing: what the game's condition is (its file's words) and the lever.
GATES = {
    "NOT_AI": ("your ruler is not run by the game's AI: the effect refuses a character the AI does not run",
               None),
    "TIER_BARONY": ("a baron hosts none of these (highest_held_title_tier > tier_barony)", None),
    "EARLY": ("the game's AI hosts none of these in the first 3 years after the game's start "
              "(years_from_game_start >= 3)", None),
    "AT_WAR": ("the game's AI hosts none of these while its ruler is at war (is_at_war = no)", None),
    "BUILD_FOCUS": ("your capital has a building slot to fill (a free slot, or fewer buildings than your "
                    "culture's innovations allow): the game's AI builds there before it hosts a feast or a "
                    "hunt (ai_should_focus_on_building_in_their_capital = no)",
                    {"tool": "do", "args": {"action_class": "buildings",
                                            "args": {"location_id": "<your capital's barony>"}}}),
    "CONQUEROR": ("a conqueror goes on no pilgrimage (NOT has_variable = conqueror)", None),
    "UNCROWNED": ("an uncrowned ruler's AI hosts nothing until he is crowned (has_realm_law = uncrowned)",
                  None),
    "NOT_AVAILABLE_ADULT": ("your ruler is not an available adult (a child, imprisoned, incapable or away)",
                            None),
    "IN_ACTIVITY": ("your ruler is in an activity now", None),
}
#: The game's
#: ai_should_focus_on_building_in_their_capital (common/scripted_triggers/00_ai_value_triggers.txt) reads
#: capital_county.title_province, the SEAT of your capital county, and holds when one of these does; the
#: bridge's activity template writes each that holds (script_templates.h kBuildFocusBranches)
BF_BRANCHES = {
    "BF_FREE_SLOT": ("the seat of your capital county has a free building slot (free_building_slots > 0): "
                     "a building in it clears this"),
    "BF_LEDGER": ("your culture has innovation_ledger and the buildings at the seat of your capital county "
                  "total level 4 or less (combined_building_level <= 4): building or upgrading there past 4 "
                  "clears this"),
    "BF_BALIFFS": ("your culture has innovation_baliffs and the buildings at the seat of your capital county "
                   "total level 6 or less: past 6 clears this"),
    "BF_GUILDS": ("your culture has innovation_guilds and the buildings at the seat of your capital county "
                  "total level 9 or less: past 9 clears this"),
    "BF_CRANES": ("your culture has innovation_cranes and the buildings at the seat of your capital county "
                  "total level 12 or less: past 12 clears this"),
}
BF_UNKNOWN = ("unknown: this bridge does not name which part of the condition holds (no BF_ mark); the "
              "condition reads the seat of your capital county: a free building slot there, or its buildings' "
              "total level at most 4/6/9/12 by your culture's innovations")


async def build_focus(gw, me: int, marks: list) -> Optional[dict]:
    """The BUILD_FOCUS gate's words and fix for this ruler: which part of the game's condition
    holds (the template's BF_ marks), at which barony (query_realm capital_barony_id, the seat when the
    template says the seat is the capital province, else the first holding of the capital's county, as the
    bridge reads a county), and the read that lists what can be built there with the ready build calls."""
    m = set(marks or [])
    if "BUILD_FOCUS" not in m:
        return None
    held = [w for w in BF_BRANCHES if w in m]
    out: dict = {"holds": [BF_BRANCHES[w] for w in held] if held else BF_UNKNOWN}
    realm = await gw.read("query_realm", {"character_id": me})
    cap = realm.get("capital_barony_id") if isinstance(realm, dict) else None
    seat = None
    if isinstance(cap, int) and cap > 0:
        if "BF_SEAT_IS_CAPITAL" in m:
            seat = {"barony_title_id": cap, "basis": "your capital barony is the seat of your capital county"}
        else:
            import buildings_read as BR  # noqa: PLC0415
            details = await gw.read("query_county_details", {"character_id": me})
            county, _h = BR.find_county(details, cap)
            hold = [h for h in (county or {}).get("holdings") or [] if isinstance(h, dict)]
            if county and hold:
                seat = {"barony_title_id": hold[0].get("barony_title_id"), "county": county.get("key"),
                        "basis": ("the first holding of your capital's county, as the bridge reads a county's "
                                  "seat (the template did not read it as your capital barony)")}
    if seat is None:
        out["seat"] = "unknown: your capital barony was not read (query_realm capital_barony_id)"
        out["fix"] = {"tool": "query", "args": {"domain": "county"}}
        return out
    out["seat"] = seat
    out["fix"] = {"tool": "query", "args": {"domain": "buildings", "title_id": seat["barony_title_id"]},
                  "what": "what can be built at that seat, each with the game's price and the ready build call"}
    return out


BF_SAYS = ("the game's AI builds at the seat of your capital county before it hosts a feast or a hunt "
           "(ai_should_focus_on_building_in_their_capital): ")
#: The gates a stated lever can clear this turn; the others wait for the game.
_LEVER_GATE = {"COOLDOWN": "cooldown_active", "AT_WAR": "not_at_peace"}


def _R():
    import redesign  # noqa: PLC0415 — redesign imports this module
    return redesign


def base_price(tier: Any) -> Optional[dict]:
    """The short-term gold the game's AI must hold before it weighs the activity: feast_activity_cost,
    hunt_activity_base_cost and pilgrimage_base_cost are each standard_activity_base_cost
    (00_activity_values.txt): 30 x (the primary title's tier - 1, at least 1), +50 above a duchy."""
    if not isinstance(tier, int) or isinstance(tier, bool) or tier < 1:
        return None
    gold = 30 * max(1, tier - 1) + (50 if tier > 3 else 0)
    return {"gold": gold,
            "basis": ("standard_activity_base_cost: 30 x (your tier - 1, at least 1), +50 above a duchy; the "
                      "AI weighs it only with this much short-term gold"),
            "unread": ("x0.5 for a tribal ruler; feast x2 for a culture with more_expensive_feasts; a hunting "
                       "falcon or master of the hunt x0.9 (each not read)"),
            "charged": ("the price the game charges when the AI plans it is this base times your culture "
                        "era's scale plus the options the AI picks (food, decoration, entourage): read in the "
                        "purse once it starts")}


def _rows(marks: list, purse_budget: Optional[dict], tier: Any, bf: Optional[dict] = None) -> tuple[list, list]:
    """(available, unavailable) rows from the template's marks; `bf` is build_focus's reading."""
    R = _R()
    m = set(marks)
    price = base_price(tier)
    short = (purse_budget or {}).get("short_term")
    common = [w for w in GATES if w in m]
    available, unavailable = [], []
    for name, typ in ACTIVITIES.items():
        W = _WORD[name]
        gates = []
        for w in common:
            if w == "BUILD_FOCUS" and name == "pilgrimage":
                continue
            if w == "CONQUEROR" and name != "pilgrimage":
                continue
            words, fix = GATES[w]
            if w == "BUILD_FOCUS" and bf:
                # Which part holds, at which barony, and the read that lists the builds there
                words = BF_SAYS + "; ".join(bf["holds"]) if isinstance(bf["holds"], list) else BF_SAYS + bf["holds"]
                fix = bf.get("fix")
                gates.append({"condition": "build_focus", "game_says": words, "seat": bf.get("seat"),
                              **({"fix": fix} if fix else {})})
                continue
            gates.append({"condition": w.lower(), "game_says": words, **({"fix": fix} if fix else {})})
        if f"{W}_COOLDOWN" in m:
            gates.append({"condition": "cooldown", "game_says": f"the game's cooldown on {typ} runs",
                          "cooldown": FACTS[name]["cooldown"]})
        if "COUNT_TIER" in m and f"{W}_AXIS_BELOW_100" in m:
            ax = AXIS[name]
            gates.append({"condition": f"ai_{ax}_below_100",
                          "game_says": (f"the game's AI hosts a {name} for a count only with ai_{ax} >= 100 "
                                        f"({typ.split('_', 1)[1]}.txt, Throttle Counts)"),
                          "fix": {"tool": "steer", "args": {"personality": {ax: 100}}}})
        gold = {"short_term_gold": short, **(price or {"price_unread": "your primary tier was not read"})}
        can = f"{W}_CAN_HOST" in m
        if price and isinstance(short, (int, float)) and short < price["gold"] and can:
            # A live check, 1069.09.18 (ruler 29803): the game's own check said yes below this estimate; the
            # game's value is rooted at the ruler and holds factors not read here: its verdict stands
            gold["estimate_below"] = ("short-term gold reads below this estimate, and the game's own "
                                      "can_host_activity says yes: the game's verdict stands")
        elif price and isinstance(short, (int, float)) and short < price["gold"]:
            gates.append({"condition": "short_term_gold", "game_says": (
                f"short-term gold {round(short, 2)} below the base price {price['gold']}: the game's AI weighs "
                "it only when its short-term gold covers that"),
                "fix": {"tool": "do", "args": {"intent": "gold_reserve", "args": {"amount": 0}},
                        "what": "a reserve holds gold out of short term; amount 0 moves it back"}})
        row = {"activity": name, "type": typ, "game_can_host": can, "gold": gold,
               "gives": FACTS[name]["gives"], "cooldown": FACTS[name]["cooldown"],
               "ai_score": FACTS[name]["ai_score"],
               "do_call": {"intent": "host_activity", "args": {"activity": name}}}
        if can and not gates:
            available.append(row)
            continue
        if gates:
            row["gates"] = gates
            first = gates[0]
            gate = _LEVER_GATE.get(first["condition"].upper(), "ai_potential_not_met")
            if first["condition"] == "short_term_gold":
                gate = "insufficient_gold"
            row["blocked"] = R.why_blocked(gate, summary=first["game_says"], raw_reason="activity_gate",
                                           fix=first.get("fix"))
        else:
            row["blocked"] = R.why_blocked("engine_refused", raw_reason="can_host_activity_false",
                                           summary=(f"the game's can_host_activity says no for {typ} and none of "
                                                    "the AI conditions read here failed: the type's own can_start "
                                                    "holds a condition this does not read"))
        unavailable.append(row)
    return available, unavailable


#: The bridge's kInvolvedActivityTypes (bridge/include/script_templates.h), in its order: the listing marks
#: IN_T_<index> for the type of the activity the ruler is in (test_defeat_prices checks the two lists agree).
INVOLVED_ACTIVITY_TYPES = (
    "activity_camp_party", "activity_chariot_race", "activity_coronation", "activity_debate", "activity_feast",
    "activity_festival", "activity_funeral", "activity_gruesome_festival", "activity_roaming", "activity_hunt",
    "activity_imperial_examination", "activity_survey", "activity_local_examination", "activity_journey",
    "activity_pilgrimage", "activity_playdate", "activity_tournament", "activity_tour", "activity_adult_education",
    "activity_wedding", "activity_witch_ritual")
IN_ACTIVITY_END_UNREAD = ("its end date is not read: no script trigger the bridge's console can log gives it; "
                          "do(action_class='activities') shows whether he is still in it")
#: Recorded games refused declarations for nine turns while the ruler was a guest at a coronation, and a
#: kingdom claim war until the ruler died. What the activity blocks (the game's files,
#: 1.18.3.1: `exists = involved_activity` in common/character_interactions) and the way out
IN_ACTIVITY_BLOCKS = ("it blocks declaring war (00_war.txt) and some tributary and artifact interactions; not peace "
                      "offers, ally calls or other sends")
#: the name do(intent='host_activity') takes for leaving (args.activity=leave)
LEAVE = "leave"
LEAVE_CALL = {"tool": "do", "args": {"intent": "host_activity", "args": {"activity": LEAVE}}}
#: re/findings/re_activity_invites.md: the game's AI answers an invitation for its ruler inside the activity's own
#: update (accept/decline lists applied by the activity, no command posted), so no ai_veto kind can drop it
INVITES_NO_VETO = ("the game's AI accepts invitations for your ruler inside the activity's own update, not by a "
                   "command: no ai_veto kind stops it; leave after")
LEAVE_UNPROVEN = "built, not proven live"


def in_activity(marks: list) -> Optional[dict]:
    """The activity the ruler is in now, from the listing's marks (None: none)."""
    m = set(marks)
    if "IN_ACTIVITY" not in m:
        return None
    typ = next((ACTIVITIES[n] for n, W in _WORD.items() if f"IN_{W}" in m), None)
    if typ is None:
        typ = next((t for i, t in enumerate(INVOLVED_ACTIVITY_TYPES) if f"IN_T_{i}" in m),
                   "another activity type (an older bridge names only feast, hunt and pilgrimage)")
    out = {"type": typ, "you_host": "HOSTING" in m, "ends": IN_ACTIVITY_END_UNREAD, "blocks": IN_ACTIVITY_BLOCKS}
    if not out["you_host"]:
        # A guest goes home by the game's own "Go home" effects (activity_system.0400)
        out["leave"] = dict(LEAVE_CALL, proven=LEAVE_UNPROVEN)
    return out


async def current(gw, me: int) -> tuple:
    """A recorded case (1075.03-10: two declarations refused involved_activity after the pre-send
    check passed; the AI had started a coronation journey): (the activity the ruler is in now or None,
    why it is unread or None), from the activity listing's marks."""
    sent = await _listing(gw, me)
    bad = _unread_listing(sent)
    if bad is not None:
        return None, bad
    return in_activity(sent["marks"]), None


EVENTS_NOTE = ("the activity's events are answered by the game's AI for your ruler, unless you set a standing "
               "answer with do(intent='event_rule')")


async def _listing(gw, me: int, host: Optional[str] = None, leave: bool = False) -> dict:
    R = _R()
    params = {"actor_id": me}
    if host:
        params.update(activity=ACTIVITIES[host], host=True)
    if leave:
        params["leave"] = True
    return await R._script_write(gw, "activity", params)


async def do_leave_activity(gw, me: int, actor) -> dict:
    """do(intent='host_activity', args={activity: leave}): the guest's way home, the game's own effects
    (remove_from_activity = involved_activity, return_home = yes; bridge template `activity` with leave), guarded by
    the game: in an activity and not its host. Read back by a second listing. Built, not proven live."""
    R = _R()
    sent = await _listing(gw, me, leave=True)
    raw = sent.get("raw") or {}
    was = in_activity(sent.get("marks") or [])
    base = {"intent": "host_activity", "activity": LEAVE, "was_in": was, "path": "script", "token": raw.get("token"),
            "guard": sent.get("guard"), "game_errors": sent.get("errors") or [], "proven": LEAVE_UNPROVEN}
    if sent.get("guard") == "refused":
        why = ("your ruler hosts it: a host cannot leave his own activity" if was and was.get("you_host") else
               "your ruler is in no activity")
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), bridge_health=gw.health,
                          result=base, blocked=R.write_blocked(gate="already_in_that_state" if not was else
                                                               "invalid_argument", raw_reason="leave_refused",
                                                               summary=f"the game's guard refused: {why}"))
    bad = _unread_listing(sent)
    if bad is not None:
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), bridge_health=gw.health,
                          result=base, blocked=R.write_blocked(gate="bridge_fault", raw_reason="activity_unread",
                                                               summary=f"the leave was not read: {bad}"))
    after = await _listing(gw, me)
    bad = _unread_listing(after)
    if bad is not None:
        return R.envelope("do", verdict="pending", actor=actor, game=gw.game(), bridge_health=gw.health,
                          result=dict(base, after=f"unread: {bad}"),
                          pending=R.write_pending("clock_advances", game_date_at_submit=(gw.game() or {}).get("date"),
                                                  expected_change="your ruler in no activity",
                                                  raw_reason="leave_read_back_unread",
                                                  why_pending="the game ran the leave; the listing after it was not read",
                                                  verify_with="do(action_class='activities') in_activity"))
    now = in_activity(after.get("marks") or [])
    if now is None:
        return R.envelope("do", verdict="applied", actor=actor, game=gw.game(), bridge_health=gw.health,
                          result=dict(base, after="in no activity: a declaration is no longer refused for it"))
    return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), bridge_health=gw.health,
                      result=dict(base, after=now),
                      blocked=R.write_blocked(gate="write_not_applied", raw_reason="still_in_activity",
                                              summary="the game ran the leave and the listing still reads him in an activity"))


async def _purse_reads(gw, me: int) -> tuple:
    import seat_events  # noqa: PLC0415
    ch = await gw.read("query_character", {"character_id": me})
    ch = ch if isinstance(ch, dict) else {}
    bud = seat_events.budget_row(await gw.read("query_ai_budget", {"character_id": me}))
    return ch, bud


def _unread_listing(sent: dict) -> Optional[str]:
    raw = sent.get("raw") or {}
    if raw.get("success") is not True:
        return str(raw.get("reason") or raw.get("error") or "script_action_failed")
    if sent.get("guard") not in ("ran", "refused"):
        return f"the template's marker was not read ({sent.get('guard')})"
    return None


async def enumerate_activities(gw, me: int, actor) -> dict:
    """do(action_class='activities'): each activity with the game's verdict, the gates that fail, its price,
    cooldown and what it gives, and the do_call."""
    R = _R()
    sent = await _listing(gw, me)
    bad = _unread_listing(sent)
    if bad is not None:
        return R.envelope("do", ok=False, actor=actor, game=gw.game(), bridge_health=gw.health,
                          blocked=R.why_blocked("bridge_fault", raw_reason="activity_listing_unread",
                                                summary=f"the activity listing was not read: {bad}"))
    ch, bud = await _purse_reads(gw, me)
    available, unavailable = _rows(sent["marks"], bud, ch.get("ruler_tier"), await build_focus(gw, me, sent["marks"]))
    result = {"action_class": "activities", "available": available, "unavailable": unavailable,
              "how": ("host_activity puts the type to the game's AI for your ruler: at its next monthly pass "
                      "it weighs every activity it may host and hosts the best with a chance of its own score "
                      "(ai_score) in 100, picks the options, place and guests, and pays"),
              "events": EVENTS_NOTE, "game_errors": sent.get("errors") or []}
    now = in_activity(sent["marks"])
    if now:
        result["in_activity"] = now
    return R.envelope("do", result=result, actor=actor, game=gw.game(), bridge_health=gw.health)


async def do_host_activity(gw, a: dict, character_id: int, actor) -> dict:
    """do(intent='host_activity', args={activity: feast|hunt|pilgrimage})."""
    R = _R()
    import news as _news  # noqa: PLC0415
    me = R._as_int(character_id) or R._as_int((actor or {}).get("id"))
    name = str(a.get("activity") or "")
    if name == LEAVE:
        return await do_leave_activity(gw, me, actor)
    if name not in ACTIVITIES:
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), bridge_health=gw.health,
                          blocked=R.write_blocked(gate="invalid_argument", raw_reason="bad_activity",
                                                  summary=f"args.activity is one of {', '.join(ACTIVITIES)}, {LEAVE}",
                                                  fix={"rejected_fields": ["activity"],
                                                       "enumerate": {"tool": "do",
                                                                     "args": {"action_class": "activities"}}}))
    ch, bud = await _purse_reads(gw, me)
    sent = await _listing(gw, me, host=name)
    raw = sent.get("raw") or {}
    rows = _rows(sent.get("marks") or [], bud, ch.get("ruler_tier"),
                 await build_focus(gw, me, sent.get("marks") or []))
    row = next(r for r in rows[0] + rows[1] if r["activity"] == name)
    base = {"intent": "host_activity", "activity": name, "type": ACTIVITIES[name], "gives": row["gives"],
            "gold": row["gold"], "events": EVENTS_NOTE, "path": "script", "token": raw.get("token"),
            "guard": sent.get("guard"), "game_errors": sent.get("errors") or []}
    if sent.get("guard") == "refused" or row.get("blocked"):
        blk = row.get("blocked") or R.why_blocked("engine_refused", raw_reason="can_host_activity_false",
                                                  summary=f"the game's can_host_activity says no for {ACTIVITIES[name]}")
        if sent.get("guard") == "ran":
            # the game put it to the AI; a condition read here says the AI will not host it
            base["sent_to_ai"] = True
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), bridge_health=gw.health,
                          result=dict(base, gates=row.get("gates") or []), blocked=blk)
    bad = _unread_listing(sent)
    if bad is not None:
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), bridge_health=gw.health,
                          result=base, blocked=R.write_blocked(gate="bridge_fault", raw_reason="activity_unread",
                                                               summary=f"the host call was not read: {bad}"))
    _news.note_act(me, (gw.game() or {}).get("date"), f"host_activity {name} (put to your ruler's AI)",
                   cost_known=False, unpriced_kind="activity")
    spared = await _spare_under_veto(gw, me, ACTIVITIES[name])
    pend = R.write_pending("clock_advances", game_date_at_submit=(gw.game() or {}).get("date"),
                           expected_change=f"your ruler hosts {ACTIVITIES[name]} (in_activity in "
                                           "do(action_class='activities'))",
                           raw_reason="ai_decides_at_its_monthly_pass",
                           why_pending=("the game took it: its AI weighs it at its next monthly activity pass and "
                                        "hosts the best activity it may host with a chance of its score in 100; "
                                        "one call is one weighing"),
                           verify_with="do(action_class='activities') in_activity, after a month")
    return R.envelope("do", verdict="pending", actor=actor, game=gw.game(), bridge_health=gw.health,
                      result=dict(base, ai_score=row["ai_score"], sent_to_ai=True,
                                  **({"activity_veto": spared} if spared else {})), pending=pend)


async def _spare_under_veto(gw, me: int, typ: str) -> Optional[dict]:
    """The game's AI hosts what host_activity puts to it through the same post the activity veto drops; while
    that veto is set, the type is spared (set_ai_veto spare_keys), and read back. None when no activity veto is set."""
    import seat_events as _se  # noqa: PLC0415
    R = _R()
    cur = _se._veto_ruler(_se._d(R._unwrap_reply(await gw.read("query_ai_veto", {"character_id": me}))), me) or {}
    if "activity" not in (cur.get("vetoes") or []):
        return None
    params = {"character_id": me, "op": "set", "what": "activity", **(await _se.activity_spare_params(gw, me, typ))}
    raw = _se._d(R._unwrap_reply(await gw.write("set_ai_veto", params)))
    back = _se._veto_ruler(_se._d(R._unwrap_reply(await gw.read("query_ai_veto", {"character_id": me}))), me) or {}
    keys = list(back.get("activity_spare_keys") or [])
    if raw.get("success") is not True or typ not in keys:
        return {"spared": keys, "not_spared": str(raw.get("reason") or "readback_disagrees"),
                "note": f"the activity veto drops {typ} when your ruler's AI starts it: do ai_veto op=clear what=activity"}
    return {"spared": keys, "note": _se.ACTIVITY_SPARED.format(key=typ)}
