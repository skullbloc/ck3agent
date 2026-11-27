"""observe's `news`: what changed for the seat since its last observe.

Nothing told a seat that a fabricated claim arrived, that a war started or ended,
that its liege or heir changed, or that a councillor's task changed. This block is
a DIFF of the seat's own readings between two observes of this server, so every
row is a fact two reads witnessed, never an inference:

  claims_gained / claims_lost   query_claims, by title id
  wars_started / wars_ended     get_active_wars, by war id; an ended war's outcome
                                and end date from query_war_history (the bridge's
                                war-end ledger), null with `outcome_unread` when the
                                ledger did not answer
  ruler_changed                 the character observe answered for is not the one it
                                answered for last time (a death and succession, or a
                                session(control) switch)
  liege_changed, heir_changed   query_character liege_id; the succession verdict's
                                primary heir
  council_changed               query_council: a seat whose holder or task changed
                                (a task that finished and one that was replaced read
                                the same: the reading cannot tell them apart); a
                                fabrication this server set that left the seat names
                                its title and the fabricate call for the next one
  schemes_ended                 query_schemes, by scheme id; no read gives the outcome
                                (`outcome_unread`), `next` reads the target's opinion
  titles_gained / titles_lost   query_title_hierarchy held_titles, by title id; `how` is a
                                war ended in the same diff whose target titles hold it
                                (a barony gained in the same diff as its de-jure county
                                takes the county's, `with_county`), else null with
                                `how_unread` (inheritance, grant and creation read the same)

It also reports:
  fabrication                   query_council_tasks: a fabricate task running (on the title
                                this server set when its province matches, else unknown;
                                since, days; progress unknown), or the one this server set
                                no longer running, with the call that sets it again
  purse                         query_character prestige, piety, gold that moved, with the
                                acts this server sent whose cost it read; else cause unknown
  wars_at_we_win                get_active_wars: a war whose total reached what we_win needs
                                for your side, with the end_war call
and a second baseline: observe(depth="news") diffs against its own last read ("turn"),
the seat's observe against its own ("observe"); compact() is the per-turn form.

claims_gained carries the claim war's declare_args when the casus-belli
read (redesign.declarable_now, the read cbs answers) says it is declarable now.

Each row carries `next`, a ready call to read or act on it. The first observe of a
server has nothing to compare against and says so (`first_observe: true`); a read
that did not answer this time or last time makes its rows `unread`, never empty.
Offers waiting for an answer are `incoming_proposals` (served when the episode
declares the proposal mod); `offers` here says which.
"""
from __future__ import annotations

from typing import Any, Optional

import seat_events as _seat_events
import faction_letters as _faction_letters
import map_view as _map_view
import ruler_death as _rd  # The act-now condition of the split row
import threats as _threats  # Who can declare on you
import task_goal as _task_goal  # The task's goal kind (a peaceful goal's news offers no war)
import diarchy as _diarchy  # The ruler's diarch

#: (baseline, requested character id (0 = the primary)) -> the last snapshot. Two
#: baselines: "observe", the seat's own observe; "turn", observe(depth=
#: "news"), the read an environment makes once per turn. Neither moves the other.
_LAST: dict = {}
BASELINES = ("observe", "turn")
#: (baseline, requested id) -> (date, the block shown then)
_SHOWN: dict = {}
#: Per ruler, the newest read of the kingdom's counties in his realm, by any baseline (date, {county id: holder}):
#: the counties a succession took are counted from his last read, not from the last read of the baseline asking (an
#: observe whose last read was five years before the death counted 7 -> 6 where the turn's news, a step before it,
#: counted 9 -> 6)
_KINGDOM_COUNTIES_BY: dict = {}
#: Per ruler, the wars any read listed ({war id: first date listed}), whichever baseline read it
_WARS_LISTED: dict = {}


def reset() -> None:
    _CULTURE_WARNED.clear()
    _VETO_CARRIED.clear()
    _RULES_CARRIED.clear()
    _REFUSED_SENDS.clear()
    _CREATION_PRICE.clear()
    _CREATION_PRICE_BY.clear()
    _PRICE_BASIS_BY.clear()
    _HOF_CLAIM_OK.clear()
    _LAST.clear()
    _SHOWN.clear()
    _KINGDOM_COUNTIES_BY.clear()
    _WARS_LISTED.clear()
    _map_view.reset()
    _threats.reset()
    _THREAT_READ.clear()
    _THREAT_SEEN.clear()
    _TURN_ONE_SERVED.clear()
    _WAR_TITLES.clear()
    _COUNTY_CAUSE.clear()
    _FREE_WARNED.clear()
    _REVERTS.clear()
    _LOSS_FIRST.clear()
    _MERC_SEEN.clear()
    _ACTS.clear()
    _FAB_SEEN.clear()
    _FAB_PROGRESS.clear()
    _CALLS.clear()
    _FAB_CHAPLAIN.clear()
    _ACCEPT_NO.clear()
    _ASKS.clear()
    _VJ_NAMED.clear()
    _DEATHS_SEEN.clear()
    _seat_events.SPARED_CHAIN.clear()
    _seat_events.SPARED_EVER.clear()


def _d(x) -> dict:
    return x if isinstance(x, dict) else {}


def _i(v) -> Optional[int]:
    return v if isinstance(v, int) and not isinstance(v, bool) else None


#: actor id -> the fabrication this server set {title_id, title_key, seat, since}
_FABRICATING: dict = {}


def note_fabrication(actor_id, title_id, title_key, seat, since) -> None:
    if actor_id and title_id:
        _FABRICATING[actor_id] = {"title_id": title_id, "title_key": title_key, "seat": seat,
                                  "since": since}


def fabrication_of(actor_id) -> Optional[dict]:
    return _FABRICATING.get(actor_id)


def note_fabrication_province(actor_id, province_id) -> None:
    """The province the game stored as the target of the fabrication this server just
    set (read back from query_council_tasks): the one way this server can tell, later,
    that the task running is the one it set (no read maps a province to its county)."""
    fab = _FABRICATING.get(actor_id)
    if fab is not None and isinstance(province_id, int):
        fab["province_id"] = province_id


#: Actor id -> {province_id, since}: the first date this server read the
#: fabricate task running on that province (any read: observe, query council_tasks).
_FAB_SEEN: dict = {}

#: Actor id -> {province_id, date, progress}: the first progress read on the running
#: task (since its last restart), the base of the measured rate
_FAB_PROGRESS: dict = {}


#: A live 1066 game (1066.09.15 -> 1067.11.24, Murchad's chaplain on c_athlone): the progress read moved in steps of
#: the whole monthly rate once every 30 days (0 -> 7.02 -> 14.04 -> 35.10 -> 56.16 -> 77.22 -> 98.28, the
#: claim on the 15th step, day 435), not by rate / 30 each day as the game's own documentation says.
#: A rate per day from two reads is then off by the part-month between them (live: 429 days left
#: measured at 1066.11.24, 365 came); counting whole steps is not.
PROGRESS_STEPS = ("the game adds the task's monthly rate in one step every 30 days; days left = "
                  "the steps still needed x 30, at most")


def _progress_measure(actor_id, prov, pct: float, date, monthly=None) -> dict:
    """The rate measured between the first progress read on this task and now, and the days
    left at it; {} on the first read, when the date did not move, or after a drop (the task
    restarted: the measure starts again). Live: the progress moves in monthly steps, so the
    step is the progress gained over the whole steps between the reads (counted with the
    expected monthly rate when known, else the 30-day periods), and the days left are the
    steps still needed x 30."""
    base = _FAB_PROGRESS.get(actor_id)
    if not base or base.get("province_id") != prov or pct < base["progress"]:
        _FAB_PROGRESS[actor_id] = {"province_id": prov, "date": date, "progress": pct}
        return {}
    days = _days(base["date"], date)
    if not isinstance(days, int) or days <= 0 or pct <= base["progress"]:
        return {}
    gained = pct - base["progress"]
    per_day = round(gained / days, 4)
    import math
    steps = (round(gained / monthly) if _isnum(monthly) and monthly > 0 else round(days / 30.0)) or 1
    step = gained / steps
    return {"progress_rate": {"per_day": per_day, "from": base["date"], "days": days, "gained": round(gained, 4),
                              "monthly_step": round(step, 2), "steps": steps, "how": PROGRESS_STEPS},
            "days_left_measured": int(math.ceil(round((100.0 - pct) / step, 6))) * 30}


#: Two live fabrications on this toolkit's 1066 start (Murchad of Dublin's
#: chaplain on c_athlone, set 1066.09.15) gave the claim on 1067.11.27, 438 days.
#: The rule below is the task's own progress block (FABRICATION_RULE's sources).
FABRICATION_MEASURED = ("the game's rule, not a measurement: while the task runs it adds 3 + the chaplain's "
                        "learning / 5 percent a month, x0.5 off your border, x0.3 on a county held inside your "
                        "realm, x0.8 with a chaplain who is your rival and x0.7 with your nemesis (no read gives the "
                        "relation), x1.3 against another faith, more with the terms that add; a claim comes at 100, "
                        "so with none of those slowing terms (border, realm, rival, nemesis) in at most 100 / (3 + "
                        "the chaplain's learning / 5) months; some take longer (the task is reset) or never come")
FABRICATION_PROGRESS_UNREAD = ("unknown: the game keeps a monthly progress on the task, and the bridge "
                               "reads none (no offset for it is known)")
FABRICATE_TASK = "task_fabricate_claim"

#: (Breifne ran 1,460 days and Athlone ~46 months with no claim, read
#: `progress: null`). The game's task progress is not read (no offset for the active council
#: task's progress is known: the CActiveCouncilTask layout is unmapped,
#: re/findings/re_county_conversion_mechanics.md); the rate it adds each month is the task's
#: own `progress` block (common/council_tasks/00_court_chaplain_tasks.txt task_fabricate_claim,
#: task_progress_percentage: done at 100): court_chaplain_fabricate_claim_base 3 plus the
#: chaplain's learning / 5 (99_court_chaplain_values.txt), x0.5 on a county not bordering your
#: realm (court_chaplain_fabricate_claim_not_adjacent_penalty), x0.3 on a county whose holder has you
#: as liege or above (court_chaplain_fabricate_claim_vassal_penalty), x1.3 against another faith
#: (court_chaplain_heretic_bonus). Two terms slow it and are not read: a chaplain who is your rival
#: adds -20% of the base, your nemesis -30% (council_rival/nemesis_impact_percentage,
#: 00_council_values.txt). Every other term adds (friends, perks, innovations). So 100 / the rate is
#: the most months for a chaplain who is neither, and 100 / (0.7 x the rate) the most for any
#: chaplain: `overdue` is judged against that, and only when the holder's place was read.
FABRICATION_BASE_RATE = 3.0
FABRICATION_VASSAL = 0.3
FABRICATION_NEMESIS = 0.7
FABRICATION_RULE = ("the most months the game's fabrication can take: 100 / (3 + the chaplain's learning / 5, "
                    "x0.5 off your border, x0.3 on a county held inside your realm, x1.3 against another faith), "
                    "its own progress block (00_court_chaplain_tasks.txt), for a chaplain who is neither your "
                    "rival (x0.8) nor your nemesis (x0.7), which no read gives; every other term adds. For a county "
                    "inside your realm the figure is for the x0.3 term alone and may be longer (the files do not "
                    "say whether the game also counts it as off your border). overdue is judged against the most a "
                    "nemesis allows (that rate x0.7), only once the county's holder was read and only for a county "
                    "outside your realm: past it the task was reset (re-tasked, a new chaplain) or cannot finish")
#: Said beside `expected` when the county's holder was not read
FABRICATION_HOLDER_UNREAD = ("whether the county is held inside your realm (the game's x0.3 there) was not read: "
                             "the rate is the one outside it, and overdue is not judged")
#: Said beside `expected` for a county inside your realm: the task's x0.5 applies to a county that is NOT
#: is_neighbor_to_realm of your ruler, an engine trigger no game file defines, so whether a county inside the realm
#: also takes it is not determinable from the files
FABRICATION_IN_REALM_UNREAD = ("the county is inside your realm: the rate has the game's x0.3 there and not its x0.5 "
                               "off your border, and whether the game also applies that x0.5 to a county inside "
                               "your realm is not known (its files do not define the border trigger), so the months "
                               "may be up to twice these; overdue is not judged")


#: x1.3 when the county's holder is of another faith than your ruler (court_chaplain_heretic_bonus,
#: 99_court_chaplain_values.txt). Live: learning 12, Athlone's holder insular_celtic against catholic:
#: (3 + 12/5) x 1.3 = 7.02 % a month, the step read live exactly.
FABRICATION_OTHER_FAITH = 1.3


def fabrication_expected(learning: Any, borders: Any, other_faith: Any = None, in_realm: Any = None) -> Optional[dict]:
    """{monthly_pct_at_least, months_at_most, read} from the chaplain's learning, whether the
    county borders the realm (None: unread, priced at the slower rate), whether its holder is
    of another faith (x1.3; None: unread, not applied) and whether its holder has your ruler as
    liege or above (x0.3; None: unread, not applied), or None without learning. Both figures are
    for a chaplain who is neither your rival nor your nemesis (not read; FABRICATION_RULE)."""
    if not isinstance(learning, (int, float)) or isinstance(learning, bool):
        return None
    rate = FABRICATION_BASE_RATE + learning / 5.0
    if borders is not True:
        rate *= 0.5
    if other_faith is True:
        rate *= FABRICATION_OTHER_FAITH
    if in_realm is True:
        rate *= FABRICATION_VASSAL
    import math
    return {"monthly_pct_at_least": round(rate, 2), "months_at_most": int(math.ceil(round(100.0 / rate, 6))),
            "learning": learning, "borders_your_realm": borders,
            **({"holder_other_faith": other_faith} if isinstance(other_faith, bool) else {})}


def fabrication_overdue_after(exp: dict) -> Optional[int]:
    """The most months any chaplain's fabrication can take at `exp`'s rate: a nemesis's (x0.7, the
    slowest relation term)."""
    rate = exp.get("monthly_pct_at_least") if isinstance(exp, dict) else None
    if not _isnum(rate) or rate <= 0:
        return None
    import math
    return int(math.ceil(round(100.0 / (rate * FABRICATION_NEMESIS), 6)))


#: (after each succession the fabrication read target "unknown", the
#: seat set the same task again, and the claim on Athlone came 17 months after the last
#: re-set): what a succession does to a running fabrication, as far as any read shows.
FABRICATION_ACROSS_SUCCESSION = (
    "set by your predecessor: the same task on the same province kept running through the "
    "succession, and a claim it makes goes to the ruler when it lands. Whether the progress "
    "survived the succession, or survives setting the task again, is not read")


def _adopt_across_succession(actor_id, task_rows: list) -> Optional[dict]:
    """A fabrication another ruler set (a predecessor: this server keys them by
    ruler) whose province is the one the running task targets now is this ruler's: it moves
    to him, `set_by` naming who set it. None when no such task runs."""
    row = next((r for r in task_rows if isinstance(r, dict) and r.get("task_name") == FABRICATE_TASK), None)
    prov = (row or {}).get("target_province_id")
    if not actor_id or not isinstance(prov, int):
        return None
    for other, fab in list(_FABRICATING.items()):
        if other != actor_id and fab.get("province_id") == prov:
            moved = dict(fab, set_by=fab.get("set_by") or other)
            _FABRICATING.pop(other, None)
            _FABRICATING[actor_id] = moved
            return moved
    return None


def fabrication_running_on(actor_id, title_id, task_rows: Any) -> Optional[dict]:
    """The fabrication this server knows runs now on `title_id` for this ruler
    (set here, or by his predecessor: _adopt_across_succession), or None. Read from the
    council_tasks rows; None when they did not answer."""
    if not isinstance(task_rows, list):
        return None
    fab = _FABRICATING.get(actor_id) or _adopt_across_succession(actor_id, task_rows)
    row = next((r for r in task_rows if isinstance(r, dict) and r.get("task_name") == FABRICATE_TASK), None)
    if fab and row and fab.get("title_id") == title_id and fab.get("province_id") is not None \
            and fab.get("province_id") == row.get("target_province_id"):
        return fab
    return None


#: The chaplain's claim letter (events/councillor_task_events/court_chaplain_task_events.txt):
#: option a takes the claim and its price, option b declines it
CLAIM_LETTER = "court_chaplain_task.0301"
#: (a test run, step 35, 1077.06: a Breifne letter came as court_chaplain_task.0302, the AI paid it into debt
#: and the purse read "not by your chaplain's letter", -78.22 unexplained): every event of the chaplain's task that
#: charges a claim letter, enumerated from the game's files (court_chaplain_task_events.txt: the events whose options
#: run court_chaplain_task_0301_charge_prestige_or_gold_effect; test_spending_veto_kinds checks this list against the files).
#: 0301: .a a county claim, .b declines. 0302: .a a county claim, .b a DUCHY claim (7 x (40 - learning), at least
#: 80-100), and its decline is the option named court_chaplain_task.0301.b.
CLAIM_LETTERS = ("court_chaplain_task.0301", "court_chaplain_task.0302")
CLAIM_DECLINE_OPTION = "court_chaplain_task.0301.b"
CLAIM_DUCHY_OPTION = "court_chaplain_task.0302.b"
CLAIM_DECLINED = ("the claim this fabrication made was declined (the letter's other option was applied): no "
                  "claim was taken. The game starts the task again on the same county (restart_on_finish): "
                  "its months are counted again from the decline")


def _declined(pick: dict) -> bool:
    """A pick whose applied option is not the first: the news row's form (applied a key or an index)
    and the bridge's record ({index, key}; read as declined, so a paid letter was not priced).
    A claim letter's decline is its option named court_chaplain_task.0301.b (0302's second option
    takes a duchy claim and is paid)."""
    opts = pick.get("options") or []
    ap = pick.get("applied")
    if isinstance(ap, dict):
        ap = ap.get("key") if ap.get("key") else ap.get("index")
    if pick.get("event") in CLAIM_LETTERS and isinstance(ap, str):
        return ap == CLAIM_DECLINE_OPTION
    first = opts[0] if opts else None
    if isinstance(first, dict):
        first = first.get("key")
    return bool(opts) and ap is not None and ap != first and ap != 0


#: (actor id, title id) -> {times, since_counted, last_seen, seat_task_now}: how often a
#: fabrication this server set on that title stopped running before its claim came
_REVERTS: dict = {}
#: after this many resets the same council_task call is no longer offered
REVERT_STOP = 2


def note_revert(actor_id, fab: dict, task_rows: list, date) -> dict:
    """Count one reset of the fabrication `fab` (once per time it was set: a fabrication read
    stopped by several observes counts once) and say what the seat does now."""
    key = (actor_id, fab.get("title_id"))
    rec = _REVERTS.setdefault(key, {"times": 0})
    if rec.get("since_counted") != fab.get("since"):
        rec["times"] += 1
        rec["since_counted"] = fab.get("since")
    rec["last_seen"] = date
    seat = str(fab.get("seat") or "court_chaplain")
    now = next((r.get("task_name") for r in task_rows if isinstance(r, dict)
                and seat in str(r.get("position_key") or "")), None)
    if now:
        rec["seat_task_now"] = now
    out = {"times": rec["times"], "last_seen": date}
    if rec.get("seat_task_now"):
        out["seat_task_now"] = rec["seat_task_now"]
    return out


def reverts_of(actor_id, title_id) -> int:
    return int((_REVERTS.get((actor_id, title_id)) or {}).get("times") or 0)


#: A live 1066 game (1066.09.15): the game's own check for a fabricate task
#: on one county IS read: the bridge's query_fabricate_targets with `title_id` (target_scope county_province)
#: runs the task's target check (the county filter, then potential_county) through the county's own province.
#: Live it listed the 12 Irish counties other than Dublin and Leinster for Murchad 31506 (Athlone at province 34),
#: refused c_leinster (holder 27052, his father): the native set was refused at once (`target_rejected`) and the
#: script path's task was reset to the default between +10 and +15 days. Over 40 (actor, county) pairs of
#: relatives in range it refused 10 of 10 counties a living FATHER or MOTHER holds and accepted 28 of 30 a
#: child or sibling holds; the 2 refused held explicit claims of the actor. The term: potential_county's
#: `NOR { ... has_claim_on = scope:county }`, which counts a claim on a living parent's titles (the claim read
#: lists none; every other clause read true; the reverse pair, a parent on his child's county, was accepted).
FAB_KIN_REFUSED = ("{holder} holds it, your parent: the game's own fabricate check refuses a county you already "
                   "have a claim on, and it counts your claim on a living parent's titles (a task set on one "
                   "is reset to the chaplain's default)")
FAB_GAME_REFUSES = ("the game's own fabricate check refuses {county}: a fabrication is possible only on a county "
                    "whose holder's top liege is in your diplomatic range, that you do not hold, and that you "
                    "have no claim on (a claim you hold, or your claim on a living parent's titles); a task set "
                    "anyway is reset to the chaplain's default within about a month")
FAB_GAME_ACCEPTS = "the game's own fabricate check accepts it (query_fabricate_targets, read now)"
FAB_CHECK_UNREAD = ("unknown: the game's own check for this county did not answer; checked here: {checked}")
FAB_KIN_CHECKED = "the holder is not your parent"


async def game_fabricate_verdict(gw, actor_id, title_id) -> Optional[dict]:
    """The game's own fabricate-target check for one county (live: proven against the game's resets):
    {eligible, assignable, target_province_id} or None when the bridge did not answer it."""
    if not _i(actor_id) or not _i(title_id):
        return None
    v = _d(await gw.read("query_fabricate_targets", {"actor_id": _i(actor_id), "title_id": _i(title_id)}))
    one = v.get("verdict") if v.get("target_scope") == "county_province" else None
    if isinstance(one, dict) and _i(one.get("title_id")) == _i(title_id) and isinstance(one.get("eligible"), bool):
        return {"eligible": one["eligible"], "assignable": one.get("assignable"),
                "target_province_id": one.get("target_province_id")}
    return None


async def holder_kin(gw, actor_id, holder_id, cache: Optional[dict] = None) -> Optional[str]:
    """"parent", "child" or "sibling" when `holder_id` is that to `actor_id`,
    "" when he is none of them, None when a character read did not answer."""
    cache = {} if cache is None else cache

    async def char(cid):
        if cid not in cache:
            r = await gw.read("query_character", {"character_id": cid})
            cache[cid] = r if isinstance(r, dict) and _i(r.get("id")) == cid else None
        return cache[cid]
    me, h = _i(actor_id), _i(holder_id)
    if not me or not h:
        return None
    a = await char(me)
    if a is None:
        return None
    if h in (_i(a.get("father_id")), _i(a.get("mother_id"))):
        # Live read 27052 (Diarmait) as Murchad's mother_id: the bridge's family offsets were
        # swapped (re_fabricate_claim.md §4). Which parent is not needed here, so not said
        return "parent"
    if h in {_i(c) for c in a.get("children") or []}:
        return "child"
    b = await char(h)
    if b is None:
        return None
    if any(_i(a.get(k)) and _i(a.get(k)) == _i(b.get(k)) for k in ("father_id", "mother_id")):
        return "sibling"
    return ""


async def fabricate_check(gw, actor_id, holder_id, cache: Optional[dict] = None, title_id=None) -> dict:
    """{game_check: refuses|accepts|unknown, why[, game_verdict]}. Live: the game's own verdict for the county
    (game_fabricate_verdict) decides when it answers; when it does not, the one rule proven live stands in: a
    county a living parent holds is refused; anything else is unknown."""
    verdict = await game_fabricate_verdict(gw, actor_id, title_id) if title_id is not None else None
    rel = await holder_kin(gw, actor_id, holder_id, cache)
    if verdict is not None:
        if verdict["eligible"] is False:
            why = (FAB_KIN_REFUSED.format(holder=holder_id) if rel == "parent"
                   else FAB_GAME_REFUSES.format(county=title_id))
            return {"game_check": "refuses", "why": why, "game_verdict": verdict}
        return {"game_check": "accepts", "why": FAB_GAME_ACCEPTS, "game_verdict": verdict}
    if rel == "parent":
        return {"game_check": "refuses", "why": FAB_KIN_REFUSED.format(holder=holder_id)}
    return {"game_check": "unknown", "why": FAB_CHECK_UNREAD.format(
        checked=FAB_KIN_CHECKED if rel is not None else "nothing (the holder's character read did not answer)")}


def _days(a, b) -> Optional[int]:
    import cycle_cadence as _cc
    oa, ob = _cc.ordinal(a), _cc.ordinal(b)
    return None if oa is None or ob is None else ob - oa


def fabrication_status(actor_id, task_rows: Any, date: Optional[str]) -> Optional[dict]:
    """What the code can know about the claim fabrication, from the
    council_tasks rows (query_council_tasks seats, or None when that read did not
    answer): whether a fabricate task runs now, on what (the title when it is the
    one this server set, read back as its province; else unknown), since when this
    server saw it, days elapsed, and progress unknown. A fabrication this server set
    that no longer runs is the loud case: running false with the call to set it again.
    None when there is nothing to say (no task running and none set here)."""
    fab = _FABRICATING.get(actor_id)
    if not fab and isinstance(task_rows, list):
        fab = _adopt_across_succession(actor_id, task_rows)
    if not isinstance(task_rows, list):
        return ({"running": None, "unread": "query_council_tasks did not answer", "set_here": dict(fab)}
                if fab else None)
    row = next((r for r in task_rows if isinstance(r, dict) and r.get("task_name") == FABRICATE_TASK), None)
    if row is None:
        _FAB_SEEN.pop(actor_id, None)
        if not fab:
            return None
        rv = note_revert(actor_id, fab, task_rows, date)
        out = {"running": False, "was": {k: fab.get(k) for k in ("title_id", "title_key", "since")},
               "detail": ("the fabricate task this server set is no longer running: the game set the seat "
                          f"to {rv['seat_task_now']}" if rv.get("seat_task_now") else
                          "the fabricate task this server set is no longer running") + "; the claim is not being made",
               "reverted": rv}
        if rv["times"] < REVERT_STOP:
            out["next_call"] = {"tool": "do", "args": {"intent": "council_task", "args": {
                "seat": fab.get("seat") or "court_chaplain", "task": FABRICATE_TASK,
                "target_title_id": fab.get("title_id")}}}
        else:
            # (the same call offered 35 times, the game reset it each month)
            out["not_offered_again"] = (f"set on {fab.get('title_key') or fab.get('title_id')} {rv['times']} times "
                                        "and reset by the game each time; next_call lists the counties instead")
        return out
    prov = row.get("target_province_id")
    seen = _FAB_SEEN.get(actor_id)
    if not seen or seen.get("province_id") != prov:
        seen = _FAB_SEEN[actor_id] = {"province_id": prov, "since": date}
    mine = bool(fab) and fab.get("province_id") is not None and fab.get("province_id") == prov
    since = fab.get("since") if mine else seen.get("since")
    game_target = row.get("target") if row.get("target_read") == "game" and isinstance(row.get("target"), dict) else None
    out = {"running": True, "councillor_id": row.get("councillor_id"),
           "target": ({"title_id": fab.get("title_id"), "title_key": fab.get("title_key")} if mine
                      else dict(game_target) if game_target else "unknown"),
           "target_province_id": prov, "since": since, "days": _days(since, date),
           "since_is": ("the date this server set it" if mine else "the first date this server read it running"),
           "progress": None, "progress_unread": FABRICATION_PROGRESS_UNREAD, "measured": FABRICATION_MEASURED}
    pct = row.get("progress")
    if isinstance(pct, (int, float)) and not isinstance(pct, bool):
        # The game's own progress (CActiveCouncilTask+0x20, re_fabricate_claim.md §3)
        out["progress"] = float(pct)
        out.pop("progress_unread", None)
        out.update(_progress_measure(actor_id, prov, float(pct), date))
    if mine and fab.get("set_by"):
        out["set_by"] = fab["set_by"]
        out["succession_note"] = FABRICATION_ACROSS_SUCCESSION
    if fab and not mine:
        out["set_here"] = {k: fab.get(k) for k in ("title_id", "title_key", "since")}
        out["set_here_note"] = ("this server set a fabrication on the title above; the running task's "
                                "province does not match the one read back then"
                                if fab.get("province_id") is not None else
                                "this server set a fabrication on the title above; its province was "
                                "not read back, so the running task cannot be matched to it")
    return out


#: Actor id -> the acts this server sent with what each cost, by date
_ACTS: dict = {}
#: One counter for this server's acts and news reads, so an act and a read made on the same game day are in order:
#: an act sent while the game was stopped on a day is either before that day's read (its effect is in the read, and
#: the step that read ends is its step) or after it (the next step's). The date alone cannot say which: an order fired
#: on the day the step ended was charged in that step and again in the next, whose window began on the same day
_READ_ORDER: list = [0]


def _next_order() -> int:
    _READ_ORDER[0] += 1
    return _READ_ORDER[0]


def note_act(actor_id, date, what: str, *, cost_known: bool, **costs) -> None:
    """An act this server applied and what it cost (negative numbers), for the news'
    prestige / piety / gold change. cost_known false: the act's cost was not read."""
    if actor_id:
        _ACTS.setdefault(actor_id, []).append({"date": date, "what": what, "cost_known": cost_known,
                                               "order": _next_order(),
                                               **{k: v for k, v in costs.items() if v}})
        del _ACTS[actor_id][:-16]


def act_in_step(x: dict, prev: dict, snap: dict) -> bool:
    """THE rule for which step an act of this server belongs to: the step between two reads holds the acts sent after
    the first read and before the second, so an act is counted in one step only. Both reads and the act carry their
    place in this server's order (snapshot, note_act); a read or act without one (a record made by hand) falls back
    to the dates, both ends included, which counts an act dated on a read's day in both steps that day bounds."""
    a, p, s = x.get("order"), prev.get("read_order"), snap.get("read_order")
    if all(isinstance(v, int) and not isinstance(v, bool) for v in (a, p, s)):
        return p < a < s
    lo, hi = _ordinal_of(prev.get("date")), _ordinal_of(snap.get("date"))
    return lo is not None and hi is not None and lo <= (_ordinal_of(x.get("date")) or -1) <= hi


def acts_in_step(prev: dict, snap: dict) -> list:
    """The acts this server sent for the ruler of `snap` in the step from `prev` (act_in_step)."""
    return [x for x in _ACTS.get(snap.get("actor_id")) or [] if act_in_step(x, prev, snap)]


#: The game's price of a fabricated claim on a county, charged when the claim
#: arrives (events/councillor_task_events/court_chaplain_task_events.txt, the letter event
#: court_chaplain_task.0301, its charge effect; common/script_values/99_court_chaplain_values.txt
#: court_chaplain_fabricate_claim_county_cost): (40 - the chaplain's learning) x 3, at least
#: 40 to 60, in gold (short-term gold: it can go below 0), when the game's AI for the ruler
#: takes the claim. Live on the 1066 start each of four claims that arrived took 79 to 101
#: gold in its step (1067.11.27, 1069.04.22, 1070.09.15, 1074.11.27).
CLAIM_COST_BASIS = ("the game's price of a fabricated county claim, charged as it arrives: (40 - the "
                    "chaplain's learning) x 3 gold, at least 40-60")
PURSE_UNREAD = "no read gives what the game's AI spent, nor the monthly prestige or piety change"
#: The monthly prestige / piety change is read (income.expected); what the AI spent is not
PURSE_UNREAD_AI = "no read gives what the game's AI spent"


#: Title key -> the last creation price read {currency, cost, date, actor_id}
_CREATION_PRICE: dict = {}
#: The last price read for each (ruler, title), and the sources of its discount: a read for one ruler never stands in
#: for another's (the ruler-change briefing reads the late ruler's from here, after his heir's reads moved the slot
#: above; it said "no price read under your late ruler" beside a dozen replies that had read 325 for him)
_CREATION_PRICE_BY: dict = {}
_PRICE_BASIS_BY: dict = {}

#: (1074.11.11: d_meath read cannot_afford 212.5 vs 29.6 with the game's
#: check met; the next step the game's AI created it and gold went 29.6 -> -181.63). The game's
#: create-title check (can_create_title, image 0x0621a500) skips its gold test when the creator is run
#: by the game's AI (his character's AI strategy reports active), holds count tier as his highest, is
#: over his domain limit (the counties he holds himself, less the cached limit at title_realm+0x3D0)
#: and the title is a duchy; the creation (CreateTitle_Apply) charges the price anyway, into debt.
#: Read from the decompile; the run's numbers agree (1066.09.15: one county, the check said "does not
#: have gold 212"; 1074.11.11: five counties against a limit of four, the check passed at 29.6 gold).
AI_CREATE_RULE = ("the game's own check passes without the price: it skips the gold test for a ruler "
                  "run by the game's AI whose highest title is a county, who holds more counties himself "
                  "than his domain limit, creating a duchy; the creation charges the price all the same, "
                  "into debt. The game's AI makes that creation for your ruler when it chooses")
#: Live (a live check, 1066.09.15 -> 1067.09.15): six AI-run counts over their domain limit, each with
#: the game's check met and the price not covered, all created their duchy within the year into debt
#: (gold -10 to -75), two of them with gold_reserve 1000 set and their gold moved into it;
#: nine counts within their limit, the check naming the gold, none created one in five years.
AI_CREATE_NO_VETO = ("gold_reserve does not stop it (the check reads the whole gold, not the AI's budget "
                     "buckets: a count with a reserve set can create one into debt) and event_rule does not (a creation is not an event); ai_veto does: the "
                     "bridge drops the AI's creation for your ruler before the game runs it")


#: (requested id, new ruler id) -> the row that says which vetoes followed the seat to him
_VETO_CARRIED: dict = {}
VETO_CARRIED_NOTE = ("a veto set with ai_veto is the seat's: when the seat's ruler changes, this server sets the "
                     "same vetoes for the new ruler (a veto set for the old ruler does not hold for the new one by "
                     "itself: the AI can then pass a crown-authority law for him at a prestige price)")


async def carry_vetoes(gw, requested_id, prev: dict, snap: dict) -> Optional[dict]:
    """The vetoes the seat's previous ruler held, set again for the new one (set_ai_veto,
    read back with query_ai_veto). None when the old ruler held none or they were not read."""
    new_id, old_id = snap.get("actor_id"), prev.get("actor_id")
    done = _VETO_CARRIED.get((requested_id, new_id))
    if done is not None:
        return done
    had = prev.get("ai_vetoes")
    if not isinstance(had, list) or not had or not new_id:
        return None
    have = snap.get("ai_vetoes") if isinstance(snap.get("ai_vetoes"), list) else []
    # A decision veto carries its keys (none = every decision)
    keys = [k for k in prev.get("ai_veto_decision_keys") or [] if isinstance(k, str)]
    failed: dict = {}
    todo = [w for w in had if w not in have]
    # The create_title veto keeps sparing the goal title for the heir; and the titles it needs for him
    spare = await _seat_events._goal_spare(gw, new_id) if "create_title" in todo else None
    for w in todo:
        raw = await gw.write("set_ai_veto", {"character_id": new_id, "op": "set", "what": w,
                                             **({"keys": ",".join(keys)} if w == "decision" and keys else {}),
                                             **(_seat_events.spare_params(spare) if w == "create_title" else {})})
        if not (isinstance(raw, dict) and raw.get("success") is True):
            failed[w] = str((raw or {}).get("reason") or "no_reply") if isinstance(raw, dict) else "no_reply"
    back = await gw.read("query_ai_veto", {"character_id": new_id})
    ruler = (_seat_events._veto_ruler(back, new_id) if isinstance(back, dict) and back.get("success") is True
             else None)
    now = list((ruler or {}).get("vetoes") or []) if ruler is not None else None
    row: dict = {"from": old_id, "to": new_id, "vetoes": list(had), "note": VETO_CARRIED_NOTE}
    if spare is not None and ruler is not None:
        gs = _seat_events._goal_spare_result(spare, ruler, new_id) or {}
        if gs.get("chain"):
            row["goal_chain"] = gs["chain"]
    if now is None:
        row["carried"] = "unread: query_ai_veto did not answer after the set"
    else:
        row["carried"] = [w for w in had if w in now]
        snap["ai_vetoes"] = now
        if "decision" in had:
            got = list((ruler or {}).get("decision_keys") or [])
            row["decision_keys"] = got or "every decision"
            snap["ai_veto_decision_keys"] = got
            if "decision" in now and sorted(got) != sorted(keys):
                failed["decision"] = "decision_keys_readback_disagrees"
    if failed:
        row["not_carried"] = failed
        row["next"] = ai_veto_call(next(iter(failed)))
    elif now is not None and set(had) - set(now):
        row["not_carried"] = {w: "readback_disagrees" for w in had if w not in now}
        row["next"] = ai_veto_call(next(w for w in had if w not in now))
    _VETO_CARRIED[(requested_id, new_id)] = row
    return row


_RULES_CARRIED: dict = {}
#: (1072.07.16: Murchad's claim-letter rules died with him while his vetoes were carried, and nothing
#: said so; his heir's AI paid two letters, 162 gold)
EVENT_RULES_CARRIED_NOTE = ("an event rule is the seat's, as a veto is: when the seat's ruler changes, the same rules are "
                            "set for the new ruler (the bridge at the handoff, this server again here)")
#: the rule fields set_event_rule takes, as query_event_rules writes them
_RULE_FIELDS = ("event", "answer", "option", "option_index", "currency", "amount", "else")


async def carry_event_rules(gw, requested_id, prev: dict, snap: dict) -> Optional[dict]:
    """The event rules the seat's previous ruler held, set again for the new one, as carry_vetoes does for the
    vetoes (the bridge carries both at the handoff; this sets any it did not and reads them back). None when the old
    ruler held no rule or his table was not read."""
    new_id, old_id = snap.get("actor_id"), prev.get("actor_id")
    done = _RULES_CARRIED.get((requested_id, new_id))
    if done is not None:
        return done
    if not new_id or not old_id or not prev.get("event_rule_events"):
        return None
    got = await gw.read_many([("query_event_rules", {"character_id": old_id}),
                              ("query_event_rules", {"character_id": new_id})])
    old_raw, new_raw = (list(got) + [None, None])[:2]
    old = (_seat_events._rules_of(old_raw, old_id) if isinstance(old_raw, dict) and old_raw.get("success") is True
           else None)
    if not old:
        return None
    have = {str(r.get("event")) for r in (_seat_events._rules_of(new_raw, new_id) or [])} \
        if isinstance(new_raw, dict) and new_raw.get("success") is True else set()
    failed: dict = {}
    for r in old:
        ev = str(r.get("event") or "")
        if not ev or ev in have:
            continue
        params = {k: r[k] for k in _RULE_FIELDS if k in r and r[k] is not None}
        if "option" in params:
            params.pop("option_index", None)
        if params.get("answer") != "pay_if":
            params.pop("currency", None)
            params.pop("amount", None)
        raw = await gw.write("set_event_rule", {"character_id": new_id, "op": "set", **params})
        if not (isinstance(raw, dict) and raw.get("success") is True):
            failed[ev] = str((raw or {}).get("reason") or "no_reply") if isinstance(raw, dict) else "no_reply"
    back = await gw.read("query_event_rules", {"character_id": new_id})
    now = ((_seat_events._rules_of(back, new_id) or []) if isinstance(back, dict) and back.get("success") is True
           else None)                                   # no table for him: no rule
    events = [str(r.get("event")) for r in old]
    row: dict = {"from": old_id, "to": new_id, "events": events, "note": EVENT_RULES_CARRIED_NOTE}
    if now is None:
        row["carried"] = "unread: query_event_rules did not answer after the set"
    else:
        held = {str(r.get("event")) for r in now}
        row["carried"] = [e for e in events if e in held]
        snap["event_rule_events"] = sorted(held)
        snap["event_watched"] = True
        for e in events:
            if e not in held and e not in failed:
                failed[e] = "readback_disagrees"
    if failed:
        row["not_carried"] = failed
    _RULES_CARRIED[(requested_id, new_id)] = row
    return row


#: What the news says when the spared chain changed with the titles held
CHAIN_MET_NOTE = ("the titles {goal} needed are held now: the create_title veto spares {goal} alone again; every "
                  "other title's creation is dropped")
CHAIN_RESYNC_NOTE = ("the titles your goal still needs changed with the titles you hold, so the create_title veto now "
                     "spares these (and the goal); every other title's creation is still dropped")


async def resync_goal_chain(gw, snap: dict, changed_keys: list) -> Optional[dict]:
    """After a title of duchy tier or above was gained or lost, the create_title veto's spared chain is computed
    again (seat_events._goal_spare) and re-sent when it differs from the one last spared: a duchy the tier rule no
    longer needs is not left spared (the AI would buy it), and a newly needed one is. None when nothing changed."""
    me = snap.get("actor_id")
    if not me or not any(k[:2] in ("d_", "k_", "e_") for k in changed_keys):
        return None
    if "create_title" not in (snap.get("ai_vetoes") or []) or _seat_events.goal_title() is None:
        return None
    was = _seat_events.SPARED_CHAIN.get(me)
    spare = await _seat_events._goal_spare(gw, me)
    if not (spare or {}).get("title_id") or spare.get("chain_unread"):
        return None
    ids = sorted(t["title_id"] for t in spare.get("chain") or [])
    if was is not None and was.get("ids") == ids:
        return None
    raw = await gw.write("set_ai_veto", {"character_id": me, "op": "set", "what": "create_title",
                                         **_seat_events.spare_params(spare)})
    if not (isinstance(raw, dict) and raw.get("success") is True):
        return {"not_set": str((raw or {}).get("reason") or "no_reply") if isinstance(raw, dict) else "no_reply"}
    back = await gw.read("query_ai_veto", {"character_id": me})
    ruler = _seat_events._veto_ruler(back, me) if isinstance(back, dict) and back.get("success") is True else None
    gs = _seat_events._goal_spare_result(spare, ruler, me) or {}
    left = (gs.get("chain") or {}).get("spared", [])
    # The list names every title the
    # veto spares, the goal first; `chain` the titles spared for it
    return {"spared": [spare["key"]] + list(left), "chain": list(left),
            "note": CHAIN_RESYNC_NOTE if left else CHAIN_MET_NOTE.format(goal=spare["key"]),
            **({"not_spared": gs["chain"]["not_spared"]} if (gs.get("chain") or {}).get("not_spared") else {})}


def ai_veto_call(what: str, keys: Optional[list] = None) -> dict:
    """The call that vetoes one kind of the AI's posts for the ruler (a decision veto may
    name its decisions)."""
    return {"tool": "do", "args": {"intent": "ai_veto", "args": {"op": "set", "what": what,
                                                                 **({"keys": list(keys)} if keys else {})}}}
#: Live (a live check): the grant to a courtier was refused for a count: "its new holder cannot be your
#: vassal, so the title LEAVES your realm"
AI_CREATE_GATE = ("the game's check asks for the gold again only while the counties you hold yourself are "
                  "within your domain limit, or once you hold a duchy title (the skip is for a county-tier "
                  "creator). A count cannot grant a county inside his realm: its new holder cannot be his "
                  "vassal, so a county given away leaves the realm")


#: What the gold reserve is proven to do (live: income filled it and nothing
#: discretionary came out of it in 22 turns; a forced price took it, last)
#: ("I set the game's AI to keep up to 500 gold in reserve so it doesn't spend the money";
#: its AI hired a company at 1089.06, 169 gold, under that reserve): what the
#: reserve does, and that it is not a stop
RESERVE_PROVEN = ("gold_reserve keeps income in the AI's reserved budget; a forced price (a claim letter, "
                  "a title's creation) still takes it, and it does not stop the AI's purchases (the AI can hire a "
                  "company under a reserve): do ai_veto what=spending does")


#: The date the turn-one notes were served by this server (served once)
_TURN_ONE_SERVED: list = []


def turn_one_notes() -> Optional[dict]:
    """The shared notes' turn-one block (notes.Notes.turn_one_reply), None when off."""
    import notes as _notes  # noqa: PLC0415
    return _notes.SERVED.turn_one_reply() if _notes.SERVED is not None else None


def ai_spends(levers_only: bool = False) -> dict:
    """The game's AI spends the ruler's gold and prestige on its own picks unless a lever
    is set; each spend, its lever (a ready call) or that none exists. Facts, not advice.
    levers_only: the calls alone, when the turn-one notes beside it carry the facts."""
    if levers_only:
        return {"what": "the calls that stop or answer the game's AI spending for your ruler (the notes price them)",
                "claim_letters": {"tool": "do", "args": {"intent": "event_rule", "args": {
                    "op": "set", "event": CLAIM_LETTER, "answer": "pay_if", "option": CLAIM_LETTER + ".a",
                    "currency": "gold", "amount": "<gold>"}}, "also_event": CLAIM_LETTERS[1]},
                # The six kinds `spending` sets are in the turn-one note beside this block
                "veto": ai_veto_call("<spending, or vassal_offer|ally_call|perk|decision>")}
    # gold_reserve is no longer listed here: it stops none of the AI's purchases (RESERVE_PROVEN)
    return {"what": ("the game's AI for your ruler spends his gold and prestige on its own picks; news names "
                     "each spend it reads with its lever"),
            "claim_letters": {"lever": {"tool": "do", "args": {"intent": "event_rule", "args": {
                "op": "set", "event": CLAIM_LETTER, "answer": "pay_if", "option": CLAIM_LETTER + ".a",
                "currency": "gold", "amount": "<gold>"}}}, "proven": "live (the rule's option is the one the game applies)"},
            "title_creation": {"when": AI_CREATE_RULE, "lever": ai_create_lever()},
            "buildings_and_mercenaries": {"lever": {"tool": "do", "args": {"intent": "ai_veto", "args": {
                                              "op": "set", "what": "<mercenary or construction>"}}},
                                          "not_gold_reserve": RESERVE_NOT_A_STOP},
            "laws": {"lever": ai_veto_call("law"), "what": LAW_LEVER},
            "allies_called_into_your_wars": {"lever": ai_veto_call("ally_call"),
                                             "what": ("the game's AI calls your allies into your wars; each who "
                                                      "joins a war you attack in costs prestige by his tier. The "
                                                      "veto drops its calls, never yours (do call_ally)")},
            "perks": {"lever": ai_veto_call("perk"), "what": ("the game's AI spends each lifestyle point some "
                                                               "weeks after it appears; the veto keeps them for do lifestyle "
                                                               "perk=<key>")},
            # (the list named every kind but vassal_offer)
            "vassal_offers": {"lever": ai_veto_call("vassal_offer"),
                              "what": "another ruler's offer of vassalage, which your ruler's AI answers at once"},
            "decisions": {"lever": ai_veto_call("decision", [CONVERT_DECISION_KEY]),
                          "what": ("the game's AI takes decisions for your ruler; it can convert him to his "
                                   "capital's culture for prestige (1,000 before the decision's multipliers). keys names the "
                                   "decisions it drops")}}


def ai_create_lever(title_key: Any = None) -> dict:
    """The lever row for a title the game's AI may create (or created) for the ruler. The
    gold-skip gate is a duchy's (a county-tier creator); a kingdom's or an empire's row leaves it out."""
    gate = {} if title_key and not str(title_key).startswith("d_") else {"what": AI_CREATE_GATE}
    return {**gate, "no_veto": AI_CREATE_NO_VETO, "veto": ai_veto_call("create_title"),
            "read": {"tool": "inspect", "args": {"subject": "economy"}},
            "mint_yourself": {"tool": "do", "args": {"intent": "mint_title",
                                                     "args": {"title_key": title_key or "<duchy key>"}}},
            "mint_yourself_when": "your gold covers its price: the toolkit refuses a mint you cannot pay"}


#: The prestige the game grants for creating a title (00_defines.txt CREATE_TITLE_PRESTIGE_GAIN,
#: by tier: duchy 300, kingdom 400, empire 500); 1075.02.09 read +304.37 for d_meath
CREATE_TITLE_PRESTIGE = {"d_": 300, "k_": 400, "e_": 500}


def note_creation_price(actor_id, key, currency, cost, date) -> None:
    if key and isinstance(cost, (int, float)):
        _CREATION_PRICE[str(key)] = {"currency": currency, "cost": cost, "date": date, "actor_id": actor_id}
        _CREATION_PRICE_BY[(actor_id, str(key))] = _CREATION_PRICE[str(key)]


def note_price_basis(actor_id, key, basis: dict) -> None:
    """The discount sources read for (ruler, title) with the price they explain (creation_price_basis)."""
    if key and isinstance(basis, dict) and isinstance(basis.get("sources"), list):
        _PRICE_BASIS_BY[(actor_id, str(key))] = basis


def price_change_why(was_basis: Any, now_basis: Any) -> Optional[str]:
    """In words, the discount sources the late ruler's price had and his successor's has not (and the reverse),
    from the two creation_price_basis reads; None when either did not read its sources."""
    if not (isinstance(was_basis, dict) and isinstance(now_basis, dict)
            and isinstance(was_basis.get("sources"), list) and isinstance(now_basis.get("sources"), list)):
        return None
    def keyed(b):
        return {(x.get("source"), x.get("key")): x for x in b["sources"] if isinstance(x, dict)}
    a, b = keyed(was_basis), keyed(now_basis)
    lost = [f"{k[1] or k[0]} ({a[k].get('gold')})" for k in a if k not in b]
    gained = [f"{k[1] or k[0]} ({b[k].get('gold')})" for k in b if k not in a]
    if not lost and not gained:
        return "the same discount sources: the base or another part moved"
    out = []
    if lost:
        out.append("the late ruler's own discount " + ", ".join(lost) + " died with him")
    if gained:
        out.append("your new ruler's own " + ", ".join(gained))
    return "; ".join(out)


#: The head of faith's gold (hof_ask_for_gold_interaction): 18 months of the
#: asker's income, at least 100, for 250 piety (redesign.HOF_GOLD_*); the game's AI makes the
#: ask for its ruler (1074.09.15: gold +136.46, piety -241.9, no call of the seat's).
HOF_GOLD_PIETY_LOSS = 250
#: The letter hof_ask_for_gold_interaction's on_accept sends the asker (00_religious_interactions.txt), and the one
#: its on_decline sends: the record that the ask was made and answered. A purse row names the head of faith's gold
#: only when this step's answered events hold the first; a fall of about 250 piety beside a gold rise is no record
#: of it (a request_excommunication_interaction charges 500 piety while the income raises the gold).
HOF_GOLD_LETTER = "religious_interaction.3"
HOF_GOLD_DECLINED_LETTER = "religious_interaction.32"
HOF_GOLD_BASIS = ("the head of faith's gold (hof_ask_for_gold_interaction: 18 months of the asker's income, at least "
                  "100, for 250 piety unless a hook is used): its accept letter " + HOF_GOLD_LETTER
                  + " is among this step's answered events")
#: The reparations a losing attacker pays (redesign.PEACE_PRICE gold_years)
REPARATIONS_BASIS = ("an attacker who loses a war pays the defender {y} years of its yearly income as the game "
                     "reckons it (the casus belli's on_defeat); this row's figure, when it has one, is this tool's "
                     "estimate of that charge, {y} x 12 x the monthly income read, and can differ")
TIER_RULE_NOTE = "a duchy title you hold counts toward the kingdom's rule of two duchy titles"


def tier_note(key: Any) -> dict:
    """A recorded case (1080.05.25: k_ireland's row carried the duchy rule): the `counts` line for a duchy
    only; a kingdom or an empire has none of its own here."""
    return {"counts": TIER_RULE_NOTE} if str(key or "").startswith("d_") else {}


def ai_created(key: str, out_events: dict) -> Optional[dict]:
    """A recorded case (1070.12.16: d_meath read "no read says which" and -138.54 unexplained beside the same
    step's ai_acted create_title d_meath price 162.5): the bridge's own record of the AI's creation for your ruler
    in this step (ai_acted what=create_title), with its price, or None."""
    for a in out_events.get("ai_acted") or []:
        if isinstance(a, dict) and a.get("what") == "create_title" and a.get("key") == key:
            return a
    return None


def ai_created_by(key: str, a: dict) -> str:
    """Who decided, with the rule that applies to THIS creation: the duchy-for-a-count gold skip only where the
    creator's gold at the attempt did not cover the price (the game's check passed without it)."""
    pr = a.get("price") if isinstance(a.get("price"), dict) else {}
    gold, at = pr.get("gold"), a.get("gold_at_attempt")
    if str(key).startswith("d_") and _isnum(gold) and _isnum(at) and at < gold:
        return "the game's AI for your ruler: " + AI_CREATE_RULE
    cover = (f"; your gold at the attempt ({at}) covered its price ({gold})" if _isnum(gold) and _isnum(at)
             else "")
    return "the game's AI for your ruler (no create_title veto set)" + cover
#: The chaplain's claim is unpressed (add_unpressed_claim); the game passes only
#: pressed claims to a ruler's children (redesign.CLAIM_DIES_WITH_RULER)
CLAIM_DIES = ("lost when you die: an unpressed claim does not pass to your heir; declare before "
              "then, or a white peace in its war presses it")


#: The claim letter's price, from the game's files.
CLAIM_PRICE_RULE = ("court_chaplain_fabricate_claim_county_cost (99_court_chaplain_values.txt): (40 - the "
                    "chaplain's learning when the letter comes) x 3 gold, x2 on a county in a struggle region "
                    "whose struggle you are not part of, then at least a random 40-60 drawn as it is charged; paid "
                    "in prestige instead where a struggle phase says so. Charged when you take the letter's "
                    "first option (court_chaplain_task.0301); its other option declines and costs nothing")


def claim_letter_price(learning: Any) -> Optional[dict]:
    """The county claim letter's gold for a chaplain of this learning: exact while (40 - learning)
    x 3 is at least 60, else the range the random minimum leaves. None without learning."""
    if not isinstance(learning, (int, float)) or isinstance(learning, bool):
        return None
    base = 3.0 * (40 - learning)
    if base >= 60:
        return {"gold": base, "exact": True, "learning": learning}
    return {"gold_low": max(base, 40.0), "gold_high": 60.0, "exact": False, "learning": learning}


def claim_gold_cost(learning: Any) -> Optional[float]:
    """The expected gold of one fabricated county claim for a chaplain of this learning."""
    if not isinstance(learning, (int, float)) or isinstance(learning, bool):
        return None
    return float(max((40 - learning) * 3, 40))


CLAIM_DUCHY_BASIS = ("a duchy claim by the chaplain's letter (court_chaplain_task.0302.b): "
                     "court_chaplain_fabricate_claim_duchy_cost, 7 x (40 - his learning), at least 80-100")


def claim_duchy_gold_cost(learning: Any) -> Optional[float]:
    """The expected gold of a duchy claim (court_chaplain_task.0302.b): court_chaplain_fabricate_claim_duchy_cost,
    7 x (40 - learning), at least a random 80-100 (the midpoint 90 where the floor binds)."""
    if not isinstance(learning, (int, float)) or isinstance(learning, bool):
        return None
    return float(max((40 - learning) * 7, 90))


def _is_duchy_claim(c: dict) -> bool:
    k = str(c.get("title_key") or c.get("key") or "")
    return k.startswith("d_") or c.get("tier") == 3


#: (a seated game 1093.08: the game's AI answered stress_threshold.1731 with the profligate option, gold
#: 257.61 at the pick, 162.41 at the next read, "unexplained -125.74"; bishop.2002 1095.02 about -23)
AI_PICK_GOLD_BASIS = ("not priced from the event's file: the gold read at the pick (the bridge's event hook) to the "
                      "next pick or this read, less your income over those days; other spends in those days are in "
                      "it; built, not proven live")


#: (a test run, step 19: a pilgrimage's gold, -155, read as "event char_interaction.0080 answered EXCELLENT ...
#: expected -193", a guardianship letter whose option moves no gold; step 35 the same on birth.3002; step 45 the seat's
#: own mint, -425, read into coming_of_age.1002 beside your_acts, "unexplained +423.53"): the step's gold reads in date
#: order are the AI's answered events and the activities its AI started (the bridge's records, each with the gold at
#: it); the fall from each to the next, less the income and less every priced row dated inside (the seat's own acts
#: first, then the AI's priced posts and lost wars), is the read's own, and only when it can be: an event whose
#: answered option moves no gold (its file) is given none of it, and what is left stays `unexplained`
ACTIVITY_GOLD_BASIS = ("the gold at the AI's post of the activity (the bridge's record) to the next read, less your "
                       "income over those days and the priced rows inside; the game charges an activity's gold when it "
                       "starts")


def _priced_in_step(prev: dict, snap: dict, out_events: dict) -> list:
    """(date, gold) of the priced moves of this step a window must not count again: the seat's own acts with a
    known gold cost, then the AI's posts the bridge priced in gold (a creation, a regiment)."""
    lo, hi = _ordinal_of(prev.get("date")), _ordinal_of(snap.get("date"))
    out: list = []
    if lo is not None and hi is not None:
        out += [(x.get("date"), x["gold"]) for x in acts_in_step(prev, snap)
                if x.get("cost_known") and _isnum(x.get("gold"))]
    for a in out_events.get("ai_acted") or []:
        pr = a.get("price") if isinstance(a, dict) and isinstance(a.get("price"), dict) else {}
        if _isnum(pr.get("gold")) and a.get("date"):
            out.append((a["date"], -abs(pr["gold"])))
    return out


def answered_by_words(e: dict) -> str:
    """Who answered an events_answered row (seat_events.pick_row): the rule's else option is the rule's answer,
    named so; the game's AI only when no rule of yours took it."""
    if e.get("rule") and e.get("rule_wanted") is not None:
        why = f"{e.get('rule_wanted')} was not taken: {e.get('why') or 'unread'}"
        if str(e.get("applied_by") or "").startswith("your rule's else"):
            return f"by your rule's else option ({why})"
        return f"by the game's AI (your rule's {why}; its else: ai)"
    return "by the game's AI"


def ai_pick_gold(prev: dict, snap: dict, answered: list, dated: Optional[list] = None,
                 acted: Optional[list] = None) -> list:
    """Each event the game's AI answered for your ruler in this step (no rule of yours took it) after which
    gold fell beyond the income, with that fall; the claim letters are priced by their own row. A pick with no
    gold read at it or after adds no row (the purse's `unexplained` stays). A priced row of this step dated
    inside a pick's window (`dated`: (date, gold)) comes out of that window's fall. An activity the AI started
    (`acted`, ai_acted rows of what=activity with the gold at the post) is a read of its own, and an event whose
    answered option moves no gold is given no fall."""
    import event_prices as _ep  # noqa: PLC0415
    inc = prev.get("monthly_income")
    rows = [("event", e, e.get("date"), e.get("gold_at_pick")) for e in answered
            if isinstance(e, dict) and e.get("event") not in CLAIM_LETTERS
            and (not e.get("rule") or e.get("rule_wanted") is not None)]
    rows += [("activity", a, a.get("date"), a.get("gold_at_attempt")) for a in acted or []
             if isinstance(a, dict) and a.get("what") == "activity" and _isnum(a.get("gold_at_attempt"))]
    rows.sort(key=lambda r: _ordinal_of(r[2]) if _ordinal_of(r[2]) is not None else 1 << 30)
    out: list = []
    for i, (kind, e, d0, g0) in enumerate(rows):
        if kind == "event":
            row = {"what": f"event {e.get('event')} answered {e.get('applied')} {answered_by_words(e)} "
                           f"({e.get('date')})", "basis": AI_PICK_GOLD_BASIS}
        else:
            row = {"what": f"activity {e.get('key') or '(type unread)'} started for your ruler by the game's AI ({d0})",
                   "basis": ACTIVITY_GOLD_BASIS, "lever": ai_spend_veto("activity")}
        g1, d1 = (rows[i + 1][3], rows[i + 1][2]) if i + 1 < len(rows) else (snap.get("gold"), snap.get("date"))
        lo, hi = _ordinal_of(d0), _ordinal_of(d1)
        if all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in (g0, g1, inc)) \
                and lo is not None and hi is not None and hi >= lo:
            fell = round(g1 - g0 - inc * (hi - lo) / 30.44, 2)
            inside = [g for d, g in dated or [] if _isnum(g) and lo <= (_ordinal_of(d) or -1) <= hi]
            if inside:
                fell = round(fell - sum(inside), 2)
                row["less_priced_in_window"] = round(sum(inside), 2)
            if fell <= -1:
                if kind == "event" and _ep.option_gold(e.get("event"), e.get("applied")) is False:
                    continue                  # None of the fall is its; it stays unexplained
                row["expected"] = fell
                if kind == "event":
                    # The option's own price from its file (the gold script value it names, at the income read),
                    # a second reading beside the window's fall
                    fp = _ep.option_gold_taken(e.get("event"), e.get("applied"), inc)
                    if fp is not None:
                        row["file_price"] = {k: v for k, v in fp.items() if k != "not_read"}
                if kind == "event" and e.get("next"):
                    row["next"] = e["next"]
                    if e.get("next_does"):
                        row["next_does"] = e["next_does"]
                out.append(row)
    return out


def _events_for(key: str, prev: dict, snap: dict, out_events: dict) -> list:
    """What happened in this diff that moves `key`, each with the change the
    game's rules give when the code can price it (expected), else only named."""
    ev: list = []
    if key == "gold":
        cost = claim_gold_cost(prev.get("chaplain_learning"))
        gained = out_events.get("claims_gained") or []
        letters = _letter_claims(prev, snap, gained)
        for c in gained:
            name = c.get("title_name") or c.get("title_id")
            if letters.get(c.get("title_id")) is False:
                # (1091.05.10: three claims priced -315 while the chaplain's
                # fabrication on c_mayo ran before and after the step; gold rose, +323.75 "unexplained")
                ev.append({"what": f"claim on {name} arrived, not by your chaplain's letter",
                           "basis": CLAIM_NOT_LETTER.format(why=letters.get("_why") or "")})
                continue
            c_cost = claim_duchy_gold_cost(prev.get("chaplain_learning")) if _is_duchy_claim(c) else cost
            ev.append({"what": f"claim on {name} arrived",
                       **({"expected": -c_cost} if c_cost is not None and letters.get(c.get("title_id")) is True
                          else {}), "basis": CLAIM_COST_BASIS if c_cost == cost else CLAIM_DUCHY_BASIS,
                       **({"letter": "one of this step's claims is the chaplain's letter; which one is not read"}
                          if letters.get(c.get("title_id")) is None else {}),
                       **_letter_decided(snap, cost)})
        if letters.get("_one_of") and cost is not None:
            ev.append({"what": "your chaplain's claim letter (one of the claims above)", "expected": -cost,
                       "basis": CLAIM_COST_BASIS})
        for t in out_events.get("titles_gained") or []:
            if str(t.get("key") or "").startswith("c_") and t.get("how") is None:
                ev.append({"what": f"{t.get('key')} gained (how not read: inheritance, grant and creation read the same)"})
            key = str(t.get("key") or "")
            mine = own_act_in_step(prev, snap, "mint_title", key) if key[:2] in ("d_", "k_", "e_") else None
            if mine is not None and (t.get("how") is None or t.get("by") == "you"):
                # (1075.10.06: "d_leinster created for you by the game's AI" beside
                # your_acts mint_title d_leinster, and the price counted twice into `unexplained`)
                ev.append({"what": f"{key} created by you (do mint_title, sent {mine.get('date')})",
                           "decided_by": "you", "counted_in": "your_acts", **tier_note(key)})
                continue
            if key[:2] in ("d_", "k_", "e_") and (t.get("how") is None or t.get("by") == "ai"):
                # (1074.02.08: d_meath came with gold -214 and no mint call)
                row = {"what": f"{key} gained not in a war: created for you by the game's AI, granted or "
                               "inherited (no read says which)", **tier_note(key)}
                acted = ai_created(key, out_events)
                apr = (acted.get("price") or {}).get("gold") if acted and isinstance(acted.get("price"), dict) \
                    else None
                pr = _CREATION_PRICE.get(key)
                drop = _num(snap.get("gold"), prev.get("gold"))
                if acted is not None and _isnum(apr):
                    # The bridge recorded the AI's creation in this step, with its price
                    row.update(expected=-apr, what=f"{key} created for you by the game's AI on {acted.get('date')} "
                                                   f"for {apr} gold (the bridge's record of its post, ai_acted)",
                               decided_by=ai_created_by(key, acted), lever=ai_create_lever(key))
                elif ai_posts_read(snap) and acted is None:
                    # The bridge's record of the AI's posts was read and holds no creation of it: a gold fall of about
                    # its price is no record that the AI created it
                    row["not_created_by_the_ai"] = ("the bridge's record of your ruler's AI posts in this step holds no "
                                                    f"creation of {key}")
                elif pr and pr.get("currency") == "gold" and drop is not None and drop <= -0.8 * pr["cost"]:
                    row.update(expected=-pr["cost"], what=f"{key} created for you by the game's AI: its "
                                                          f"price read on {pr.get('date')} was {pr['cost']} gold "
                                                          "and the step's gold fell by about that",
                               # Who decided, and the lever (1075.02.09 named none)
                               decided_by=(ai_created_by(key, {"price": {"gold": pr["cost"]}})
                                           if not key.startswith("d_")
                                           else "the game's AI for your ruler: " + AI_CREATE_RULE),
                               lever=ai_create_lever(key))
                ev.append(row)
        for a in out_events.get("ai_acted") or []:
            # A men-at-arms purchase the bridge recorded in this step, at the type's price in the game's database
            pr_ = a.get("price") if isinstance(a, dict) and isinstance(a.get("price"), dict) else {}
            if isinstance(a, dict) and a.get("what") == "men_at_arms" and _isnum(pr_.get("gold")):
                ev.append({"what": f"men-at-arms ({a.get('key') or 'type unread'}, {a.get('regiment') or 'a'} "
                                   f"regiment) bought for you by the game's AI on {a.get('date')} (ai_acted)",
                           "expected": -round(pr_["gold"], 2), "basis": MAA_PRICE_BASIS,
                           "lever": ai_spend_veto("men_at_arms")})
        inc = prev.get("monthly_income")
        hof = _hof_gold_step(prev, snap, out_events)
        if hof:
            ev.append({"what": f"your head of faith paid you gold for piety (his letter {HOF_GOLD_LETTER} came on "
                               f"{hof.get('date')})",
                       **({"expected": round(max(100.0, 18 * inc), 2)} if isinstance(inc, (int, float)) else {}),
                       "basis": HOF_GOLD_BASIS})
        ev += letter_rows(prev, snap, out_events, "gold")
        # Only an end of THIS
        # war recorded in THIS step with its gold counts it already (an end of an older war against the same
        # enemy, or one recorded without its gold, did not)
        acts = " ".join(x.get("what", "") for x in acts_in_step(prev, snap) if "gold" in x)
        dated: list = []                     # (date, gold) of the priced rows, for the AI picks' windows
        for w in out_events.get("wars_ended") or []:
            yrs = w.get("_gold_years")
            enemy = w.get("defender_id") if w.get("my_side") == "attacker" else w.get("attacker_id")
            if f"against {enemy}" in acts:
                continue
            if w.get("my_side") == "attacker" and w.get("outcome") == "defender_victory" and yrs:
                ev.append({"what": f"war lost as its attacker: {w.get('name')}",
                           **({"expected": -round(yrs * 12 * inc, 2)} if isinstance(inc, (int, float)) else {}),
                           "basis": REPARATIONS_BASIS.format(y=yrs)})
                if isinstance(inc, (int, float)) and w.get("end_date"):
                    dated.append((w["end_date"], -round(yrs * 12 * inc, 2)))
            elif w.get("my_side") == "defender" and w.get("outcome") == "defender_victory" and yrs:
                ev.append({"what": f"war won as its defender: {w.get('name')}",
                           "basis": (f"the losing attacker pays you {yrs} years of his income (the casus belli's "
                                     "on_defeat); his income is not read")})
        gains = event_gold_rows(out_events, inc)
        ev += gains
        dated += [(r["_date"], r["expected"]) for r in gains if _isnum(r.get("expected")) and r.get("_date")]
        for r in gains:
            r.pop("_date", None)
        ev += ai_pick_gold(prev, snap, out_events.get("events_answered") or [],
                           dated + _priced_in_step(prev, snap, out_events), out_events.get("ai_acted"))
    if key == "piety":
        # An answered event's own piety (culture_tradition_events.0100.b takes massive_piety_loss, 500) is priced
        # from its file before the head of faith's gold is guessed: a fall it explains is not his
        ev += _event_rows(out_events, "piety")
        ev += letter_rows(prev, snap, out_events, "piety")
        sp = spouse_changed(out_events)
        if sp:
            ev.append(sp)
        hof = _hof_gold_step(prev, snap, out_events)
        if hof:
            ev.append({"what": f"piety paid to your head of faith for gold (his letter {HOF_GOLD_LETTER} came on "
                               f"{hof.get('date')})", "expected": -HOF_GOLD_PIETY_LOSS, "basis": HOF_GOLD_BASIS})
    if key == "prestige":
        acts = " ".join(x.get("what", "") for x in _ACTS.get(snap.get("actor_id")) or [])
        for w in out_events.get("wars_started") or []:
            if w.get("my_side") == "attacker" and f"war {w.get('war_id')} " not in acts + " ":
                ev.append({"what": f"war declared: {w.get('name')}",
                           "basis": "a declaration costs prestige (the casus belli's cost)"})
        ended_by_you = _war_end_acts(prev, snap)
        for w in out_events.get("wars_ended") or []:
            row = dict({"what": f"war ended {w.get('outcome') or '(outcome unread)'}: {w.get('name')}"},
                       **war_end_prestige(w))
            mine = _act_that_ended(w, ended_by_you, len(out_events.get("wars_ended") or []))
            if mine is not None and "expected" in row:
                # (a test run: a surrender's -350 in your_acts AND here, so
                # unexplained read +253.16 for a real -96.84): your act ended it; its price is counted there
                row.pop("expected")
                row["counted_in"] = "your_acts"
                row["your_act"] = mine.get("what")
            ev.append(row)
        for j in out_events.get("allies_joined") or []:
            ev.append({"what": f"ally {j['ally_id']} joined your offensive war {j['war_id']}; called by "
                               + j["called_by"],
                       **({"expected": j["prestige"]} if isinstance(j.get("prestige"), (int, float)) else {}),
                       "basis": j["basis"]})
        a, b = prev.get("laws"), snap.get("laws")
        if isinstance(a, list) and isinstance(b, list):
            for law in sorted(set(b) - set(a)):
                ev.append(law_row(law, prev, snap))
        for t in out_events.get("titles_gained") or []:
            k = str(t.get("key") or "")
            mine = own_act_in_step(prev, snap, "mint_title", k) if k[:2] in CREATE_TITLE_PRESTIGE else None
            if mine is not None and (t.get("how") is None or t.get("by") == "you"):
                # The seat's own mint: its grant is in your_acts, not the AI's and not twice
                ev.append({"what": f"{k} created by you (do mint_title, sent {mine.get('date')})",
                           "decided_by": "you", "counted_in": "your_acts"})
                continue
            if k[:2] in CREATE_TITLE_PRESTIGE and (t.get("how") is None or t.get("by") == "ai"):
                # (1075.02.09: d_meath created, prestige +304.37 read "unexplained")
                ev.append({"what": f"{k} gained not in a war (created, granted or inherited: no read says which)",
                           "if_created": CREATE_TITLE_PRESTIGE[k[:2]],
                           "basis": "creating a title grants prestige by its tier (00_defines.txt "
                                    "CREATE_TITLE_PRESTIGE_GAIN: duchy 300, kingdom 400, empire 500)",
                           **({"expected": CREATE_TITLE_PRESTIGE[k[:2]]}
                              if _created_here(k, prev, snap) or ai_created(k, out_events) else {})})
                if ai_created(k, out_events):
                    # The bridge recorded the AI's creation in this step
                    ev[-1]["what"] = (f"{k} created for you by the game's AI on {ai_created(k, out_events).get('date')} "
                                      "(the bridge's record of its post, ai_acted)")
        for x in _own_acts_unpriced(prev, snap):
            ev.append(x)
        ev += _prestige_causes(prev, snap, out_events)
        ev += letter_rows(prev, snap, out_events, "prestige")
    ev += ai_acted_rows(key, prev, snap, out_events, ev)
    return ev


def _ai_acted_charge(a: dict, key: str) -> Optional[float]:
    """What an ai_acted row's price moves `key` by: every price of a post is what it costs (a decision's cost block,
    read signed as -300; the bridge's price, a positive amount), so its move is minus its size."""
    pr = a.get("price") if isinstance(a.get("price"), dict) else {}
    v = pr.get(key)
    if not _isnum(v) or not v:
        return None
    return -round(abs(float(v)), 2)


def ai_acted_rows(key: str, prev: dict, snap: dict, out_events: dict, ev: list) -> list:
    """A row for each post the game's AI made for your ruler in this step (ai_acted) whose price moves `key`, with
    that price as `expected`: a priced post is a priced row of the step, subtracted whatever the amount (a decision's
    -300 beside a -421.61 fall). A post another row of the step already prices (a men-at-arms purchase's or a
    creation's gold, a law's pass price) is not counted again."""
    lo, hi = _ordinal_of(prev.get("date")), _ordinal_of(snap.get("date"))
    whats = " | ".join(str(e.get("what") or "") for e in ev if isinstance(e, dict))
    out: list = []
    for a in out_events.get("ai_acted") or []:
        if not isinstance(a, dict):
            continue
        amt = _ai_acted_charge(a, key)
        d = _ordinal_of(a.get("date"))
        if amt is None or (lo is not None and hi is not None and d is not None and not lo <= d <= hi):
            continue
        what, k = a.get("what"), a.get("key")
        if key == "gold" and what == "men_at_arms":
            continue                                     # its own row above
        if what == "create_title" and k and f"{k} created for you by the game's AI on" in whats:
            continue
        if what == "law" and k and any(isinstance(e, dict) and e.get("what") == f"realm law passed: {k}"
                                       and "expected" in e for e in ev):
            continue
        out.append({"what": f"{what}{' ' + k if k else ''} taken for your ruler by the game's AI on {a.get('date')} "
                            "(ai_acted)",
                    "expected": amt, "basis": a.get("price_basis") or a.get("price_source")
                    or "the price the bridge read for the post",
                    **({"lever": a["veto"]} if isinstance(a.get("veto"), dict) else {})})
    return out


#: The least gold an answered event's option gives for the purse to price it
EVENT_GOLD_MIN = 1.0


def event_gold_rows(out_events: dict, monthly_income: Any) -> list:
    """A row for each event the game answered for your ruler in the step whose answered option gives gold, by the
    script value its file names at the income read (marriage_effect.2001.a: major_treasury_or_gold_value, a year's
    income, at least 75): `expected` when the option gives it unconditionally, else named. `_date` is the pick's
    date (for the AI picks' windows; dropped before the row is shown)."""
    import event_prices as _ep  # noqa: PLC0415
    out: list = []
    for e in out_events.get("events_answered") or []:
        if not isinstance(e, dict) or not e.get("event") or e.get("event") in CLAIM_LETTERS:
            continue
        g = _ep.option_gold_given(e.get("event"), e.get("applied"), monthly_income)
        if not g:
            continue
        row = {"what": f"event {e.get('event')} answered {e.get('applied')} ({e.get('date')}): its option gives gold",
               "basis": g.get("basis") or g.get("rule") or g.get("unknown"), "_date": e.get("date")}
        if _isnum(g.get("gold")) and g["gold"] >= EVENT_GOLD_MIN and not g.get("conditional"):
            row["expected"] = round(g["gold"], 2)
        elif g.get("conditional") and _isnum(g.get("gold")):
            row["if_its_conditions_held"] = round(g["gold"], 2)
        else:
            row["unknown"] = g.get("unknown") or "its amount is computed in play"
        out.append(row)
    return out


_CULTURE_WARNED: dict = {}
#: The decision the AI took for the seat's ruler at 1,000 prestige in both arms of one recorded series
CONVERT_DECISION_KEY = "convert_to_local_culture_decision"


async def culture_conversion_ahead(gw, snap: dict) -> Optional[dict]:
    """The ruler's culture differs from his capital county's, so the game's AI takes
    convert_to_local_culture_decision for him (ai_will_do +200 for an independent count with a domain of
    three counties or fewer) once his prestige covers its price: 1087.10 in one game and 1082-83 in another, -1,000
    each, Domnall irish -> norse at Dublin; a cultural populist faction followed both times. The row
    carries the decision veto keyed to it. Said once per ruler and pair of cultures, with the price and the
    prestige held."""
    import event_prices as _ep  # noqa: PLC0415
    me, mine, cap = snap.get("actor_id"), snap.get("culture"), snap.get("primary_title_key")
    if not (me and mine and isinstance(cap, str) and cap.startswith("c_")):
        return None
    key = (me, mine)
    if key in _CULTURE_WARNED:
        return None
    raw = await gw.read("query_map_graph", {"character_id": me, "scope": "explicit", "counties": cap,
                                            "hops": 0, "max_nodes": 4})
    import map_view as _mv  # noqa: PLC0415
    g = _mv.parse_graph((raw or {}).get("text") if isinstance(raw, dict) else "")
    node = g.get(cap) or g.get(cap[2:]) or {}      # the graph spells county keys whole (@c_dublin)
    theirs = node.get("culture")
    if not theirs or theirs == mine:
        return None
    _CULTURE_WARNED[key] = theirs
    _ep.CAPITAL_CULTURE[me] = theirs
    price = _ep.convert_price(mine, theirs)
    have = snap.get("prestige")
    return {"your_culture": mine, "capital": cap, "capital_culture": theirs,
            "price": price.get("prestige"), "multipliers": price.get("multipliers"),
            "prestige": round(have, 2) if isinstance(have, (int, float)) else None,
            "what": ("the game's AI converts your ruler to his capital's culture (convert_to_local_culture_decision) "
                     "when his prestige covers the price; a cultural populist faction can follow it"),
            **({} if "decision" in (snap.get("ai_vetoes") or []) and (
                not snap.get("ai_veto_decision_keys") or CONVERT_DECISION_KEY in snap["ai_veto_decision_keys"])
               else {"veto": ai_veto_call("decision", [CONVERT_DECISION_KEY])}),
            "basis": _ep.CONVERT_SOURCE}


#: (a seated game, 1071.09.08: the AI took take_vow_of_poverty_decision for Murchad; free, no cost block;
#: monthly income read 0.93 before and 0.44 two weeks after): the decisions in common/decisions (1.18.3.1) that
#: have no cost block, that the AI takes for a 1066 count (ai_will_do not ruled out for him), and that cut
#: income, levy, prestige gain or opinion for years. Not a blanket veto: the free stress_loss_* decisions stay.
#: Scanned: 35 no-cost decisions add a modifier or trait; the others are oaths (bonuses, achievement DLC),
#: deity picks, weight, adventurer/admin/Japan decisions out of reach in 1066 Ireland.
FREE_DECISIONS_THAT_COST = {
    "take_vow_of_poverty_decision": ("vow_of_poverty_modifier for life: monthly income -20% "
                                     "(00_religion_modifiers.txt:24-31); +2 piety a month"),
    # (1086.02.08, 1087.11.05: taken by the AI, -350 prestige each time)
    "isolate_family_decision": ("fires epidemic_events.0100, whose only option is -350 prestige "
                                "(06_ce1_decisions.txt; events/dlc/ce1/epidemic_events.txt)"),
}
_FREE_WARNED: set = set()


def free_decisions_ahead(snap: dict) -> Optional[dict]:
    """Once per ruler: the free decisions the AI may take for him that cost him for years, each with the keyed
    veto unless already keyed."""
    me = snap.get("actor_id")
    if not me or me in _FREE_WARNED:
        return None
    _FREE_WARNED.add(me)
    keyed = set(snap.get("ai_veto_decision_keys") or [])
    every = "decision" in (snap.get("ai_vetoes") or []) and not keyed
    rows = [{"decision": k, "effect": v} for k, v in FREE_DECISIONS_THAT_COST.items() if not every and k not in keyed]
    if not rows:
        return None
    return {"decisions": rows, "why": "no cost block: the game's AI may take them for your ruler unasked",
            "veto": ai_veto_call("decision", [r["decision"] for r in rows]),
            "note": "ai_veto set ADDS these keys to the ones you hold"}


def _event_rows(out_events: dict, currency: str) -> list:
    """A row for each event the game answered for the ruler in the step whose answered option adds or takes
    `currency` (prestige or piety, event_prices.option_amount), with its amount when the file gives one."""
    import event_prices as _ep  # noqa: PLC0415
    out: list = []
    for e in out_events.get("events_answered") or []:
        if not isinstance(e, dict):
            continue
        p = _ep.option_amount(e.get("event"), e.get("applied"), currency)
        if not p or p.get("unread"):
            continue
        row = {"what": f"event {e.get('event')} answered {e.get('applied')} ({e.get('date')})",
               "basis": f"add_{currency} in that option ({p['file']})"}
        if isinstance(p.get("amount"), (int, float)):
            row["expected"] = p["amount"]
        if p.get("if_conditions_held") is not None:
            row["if_its_conditions_held"] = p["if_conditions_held"]
        out.append(row)
    return out


def _prestige_causes(prev: dict, snap: dict, out_events: dict) -> list:
    """A recorded case (-300 in 1070-71, -1,000 in 1087, -280 in 1092 read "partly unknown"):
    the prestige causes the game's files price, each with its amount: an event the game answered for
    the ruler whose option adds or takes prestige, his culture changed (the AI's
    convert_to_local_culture_decision), a war declared on a ruler he held a truce on."""
    import event_prices as _ep  # noqa: PLC0415
    import truces as _tr  # noqa: PLC0415
    out: list = _event_rows(out_events, "prestige")
    a, b = prev.get("culture"), snap.get("culture")
    if a and b and a != b and prev.get("actor_id") == snap.get("actor_id"):
        price = _ep.convert_price(a, b)
        out.append({"what": f"your ruler's culture changed {a} -> {b}: the game's AI took {_ep.CONVERT_DECISION}",
                    **({"expected": price["prestige"]} if isinstance(price.get("prestige"), int) else
                       {"price": price["prestige"]}),
                    "basis": price["basis"], **({"multipliers": price["multipliers"]} if "multipliers" in price else {})})
    tr = prev.get("truces")
    if isinstance(tr, dict):
        for w in out_events.get("wars_started") or []:
            d = _i(w.get("defender_id"))
            until = tr.get(d) if d is not None else None
            if w.get("my_side") == "attacker" and until and (_ordinal_of(until) or 0) > (_ordinal_of(prev.get("date")) or 0):
                out.append({"what": f"war declared on {d} inside your truce on him (until {until}): the truce broken",
                            "expected": _tr.BREAK_PRICE["prestige"], "and": "one prestige level, legitimacy",
                            "basis": _tr.BREAK_PRICE["source"] + "; the prestige and the level unless the "
                                     "flexible truces perk is held (the perk is not read here)"})
    return out


#: The age the game makes a character an adult (00_defines.txt MALE_ADULT_AGE, FEMALE_ADULT_AGE)
ADULT_AGE = 16
CHILD_CLAIM_X = 1.5
CHILD_FACTS_SOURCE = ("claim_cb_prestige_cost (common/script_values/99_casus_belli_values.txt): "
                      "CB_COST_ATTACKER_IS_CHILD x1.5 for a child pressing a claim on an adult; "
                      "CB_COST_DEFENDER_IS_CHILD x0.5 for an adult pressing one on a child; the gender rule "
                      "x1.25 (x0.75) when both share a faith with a male- (female-) dominated doctrine")


def _takes_all(w: dict) -> dict:
    """{"if_you_lose": plain words} for a war whose defeat takes your titles and life, else {}."""
    import redesign as _R  # noqa: PLC0415
    words = _R.takes_all_words(w.get("cb_type"), _side(w), "surrender")
    return {"if_you_lose": words} if words else {}


#: How many of the late ruler's casus-belli targets the briefing re-reads for the new one
BRIEF_CB_TARGETS = 3
BRIEF_PROOF = "built, not proven live"


async def ruler_change_briefing(gw, prev: dict, snap: dict, kingdom: Optional[str]) -> dict:
    """Item e: at a ruler change, beside the change itself: the casus belli the new ruler gained or lost
    against the targets his predecessor's cbs reads named (re-read for him now), the kingdom's price before and
    now with its basis (a personal discount dies with its holder), the perk points he holds with the perk note,
    and, when his dynasty is not the late ruler's, that the seat follows the title and not the blood."""
    import redesign as _R  # noqa: PLC0415
    old, new = prev.get("actor_id"), snap.get("actor_id")
    out: dict = {"proof": BRIEF_PROOF}
    da, db = prev.get("dynasty_id"), snap.get("dynasty_id")
    if da is not None and db is not None and da != db:
        out["another_dynasty"] = (f"your seat now plays {new} of dynasty {db}, not your late ruler's {da}: the seat "
                                  "follows whoever holds his title (a conqueror too), not his blood; a scored task "
                                  "ends there as a loss")
    if isinstance(snap.get("line_risk"), dict):
        # (1070.10.26: the new ruler's matrilineal marriage was found by the seat, not told)
        out["line_risk"] = {k: v for k, v in snap["line_risk"].items() if k != "levers"}
    seen = _R.cbs_seen(old)
    if seen:
        rows = []
        for tid, (date, before) in list(seen.items())[-BRIEF_CB_TARGETS:]:
            raw = await _R._cb_read_with_retry(gw, "query_cbs_for_target",
                                               {"attacker_id": new, "target_id": tid, "timeout_ms": 6000})
            now = _R.engine_valid_cbs(raw)
            row = {"target_id": tid, "before": before, "read": date}
            if now is None:
                row["now"] = "unread: the casus-belli read did not answer"
            else:
                row.update(lost=sorted(set(before) - set(now)), gained=sorted(set(now) - set(before)))
            rows.append(row)
        out["casus_belli"] = rows
    else:
        out["casus_belli"] = "unread: no cbs read of your late ruler's; call cbs(target_id) for each target"
    if isinstance(kingdom, str) and kingdom:
        was = _CREATION_PRICE_BY.get((old, kingdom))
        was = dict(was) if isinstance(was, dict) else None
        _blk, price = await _R.mint_title_price(gw, new, kingdom)
        row = {"title": kingdom, "was": (was or {}).get("cost", "unread: no price read under your late ruler"),
               **({"was_read": was.get("date")} if was and was.get("date") else {}),
               "now": (price or {}).get("cost", "unread")}
        old_pb = _PRICE_BASIS_BY.get((old, kingdom))
        if was and isinstance(old_pb, dict):
            row["was_basis"] = price_basis_words(old_pb)
        if isinstance((price or {}).get("cost"), (int, float)) and price.get("currency") == "gold":
            pb = await _R.creation_price_basis(gw, new, kingdom, price["cost"])
            row["basis"] = price_basis_words(pb)
            if was and isinstance(was.get("cost"), (int, float)) and abs(was["cost"] - price["cost"]) >= 0.005:
                why = price_change_why(old_pb, pb)
                if why:
                    row["why_changed"] = why
        out["price"] = row
    ls = await gw.read("query_lifestyle", {"character_id": new})
    pts = {x.get("lifestyle"): x.get("points") for x in (ls.get("lifestyles") or [])
           if isinstance(x, dict) and isinstance(x.get("points"), int) and x["points"] > 0} \
        if isinstance(ls, dict) and isinstance(ls.get("lifestyles"), list) else None
    out["perk_points"] = pts if pts is not None else "unread: query_lifestyle did not answer"
    t1 = turn_one_note(PERK_NOTE_ID)
    if t1:
        out["perk_note"] = t1
    return out


#: The turn-one note the ruler-change briefing shows again (its perk path)
PERK_NOTE_ID = "perk-path-ducal-conquest"


def turn_one_note(note_id: str) -> Optional[str]:
    """One served note's text, or None when the notes are off or it is not there."""
    import notes as _N  # noqa: PLC0415
    s = _N.SERVED
    if s is None or not s.enabled:
        return None
    return next((n["text"] for n in s.doc.get("notes") or [] if n.get("id") == note_id), None)


def price_basis_words(pb: Any) -> str:
    """creation_price_basis in one line (base, discount and its sources; a personal one dies with
    the ruler)."""
    if not isinstance(pb, dict) or "base" not in pb:
        return "unread: " + str((pb or {}).get("unread") or "the price's basis was not read")
    src = ", ".join(f"{x.get('source')} {x.get('gold')}" for x in pb.get("sources") or [])
    return (f"base {pb['base']} less {round(pb.get('discount', 0) * 100, 1)}%"
            + (f" ({src})" if src else "") + "; a perk or trait discount is the ruler's own and dies with him")


async def child_ruler_facts(gw, snap: dict) -> dict:
    """What the game changes for a child ruler that the seat can read: declaring war (no age or regency
    condition in declare_war_interaction, 00_war.txt is_valid), the claim-war price multipliers in force
    with their sizes, and when they end (the 16th year: the birthday itself is not read)."""
    import redesign as _R  # noqa: PLC0415
    age = snap.get("age")
    year = int(str(snap.get("date")).split(".")[0]) if _ordinal_of(snap.get("date")) is not None else None
    # an age N on this date: born N to N+1 years ago, so the 16th birthday falls in one of two years
    ends = (year + ADULT_AGE - int(age)) if year and isinstance(age, (int, float)) else None
    facts = await _R.claim_price_facts(gw, snap.get("actor_id"))
    mult = [{"x": CHILD_CLAIM_X, "when": "you press a claim on an adult ruler",
             "ends": (f"at your majority (age {ADULT_AGE}): in {ends - 1} or {ends} (the birthday is not read)"
                      if ends else f"at your majority (age {ADULT_AGE})")}]
    if facts is not None and facts.get("faith_gender") in ("male_dominated", "female_dominated"):
        female = not facts.get("is_male")
        dom = facts["faith_gender"]
        x = 1.25 if (female and dom == "male_dominated") or (not female and dom == "female_dominated") else 0.75
        mult.append({"x": x, "when": f"you press a claim on a {'man' if female else 'woman'} of your faith "
                                     f"({dom.replace('_', '-')} doctrine)", "ends": "never: not age"})
    out = {"age": age, "war_declaration": ("allowed: the game's declare_war_interaction has no age or regency "
                                           "condition (00_war.txt); its price is the claim price below"),
           "claim_price_multipliers": mult, "source": CHILD_FACTS_SOURCE,
           "regency": ("the game gives a child ruler a regent (a diarchy, 00_regencies.txt) until the majority; "
                       "who it is and his powers are not read")}
    if facts is None:
        out["unread"] = "your faith's gender doctrine and your sex were not read (claim_price_facts)"
    if facts is not None and facts.get("is_adult") is True:
        out["note"] = "the game's is_adult reads adult: the age read and the game disagree"
    return out


def own_act_in_step(prev: dict, snap: dict, intent: str, subject: str = "") -> Optional[dict]:
    """The act this server sent for the ruler in the step (its date between the two reads)
    whose `what` is `intent` and names `subject`, or None. THE test every row uses before it says the
    game's AI decided something: anything the seat did in the step is the seat's."""
    for x in reversed(acts_in_step(prev, snap)):
        w = str(x.get("what") or "").split()
        if w and w[0] == intent and (not subject or subject in w):
            return x
    return None


def ai_posts_read(snap: dict) -> bool:
    """The bridge's record of the AI's posts for the ruler (ai_veto_records) was read in this snapshot: a post not
    among them was not made through the paths it watches."""
    return isinstance(snap.get("ai_veto_records"), list)


def _created_here(key: str, prev: dict, snap: dict) -> bool:
    """The step's gold fell by about the creation price last read for `key` (the read that names
    the creation in the gold row): the prestige grant is then the creation's. Not when the bridge's record of the AI's
    posts was read and holds no creation of it (ai_posts_read): the shape is then no record."""
    if ai_posts_read(snap):
        return False
    pr = _CREATION_PRICE.get(key)
    drop = _num(snap.get("gold"), prev.get("gold"))
    return bool(pr and pr.get("currency") == "gold" and drop is not None and drop <= -0.8 * pr["cost"])


def _law_decided(law: str, snap: dict, prev: dict) -> dict:
    """A realm law that passed in the step: by a change_law this server sent, or by the
    game's AI for the ruler; the AI's own law passes have no per-ruler lever."""
    lo = _ordinal_of(prev.get("date"))
    own = [x for x in acts_in_step(prev, snap) if "change_law" in x.get("what", "")
           and law in x.get("what", "")]
    if own:
        return {"decided_by": "you (do change_law)"}
    row = {"decided_by": "the game's AI for your ruler: this server sent no change_law for it",
           "lever": ai_veto_call("law")}
    # The AI's post of this law, recorded by the bridge with the price it read then
    found = False
    for r in reversed(_seat_events.new_veto_records({}, snap)):
        if r.get("what") == "law" and r.get("key") == law and r.get("dropped") is not True \
                and (lo is None or (_ordinal_of(r.get("date")) or -1) >= lo):
            found = True
            pr = _seat_events.price_of(r)
            if pr is not None:
                row["price"] = pr
                # a veto test run: the row kept "its amount is not read" beside the read price
                row["basis"] = ("its pass price, read for your ruler by the game's own cost when its AI sent "
                                "the change (the step's change can hold other prestige moves)")
                if pr.get("prestige"):
                    row["expected"] = -pr["prestige"]
            break
    if not found and isinstance(snap.get("ai_veto_records"), list):
        # a veto test run (top_liege_redirected_to_border_wars_law: no post of the ruler's AI was recorded for it):
        # the change came another way, which ai_veto does not drop
        return {"decided_by": "not your ruler's own AI law change (the bridge recorded none for this law in the "
                              "step): another way, such as your liege's law or an event's effect; ai_veto does "
                              "not cover it"}
    return row


def law_row(law: str, prev: dict, snap: dict) -> dict:
    """The purse row of a realm law that passed in the step: who passed it (_law_decided) and its pass cost from its
    block in common/laws (event_prices.law_pass_cost): a law with no pass_cost block costs nothing (`expected` 0);
    an amount the file states is the step's charge when your ruler passed it (your act, or his AI's recorded
    post), and is named, not counted, when it came another way (the game's own switch, a liege's law)."""
    import event_prices as _ep  # noqa: PLC0415
    row = {"what": f"realm law passed: {law}",
           "basis": "a law's pass cost is prestige (the game's laws files); its amount is not read",
           **_law_decided(law, snap, prev)}
    if "expected" in row:
        return row
    lp = _ep.law_pass_cost(law)
    if not lp:
        return row
    if lp.get("none"):
        row.update(expected=0, basis=lp["basis"])
        return row
    if not _isnum(lp.get("prestige")):
        row["basis"] = lp["basis"] + ": " + "; ".join(f"{k} {v}" for k, v in (lp.get("unknown") or {}).items())
        return row
    by = str(row.get("decided_by") or "")
    row["basis"] = lp["basis"]
    if by.startswith("you"):
        own = [x for x in acts_in_step(prev, snap) if "change_law" in str(x.get("what")) and law in str(x.get("what"))
               and x.get("cost_known") and _isnum(x.get("prestige"))]
        if own:
            row["counted_in"] = "your_acts"
        else:
            row["expected"] = lp["prestige"]
    elif by.startswith("the game's AI for your ruler"):
        row["expected"] = lp["prestige"]
    else:
        row["pass_cost"] = lp["prestige"]
    return row


#: The game's AI passes laws for its ruler from its Laws pass, posting each as
#: a CAddLawCommand; the bridge's ai_veto drops that post for the ruler (re_ai_veto.md)
LAW_LEVER = ("the game's AI changes your ruler's laws itself (each change costs prestige: a crown-authority "
             "raise (100 + 10 a realm county, at most 1000) x his learning's factor, before faith and culture "
             "modifiers); ai_veto what=law drops its law changes for your ruler, and do change_law still changes "
             "one when you choose")


def _own_acts_unpriced(prev: dict, snap: dict) -> list:
    """A recorded case (1089.08: the seat took stress_loss_drunkard_decision, whose prestige loss is
    applied by its event later; the next step read -139.92 'cause unknown'): the acts this server
    sent in the step (or the one before) whose cost was not read at the send."""
    # A name with no amount: an act on a read's own day is named in both steps that day bounds (its cost may come
    # in either), so the dates decide here, not act_in_step (which keeps a priced act in one step)
    lo, hi = _ordinal_of(prev.get("date")), _ordinal_of(snap.get("date"))
    return [{"what": f"your act {x['what']} (sent {x.get('date')}): its cost was not read when it was sent; "
                     "a decision's or interaction's event may charge it later"}
            for x in _ACTS.get(snap.get("actor_id")) or []
            if not x.get("cost_known") and x.get("unpriced_kind") == "deferred" and lo is not None
            and hi is not None and lo <= (_ordinal_of(x.get("date")) or -1) <= hi]


#: A claim gained in a step the chaplain's fabrication did not end in
CLAIM_NOT_LETTER = ("not the chaplain's claim letter, so no price is expected: {why}. How the claim came "
                    "(an event, a decision, a relative) is not read")


def _fab_province(rows: Any) -> Any:
    """The target province of the fabricate task in council_tasks rows; None when none runs;
    the string "unread" when the rows were not read."""
    if not isinstance(rows, list):
        return "unread"
    r = next((x for x in rows if isinstance(x, dict) and x.get("task_name") == FABRICATE_TASK), None)
    return r.get("target_province_id") if r else None


def _letter_claims(prev: dict, snap: dict, gained: list) -> dict:
    """Which claims gained in this step are the chaplain's letter. {title_id: True} (priced
    as the letter), False (not the letter: the evidence in "_why"), None (one of several is, which
    is not read; "_one_of" true prices the one letter once). Evidence, in order: the letter's picks
    recorded for a watched ruler; the fabrication's province before and after the step (the same
    province on both sides: the task did not finish, so no letter came); else the claims are priced
    as letters (the reads cannot tell)."""
    out: dict = {}
    ids = [c.get("title_id") for c in gained if isinstance(c, dict)]
    if not ids:
        return out
    if snap.get("event_watched") and isinstance(snap.get("event_picks"), list):
        last = max([p.get("seq") or 0 for p in prev.get("event_picks") or [] if isinstance(p, dict)] or [0])
        paid = [p for p in snap["event_picks"] if isinstance(p, dict) and (p.get("seq") or 0) > last
                and p.get("event") in CLAIM_LETTERS and not _declined(p)]
        if not paid:
            out.update({t: False for t in ids})
            out["_why"] = ("no chaplain's claim letter (" + " or ".join(CLAIM_LETTERS)
                           + ") was answered for your ruler in this step (the event record)")
            return out
        if len(paid) >= len(ids):
            out.update({t: True for t in ids})
            if any(_applied_key(p) == CLAIM_DUCHY_OPTION for p in paid):
                out["_duchy"] = True
            return out
    a, b = _fab_province(prev.get("council_tasks")), _fab_province(snap.get("council_tasks"))
    if a not in (None, "unread") and a == b:
        out.update({t: False for t in ids})
        out["_why"] = (f"your chaplain's fabrication ran on province {a} at the start and at the end of this "
                       "step, so it made no claim in it")
        return out
    if len(ids) > 1 and a not in (None, "unread") and b != a:
        # one fabrication ended: one letter among these claims
        out.update({t: None for t in ids})
        out["_one_of"] = True
        return out
    out.update({t: True for t in ids})
    return out


def _applied_key(pick: dict) -> Any:
    ap = pick.get("applied")
    return ap.get("key") if isinstance(ap, dict) else ap


def letter_rule_calls(cost: Any) -> dict:
    """The rule for each event of the claim letter's family. Pay_if on both; 0302 has three options (a
    county claim, a duchy claim, decline), so its rule names the decline as `else` (and a rule set on either event is
    set on the other, seat_events.EVENT_VARIANTS)."""
    amt = int(cost) if isinstance(cost, (int, float)) else "<gold>"
    return {e: {"tool": "do", "args": {"intent": "event_rule", "args": {
                "op": "set", "event": e, "answer": "pay_if", "option": e + ".a", "currency": "gold", "amount": amt,
                **({"else": CLAIM_DECLINE_OPTION} if e != CLAIM_LETTERS[0] else {})}}}
            for e in CLAIM_LETTERS}


def _letter_decided(snap: dict, cost) -> dict:
    """A recorded case (five letters, 492 gold, every one paid by the game's AI; no rule was
    ever set): who decided the claim letter's price, and the call that decides it instead.
    The family (0301, 0302); a rule on one does not cover the other."""
    rules = snap.get("event_rule_events")
    if rules is None:
        return {"decided_by": "not read (query_event_rules did not answer)"}
    have = [e for e in CLAIM_LETTERS if e in rules]
    if len(have) == len(CLAIM_LETTERS):
        return {"decided_by": "your event rules on " + " and ".join(CLAIM_LETTERS)}
    calls = letter_rule_calls(cost)
    miss = [e for e in CLAIM_LETTERS if e not in rules]
    return {"decided_by": (f"your event rule on {have[0]}, or the game's AI where the letter came as " if have
                           else "the game's AI for your ruler: no event rule on ") + " or ".join(miss),
            "lever": calls[miss[0]], **({"lever_also": calls[miss[1]]} if len(miss) > 1 else {})}


def letters_read(snap: dict) -> bool:
    """The step's answered events were read (a watched ruler, the picks answered): a letter not among them was not
    received."""
    return bool(snap.get("event_watched")) and isinstance(snap.get("event_picks"), list)


def _letter_in_step(out_events: Optional[dict], event: str) -> Optional[dict]:
    """The step's answered-event row of `event`, or None."""
    return next((e for e in (out_events or {}).get("events_answered") or []
                 if isinstance(e, dict) and e.get("event") == event), None)


def _hof_gold_step(prev: dict, snap: dict, out_events: Optional[dict] = None) -> Optional[dict]:
    """The head of faith's gold came in this step: its accept letter (HOF_GOLD_LETTER) is among the step's answered
    events, and the seat's own ask (an act this server sent, priced in your_acts) is not in the step. Returns the
    letter's row, else None. No shape of the purse (gold up, piety down by about 250) stands in for the letter."""
    row = _letter_in_step(out_events, HOF_GOLD_LETTER)
    if row is None:
        return None
    own = [x for x in acts_in_step(prev, snap) if "hof_ask_for_gold" in x.get("what", "") and x.get("cost_known")]
    return None if own else row


#: How long before a letter the seat's own send of its interaction is looked for (the recipient's AI answers in
#: days; the letter can come in the step after the send's)
LETTER_ACT_DAYS = 60


def _short(name: str) -> str:
    return name[: -len("_interaction")] if name.endswith("_interaction") else name


def letter_sender(prev: dict, snap: dict, out_events: dict, e: dict, li: dict) -> dict:
    """Who sent the interaction an answered letter `e` is the receipt of, from the rows the same news carries and
    the acts this server sent, across steps: {"by": words, "interaction": the one that ran or None, "ally_id"?}.
    In order: an ask of yours this news answers with this letter (asks_answered names the letter and its date); an
    act of yours of that interaction sent up to LETTER_ACT_DAYS before the letter, in this step or an earlier one; a
    call of your ruler's AI recorded in this step (ai_acted ally_call) before it; else the game's AI."""
    event, date = e.get("event"), str(e.get("date") or "")
    names = [li.get("interaction")] + [x.split(" ")[0] for x in li.get("one_of") or []]
    names = [n for n in dict.fromkeys(names) if n]
    for r in out_events.get("asks_answered") or []:
        if isinstance(r, dict) and r.get("interaction") in names and event and date \
                and f"{event} to " in str(r.get("how") or "") and date in str(r.get("how") or ""):
            return {"by": f"you (your act do {_short(r['interaction'])} to {r.get('target_id')}, sent {r.get('sent')}; "
                          "its answer is in asks_answered)",
                    "interaction": r["interaction"], "ally_id": r.get("target_id"), "act": "ask"}
    d = _ordinal_of(date)
    for x in reversed(_ACTS.get(snap.get("actor_id")) or []):
        w = str(x.get("what") or "")
        n = next((n for n in names if n in w.split()), None)
        xd = _ordinal_of(x.get("date"))
        if n and d is not None and xd is not None and 0 <= d - xd <= LETTER_ACT_DAYS:
            return {"by": f"you (your act {w}, sent {x.get('date')})", "interaction": n, "act": x}
    if CALL_ALLY in names:
        lo = _ordinal_of(prev.get("date"))
        calls = [a for a in out_events.get("ai_acted") or [] if isinstance(a, dict) and a.get("what") == "ally_call"
                 and (_ordinal_of(a.get("date")) or 1 << 30) <= (d if d is not None else 1 << 30)
                 and (lo is None or (_ordinal_of(a.get("date")) or -1) >= lo)]
        if calls:
            c = calls[-1]
            return {"by": (f"the game's AI for your ruler: its call to ally {c.get('ally_id')} into war "
                           f"{c.get('war_id')} on {c.get('date')} (ai_acted)"),
                    "interaction": CALL_ALLY, "ally_id": c.get("ally_id"), "act": "ai"}
    return {"by": "the game's AI for your ruler: no act of yours sent it (in this step or the "
                  f"{LETTER_ACT_DAYS} days before the letter)", "interaction": None}


def letter_rows(prev: dict, snap: dict, out_events: dict, currency: str) -> list:
    """A row for each answered event of the step that is the letter an interaction's on_accept / on_decline sends
    (event_prices.interaction_letter), whose block moves `currency` for your ruler: the interaction ran, and what
    its block charged, from its file. Who sent it (letter_sender): your own act of that interaction, in this step or
    the one before it (the letter comes days after the send), else the game's AI for your ruler (when the letter
    went to the sender) or another character (when it went to the recipient). A letter several interactions send
    names them (`senders`) and is priced only when an act names the one that ran. An own act whose cost was read at
    the send is counted in your_acts; an ally's joining that an allies_joined row prices by his tier is counted
    there. The head of faith's gold has its own rows (_hof_gold_step)."""
    import event_prices as _ep  # noqa: PLC0415
    out: list = []
    for e in out_events.get("events_answered") or []:
        if not isinstance(e, dict) or e.get("event") in (HOF_GOLD_LETTER,):
            continue
        li = _ep.interaction_letter(e.get("event")) or {}
        if not li:
            continue
        snd = letter_sender(prev, snap, out_events, e, li) if li.get("to") == "scope:actor" else {}
        lc = _ep.letter_charge(e.get("event"), currency, snd.get("interaction"))
        if not lc:
            continue
        name = lc["interaction"] if not lc.get("senders") else " or ".join(x.split(" ")[0] for x in lc["senders"])
        mine = [x for x in acts_in_step(prev, snap) if lc["interaction"] in str(x.get("what") or "").split()]
        if li.get("to") == "scope:actor":
            by = snd["by"]
        else:
            by = f"another character, to your ruler (the letter goes to the interaction's {li.get('to')})"
        if lc.get("senders"):
            # which interaction (or other script) sent it is not read: the row says the letter came, and of what
            what = (f"letter {e.get('event')} came on {e.get('date')}: sent by {name}"
                    + (" or by another script that fires it (an event, an effect)" if lc.get("elsewhere") else ""))
        else:
            what = (f"{name} {'accepted' if lc['block'] == 'on_accept' else 'answered'}: its letter "
                    f"{e.get('event')} came on {e.get('date')}")
        row = {"what": what, "sent_by": by, "basis": lc["basis"]}
        joined = next((j for j in out_events.get("allies_joined") or [] if snd.get("ally_id") is not None
                       and _i(j.get("ally_id")) == _i(snd.get("ally_id"))), None) \
            if lc["interaction"] == CALL_ALLY and currency == "prestige" else None
        if joined is not None:
            row["counted_in"] = (f"the ally-joined row of {joined.get('ally_id')} (priced by his title's tier: "
                                 "the branch that applies)")
        elif any(x.get("cost_known") and _isnum(x.get(currency)) for x in mine):
            row["counted_in"] = "your_acts"
        elif _isnum(lc.get("amount")):
            row["expected"] = lc["amount"]
        if joined is None:
            for x in ("if_its_conditions_held", "one_of", "range", "senders", "elsewhere"):
                if lc.get(x) is not None:
                    row[x] = lc[x]
        if not any(x in row for x in ("expected", "counted_in", "if_its_conditions_held", "one_of", "range",
                                      "senders")):
            continue                 # no amount of this currency stands behind it: nothing to count or weigh
        out.append(row)
    return out


#: The piety a divorce costs its sender (divorce_character_interaction's cost, the script value divorce_cost); the
#: interaction's letter goes to the divorced spouse, so the ruler's own answered events never hold it
DIVORCE_INTERACTION = "divorce_character_interaction"


def spouse_changed(out_events: dict) -> Optional[dict]:
    """A purse row, named and not priced, for a step whose council read shows the spouse's seat held by another
    character: a divorce the ruler's AI sent costs divorce_cost piety, a spouse's death costs none, and which it was
    is not read."""
    import event_prices as _ep  # noqa: PLC0415
    for r in out_events.get("council_changed") or []:
        if not isinstance(r, dict) or not str(r.get("seat") or "").endswith("spouse"):
            continue
        a, b = r.get("before"), r.get("now")
        ha = a.get("holder_id") if isinstance(a, dict) else None
        hb = b.get("holder_id") if isinstance(b, dict) else None
        if ha and ha != hb:
            cost = _ep.script_value("divorce_cost")
            return {"what": f"your ruler's spouse seat changed holder: {ha} -> {hb or 'none'} (the council read)",
                    **({"if_a_divorce": -cost} if cost is not None else {}),
                    "basis": (f"a divorce your ruler's AI sends ({DIVORCE_INTERACTION}) costs divorce_cost piety "
                              "(its cost block); a spouse's death costs none; which it was is not read")}
    return None


#: The least piety move the purse names what it ruled out for
PIETY_CAUSE_MIN = 50


def _piety_ruled_out(prev: dict, snap: dict, out_events: dict) -> dict:
    """{ruled_out, not_read} for a piety move the reads do not explain: the head of faith's gold by its letter
    (and the last acceptance read of it), the interactions whose letters charge piety, the seat's own acts."""
    ruled, unread = [], []
    if letters_read(snap):
        if _letter_in_step(out_events, HOF_GOLD_LETTER) is None:
            ruled.append(f"not the head of faith's gold: its accept letter {HOF_GOLD_LETTER} is not among this "
                         f"step's {len(out_events.get('events_answered') or [])} answered events")
        ruled.append("no other answered event of this step is an interaction's letter whose block charges piety"
                     if not letter_rows(prev, snap, out_events, "piety") else
                     "the interactions whose letters came are priced above; no other letter came")
    else:
        unread.append("the step's answered events (the letters an interaction sends when it runs: the head of "
                      "faith's gold, an excommunication request) were not read")
    no = _ACCEPT_NO.get((snap.get("actor_id"), HOF_GOLD_SEND))
    if no:
        ruled.append(f"your last read of the head of faith's gold said no (acceptance on {no.get('date')})")
    if not [x for x in acts_in_step(prev, snap) if x.get("cost_known") and _isnum(x.get("piety"))]:
        ruled.append("no act of yours with a piety cost")
    unread += ["piety a decision or an event's option computes in play", "a war's end in piety (a holy war)"]
    return {"ruled_out": ruled, "not_read": unread}


def _num(b, a) -> Optional[float]:
    """b - a when both are numbers, else None."""
    ok = all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in (a, b))
    return (b - a) if ok else None


#: Per ruler, the gold that moved a month over the last step read (all spends in it)
_REALISED: dict = {}


def realised_net(actor_id: Any) -> Optional[dict]:
    """The last step's gold change a month for this ruler (income less every spend), or None (no step read)."""
    return _REALISED.get(actor_id)


def note_one_offs(actor_id: Any, known: list, ev: list, change: float, lo: Any, hi: Any) -> None:
    """("never at the last step's net (-40.59 gold a month)" for a step that held the seat's
    own 162.5-gold d_ulster mint): the step's priced one-offs (the seat's own acts with a known gold cost, and the
    purse's priced rows other than the levy upkeep) beside the realised net, and the net without them, so the
    months to a price are not read off a purchase. Nothing is added when the step had none."""
    rec = _REALISED.get(actor_id)
    if not isinstance(rec, dict) or lo is None or hi is None or hi <= lo:
        return
    rows = [(str(x.get("what") or "your act"), float(x["gold"])) for x in known
            if isinstance(x.get("gold"), (int, float)) and not isinstance(x.get("gold"), bool) and x["gold"]]
    rows += [(str(e.get("what") or "a priced row"), float(e["expected"])) for e in ev or []
             if isinstance(e, dict) and isinstance(e.get("expected"), (int, float))
             and not isinstance(e.get("expected"), bool) and e["expected"]
             and e.get("basis") != ARMY_UPKEEP_BASIS]
    rec.pop("one_offs", None)
    rec.pop("monthly_without_one_offs", None)
    if not rows:
        return
    total = round(sum(v for _w, v in rows), 2)
    rec["one_offs"] = {"total": total, "rows": [{"what": w[:80], "gold": round(v, 2)} for w, v in rows[:4]]}
    rec["monthly_without_one_offs"] = round((change - total) / ((hi - lo) / 30.44), 2)


def _resource_change(prev: dict, snap: dict, key: str, out_events: Optional[dict] = None) -> Optional[dict]:
    a, b = prev.get(key), snap.get(key)
    # Every step has its
    # row; a change under 1 is the row with its numbers, a read that did not answer says so
    if not isinstance(a, (int, float)) or not isinstance(b, (int, float)):
        if key in prev and key in snap and (isinstance(a, (int, float)) or isinstance(b, (int, float))):
            return {"unread": f"the {key} read did not answer in this observe or the last"}
        return None
    lo, hi = _ordinal_of(prev.get("date")), _ordinal_of(snap.get("date"))
    if key == "gold" and lo is not None and hi is not None and hi > lo:
        # The gold that moved a month over this step, every spend in it (the income read leaves the
        # expenses out; the bridge's expense fields are not used: they read 0 in every recorded reply).
        # Before the small-change return, so a still purse is a step read too (not the step before)
        _REALISED[snap.get("actor_id")] = {"monthly": round((b - a) / ((hi - lo) / 30.44), 2),
                                           "from": prev.get("date"), "to": snap.get("date"),
                                           # The men raised at both reads (their levy upkeep was in the step)
                                           **({"armies_men": [prev.get("armies_men"), snap.get("armies_men")]}
                                              if isinstance(snap.get("armies_men"), int) else {})}
    acts = acts_in_step(prev, snap)
    known = [x for x in acts if x.get("cost_known") and key in x]
    if abs(b - a) < 1:
        if key == "gold":
            note_one_offs(snap.get("actor_id"), known, [], b - a, lo, hi)
        return {"was": round(a, 2), "now": round(b, 2), "change": round(b - a, 2)}
    out: dict = {"was": round(a, 2), "now": round(b, 2), "change": round(b - a, 2)}
    if known:
        out["your_acts"] = [{"what": x["what"], key: x[key]} for x in known]
    ev = _events_for(key, prev, snap, out_events or {})
    if ev:
        out["in_this_step"] = ev
    inc = prev.get("monthly_income") if key == "gold" else prev.get(f"monthly_{key}")
    if key in ("gold", "prestige", "piety") and isinstance(inc, (int, float)) and lo is not None and hi is not None:
        # Prestige and piety too, from the game's own monthly change (the rate read at the last
        # observe, over the days between: the game adds it once a month)
        exp = round(inc * (hi - lo) / 30.44, 2)
        out["income"] = {"monthly": round(inc, 2), "expected": exp}
    if key == "gold" and lo is not None and hi is not None:
        up = army_upkeep(prev.get("armies_men"), snap.get("armies_men"), (hi - lo) / 30.44)
        if up is not None:
            out.setdefault("in_this_step", []).append(up)
        note_one_offs(snap.get("actor_id"), known, out.get("in_this_step") or [], b - a, lo, hi)
    rest = unexplained_of(out, key)
    if key == "gold" and rest < -1 and isinstance(inc, (int, float)) and lo is not None and hi is not None:
        # What the income read leaves out, with its amount (a seated game's records)
        out["likely"] = {"expenses_not_in_the_income_read": round(INCOME_READ_SHORT * (hi - lo) / 30.44, 2),
                         "basis": INCOME_READ_SHORT_BASIS}
    unread = PURSE_UNREAD if key == "gold" or not isinstance(inc, (int, float)) else PURSE_UNREAD_AI
    if not known and not ev and "income" not in out:
        out["cause"] = "unknown: " + unread
    elif abs(rest) >= _unexplained_floor(out):
        out["unexplained"] = rest
        if not ev or not any("expected" in e for e in ev):
            out["cause"] = "partly unknown: " + unread
    left = out.get("unexplained")
    if _isnum(left) and lo is not None and hi is not None:
        m = price_match(key, left, out_events or {}, lo, hi, (out.get("income") or {}).get("expected"))
        if m:
            out["matches_price"] = m
            # (the 300 of a court type matched, and the same block still said "unexplained" beside it): a matched
            # price is explained; what is left after it, if anything, is not
            settle(out, key)
        act = activity_ran(key, left, out_events or {})
        if act:
            out["activity"] = act
    rest = unexplained_of(out, key)
    if key == "prestige" and abs(rest) > PRESTIGE_CAUSE_MIN:
        # A prestige move over 50 the reads do not explain names what was ruled out; what is left is the change less
        # every priced row (the income too: a row that left it in read the whole change as unexplained)
        out["unexplained"] = rest
        out["ruled_out"] = _prestige_ruled_out(prev, snap, out_events or {}, acts)
        out["not_read"] = list(PRESTIGE_NOT_READ if not isinstance(inc, (int, float)) else PRESTIGE_NOT_READ_MONTHLY)
    if key == "piety" and _isnum(out.get("unexplained")) and abs(out["unexplained"]) > PIETY_CAUSE_MIN:
        # A piety move the reads do not explain says what was looked at, and stays unexplained
        out.update(_piety_ruled_out(prev, snap, out_events or {}))
    return out


#: Every kind of purse row that carries a price, and where its amount sits in the row. THE census of what
#: unexplained_of subtracts: a new kind of priced row is added here or it is not subtracted (the census test reads
#: the producers' code for `expected` and fails on one not listed)
PRICED_ROW_KINDS = {
    "your_acts": "your_acts[].<currency>: the acts this server sent in the step, their cost read at the send",
    "income": "income.expected: the monthly change read at the last observe, over the step's days",
    "in_this_step": "in_this_step[].expected: every row of the step the files or the bridge price (claims, titles, "
                    "wars, allies, laws, letters, events, the AI's priced posts, upkeep, hires, constructions); a row "
                    "with counted_in is counted where it says, never twice",
    "matches_price": "matches_price.<currency>: a dropped AI post whose price the fall matches",
}
#: The functions whose rows carry `expected` into a purse row, each with what it prices (the census test reads the
#: code for every function that writes an `expected` and fails on one that is not here or in NOT_PURSE_EXPECTED)
PRICED_ROW_PRODUCERS = {
    "_resource_change": "income.expected",
    "_events_for": "claims, the AI's creations and men-at-arms, the head of faith's gold, a lost war's reparations, "
                   "a war's end, an ally's joining, a title's creation",
    "war_end_prestige": "a war's end by its casus belli's block",
    "_prestige_causes": "a culture change, a broken truce",
    "_event_rows": "an answered event's option prestige or piety",
    "event_gold_rows": "an answered event's option gold",
    "ai_pick_gold": "the gold that fell after an AI-answered event or an activity",
    "ai_acted_rows": "a post the game's AI made with a price",
    "_law_decided": "a realm law's price the bridge read for the AI's post",
    "law_row": "a realm law's pass cost from its file",
    "letter_rows": "an interaction's letter: the block that sent it",
    "army_upkeep": "the upkeep of raised armies",
    "count_hires_in_step": "a company the AI hired in the step",
    "count_constructions_in_step": "a construction started in the step",
}
#: The functions that write an `expected` that is not a purse row's (the fabrication's months)
NOT_PURSE_EXPECTED = ("_fabrication_timing", "compact")
#: Rows of a purse that name an amount and are NOT subtracted, each with why
NAMED_NOT_SUBTRACTED = {
    "if_its_conditions_held": "a branch's amount: whether it ran is not read",
    "one_of": "the totals exclusive branches can come to: which ran is not read",
    "if_created": "a title gained not in a war: created, granted or inherited, which is not read",
    "if_a_divorce": "a spouse seat changed holder: a divorce or a death, which is not read",
    "likely": "the expenses the income read leaves out: an estimate for a small realm, not this step's",
    "mercenaries_expected": "a company first named now and not recorded as hired in this step",
    "constructions_expected": "the started constructions' price, beside the unexplained part",
    "pass_cost": "a realm law's pass cost, for a law that came another way than your ruler's own change",
    "unknown": "a row whose amount the files compute in play",
}


def priced_total(row: dict, key: str) -> float:
    """THE sum of every priced row of a purse row for `key` (PRICED_ROW_KINDS), each counted once: a row that says
    counted_in is counted where it says."""
    total = 0.0
    for x in row.get("your_acts") or []:
        if isinstance(x, dict) and _isnum(x.get(key)):
            total += x[key]
    for e in row.get("in_this_step") or []:
        if isinstance(e, dict) and _isnum(e.get("expected")) and not e.get("counted_in"):
            total += e["expected"]
    inc = (row.get("income") or {}).get("expected") if isinstance(row.get("income"), dict) else None
    if _isnum(inc):
        total += inc
    m = row.get("matches_price")
    if isinstance(m, dict) and _isnum(m.get(key)):
        total += m[key]
    return total


def unexplained_of(row: dict, key: str) -> float:
    """THE rule for a purse row's unexplained part: its change less every priced row of the step (priced_total)."""
    return round((row.get("change") or 0.0) - priced_total(row, key), 2)


def _unexplained_floor(row: dict) -> float:
    """The least unexplained part a purse row names: 1, or a tenth of the change."""
    ch = row.get("change") if _isnum(row.get("change")) else 0.0
    return max(1.0, 0.1 * abs(ch))


def settle(row: dict, key: str) -> None:
    """After a priced row is added to a purse row: its unexplained part again by unexplained_of, dropped with its
    cause when what is left is under the floor."""
    rest = unexplained_of(row, key)
    if abs(rest) >= _unexplained_floor(row):
        row["unexplained"] = rest
    else:
        row.pop("unexplained", None)
        if str(row.get("cause") or "").startswith("partly unknown"):
            row.pop("cause", None)


#: (a test run, step 5, 869.08.20-869.11.18: prestige -499.86 unexplained; the bridge's veto log: the AI's
#: court-type change to court_scholarly dropped 0869.10.06 at 566.60 prestige, the next record 71.58, one month's
#: income apart: 566.60 - 500 + 5.02; every court type costs 500, common/court_types/00_court_types.txt, and no
#: other of the 30 court_type records or of any kind was followed by such a fall): an unexplained move that
#: matches, within the step's income, the price of an AI post recorded in the step names that post
PRICE_MATCH_MIN = 100
PRICE_MATCH_WORDS = ("the {key} that moved matches, within this step's income, the {price} {key} price of the {what}"
                     " your ruler's AI posted on {date} ({source}); no other record of the step carries that amount."
                     "{veto}")
PRICE_MATCH_VETOED = (" The ai_veto dropped that post, so the charge came another way (the game's own path for the "
                      "change, not the one the veto watches); what changed is not read")


def price_match(key: str, left: float, out_events: dict, lo: int, hi: int, income: Any) -> Optional[dict]:
    """The AI post recorded in this step whose price explains the unexplained `left`, or None."""
    if abs(left) < PRICE_MATCH_MIN:
        return None
    tol = max(10.0, abs(income) if _isnum(income) else 0.0)
    hits = []
    # A post the AI made (ai_acted) with a price is a priced row of the step (ai_acted_rows), subtracted whatever the
    # amount; a dropped post's price can only be matched
    for src in ("ai_vetoed",):
        for r in out_events.get(src) or []:
            pr = (r.get("price") or {}).get(key) if isinstance(r, dict) and isinstance(r.get("price"), dict) else None
            d = r.get("last") or r.get("date") or r.get("first") if isinstance(r, dict) else None
            if not _isnum(pr) or not (lo <= (_ordinal_of(d) or -1) <= hi):
                continue
            if abs(left + abs(pr)) <= tol:
                hits.append((src, r, abs(pr), d))
    if len(hits) != 1:
        return None
    src, r, pr, d = hits[0]
    what = f"{r.get('what')}" + (f" ({r.get('key')})" if r.get("key") else "")
    return {"what": what, "date": d, key: -pr, "source": src,
            "words": PRICE_MATCH_WORDS.format(key=key, price=round(pr), what=what, date=d, source=src,
                                              veto=PRICE_MATCH_VETOED if src == "ai_vetoed" else "")}


#: An activity's own
#: events in the step say it ran; the game charges an activity's gold when it starts (its cost block in
#: common/activities/activity_types/<type>.txt, scaled by realm size and era), not read here
ACTIVITY_EVENT_PREFIXES = ("coronation", "ach_coronation", "feast", "hunt", "pilgrimage", "tournament", "wedding",
                           "activity_", "grand_activity", "funeral")


def activity_ran(key: str, left: float, out_events: dict) -> Optional[dict]:
    """A gold fall beyond GOLD_CAUSE_MIN in a step whose answered events are an activity's: name it."""
    if key != "gold" or left > -GOLD_CAUSE_MIN:
        return None
    evs = sorted({str(e.get("event") or "") for e in out_events.get("events_answered") or [] if isinstance(e, dict)
                  and str(e.get("event") or "").startswith(ACTIVITY_EVENT_PREFIXES)})
    if not evs:
        return None
    return {"events": evs[:4], "likely": ("an activity of your ruler ran in this step (its events were answered): the "
                                          "game charges its gold when it starts (the activity type's cost, by realm "
                                          "size and era, in common/activities/activity_types); not priced here")}


#: The game's upkeep of a raised
#: army, per man a month (common/defines/00_defines.txt NArmy GOLD_COST_PER_SOLDIER), and the men-at-arms'
#: raised upkeep, 3 x their unraised (00_men_at_arms_values.txt @high_maint_mult), not counted here
GOLD_COST_PER_SOLDIER = 0.003
ARMY_UPKEEP_BASIS = ("0.003 gold a man a month while raised (00_defines.txt GOLD_COST_PER_SOLDIER), the mean of the "
                     "men raised at the two reads (raised between reads is not seen); men-at-arms cost 3x their "
                     "upkeep while raised, not counted")
#: a seated game (every step with a gold read): the gold change fell short of the income read by 1.17 a
#: month over 192.5 months with no war and by 5.81 over 80.5 months with a war (armies raised): the read is the
#: income before expenses the game subtracts (men-at-arms upkeep, court positions; the balance is not read)
INCOME_READ_SHORT = -1.17
INCOME_READ_SHORT_BASIS = ("the income read leaves out expenses (men-at-arms upkeep, court positions; not read): for a "
                           "small realm gold falls roughly 1.2 a month short of it at peace and more at war with armies "
                           "raised; a larger realm's shortfall is larger. An estimate, not a rule")


def army_upkeep(men_was: Any, men_now: Any, months: float) -> Optional[dict]:
    """The purse row for the upkeep of raised armies in a step, or None when none was raised
    at either read (or a read did not answer)."""
    if not all(isinstance(x, int) and not isinstance(x, bool) for x in (men_was, men_now)):
        return None
    if not (men_was or men_now) or months <= 0:
        return None
    mean = (men_was + men_now) / 2
    return {"what": f"armies raised: {men_was} men at the last read, {men_now} now",
            "expected": -round(mean * GOLD_COST_PER_SOLDIER * months, 2), "basis": ARMY_UPKEEP_BASIS,
            "lever": "do army action=disband (the game allows it at home, not abroad or embarked)"}


#: The least unexplained gold fall the purse looks further into
GOLD_CAUSE_MIN = 50
def count_hires_in_step(g: dict, companies: list, hired_now: set) -> None:
    """A recorded case (a test run, step 65: "mercenaries_expected": -149.28 beside "unexplained": -198.03): a hire the
    bridge recorded in THIS step (ai_acted) is part of the step's explained change: its price estimate goes into
    in_this_step and out of `unexplained` (dropped, with its cause, when what is left is under the purse's floor).
    A company first named now but not recorded in this step keeps only mercenaries_expected (when it was hired is
    not read)."""
    est = [m["price_estimate"] for m in companies if m.get("company_id") in hired_now
           and isinstance(m.get("price_estimate"), (int, float))]
    if not est or not isinstance(g.get("unexplained"), (int, float)):
        return
    g.setdefault("in_this_step", []).append(
        {"what": "mercenary companies " + ", ".join(str(c) for c in sorted(hired_now))
                 + " hired for you by the game's AI in this step (ai_acted)",
         "expected": -round(sum(est), 2), "basis": MERC_AI_NOTE})
    _settle_added(g, -round(sum(est), 2))


#: What a grant row says when the AI sent it
GRANT_NO_VETO = "no ai_veto kind stops the AI's grants (the bridge counts them, it does not drop them)"


def granted_by_ai(prev: dict, snap: dict, lost: list) -> None:
    """A recorded case (a pre-flight run 867: c_berry, c_issoudun, c_orleans read "granted_to_vassal" with no sender; the
    bridge's census counted grant_titles_interaction 4 and grant_vassal_interaction 1 for the ruler): a county
    granted in a step in which the census of your ruler's AI's interactions counted more grants names that sender.
    The census counts only the game's AI's sends for a watched ruler, never this server's own."""
    rows = [r for r in lost if isinstance(r, dict) and r.get("how") == "granted_to_vassal"]
    was, now = prev.get("ai_grants_sent"), snap.get("ai_grants_sent")
    if not rows or not isinstance(now, list):
        return
    before = {x.get("interaction"): _i(x.get("count")) or 0 for x in was or [] if isinstance(x, dict)}
    sent = {x.get("interaction"): (_i(x.get("count")) or 0) - before.get(x.get("interaction"), 0)
            for x in now if isinstance(x, dict)}
    sent = {k: v for k, v in sent.items() if v > 0}
    if not sent:
        return
    last = max((str(x.get("last") or "") for x in now if isinstance(x, dict) and x.get("interaction") in sent),
               default=None)
    for r in rows:
        r["sent_by"] = (f"your ruler's AI: {', '.join(f'{k} x{v}' for k, v in sorted(sent.items()))} in this step "
                        f"(last {last}; the bridge's census of the AI's interactions)")
        r["veto"] = GRANT_NO_VETO


def join_constructions(ai_acted: list, built: list, actor_id: Any) -> None:
    """Each ai_acted construction row gets the price of the construction it started (constructions_started): by
    its building key, else (a pre-flight run Munster 1066.11.03: the row carries no key) the one
    construction started for the ruler on the row's own date."""
    by_b = {b.get("building"): b for b in built if isinstance(b.get("price_estimate"), (int, float))}
    for a in ai_acted:
        if not isinstance(a, dict) or a.get("what") != "construction":
            continue
        b = by_b.get(a.get("key"))
        if b is None:
            same = [x for x in built if x.get("started") == a.get("date") and x.get("ordered_by") == actor_id
                    and isinstance(x.get("price_estimate"), (int, float))]
            b = same[0] if len(same) == 1 else None
            if b is not None:
                a["building"] = b.get("building")
        if b is not None:
            a["price_estimate"] = {"gold": b["price_estimate"]}
            a.pop("price_unread", None)


def count_constructions_in_step(g: dict, built: list, actor_id: Any, ai_acted: list) -> None:
    """A recorded case (a pre-flight run Munster 1066.12.01: "unexplained": -100.77 with "in_this_step": [] beside a
    construction started 1066.11.03 for 100 gold and the step's own ai_acted row of it): the constructions your
    ruler ordered in this step with a price are part of the step's explained change, like an AI hire
    (count_hires_in_step): their price goes into in_this_step and out of `unexplained` (dropped, with its cause,
    when what is left is under the purse's floor)."""
    mine = [b for b in built or [] if b.get("ordered_by") == actor_id and isinstance(b.get("price_estimate"), (int, float))]
    if not mine or not isinstance(g.get("unexplained"), (int, float)):
        return
    acted = {a.get("date") for a in ai_acted if isinstance(a, dict) and a.get("what") == "construction"}
    total = round(sum(b["price_estimate"] for b in mine), 2)
    g.setdefault("in_this_step", []).append(
        {"what": "constructions started for your ruler in this step: " + ", ".join(
            f"{b.get('building')} at {b.get('county')} on {b.get('started')}"
            + (" (ai_acted: sent by the game's AI)" if b.get("started") in acted else "") for b in mine),
         "expected": total, "basis": CONSTRUCTION_NOTE})
    _settle_added(g, total)


def _settle_added(g: dict, amount: float) -> None:
    """A priced row of `amount` was added to the gold row `g`: its unexplained part again, by unexplained_of when the
    row carries its change (a whole purse row), else by taking the amount from the part (a row given only that)."""
    if _isnum(g.get("change")) and "was" in g:
        rest = unexplained_of(g, "gold")
    else:
        rest = round(g["unexplained"] - amount, 2)
    if abs(rest) >= _unexplained_floor(g):
        g["unexplained"] = rest
    else:
        g.pop("unexplained", None)
        g.pop("cause", None)


#: The basis of an AI men-at-arms purchase's price
MAA_PRICE_BASIS = ("the type's recruit price in the game's database (query_maa_types reads the same field), read "
                   "when the bridge recorded the purchase")
CONSTRUCTION_NOTE = ("a building started in this step (query_county_details: its start date and who "
                     "ordered it); the game charges its price when the construction starts: price_estimate is "
                     "the game's price for it now where its holding is read")
GOLD_NOT_READ = ("mercenaries the game's AI hired for you", "men-at-arms it raised or reinforced",
                 "events and decisions that charge gold")


async def constructions_started(gw, actor_id, since, date) -> Optional[list]:
    """The constructions in the ruler's counties started after `since` up to `date`
    ({building, started, ordered_by, county}), or None when the read did not answer."""
    if not actor_id:
        return None
    rep = await gw.read("query_county_details", {"character_id": actor_id})
    if not isinstance(rep, dict) or rep.get("error"):
        return None
    lo, hi = _ordinal_of(since), _ordinal_of(date)
    found: list = []

    def walk(o, county=None):
        if isinstance(o, dict):
            county = o.get("county_key") or o.get("name") or county
            c = o.get("construction")
            if isinstance(c, dict):
                st = _ordinal_of(c.get("started"))
                if lo is not None and hi is not None and st is not None and lo < st <= hi:
                    found.append({"building": c.get("building"), "started": c.get("started"),
                                  "ordered_by": c.get("ordered_by"), "county": county,
                                  **({"barony_title_id": o["barony_title_id"]}
                                     if _i(o.get("barony_title_id")) is not None else {})})
            for v in o.values():
                walk(v, county)
        elif isinstance(o, list):
            for v in o:
                walk(v, county)
    walk(rep)
    await _price_constructions(gw, actor_id, found)
    return found


#: The basis of
#: a started construction's price
CONSTRUCTION_PRICE_BASIS = ("the game's price for this building at this holding now (construction_check); "
                            "the construction was charged when it started")


async def _price_constructions(gw, actor_id, rows: list) -> None:
    """Each started construction with a holding gets `price_estimate` (gold) from the game's own price read
    (construction_check, one call per holding); a price not read says why."""
    by_loc: dict = {}
    for r in rows:
        if _i(r.get("barony_title_id")) is not None and r.get("building"):
            by_loc.setdefault(r["barony_title_id"], []).append(r)
    for loc, rs in by_loc.items():
        chk = await gw.read("construction_check", {"actor_id": actor_id, "location_id": loc,
                                                   "buildings": sorted({r["building"] for r in rs})})
        chk = chk if isinstance(chk, dict) else {}
        by_b = {v.get("building"): v for v in chk.get("rows") or [] if isinstance(v, dict)}
        for r in rs:
            v = by_b.get(r["building"]) or (chk if chk.get("building") == r["building"] else {})
            p = v.get("price") if isinstance(v.get("price"), dict) else {}
            if isinstance(p.get("gold"), (int, float)):
                r["price_estimate"] = -round(float(p["gold"]), 2)
                r["price_basis"] = CONSTRUCTION_PRICE_BASIS
            else:
                r["price_unread"] = f"construction_check gave no price ({chk.get('reason') or 'no reply'})"


#: What no read gives about a gold rise
GOLD_RISE_NOT_READ = ("gold inherited from a relative who died (the game passes a character's gold to "
                      "his heir)", "a ransom paid to you", "events and decisions that grant gold",
                      "reparations paid to you when a war ends in your favour")
#: The lever that stops the AI's hire or building is the veto, which drops the command before the game runs it
#: (ai_veto.h); a reserve does not (the AI hires under a reserve above the gold held), so it is not offered as one.
RESERVE_NOT_A_STOP = ("gold_reserve is not this lever: the AI can still hire a company while the reserve is set "
                      "above the gold held")


def ai_spend_veto(what: str) -> dict:
    return dict(ai_veto_call(what), what=f"drops the game's AI's {what} for your ruler before the game runs it",
                not_gold_reserve=RESERVE_NOT_A_STOP)


MERC_AI_NOTE = ("companies under contract to you that this server did not hire: the game's AI for your "
                "ruler hired them; expected is their price estimate (do action_class mercenaries)")
#: actor id -> company ids already named as hired for him
_MERC_SEEN: dict = {}


async def mercs_hired_for(gw, actor_id) -> Optional[list]:
    """The mercenary companies under contract to `actor_id` ({company_id, label, troops,
    price_estimate}), or None when the read did not answer."""
    if not actor_id:
        return None
    import redesign as R
    res = await gw.read("query_hireable_mercenaries", {"actor_id": actor_id, "include_hired": True,
                                                       "max_results": 0})
    rows = res.get("companies") if isinstance(res, dict) else None
    if not isinstance(rows, list):
        return None
    mine = [m for m in rows if isinstance(m, dict) and _i(m.get("hired_by")) == actor_id]
    if not mine:
        return []
    rl = await gw.read("query_realm", {"character_id": actor_id})
    mt = await R.seat_read(gw, "query_maa_types", {}, actor_id)
    cost = {t.get("maa_type"): t.get("recruit_cost") for t in ((mt or {}).get("types") or [])
            if isinstance(t, dict)} if isinstance(mt, dict) else {}
    n = rl.get("realm_total_counties") if isinstance(rl, dict) else None
    return [{"company_id": _i(m.get("company_id")), "label": m.get("label"), "troops": m.get("troops"),
             "price_estimate": R.merc_price(m.get("regiments"), n, cost)} for m in mine]


#: The least prestige
#: move that must carry a cause, or what was ruled out and what no read gives
PRESTIGE_CAUSE_MIN = 50
PRESTIGE_NOT_READ = (
    "the game's events and decisions, which charge or grant fixed amounts (35, 75, 150, 350, 750: "
    "00_basic_values.txt miniscule to massive prestige)",
    "a call to war your ruler's AI declined (call_ally_interaction on_decline: the decliner's "
    "prestige experience -350 offensive, -750 defensive)",
    "the monthly prestige income, men-at-arms paid in prestige, battles")
#: With the monthly change read, the monthly income is in `income` (expected), not unread
PRESTIGE_NOT_READ_MONTHLY = PRESTIGE_NOT_READ[:2] + ("men-at-arms paid in prestige, battles",)


def _prestige_ruled_out(prev: dict, snap: dict, ev: dict, acts: list) -> list:
    out = []
    if not any("declared war" in str(x.get("what")) for x in acts) and not [
            w for w in ev.get("wars_started") or [] if w.get("my_side") == "attacker"]:
        out.append("no war declared by you")
    letters_read = bool(snap.get("event_watched")) and isinstance(snap.get("event_picks"), list)
    unseen = any(r.get("interaction") == CALL_ALLY and r.get("answer") == "not_read"
                 for r in ev.get("asks_answered") or []) or (
        not letters_read and any(w.get("my_side") == "attacker" for w in ev.get("wars_ended") or []))
    acc = ASK_ANSWERS[CALL_ALLY]["accepted_event"]
    letter = any(isinstance(e, dict) and e.get("event") == acc for e in ev.get("events_answered") or [])
    if not ev.get("allies_joined") and not unseen and not letter:
        # an ally who joins is charged with the game's acceptance letter to your ruler; without the letters, a war
        # that ended between two reads hides a join; an acceptance letter among the step's events is a join (its
        # row names it), so it is never ruled out beside one
        out.append("no ally joined a war you attack in")
    if not ev.get("wars_ended"):
        out.append("no war of yours ended")
    a, b = prev.get("laws"), snap.get("laws")
    if isinstance(a, list) and isinstance(b, list) and not set(b) - set(a):
        out.append("no realm law passed")
    if not ev.get("titles_gained") and not ev.get("titles_lost") and prev.get("titles") is not None:
        out.append("no title gained or lost")
    return out


#: (actor id, war id) -> {gold, date}: the loss price first read for a war you attack in
_LOSS_FIRST: dict = {}
LOSS_MOVE = 0.10


def _loss_price_moved(snap: dict) -> list:
    """Each war you lead as its attacker whose price of losing, read from your income now, is
    10% or more from the one first read for it (-119.9 at the declare, -202.32 charged)."""
    inc, me, out = snap.get("monthly_income"), snap.get("actor_id"), []
    if not isinstance(inc, (int, float)):
        return out
    for x, w in sorted((snap.get("wars") or {}).items()):
        yrs = _gold_years(w.get("cb_type"))
        if not (w.get("is_attacker") and _i(w.get("attacker_id")) == me and yrs):
            continue
        now = -round(yrs * 12 * inc, 1)
        first = _LOSS_FIRST.setdefault((me, x), {"gold": now, "date": snap.get("date")})
        if first["gold"] and abs(now - first["gold"]) >= LOSS_MOVE * abs(first["gold"]):
            out.append({"war_id": x, "name": w.get("name"), "if_you_lose_now": now,
                        "first_read": first, "basis": REPARATIONS_BASIS.format(y=yrs)
                        + f"; the game charges it from your income on the day you lose ({round(inc, 2)} a month read now)"})
    return out


#: (1068.03.18: "war ended defender_victory: Munsterian-Ossoryan Vassalization War",
#: unexplained -351.07; 15 such steps in 10 runs, -3,692 prestige): the attacker's prestige on a defeat or a white
#: peace from the casus belli's file (declare_gates.OUTCOME_PRICE) where it is a fixed number; a claim or de jure war
#: pays a multiple of the war's prestige factor, which no read gives
WAR_END_FACTOR_CBS = ("claim", "de_jure")


def _war_end_acts(prev: dict, snap: dict) -> list:
    """The war ends this server sent for the ruler between the two reads (note_war_end_act's rows)."""
    return [x for x in acts_in_step(prev, snap) if str(x.get("what", "")).startswith("ended the war")]


def _act_that_ended(w: dict, acts: list, n_ended: int) -> Optional[dict]:
    """The act of yours that ended war `w`: the one carrying its war id; without ids, the only act beside the only
    war that ended (one war, one end: the same event); else None (counted as the game's)."""
    wid = w.get("war_id")
    for x in acts:
        if x.get("war_id") is not None and x.get("war_id") == wid:
            return x
    bare = [x for x in acts if x.get("war_id") is None]
    if n_ended == 1 and len(acts) == 1 and bare:
        return bare[0]
    return None


def war_end_prestige(w: dict) -> dict:
    """{expected, basis} for the prestige a war's end moved for the seat's ruler on his side of it, read from the
    casus belli's block for that ending (event_prices.cb_end_prestige: an attacker's win is prestige experience,
    a defender's loss the block's own charge); {basis} alone when the price is a multiple of the engine's factor for
    the war, which no read gives; {} when the row carries no casus belli or no side. Where the files are not readable,
    the attacker's table (declare_gates.outcome_price)."""
    import declare_gates as _dg  # noqa: PLC0415
    import event_prices as _ep  # noqa: PLC0415
    cb, side, out = str(w.get("_cb") or ""), w.get("my_side"), w.get("outcome")
    term = {"defender_victory": "defeat", "white_peace": "white_peace", "attacker_victory": "win"}.get(str(out))
    if not cb or side not in ("attacker", "defender") or term is None:
        return {}
    r = _ep.cb_end_prestige(cb, out, side)
    if r is not None:
        words = "; ".join(f"{x['via']}: {x['amount']}" + (" if its condition held" if x.get("conditional") else "")
                          for x in r["parts"]) or "no prestige moves for your side"
        if isinstance(r.get("prestige"), (int, float)) and not r.get("rule"):
            return {"expected": r["prestige"], "basis": f"{r['file']} for the {side}: {words}"}
        return {"basis": f"{r['file']} for the {side}: {r.get('rule') or words}; the factor is unread "
                         "(the engine sets it for each war)"}
    if side != "attacker":
        return {}
    if any(f in cb for f in WAR_END_FACTOR_CBS):
        n = {"defeat": 10, "white_peace": 5, "win": 10}[term]
        return {"basis": (f"{cb}: {'+' if term == 'win' else '-'}{n} x the war's prestige factor (its file); the factor "
                          "is unread")}
    op = _dg.outcome_price(cb)
    if op is None or not isinstance(op.get(term), (int, float)):
        return {"basis": f"{cb}: no price table for its ending in this code"}
    return {"expected": op[term], "basis": f"{cb} {term}: {op['basis']}"}


def _gold_years(cb) -> Optional[int]:
    import redesign as R
    row = R.PEACE_PRICE.get(str(cb or ""))
    return row.get("gold_years") if row else None


#: The game's own title-history type (CTitleHolderHistoryEntry +0x08), by the causes read beside each in
#: a seated game's 1096 save (query_title_history, read live): 0 the three ducal conquests won (Ulster 1075, Ailech
#: 1083, Ossory 1084), 2 the claim war won (Oriel 1073), 4 every death in the line (inheritance; also the history
#: files' holders), 6 a title left with no holder, 7 a duchy created, 10 Ormond taken after a refused liege demand
#: (refused_liege_demand_war 1072, the bridge's war history) and Dublin and Leinster on 1091.08.14. MEDIUM: one save.
HISTORY_TYPES = {0: "conquest", 2: "conquest by claim", 4: "inheritance", 6: "destroyed", 7: "created",
                 10: "revoked"}


#: The most title-history reads one news diff makes for gained titles with no other cause
GAINED_HISTORY_READS = 6


async def gained_from_history(gw, actor_id, title_id) -> Optional[dict]:
    """{how, history_type, from?} for a title the ruler gained, from the game's own title history (its newest
    entry names him; the one before names whom it came from); None when the history does not name him."""
    if not actor_id or not _i(title_id):
        return None
    hist = _d(await gw.read("query_title_history", {"title_id": title_id, "limit": 2}))
    rows = [x for x in hist.get("entries") or [] if isinstance(x, dict)]
    if not rows or _i(rows[0].get("char_id")) != actor_id:
        return None
    raw = _i(rows[0].get("event_type_raw"))
    kind = HISTORY_TYPES.get(raw, f"history type {raw} (not named)")
    prev = _i(rows[1].get("char_id")) if len(rows) > 1 else None
    return {"how": f"{kind}: to you on {rows[0].get('date')}" + (f", from {prev}" if prev else "")
                   + " (the game's title history)", "history_type": raw, **({"from": prev} if prev else {})}


PARTITION_SHARE = ("partition: at {dead}'s death his younger child {holder} took it as his share (the succession law "
                   "divides the late ruler's land among his eligible children); not a grant")


async def _lost_to(gw, actor_id, title_id, liege_was=None, ruler_was=None) -> Optional[dict]:
    """{how, holder_id} for a title that left the ruler's hands: granted_to_vassal when the holder is in his realm;
    else the game's own title history: who took it, when, and how (HISTORY_TYPES), and when the ruler's
    liege died in the step and the taker is the dead liege's heir, `cause` liege_died. None when unread."""
    if not actor_id or not _i(title_id):
        return None
    t = _d(_d(await gw.read("query_title_hierarchy", {"title_id": title_id})).get("title"))
    holder = _i(t.get("holder_id"))
    if not holder or holder == actor_id:
        return None
    h = _d(await gw.read("query_character", {"character_id": holder}))
    dead = _i(ruler_was)
    if dead and dead != actor_id and dead in (_i(h.get("father_id")), _i(h.get("mother_id"))):
        # (1069.09.27: c_ennis to the infant Cronan 66896 at Murchad's death read
        # "granted_to_vassal"): the seat's ruler changed in this diff and the taker is the dead ruler's child
        inrealm = _i(h.get("top_liege_id")) == actor_id or _i(h.get("liege_id")) == actor_id
        return {"how": PARTITION_SHARE.format(dead=dead, holder=holder), "cause": "partition", "holder_id": holder,
                **({"still_yours": "in your realm: the de-jure count is the realm's, so it still counts"} if inrealm
                   else {"left_realm": "his share is outside your realm"})}
    if _i(h.get("top_liege_id")) == actor_id or _i(h.get("liege_id")) == actor_id:
        return {"how": "granted_to_vassal", "holder_id": holder,
                "still_yours": "in your realm: the de-jure count is the realm's, so it still counts"}
    hist = _d(await gw.read("query_title_history", {"title_id": title_id, "limit": 1}))
    e = next((x for x in hist.get("entries") or [] if isinstance(x, dict)), None)
    if e is None or _i(e.get("char_id")) != holder:
        return None
    raw = _i(e.get("event_type_raw"))
    kind = HISTORY_TYPES.get(raw, f"history type {raw} (not named)")
    out = {"how": f"{kind}: to {holder} on {e.get('date')} (the game's title history)", "holder_id": holder,
           "history_type": raw}
    lw = _i(liege_was)
    if lw and lw != actor_id:
        old = _d(await gw.read("query_character", {"character_id": lw}))
        if old.get("alive") is False and _i(h.get("father_id")) == lw:
            out["cause"] = "liege_died"
            out["how"] = (f"your liege {lw} died; his son {holder} took it from you ({kind} in the game's title "
                          f"history, {e.get('date')})")
    return out


def _isnum(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _chaplain_row(council_rows) -> Optional[dict]:
    return next((c for c in council_rows or [] if isinstance(c, dict)
                 and c.get("position_key") == "councillor_court_chaplain" and not c.get("vacant")), None) \
        if isinstance(council_rows, list) else None


def _learning_of(row) -> Optional[int]:
    """A council row's learning when it carries one (`skills.learning`, verbose; or `learning`). Never the
    lean row's `skill`: the bridge fills it with the councillor's MARTIAL (query_handlers.cpp)."""
    row = _d(row)
    v = _d(row.get("skills")).get("learning", row.get("learning"))
    return v if _isnum(v) else None


async def chaplain_learning(gw, council) -> Optional[int]:
    """The court chaplain's learning: his council row's when it carries it, else his character read
    (query_character skills.learning). None when the seat is vacant or nothing answered."""
    rows = council.get("council") if isinstance(council, dict) else council
    row = _chaplain_row(rows)
    if row is None:
        return None
    v = _learning_of(row)
    if v is not None:
        return v
    cid = _i(row.get("holder_id"))
    if not cid:
        return None
    c = _d(await gw.read("query_character", {"character_id": cid}))
    v = _d(c.get("skills")).get("learning")
    return v if _isnum(v) and _i(c.get("id")) == cid else None


def _ordinal_of(d) -> Optional[int]:
    import cycle_cadence as _cc
    return _cc.ordinal(d)


def snapshot(*, actor_id, date, chr_sect: dict, wars: dict, claims: dict, council: Any,
             heir: Optional[dict], schemes: Any = None, held: Any = None,
             council_tasks: Any = None, laws: Any = None, alliances: Any = None,
             event_rules: Any = None, ai_budget: Any = None, legitimacy: Any = None,
             ai_veto: Any = None, chaplain_learning: Any = None, faction_threat: Any = None,
             realm: Any = None, split: Any = None, truces: Any = None, armies: Any = None,
             liege_factions: Any = None, line_risk: Any = None, dynasty_head: Any = None,
             liege_of_liege: Any = None) -> dict:
    claim_rows = claims.get("claims") if isinstance(claims, dict) else None
    war_rows = wars.get("wars") if isinstance(wars, dict) else None
    council_rows = council.get("council") if isinstance(council, dict) else council
    scheme_rows = schemes.get("schemes") if isinstance(schemes, dict) else None
    held_rows = held.get("held_titles") if isinstance(held, dict) else None
    return {
        "actor_id": actor_id, "date": date,
        "schemes": ({x["scheme_id"]: {k: x.get(k) for k in ("scheme_id", "type", "target_id")}
                     for x in scheme_rows if isinstance(x, dict) and _i(x.get("scheme_id")) is not None}
                    if isinstance(scheme_rows, list) else None),
        "titles": ({t["title_id"]: t.get("key") for t in held_rows
                    if isinstance(t, dict) and _i(t.get("title_id")) is not None}
                   if isinstance(held_rows, list) else None),
        "title_parents": ({t["title_id"]: t.get("de_jure_parent_key") for t in held_rows
                           if isinstance(t, dict) and _i(t.get("title_id")) is not None}
                          if isinstance(held_rows, list) else None),
        "claims": ({c["title_id"]: {"title_id": c["title_id"], "title_name": c.get("title_name"),
                                    "type": c.get("type"), "holder_id": c.get("holder_id"),
                                    **({"war_target_id": c["war_target_id"]} if c.get("war_target_id") else {})}
                    for c in claim_rows if isinstance(c, dict) and _i(c.get("title_id")) is not None}
                   if isinstance(claim_rows, list) else None),
        "wars": ({w["war_id"]: {k: w.get(k) for k in ("war_id", "name", "attacker_id", "defender_id",
                                                      "is_attacker", "cb_type", "attackers")
                                if k != "attackers" or isinstance(w.get(k), list)}
                  for w in war_rows if isinstance(w, dict) and _i(w.get("war_id")) is not None}
                 if isinstance(war_rows, list) else None),
        "liege_id": _i(chr_sect.get("liege_id")),
        # The liege's own liege (a liege change to his vassal) and the dynasty's head (who may disinherit)
        "liege_of_liege": _i(liege_of_liege),
        "dynasty_head": _i(dynasty_head),
        # The line's blood, for a seat moved to the holder of its late ruler's title
        "dynasty_id": _i(chr_sect.get("dynasty_id")),
        # The ruler's age and health beside the claims that die with him
        "age": chr_sect.get("age") if isinstance(chr_sect.get("age"), (int, float)) else None,
        "health": (round(chr_sect["health"], 2) if isinstance(chr_sect.get("health"), (int, float)) else None),
        "heir": ({"id": heir.get("id"), "name": heir.get("name")} if isinstance(heir, dict) else None),
        # Who may be the ruler's diarch (his children and spouses), for the diarch read
        "family": _diarchy.candidates(chr_sect, None, _i(actor_id)),
        # The primary heir of another dynasty (redesign.line_risk_for), news at every read it holds
        "line_risk": line_risk if isinstance(line_risk, dict) else None,
        "council": ({str(c.get("position_key") or c.get("position_id")): {
                        "holder_id": c.get("holder_id"), "task": c.get("current_task"),
                        "vacant": c.get("vacant")}
                     for c in council_rows if isinstance(c, dict)}
                    if isinstance(council_rows, list) else None),
        # The purse, each war's total for my side, the
        # council_tasks rows with their target provinces
        **{k: (float(chr_sect[k]) if isinstance(chr_sect.get(k), (int, float))
               and not isinstance(chr_sect.get(k), bool) else None) for k in ("prestige", "piety", "gold")},
        # This read's place among the server's acts (act_in_step: an act on the read's own day is in one step)
        "read_order": _next_order(),
        "war_scores": ({w["war_id"]: _my_score(w) for w in war_rows
                        if isinstance(w, dict) and _i(w.get("war_id")) is not None}
                       if isinstance(war_rows, list) else None),
        "council_tasks": council_tasks if isinstance(council_tasks, list) else None,
        # What prices the purse's changes, and what the idle block reads
        "monthly_income": _income(chr_sect),
        # The game's own monthly prestige / piety change (query_character monthly_change); None
        # from a bridge that does not send it (not 0)
        **{f"monthly_{k}": v for k, v in (_monthly(chr_sect) or {"prestige": None, "piety": None}).items()},
        # Live: the chaplain's LEARNING (query_council's lean `skill` is his MARTIAL: live 8 against
        # learning 12, and the letter charged (40 - 12) x 3 = 84 where 96 was predicted)
        "chaplain_learning": (chaplain_learning if _isnum(chaplain_learning)
                              else _learning_of(_chaplain_row(council_rows))),
        "laws": ([str(x.get("name")) for x in laws["active_laws"] if isinstance(x, dict) and x.get("name")]
                 if isinstance(laws, dict) and isinstance(laws.get("active_laws"), list) else None),
        "alliances": (len([x for x in alliances["alliances"] if isinstance(x, dict)])
                      if isinstance(alliances, dict) and isinstance(alliances.get("alliances"), list) else None),
        # The event picks recorded for a watched ruler, the AI budget buckets
        **_seat_events.snapshot_part(actor_id, event_rules, ai_budget, ai_veto),
        # The factions aimed at the ruler and the demand letters answered for him
        **_faction_letters.snapshot_part(faction_threat),
        # The factions aimed at his liege that the ruler is in (query_factions on the liege)
        **_faction_letters.member_part(liege_factions, actor_id),
        # Legitimacy and the primary tier (the claim-war price's legitimacy multiplier)
        "legitimacy": (float(legitimacy) if isinstance(legitimacy, (int, float))
                       and not isinstance(legitimacy, bool) else None),
        "ruler_tier": (chr_sect.get("ruler_tier") if isinstance(chr_sect.get("ruler_tier"), int)
                       and not isinstance(chr_sect.get("ruler_tier"), bool) else None),
        # The counties he holds himself against his domain limit (query_realm), and the
        # level of each AI-passed law group (query_realm_laws), for the unset-veto row
        "domain": _domain_of(realm),
        # The realm's counties (vassals' too), for a vassal who joined with no war
        "realm_counties": (_i(realm.get("realm_total_counties")) if isinstance(realm, dict) else None),
        # The ruler's learning (the crown-authority price's multiplier)
        "learning": (lambda v: v if _isnum(v) else None)(_d(chr_sect.get("skills")).get("learning")),
        "law_levels": _law_levels(laws),
        # The partition forecast (redesign._forecast_brief) for the split rows
        "split": split if isinstance(split, dict) else None,
        # The ruler's culture (a change is the AI's convert_to_local_culture_decision)
        # and his truces (a war declared inside one breaks it: 250 prestige and a level)
        "culture": chr_sect.get("culture_key") if isinstance(chr_sect.get("culture_key"), str) else None,
        "primary_title_key": chr_sect.get("primary_title_key") if isinstance(chr_sect.get("primary_title_key"), str) else None,
        "truces": ({int(k): (v or {}).get("until") for k, v in truces.items()}
                   if isinstance(truces, dict) else None),
        # The men in the ruler's raised armies (query_armies), for their upkeep in the purse
        "armies_men": _armies_men(armies),
    }


def _armies_men(armies: Any) -> Optional[int]:
    import declare_gates as _dg  # noqa: PLC0415
    ar = _dg.armies_raised(armies)
    return ar["men"] if ar is not None else None


def _domain_of(realm: Any) -> Optional[dict]:
    if not isinstance(realm, dict):
        return None
    held, lim = _i(realm.get("domain_size")), _i(realm.get("domain_limit"))
    return {"held": held, "limit": lim} if held is not None and lim else None


#: The law groups the game's AI changed for the seat's ruler in the runs (three
#: crown-authority changes, about 502 prestige; crown_authority_1 sent every year for a count,
#: 101-129 prestige each, and passed for the next ruler, -159), and their top level
#: (common/laws/00_realm_laws.txt crown_authority_0..3)
AI_LAW_GROUPS = {"crown_authority": 3}


def _law_levels(laws: Any) -> Optional[dict]:
    rows = laws.get("active_laws") if isinstance(laws, dict) else None
    if not isinstance(rows, list):
        return None
    out = {}
    for x in rows:
        name = str((x or {}).get("name") or "") if isinstance(x, dict) else ""
        for g, top in AI_LAW_GROUPS.items():
            if name.startswith(g + "_") and name[len(g) + 1:].isdigit():
                lvl = _i(x.get("level")) if _i(x.get("level")) is not None else int(name[len(g) + 1:])
                mx = _i(x.get("max_level")) if _i(x.get("max_level")) is not None else top
                out[g] = {"law": name, "level": lvl, "max_level": mx}
    return out


VETO_UNSET_TITLE = ("no ai_veto for create_title: your ruler is a count holding at least as many counties as his "
                    "domain limit, so the game's check skips the gold test and its AI may create a duchy for him "
                    "at its price, even into debt")
VETO_UNSET_LAW = ("no ai_veto for law: the game's AI raises this law for your ruler when it can, at a prestige "
                  "price that grows with his realm (crown authority: (100 + 10 a realm county, the counties' part at "
                  "most 900) x (1 + (8 - his learning) / 50, at least 0.5), before faith and culture modifiers; "
                  "price_estimate has it when his learning is read); "
                  "below 0 prestige no war can be declared; do change_law still "
                  "changes it when you choose")
#: (a test run, 0867.03.23: prestige 986.67 -> -305.33, 792 for crown_authority_1 and 500
#: for the court type, the same day, in three games): a ruler with a royal court (a king or above)
VETO_UNSET_COURT = ("no ai_veto for court_type: the game's AI changes your court type for your ruler, at the price "
                    "in the game's files (500 prestige for every type), on its own day, which can be the day it "
                    "raises crown authority (the two prices then fall together); below 0 prestige no war can "
                    "be declared")
COURT_MIN_TIER = 4


def veto_unset(snap: dict) -> Optional[dict]:
    """The unset vetoes whose spend the reads say can come now, each with the price, his gold
    and prestige, and the ready ai_veto call. None when the vetoes were not read or nothing applies."""
    vet = snap.get("ai_vetoes")
    if not isinstance(vet, list):
        return None
    me = snap.get("actor_id")
    purse = {k: round(snap[k], 2) for k in ("gold", "prestige") if _isnum(snap.get(k))}
    rows: dict = {}
    dom = snap.get("domain") or {}
    if "create_title" not in vet and snap.get("ruler_tier") == 2 and isinstance(dom.get("held"), int) \
            and isinstance(dom.get("limit"), int) and dom["held"] >= dom["limit"]:
        row = {"why": VETO_UNSET_TITLE, "domain": dom, **purse}
        prices = [(k, v) for k, v in _CREATION_PRICE.items() if v.get("actor_id") == me and k.startswith("d_")]
        if prices:
            k, v = prices[-1]
            row["price"] = {"title": k, v.get("currency") or "gold": v.get("cost"), "read": v.get("date")}
        else:
            row["price_unread"] = {"tool": "inspect", "args": {"subject": "economy"}}
        row["next"] = ai_veto_call("create_title")
        rows["create_title"] = row
    lv = snap.get("law_levels") or {}
    up = [(g, v) for g, v in lv.items() if isinstance(v, dict) and isinstance(v.get("level"), int)
          and isinstance(v.get("max_level"), int) and v["level"] < v["max_level"]]
    if "law" not in vet and up:
        g, v = up[0]
        row = {"why": VETO_UNSET_LAW, "law": v["law"], "level": v["level"], "max_level": v["max_level"], **purse}
        recs = [r for r in snap.get("ai_veto_records") or [] if isinstance(r, dict) and r.get("what") == "law"
                and str(r.get("key") or "").startswith(g + "_")]
        pr = _seat_events.price_of(recs[-1]) if recs else None
        import event_prices as _ep  # noqa: PLC0415
        est = _ep.crown_authority_price(snap.get("realm_counties"), snap.get("learning")) \
            if g == "crown_authority" else None
        if pr:
            row["price"] = dict(pr, law=recs[-1].get("key"), read=recs[-1].get("date"))
        elif est:
            # The game's rule on his reads
            row["price_estimate"] = est
        else:
            row["price_unread"] = "the game prices it when its AI sends the change"
        row["next"] = ai_veto_call("law")
        rows["law"] = row
    tier = snap.get("ruler_tier")
    if "court_type" not in vet and isinstance(tier, int) and not isinstance(tier, bool) and tier >= COURT_MIN_TIER:
        row = {"why": VETO_UNSET_COURT, **purse}
        price = _seat_events.court_type_price("court_diplomatic")
        if price:
            row["price"] = dict(price, source="/".join(_seat_events.COURT_TYPES_FILE) + " cost")
        else:
            row["price_unread"] = "the court types' cost block in the game's files did not read"
        row["next"] = ai_veto_call("court_type")
        rows["court_type"] = row
    return rows or None


#: (the claim-war price read 125 in 1067-1075 and 200-312.5 after; the cbs rows
#: named "x2 your legitimacy 0.0 at your tier" from 1082 with no read of when or why it moved)
LEGITIMACY_NOTE = ("claim_cb's price is multiplied by your legitimacy level's flag for your tier "
                   "(common/legitimacy/00_legitimacy.txt; x0.15 to x2.5): a new tier (a duchy created or "
                   "inherited) or a fall in legitimacy moves it")
#: the least legitimacy move named when neither the tier nor the multiplier moved
LEGITIMACY_MOVE = 25
_TIER_WORD = {2: "county", 3: "duchy", 4: "kingdom", 5: "empire"}


def legitimacy_change(prev: dict, snap: dict) -> Optional[dict]:
    """The legitimacy row: was, now, the primary tier before and now, and claim_cb's legitimacy
    multiplier (low-high across the culture eras, declare_gates.legitimacy_factor) before and now.
    None when the tier and the multiplier stayed and legitimacy moved less than LEGITIMACY_MOVE,
    or a read is missing."""
    import declare_gates as _dg
    a, b = prev.get("legitimacy"), snap.get("legitimacy")
    ta, tb = prev.get("ruler_tier"), snap.get("ruler_tier")
    if not isinstance(a, (int, float)) or not isinstance(b, (int, float)):
        return None
    fa, fb = _dg.legitimacy_factor(a, ta), _dg.legitimacy_factor(b, tb)
    if abs(b - a) < LEGITIMACY_MOVE and ta == tb and fa == fb:
        return None
    row = {"was": round(a, 2), "now": round(b, 2), "change": round(b - a, 2)}
    if ta != tb:
        row["tier"] = {"was": _TIER_WORD.get(ta, ta), "now": _TIER_WORD.get(tb, tb)}
    if fa != fb:
        row["claim_war_price_factor"] = {"was": list(fa) if fa else None, "now": list(fb) if fb else None,
                                         "basis": LEGITIMACY_NOTE}
    return row


def _monthly(chr_sect: dict) -> Optional[dict]:
    import declare_gates as _dg  # noqa: PLC0415
    return _dg.monthly_change_of(chr_sect)


def _income(chr_sect: dict) -> Optional[float]:
    bd = chr_sect.get("income_breakdown") if isinstance(chr_sect, dict) else None
    v = bd.get("total_monthly") if isinstance(bd, dict) else None
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def _my_score(w: dict) -> Optional[int]:
    """My side's share of the war's total (get_active_wars sends the total only when the
    game's cache is fresh; the attacker's view, so a defender's is negated)."""
    import redesign as R
    t = R.war_score_view(w)["war_score"]
    if t is None or not isinstance(w.get("is_attacker"), bool):
        return None
    return t if w["is_attacker"] else -t


#: Actor id -> [{ally_id, war_id, date}] the calls to war this server sent
_CALLS: dict = {}


def note_call(actor_id, ally_id, war_id, date) -> None:
    if actor_id and ally_id:
        _CALLS.setdefault(actor_id, []).append({"ally_id": _i(ally_id), "war_id": war_id, "date": date})
        del _CALLS[actor_id][:-16]
        note_ask(actor_id, CALL_ALLY, ally_id, date, war_id=war_id)     # its answer: news.asks_answered


async def _allies_joined(gw, actor_id, old: dict, new: dict, since, date) -> list:
    """Each ally who joined, in this diff, a war you lead as its attacker, with
    the prestige the game charges you for him (call_ally_interaction_event_effect) and who made
    the call: you (a call this server sent) or the game's AI for your ruler."""
    import redesign as R
    lo, hi = _ordinal_of(since), _ordinal_of(date)
    out = []
    for x, w in sorted(new.items()):
        if not (w.get("is_attacker") and _i(w.get("attacker_id")) == actor_id and isinstance(w.get("attackers"), list)):
            continue
        before = old.get(x, {}).get("attackers") if x in old else [actor_id]
        if not isinstance(before, list):
            continue
        for ally in sorted({_i(a) for a in w["attackers"]} - {_i(a) for a in before} - {actor_id, None}):
            tier = R._tier_of(_d(await gw.read("query_character", {"character_id": ally})))
            cost = R.call_ally_cost(tier or None, "attacker")
            mine = [c for c in _CALLS.get(actor_id) or [] if c.get("ally_id") == ally and
                    (lo is None or hi is None or lo <= (_ordinal_of(c.get("date")) or -1) <= hi)]
            out.append({"war_id": x, "war": w.get("name"), "ally_id": ally,
                        "prestige": cost if cost is not None else "unknown",
                        "called_by": (f"you (do call_ally sent {mine[-1]['date']})" if mine else
                                      "the game's AI for your ruler: this server sent no call to him"),
                        "basis": R.CALL_ALLY_SOURCE})
    return out


async def _joined_by_letter(gw, prev: dict, snap: dict, answers: list, news: dict) -> list:
    """An allies_joined row for each call of yours his acceptance letter answered when no war list showed him join
    (the war began or ended between two reads), with the prestige the game charges you for him."""
    import redesign as R
    out = []
    for r in answers:
        if r.get("interaction") != CALL_ALLY or r.get("answer") != "joined" or "letter" not in str(r.get("how")):
            continue
        wid, ally = _i(r.get("war_id")), _i(r.get("target_id"))
        if any(_i(j.get("ally_id")) == ally and _i(j.get("war_id")) == wid for j in news.get("allies_joined") or []):
            continue
        ended = next((w for w in news.get("wars_ended") or [] if wid is not None and _i(w.get("war_id")) == wid), {})
        live = (snap.get("wars") or {}).get(wid) or (prev.get("wars") or {}).get(wid) or {}
        side = ended.get("my_side") or ({True: "attacker", False: "defender"}.get(live.get("is_attacker")))
        tier = R._tier_of(_d(await gw.read("query_character", {"character_id": ally}))) if side == "attacker" else None
        cost = R.call_ally_cost(tier or None, side)
        out.append({"war_id": wid, "war": ended.get("name") or live.get("name"), "ally_id": ally,
                    "prestige": cost if cost is not None else "unknown",
                    "called_by": f"you (do call_ally sent {r.get('sent')})", "seen": r.get("how"),
                    "basis": R.CALL_ALLY_SOURCE})
    return out


async def _joined_by_ai_call(gw, prev: dict, snap: dict, se: dict, news: dict) -> list:
    """An allies_joined row for each call your ruler's AI sent in this step (ai_acted ally_call) that an acceptance
    letter (call_ally.0100) after it answered, no ask of yours took that letter and no row of this news already
    shows him join: the ally joined a war you attack in, and the game charged you by his title's tier (the same
    effect as for your own call). Letters name no ally: a letter goes to the first such call before it."""
    import redesign as R
    acc = ASK_ANSWERS[CALL_ALLY]["accepted_event"]
    calls = sorted((a for a in se.get("ai_acted") or [] if isinstance(a, dict) and a.get("what") == "ally_call"
                    and _i(a.get("ally_id")) is not None), key=lambda a: _ordinal_of(a.get("date")) or 0)
    if not calls:
        return []
    taken = {str(r.get("how") or "") for r in news.get("asks_answered") or [] if isinstance(r, dict)}
    letters = sorted((e for e in se.get("events_answered") or [] if isinstance(e, dict) and e.get("event") == acc
                      and not any(str(e.get("date")) in t and acc in t for t in taken)),
                     key=lambda e: _ordinal_of(e.get("date")) or 0)
    out: list = []
    used: set = set()
    for c in calls:
        ally, wid = _i(c.get("ally_id")), _i(c.get("war_id"))
        if any(_i(j.get("ally_id")) == ally and (wid is None or _i(j.get("war_id")) == wid)
               for j in (news.get("allies_joined") or []) + out):
            continue
        cd = _ordinal_of(c.get("date")) or 0
        hit = next((i for i, e in enumerate(letters) if i not in used and (_ordinal_of(e.get("date")) or 0) >= cd),
                   None)
        if hit is None:
            continue
        used.add(hit)
        ended = next((w for w in news.get("wars_ended") or [] if wid is not None and _i(w.get("war_id")) == wid), {})
        live = (snap.get("wars") or {}).get(wid) or (prev.get("wars") or {}).get(wid) or {}
        side = ended.get("my_side") or ({True: "attacker", False: "defender"}.get(live.get("is_attacker")))
        if side != "attacker":
            continue                             # the effect charges the war's attacker only
        tier = R._tier_of(_d(await gw.read("query_character", {"character_id": ally})))
        cost = R.call_ally_cost(tier or None, side)
        out.append({"war_id": wid, "war": ended.get("name") or live.get("name"), "ally_id": ally,
                    "prestige": cost if cost is not None else "unknown",
                    "called_by": (f"the game's AI for your ruler (its call on {c.get('date')}, ai_acted ally_call; "
                                  "do ai_veto what=ally_call stops such calls)"),
                    "seen": f"his acceptance letter {acc} to your ruler on {letters[hit].get('date')}",
                    "basis": R.CALL_ALLY_SOURCE})
    return out


async def _fabrication_timing(gw, snap: dict, prev: dict, fs: dict) -> None:
    """Months elapsed against the most the game's rate allows (fabrication_expected),
    `overdue` past it; a chaplain replaced since it started restarts the count from that date."""
    actor = snap.get("actor_id")
    chap = ((snap.get("council") or {}).get("councillor_court_chaplain") or {}).get("holder_id")
    seen = _FAB_CHAPLAIN.get(actor)
    if seen is None or seen.get("province_id") != fs.get("target_province_id"):
        seen = _FAB_CHAPLAIN[actor] = {"province_id": fs.get("target_province_id"), "holder_id": chap,
                                       "since": fs.get("since")}
    elif chap is not None and seen.get("holder_id") not in (None, chap):
        fs["chaplain_changed"] = {"from": seen["holder_id"], "to": chap, "on_or_before": snap.get("date")}
        seen.update(holder_id=chap, since=prev.get("date"))
    tgt = fs.get("target") if isinstance(fs.get("target"), dict) else {}
    borders = None
    if _i(tgt.get("title_id")):
        nb = _d(await gw.read("query_county_neighbors", {"title_id": _i(tgt.get("title_id"))}))
        rows = nb.get("neighbors")
        if isinstance(rows, list):
            borders = any(isinstance(n, dict) and _i(n.get("realm_ruler_id")) == actor for n in rows)
    other_faith, in_realm = await _holder_terms(gw, actor, _i(tgt.get("title_id")))
    exp = fabrication_expected(snap.get("chaplain_learning"), borders, other_faith, in_realm)
    start = seen.get("since") or fs.get("since")
    days = _days(start, snap.get("date"))
    if exp is None:
        fs["expected_unread"] = "the chaplain's learning was not read"
        return
    months = round(days / 30.44, 1) if isinstance(days, int) else None
    fs["expected"] = dict(exp, months_elapsed=months, counted_from=start, rule=FABRICATION_RULE)
    if in_realm is None:
        fs["expected"]["unread"] = FABRICATION_HOLDER_UNREAD
    elif in_realm is True:
        fs["expected"]["unread"] = FABRICATION_IN_REALM_UNREAD
    price = claim_letter_price(snap.get("chaplain_learning"))
    if price:
        fs["letter_price"] = dict(price, rule=CLAIM_PRICE_RULE)
    pct = fs.get("progress")
    if isinstance(pct, (int, float)) and not isinstance(pct, bool):
        # The progress is read; the game adds the rate daily as rate / 30
        import math
        # Live: whole 30-day steps of the monthly rate (the progress moves once a month)
        fs["expected"]["days_left_at_most"] = int(math.ceil(round((100.0 - pct) / exp["monthly_pct_at_least"], 6))) * 30
        rate = fs.get("progress_rate")
        if isinstance(rate, dict) and _isnum(rate.get("gained")):
            # Live: whole monthly steps between the reads, counted with the expected step
            steps = round(rate["gained"] / exp["monthly_pct_at_least"]) or 1
            step = rate["gained"] / steps
            rate.update(steps=steps, monthly_step=round(step, 2))
            fs["days_left_measured"] = int(math.ceil(round((100.0 - pct) / step, 6))) * 30
        fs.pop("overdue", None)
        return
    after = fabrication_overdue_after(exp)
    if in_realm is not False or after is None:
        fs.pop("overdue", None)
    else:
        fs["overdue"] = bool(months is not None and months > after)
    fs.pop("progress_unread", None)
    fs["progress_unread"] = "the task's own progress is not read (the bridge sent none); expected is the game's rate"


async def _holder_terms(gw, actor_id, title_id) -> tuple:
    """(other_faith, in_realm) for the county's holder against the ruler: whether his faith is
    another (the task's x1.3) and whether he has the ruler as liege or above (its x0.3, the game's
    target_is_liege_or_above); each None when a read did not answer or does not decide it."""
    if not _i(actor_id) or not _i(title_id):
        return None, None
    t = _d(_d(await gw.read("query_title_hierarchy", {"title_id": _i(title_id)})).get("title"))
    holder = _i(t.get("holder_id"))
    if not holder:
        return None, None
    a = _d(await gw.read("query_character", {"character_id": _i(actor_id)}))
    h = _d(await gw.read("query_character", {"character_id": holder}))
    if _i(a.get("id")) != _i(actor_id) or _i(h.get("id")) != holder:
        return None, None
    fa, fh = _i(a.get("faith_id")), _i(h.get("faith_id"))
    other = None if fa is None or fh is None else fa != fh
    me = _i(actor_id)
    # top_liege_id: the independent ruler at the head of the chain (his own id when independent, 0 unread)
    liege, top, my_top = _i(h.get("liege_id")), _i(h.get("top_liege_id")) or None, _i(a.get("top_liege_id")) or None
    if me in (liege, top):
        in_realm = True
    elif top is not None and my_top is not None and (top != my_top or my_top == me):
        # his realm's head is not yours, or you head yours and he is not under you
        in_realm = False
    else:
        in_realm = None
    return other, in_realm


#: Actor id -> {province_id, holder_id, since} the chaplain seen on the running task
_FAB_CHAPLAIN: dict = {}


def _side(w: dict) -> Optional[str]:
    return ("attacker" if w.get("is_attacker") else "defender") if isinstance(w.get("is_attacker"), bool) else None


async def _claim_next(gw, actor_id, row: dict, war_out: Optional[dict] = None) -> dict:
    """The call after a claim arrives: declare_war with declare_args when the casus-belli
    read against the holder says a CB on this title is declarable now, else cbs on him.
    `war_out` receives {declarable_now, gates, price}: whether a war for the claim can
    be declared now and, if not, each gate the reads name (gold below 0, prestige short by N,
    a truce, raised armies), or why it is not known."""
    import redesign as R
    war = war_out if war_out is not None else {}
    holder = _i(row.get("war_target_id")) or _i(row.get("holder_id"))
    if not holder or not actor_id:
        war.update(declarable_now=None, unread="the claim's holder was not read")
        return {"tool": "plan_war", "args": {}}
    if not _i(row.get("war_target_id")):
        # A claim on a vassal's title is fought against his realm's ruler
        holder, _note = await R.realm_war_target(gw, actor_id, holder)
    opts = (await R.declarable_now(gw, actor_id, [holder])).get(holder)
    for o in opts or []:
        if R.cb_option_declarable(o) and _i(o.get("cb_title_id")) == row.get("title_id"):
            war.update(declarable_now=True, against=holder, **_war_price(o))
            if o.get("may_refuse"):
                war["may_refuse"] = o["may_refuse"]["why"]
            return {"tool": "declare_war", "args": {"declare_args": R.declare_args_of(o, holder)},
                    **({"note": o["may_refuse"]["why"]} if o.get("may_refuse") else {})}
    gated = next((o for o in opts or [] if o.get("valid") and o.get("blocked_by")
                  and _i(o.get("cb_title_id")) == row.get("title_id")), None)
    if gated is not None:
        # The game's check accepts it; debt, its cost or a truce refuse it now
        war.update(declarable_now=False, against=holder, gates=[_gate_row(b) for b in gated["blocked_by"]],
                   **_war_price(gated))
        if gated.get("may_refuse"):
            war["may_refuse"] = gated["may_refuse"]["why"]
        return {"tool": "cbs", "args": {"target_id": holder},
                "why": f"{gated.get('cb')} is refused now: " + "; ".join(b["fix"] for b in gated["blocked_by"])}
    why = ("the casus-belli read did not answer" if opts is None else
           "no casus belli on this title is declarable against its holder now")
    war.update(declarable_now=None if opts is None else False, against=holder,
               **({"unread": why} if opts is None else {"gates": [{"condition": "game_check", "fix": why}]}))
    return {"tool": "cbs", "args": {"target_id": holder}, "why": why}


def _gate_row(b: dict) -> dict:
    """One refusing condition of a claim's war, as the claim row names it."""
    out = {"condition": b.get("condition")}
    for k in ("have", "cost", "need", "missing"):
        if b.get(k) is not None:
            out[k] = b[k]
    out["fix"] = b.get("fix")
    return out


def _war_price(o: dict) -> dict:
    """The war's price as the cbs row reads it (prestige and its multipliers)."""
    out = {}
    if o.get("cost_prestige") is not None:
        out["price_prestige"] = o["cost_prestige"]
    if o.get("cost_basis"):
        out["price_basis"] = o["cost_basis"]
    return out


#: What a claim's kind means at the holder's death (redesign.CLAIM_DIES_WITH_RULER)
CLAIM_KIND = {"unpressed": {"inheritable": False, "at_your_death": "lost (an unpressed claim does not pass "
                                                                   "to your heir)"},
              "pressed": {"inheritable": True, "at_your_death": "passes to your children (a pressed claim)"}}


#: The claim war's least price on a county before the defender is known
CLAIM_PRICE_FLOOR = ("claim_cb on one county: 100 prestige (unpressed) times your legitimacy factor; the "
                     "defender then multiplies it (another faith x1.25, a lower tier x1.25, a child x0.5, ...)")


def letter_facts(snap: dict) -> dict:
    """A recorded case (~500 gold of letters paid for claims that died unpressed): before the
    claim letter is paid, the facts that decide whether its war can be declared: the letter's
    price against your gold, and the gates that do not depend on the defender (gold below 0, the
    claim war's least prestige price against your prestige). Facts only."""
    import declare_gates as _dg
    out: dict = {}
    cost = claim_gold_cost(snap.get("chaplain_learning"))
    gold, pres = snap.get("gold"), snap.get("prestige")
    if cost is not None and isinstance(gold, (int, float)):
        out["letter_price"] = {"gold": cost, "your_gold": round(gold, 2), "gold_after": round(gold - cost, 2),
                               "basis": CLAIM_COST_BASIS, **_letter_decided(snap, cost)}
    gates = []
    if isinstance(gold, (int, float)) and gold - (cost or 0) < 0:
        gates.append({"condition": "in_debt", "gold_after_letter": round(gold - (cost or 0), 2),
                      "fix": "the game refuses a declaration while gold is below 0"})
    lf = _dg.legitimacy_factor(snap.get("legitimacy"), snap.get("ruler_tier"))
    if lf is not None and isinstance(pres, (int, float)):
        lo = round(100 * lf[0], 2)
        row = {"condition": "prestige", "have": round(pres, 2), "price_at_least": lo,
               **({"price_up_to_before_defender": round(100 * lf[1], 2)} if lf[1] != lf[0] else {}),
               "basis": CLAIM_PRICE_FLOOR}
        if pres < lo:
            row["missing"] = round(lo - pres, 2)
            gates.append(row)
        else:
            out["prestige_vs_price"] = row
    elif isinstance(pres, (int, float)):
        out["prestige_vs_price"] = {"have": round(pres, 2), "unread": "your legitimacy or tier was not read: "
                                                                     "the price's least is not known"}
    if out or gates:
        out["war_gates_now"] = gates
    return out


def claim_facts(c: dict, snap: dict, war: Optional[dict]) -> dict:
    """A claim row's facts: its kind, your ruler's age and health as the game exposes
    them, and whether a war for it is declarable now (war, from _claim_next)."""
    kind = CLAIM_KIND.get(str(c.get("type") or ""))
    out = {"kind": dict({"type": c.get("type")}, **(kind or {"inheritable": "unknown: the claim's type "
                                                                            "was not read"}))}
    out["holder_is_your_ruler"] = {k: snap.get(k) for k in ("age", "health") if snap.get(k) is not None} or \
        "unread: your ruler's age and health were not read"
    if war is not None:
        out["war"] = war
    return out


#: The least piety a head of faith's claim costs (common/script_values/
#: 02_religion_values.txt claim_piety_value: 2 x 50 on a county, plus 10 per county of your
#: realm), the floor the idle block's piety fact is stated from.
HOF_CLAIM_PIETY_FLOOR = 100


#: (actor, interaction) -> the date the game last refused that send
_REFUSED_SENDS: dict = {}
HOF_CLAIM_SEND = "hof_ask_for_claim_interaction"
REFUSED_SEND_QUIET_DAYS = 365


#: (idle.piety pointed at the head of faith's gold on every step after
#: he refused, a send refused target_would_reject, six acceptance reads accept_score -5/-9): (actor,
#: interaction) -> {date, accept_score} of the last read or send that said no; a yes clears it
_ACCEPT_NO: dict = {}
HOF_GOLD_SEND = "hof_ask_for_gold_interaction"


def note_acceptance(actor_id, interaction: str, yes: Any, score: Any, date) -> None:
    if not (actor_id and interaction):
        return
    if yes is True:
        _ACCEPT_NO.pop((actor_id, interaction), None)
    elif yes is False:
        _ACCEPT_NO[(actor_id, interaction)] = {"date": date, "accept_score": score}


#: (A seated game, turn 42: vows sent to Donnchad 65830, and no news row ever said whether he answered; another,
#: Turn 35: his refusal came back as an event the game answered, religious_interaction.1031, with no name on it): an ask
#: whose recipient decides, by what the game leaves when he answers. Accepted: take_vows' on_accept sends him to the
#: clergy (send_child_to_clergy_effect: the devoted trait, 00_religious_interactions.txt take_vows_interaction).
#: Refused: on_decline fires the decline letter to your ruler (religious_interaction.1031) and gives the piety back.
#: Both are existing reads: the recipient's character read (traits) and the bridge's record of the events the game
#: answered for your ruler (query_event_rules picks, news `events_answered`).
#: A call to war: call_ally_interaction's on_accept fires call_ally.0100 ("Call to War Response: Accepted") to the
#: caller and adds the ally to the war's attackers or defenders; its on_decline fires call_ally.0101 ("Refused")
#: (00_alliance.txt; events/interaction_events/alliance_interaction_events.txt). Neither letter names the ally.
CALL_ALLY = "call_ally_interaction"
ASK_ANSWERS = {"take_vows_interaction": {"accepted_trait": "devoted", "declined_event": "religious_interaction.1031"},
               CALL_ALLY: {"accepted_event": "call_ally.0100", "declined_event": "call_ally.0101"}}
#: past this many days with neither, the answer is said not read (his AI answers in 4-9 days: ai_maybe)
ASK_WAIT_DAYS = 30
#: {actor id: [{interaction, target_id, sent}]}, the asks this server sent whose answer was not read yet
_ASKS: dict = {}


def note_ask(actor_id, interaction: str, target_id, date, war_id=None) -> None:
    """An ask in ASK_ANSWERS that the game took (applied or pending): its answer is read in the next news."""
    if actor_id and interaction in ASK_ANSWERS and _i(target_id) and date:
        _ASKS.setdefault(actor_id, []).append({"interaction": interaction, "target_id": target_id, "sent": date,
                                               **({"war_id": war_id} if war_id is not None else {})})
        del _ASKS[actor_id][:-8]


def _call_answer(a: dict, rule: dict, letters: list, used: set, who, me, news: dict, snap: dict) -> dict:
    """The answer to one call to war this server sent: `joined` (the game's acceptance letter to the caller, or the
    ally among the war's attackers in this read), `declined` (the refusal letter), `war_ended` (the war is over and no
    letter came: he did not join it), `waiting`, or `not_read` (no letter read past ASK_WAIT_DAYS, or the letters are
    not read). Letters name no ally: when more than one call was waiting, the row says so."""
    row = {"interaction": CALL_ALLY, "target_id": a["target_id"], "war_id": a.get("war_id"), "sent": a["sent"]}
    wid, ally = _i(a.get("war_id")), _i(a["target_id"])
    seen = next((j for j in news.get("allies_joined") or [] if _i(j.get("ally_id")) == ally
                 and (wid is None or _i(j.get("war_id")) == wid)), None)
    whose = "your" if who == me else "the late"
    waiting_calls = sum(1 for x in _ASKS.get(who) or [] if x.get("interaction") == CALL_ALLY)
    shared = (f" (the letter names no one; {waiting_calls} calls of yours were waiting: it is the first after this "
              "one)") if waiting_calls > 1 else ""
    hit = next((i for i, e in enumerate(letters) if (who, i) not in used
                and e.get("event") in (rule["accepted_event"], rule["declined_event"])
                and str(e.get("date") or "") >= a["sent"]), None)
    if hit is not None:
        used.add((who, hit))
        e = letters[hit]
        ok = e.get("event") == rule["accepted_event"]
        return dict(row, answer="joined" if ok else "declined",
                    how=f"the game's {'acceptance' if ok else 'refusal'} letter {e.get('event')} to {whose} ruler on "
                        f"{e.get('date')}{shared}")
    if seen is not None:
        return dict(row, answer="joined", how="he is among the war's attackers in this read")
    letters_read = who != me or (snap.get("event_watched") and isinstance(snap.get("event_picks"), list))
    running = wid is not None and wid in {_i(x) for x in snap.get("wars") or {}}
    ended = next((w for w in news.get("wars_ended") or [] if wid is not None and _i(w.get("war_id")) == wid), None)
    days = _days(a["sent"], snap.get("date"))
    if wid is not None and not running and letters_read:
        end = (f" ({ended.get('outcome')} on {ended.get('end_date')})" if ended else "")
        return dict(row, answer="war_ended", how=f"the war is over{end} and no answer letter came: he did not join it")
    if not letters_read:
        return dict(row, answer="not_read", how="the events the game answered for your ruler are not read, and he is "
                                                "not among the war's attackers in this read")
    if who != me or (days is not None and days > ASK_WAIT_DAYS):
        return dict(row, answer="not_read", how=f"{days if days is not None else '?'} days on: no answer letter "
                                                "read and he is not among the war's attackers")
    return dict(row, answer="waiting", how=f"{days if days is not None else '?'} days since the send; no answer "
                                           "letter yet and he is not among the war's attackers")


async def asks_answered(gw, prev: dict, snap: dict, events: list, news: Optional[dict] = None) -> Optional[list]:
    """Each ask this server sent whose recipient decides, with his answer when a read carries it: `refused`
    (the game's decline letter to your ruler, in the events the game answered since the send), `accepted` (his
    character read has the trait the acceptance gives), `waiting` (neither yet, inside ASK_WAIT_DAYS), or
    `not_read` (neither after that, or your ruler changed with neither: the late ruler's recorded events are read by
    his id once). An answered or unread ask is said once and dropped. A call to war is answered by _call_answer
    (`news`: this read's wars_ended and allies_joined rows)."""
    me, was = snap.get("actor_id"), prev.get("actor_id")
    keys = [me] + ([was] if was and was != me else [])
    if not any(_ASKS.get(k) for k in keys):
        return None
    rows, used = [], set()
    for who in keys:
        keep = []
        letters = [e for e in events or [] if isinstance(e, dict)]
        if who != me and _ASKS.get(who):
            # the late ruler's picks stay in the bridge's ring (query_event_rules by his id)
            got = _d(await gw.read("query_event_rules", {"character_id": who}))
            letters = [p for p in got.get("picks") or [] if isinstance(p, dict) and _i(p.get("character_id")) == who]
        for a in _ASKS.get(who) or []:
            rule = ASK_ANSWERS[a["interaction"]]
            if a["interaction"] == CALL_ALLY:
                r = _call_answer(a, rule, letters, used, who, me, news or {}, snap)
                rows.append(r)
                if r["answer"] == "waiting":
                    keep.append(a)
                continue
            row = {"interaction": a["interaction"], "target_id": a["target_id"], "sent": a["sent"]}
            hit = next((i for i, e in enumerate(letters) if (who, i) not in used
                        and e.get("event") == rule["declined_event"] and str(e.get("date") or "") >= a["sent"]), None)
            if hit is not None:
                used.add((who, hit))
                rows.append(dict(row, answer="refused", how=f"the game's decline letter {rule['declined_event']} to "
                                                            f"{'your' if who == me else 'the late'} ruler on "
                                                            f"{letters[hit].get('date')}"))
                continue
            ch = _d(await gw.read("query_character", {"character_id": a["target_id"]}))
            traits = {str(t.get("name") if isinstance(t, dict) else t) for t in ch.get("traits") or []}
            if rule["accepted_trait"] in traits:
                rows.append(dict(row, answer="accepted", how=f"he has the {rule['accepted_trait']} trait (read now)"))
                continue
            days = _days(a["sent"], snap.get("date"))
            if who != me:
                rows.append(dict(row, answer="not_read", how=(
                    f"no {rule['accepted_trait']} trait and no decline letter {rule['declined_event']} among the "
                    "late ruler's recorded events; your ruler changed, so it is not read further")))
            elif days is not None and days > ASK_WAIT_DAYS:
                rows.append(dict(row, answer="not_read", how=(
                    f"{days} days on: no {rule['accepted_trait']} trait and no decline letter "
                    f"{rule['declined_event']} read")))
            else:
                rows.append(dict(row, answer="waiting", how=f"{days if days is not None else '?'} days since the send; "
                                                            "his AI answers in 4-9 days"))
                keep.append(a)
        if keep:
            _ASKS[who] = keep
        else:
            _ASKS.pop(who, None)
    return rows or None


def note_refused_send(actor_id, interaction: str, date) -> None:
    if actor_id and interaction and date:
        _REFUSED_SENDS[(actor_id, interaction)] = date


def idle(snap: dict) -> Optional[dict]:
    """Facts the reads of this snapshot hold that nothing is using, each with
    `next`, the call that lists what can be done about it. None when none holds. Seats are
    named without their councillor_ prefix; the spouse's seat is not counted."""
    out: dict = {}
    council = snap.get("council")
    if isinstance(council, dict):
        vac = sorted(k.replace("councillor_", "") for k, v in council.items()
                     if isinstance(v, dict) and v.get("vacant") and not k.endswith("spouse"))
        if vac:
            out["vacant_seats"] = {"seats": vac, "next": {"tool": "inspect", "args": {"subject": "council"}}}
        bare = sorted(k.replace("councillor_", "") for k, v in council.items()
                      if isinstance(v, dict) and not v.get("vacant") and not v.get("task"))
        if bare:
            out["no_task"] = {"seats": bare, "next": {"tool": "do", "args": {"action_class": "council_tasks"}}}
    tasks = snap.get("council_tasks")
    if isinstance(tasks, list) and not any(isinstance(r, dict) and r.get("task_name") == FABRICATE_TASK
                                           for r in tasks):
        out["no_fabrication"] = {"next": {"tool": "do", "args": {"action_class": "fabricate_targets"}}}
    claims, wars = snap.get("claims"), snap.get("wars")
    if isinstance(claims, dict) and claims and isinstance(wars, dict) and not wars:
        # plan_war here and "no claim casus belli" in the title table
        # was a loop with no exit; the claim's own casus-belli read names its declare_args
        # or its gate (cbs resolves a vassal holder to his realm's ruler)
        first = claims[sorted(claims)[0]] if isinstance(claims[sorted(claims)[0]], dict) else {}
        whom = _i(first.get("war_target_id")) or _i(first.get("holder_id"))
        out["claims_no_war"] = {"title_ids": sorted(claims)[:6],
                                "next": ({"tool": "cbs", "args": {"target_id": whom}} if whom else
                                         {"tool": "plan_war", "args": {}})}
    if snap.get("alliances") == 0:
        out["no_alliance"] = {"next": {"tool": "do", "args": {"action_class": "interactions",
                                                              "candidates": ["alliance"]}}}
    pi = snap.get("piety")
    if isinstance(pi, (int, float)) and pi >= HOF_CLAIM_PIETY_FLOOR:
        # (the hint sent the seat to hof_ask nine times; every Irish holder was
        # of another faith, refused faith_differs, and the quiet year re-armed it): the head of
        # faith's claim is named only with a holder whose pre-check passed here; the gold ask
        # (one recipient, priced) when piety covers it
        ok = _HOF_CLAIM_OK.get(snap.get("actor_id")) or {}
        refused = _REFUSED_SENDS.get((snap.get("actor_id"), HOF_CLAIM_SEND))
        row: dict = {"have": round(pi)}
        if ok:
            holder, title = sorted(ok.items())[0]
            row["hof_claim"] = {"tool": "do", "args": {"intent": "interact", "args": {
                "interaction": HOF_CLAIM_SEND, "target_id": holder, "title_id": title}}}
        no = _ACCEPT_NO.get((snap.get("actor_id"), HOF_GOLD_SEND))
        if pi >= HOF_GOLD_PIETY_LOSS and no:
            # not named while the last acceptance read or send said no (as the claim is)
            row["hof_gold_refused"] = no
        elif pi >= HOF_GOLD_PIETY_LOSS:
            row["next"] = {"tool": "do", "args": {"action_class": "income_levers"}}
            row["lever"] = "hof_gold"
        if refused:
            row["hof_claim_refused"] = refused
        if len(row) > 1:
            out["piety"] = row
    return out or None


#: Actor id -> {holder id: title id} whose head-of-faith claim pre-check passed
_HOF_CLAIM_OK: dict = {}


def note_hof_claim_ok(actor_id, holder_id, title_id) -> None:
    if actor_id and holder_id and title_id:
        _HOF_CLAIM_OK.setdefault(actor_id, {})[holder_id] = title_id


#: The threats read in news runs at most once in this many game days per ruler (it asks the
#: casus-belli check once per candidate attacker)
THREAT_NEWS_DAYS = 90
#: (baseline, actor id) -> the date of the last threats read made for news
_THREAT_READ: dict = {}
#: (baseline, actor id) -> {attacker id: [declarable casus belli]} at that baseline's last threats read
_THREAT_SEEN: dict = {}
#: How a liege change came about. A seated game (1075.04.02, replayed live from its 1075 autosave): no war,
#: no event: Munster's AI sent offer_vassalization_interaction and Murchad's AI accepted inside the send (the
#: interaction has no reply delay). The ai_veto hook records that offer (what=vassal_offer) whether or not it
#: is vetoed; a war that ended with the new liege as its attacker is the other way in.
VASSAL_OFFER_VETO = {"tool": "do", "args": {"intent": "ai_veto", "args": {"op": "set", "what": "vassal_offer"}}}
LEAVE_LIEGE = {"tool": "inspect", "args": {"subject": "factions"},
               "read": "an independence faction against your liege (joinable_factions); at 100% discontent "
                       "its demand is pressed, refused it is a war"}


#: The reason code a title row carries when no record of the step names how it came or went (no war,
#: act, offer or succession in the step's records: inheritance, a grant, an escheat and a creation read the same)
HOW_UNKNOWN = "unknown"
HOW_REASON_NO_RECORD = "no_record_in_step"


def title_how_unknown(out: dict) -> None:
    """Last, after every reader that fills `how`: a title row still without a cause says so, never null."""
    for k in ("titles_gained", "titles_lost"):
        for r in out.get(k) or []:
            if isinstance(r, dict) and r.get("how") is None:
                r.pop("how_unread", None)
                r.update(how=HOW_UNKNOWN, how_reason=HOW_REASON_NO_RECORD)


def _war_lost(w: dict) -> bool:
    """The seat's side lost this ended war (its `my_side` against the outcome)."""
    side, oc = w.get("my_side"), w.get("outcome")
    return (side == "defender" and oc == "attacker_victory") or (side == "attacker" and oc == "defender_victory")


def liege_cause(row: dict, out: dict, prev: dict, snap: dict) -> dict:
    """The cause of a liege change, from what this diff holds: an offer of vassalage the ai_veto hook recorded
    (ai_acted what=vassal_offer: accepted by your ruler's AI), a war that ended with the new liege on the other
    side, independence (to 0); else `cause_unread` with what was looked at. (liege 15236 ->
    28873 read "no war ... no offer" and offered the vassal_offer veto set since the first turn): a war your ruler lost in an
    earlier read whose titles went to the new liege is the cause; a new liege who is your former liege's vassal was
    put above you by that liege; a veto already set is never offered, it is said to be set."""
    new = row.get("to")
    vetoes = snap.get("ai_vetoes") if isinstance(snap.get("ai_vetoes"), list) else None
    veto_on = vetoes is not None and "vassal_offer" in vetoes
    if new == 0:
        return {"cause": "independent: you have no liege now"}
    offers = [a for a in out.get("ai_acted") or [] if isinstance(a, dict) and a.get("what") == "vassal_offer"
              and a.get("offered_by") == new]
    if not offers and not veto_on:
        # (a live read of the 1076 save: the offer recorded 1077.03.20 was named in the diff to 1077.03.27, the liege
        # read changed in the next diff and read "no offer of his was recorded"): every offer the hook still holds
        # for this ruler, from him, within a year (with the veto set those records are offers it dropped)
        offers = [r for r in snap.get("ai_veto_records") or [] if isinstance(r, dict)
                  and r.get("what") == "vassal_offer" and _i(r.get("offered_by")) == new
                  and 0 <= (_days(r.get("date"), snap.get("date")) or 0) <= 365]
    if offers:
        return {"cause": "offer_accepted", "how": f"{new} offered vassalage on {offers[-1].get('date')}: your "
                "ruler's AI accepted it at once (no letter, no event)", "closes_it": VASSAL_OFFER_VETO,
                "to_leave": LEAVE_LIEGE}
    died = [r for r in out.get("titles_lost") or [] if isinstance(r, dict) and r.get("cause") == "liege_died"]
    if died:
        # (a live read of the 1091 save 1091.08.14: liege 31762 -> 77756 read cause_unread beside the titles his
        # death took)
        return {"cause": "liege_died", "how": f"your liege {row.get('from')} died; {new}, his heir, is your liege now",
                "to_leave": LEAVE_LIEGE}
    wars = [w for w in out.get("wars_ended") or [] if isinstance(w, dict)
            and new in (w.get("attacker_id"), w.get("defender_id"))]
    if wars:
        w = wars[-1]
        return {"cause": "war", "how": f"war {w.get('war_id')} ({w.get('name')}) ended {w.get('outcome')}",
                "to_leave": LEAVE_LIEGE}
    # (a test run, step 61, 889.01.28: a claimant faction's war, attacker the faction's leader 11403, ended
    # attacker_victory in this diff; k_france went to the claimant 16710 and he became the ruler's liege; the
    # row read "no war with him ended" and offered the vassal_offer veto): a war your ruler LOST in the same
    # diff is the cause, the one a lost title names first; its winner need not be the new liege (a claimant war
    # is led by the faction's leader for its claimant)
    lost = [w for w in out.get("wars_ended") or [] if isinstance(w, dict) and _war_lost(w)]
    if lost:
        named = {str(r.get("how") or "") for r in out.get("titles_lost") or [] if isinstance(r, dict)}
        w = next((x for x in lost if any(f"war {x.get('war_id')} " in h for h in named)), lost[-1])
        took = [r.get("key") for r in out.get("titles_lost") or [] if isinstance(r, dict)
                and f"war {w.get('war_id')} " in str(r.get("how") or "")]
        return {"cause": "war", "how": (f"war {w.get('war_id')} ({w.get('name')}) ended {w.get('outcome')}: your "
                                        f"ruler lost it" + (f" and {', '.join(map(str, took))} with it" if took
                                                            else "") + f"; {new} holds the title above you now"),
                "to_leave": LEAVE_LIEGE}
    # A war lost in an earlier read (the liege read can change a read later), its titles gone to him
    for w in reversed(_LOST_WARS.get(snap.get("actor_id")) or []):
        if new in w.get("to") and 0 <= (_days(w.get("end_date"), snap.get("date")) or 0) <= 365:
            return {"cause": "war", "how": (f"war {w.get('war_id')} ({w.get('name')}) ended {w.get('outcome')} on "
                                            f"{w.get('end_date')}: your ruler lost it and "
                                            f"{', '.join(map(str, w.get('titles') or []))} went to {new}, who "
                                            "holds the title above you now"), "to_leave": LEAVE_LIEGE}
    old = row.get("from")
    if old and snap.get("liege_of_liege") == old:
        return {"cause": "placed_under_his_vassal",
                "how": (f"{new} is a vassal of {old}, your liege until now: {old} put you under him (a title above "
                        "your county, a duchy or kingdom whose de jure land holds it, went to him, and the game "
                        "moved your vassalage with it), not a war or an offer of yours"),
                "to_leave": LEAVE_LIEGE}
    unread = {"cause_unread": "no war with him ended in this diff and no offer of his was recorded (the offer is "
                              "recorded only while the ai_veto hook watches your ruler)", "to_leave": LEAVE_LIEGE}
    if veto_on:
        unread["veto_already_set"] = ("your ai_veto vassal_offer is set: your ruler's AI drops an offer of "
                                      "vassalage before it can accept it, so this was not one")
    else:
        unread["closes_it"] = VASSAL_OFFER_VETO
    return unread


#: Per ruler, the wars he lost and the titles each took to whom, kept a year (liege_cause)
_LOST_WARS: dict = {}


def note_lost_wars(actor_id: Any, out: dict) -> None:
    """Remember each war your ruler lost in this diff with the titles it took and their new holders."""
    for w in out.get("wars_ended") or []:
        if not (isinstance(w, dict) and _war_lost(w)):
            continue
        went = [r for r in out.get("titles_lost") or [] if isinstance(r, dict)
                and f"war {w.get('war_id')} " in str(r.get("how") or "")]
        row = {"war_id": w.get("war_id"), "name": w.get("name"), "outcome": w.get("outcome"),
               "end_date": w.get("end_date"), "titles": [r.get("key") for r in went][:3],
               "to": sorted({_i(r.get("holder_id")) for r in went if _i(r.get("holder_id"))})}
        keep = [x for x in _LOST_WARS.get(actor_id) or [] if x.get("war_id") != row["war_id"]]
        _LOST_WARS[actor_id] = (keep + [row])[-8:]


#: (the disinherit order fired 892.02.08 and was refused; the dynasty headship had passed from
#: the ruler 14577 to Lothaire 13495 between the turn succession_plan read disinherit available and eight turns later; no read
#: said so): the dynasty's head changed, toward the ruler or away from him
DYNASTY_HEAD_LOST = ("only your dynasty's head can disinherit (00_dynast_interactions.txt) and spend its renown on "
                     "legacies; your heir does not inherit the headship unless he is the next head")


def dynasty_head_changed(prev: dict, snap: dict) -> Optional[dict]:
    """The news row when the head of the ruler's dynasty changed between two reads of it, or None."""
    a, b, me = prev.get("dynasty_head"), snap.get("dynasty_head"), snap.get("actor_id")
    if a is None or b is None or a == b or prev.get("dynasty_id") != snap.get("dynasty_id"):
        return None
    row = {"from": a, "to": b, "you_are_head": b == me}
    if a == me or (b != me and prev.get("actor_id") == me):
        row["lost"] = DYNASTY_HEAD_LOST
    if b == me:
        row["gained"] = "your ruler is the dynasty's head now: disinherit and the dynasty's legacies are his"
    row["next"] = {"tool": "succession_plan", "args": {}}
    return row


#: Per ruler, the titles a war that ended named, kept a year (a title can arrive a diff later)
_WAR_TITLES: dict = {}
#: {actor_id: {county key: {how, date}}}, each county's recorded cause when it was gained (a barony that
#: arrives in a later diff inside it is attributed to it)
_COUNTY_CAUSE: dict = {}


def barony_in_held_county(gained: list, held: dict, parents: dict, snap: dict, hist_tried: frozenset = frozenset()
                          ) -> None:
    """A recorded case (step 21 b_slemish, step 26 b_armagh: each barony arrived one diff after its county's
    war won c_ulster / c_oriel, "how unknown, no_record_in_step", and the title history did not name the ruler): a
    barony still without a cause, inside a de jure county the ruler holds whose own gain has a recorded cause, is said
    to be inside that county, with the county's cause and its date, and the source named. A county gained in this diff
    with a cause is remembered first. A barony in a county held with no recorded cause keeps `unknown` and says the
    county it is in."""
    mem = _COUNTY_CAUSE.setdefault(snap.get("actor_id"), {})
    for g in gained:
        k = str(g.get("key") or "")
        if k.startswith("c_") and g.get("how") and g.get("how") != HOW_UNKNOWN and "barony_source" not in g:
            mem[k] = {"how": g["how"], "date": snap.get("date")}
    held_keys = {str(v) for v in held.values()}
    for g in gained:
        if g.get("how") is not None or not str(g.get("key") or "").startswith("b_"):
            continue
        county = parents.get(g.get("title_id"))
        if not county or str(county) not in held_keys:
            continue
        c = mem.get(str(county))
        if c is None:
            g["inside_county"] = county
            continue
        g.pop("how_unread", None)
        g.update(how=f"inside {county}, which you gained on or before {c['date']}: {c['how']}", inside_county=county,
                 barony_source="the county's recorded cause; the barony's own arrival has no record in this step"
                               + ("; its title history did not name you" if g.get("title_id") in hist_tried else ""))

SPLIT_LEVERS = {"tool": "succession_plan", "args": {}, "read": "prevent_split (its first page): for each son who "
                                                              "would leave the realm with land, disinherit and vows "
                                                              "with the price, whether the game allows it now and the "
                                                              "call; levers.take_out_of_split has the forecast after each"}


def heir_dynasty_rows(snap: dict) -> dict:
    """Heir_other_dynasty at every read while the primary heir is of another dynasty (not only when it
    changes: a seat that saw only the change read the stale heir list for five turns and did nothing)."""
    lr = snap.get("line_risk") or {}
    if not lr.get("warning"):
        return {}
    return {"heir_other_dynasty": {"what": lr["warning"], "levers": lr.get("levers") or [],
                                   "next": {"tool": "succession_plan", "args": {}}}}


def split_rows(prev: Optional[dict], snap: dict, out_events: dict) -> dict:
    """Patch C. heir_split_ahead: the forecast splits and the ruler is 50 or older or in poor
    health (below 4), once per change of who takes land. succession_turned_split: the last read
    passed intact and this one splits (1071: Murchad inherited Leinster from his father and
    his own succession turned split the same day), naming the titles gained in the same step."""
    rows: dict = {}
    sp = snap.get("split") or {}
    takers = sp.get("takers") if isinstance(sp.get("takers"), list) else None
    alarm = None
    if takers and sp.get("intact") is False and _rd.poor(snap.get("age"), snap.get("health")):
        alarm = list(takers)
        if (prev or {}).get("split_alarm") != alarm:
            rows["heir_split_ahead"] = {
                "takers": takers, "leaves_realm": sp.get("leaves_realm") or [],
                "may_leave_realm": sp.get("may_leave_realm") or [],
                "age": snap.get("age"), "health": snap.get("health"),
                "why_now": "50 or older, or health below 4: the death may come before the levers can be paid",
                "next": SPLIT_LEVERS}
    snap["split_alarm"] = alarm
    psp = (prev or {}).get("split") or {}
    # (Ulster joined 1071.03.20 and the realm met d_ulster's county rule, so the law would create it
    # for a younger son who leaves with its counties, and a forecast read can be a year old):
    # when the forecast first has a son leave the realm with land, or a new son or title leaves, the news says so
    was = {(x.get("id"), str(x.get("takes"))) for x in psp.get("leaves_with") or [] if isinstance(x, dict)}
    now = [x for x in sp.get("leaves_with") or [] if isinstance(x, dict)]
    if now and any((x.get("id"), str(x.get("takes"))) not in was for x in now):
        rows["split_leaves_realm"] = {
            "at_your_death": now, "takers": takers,
            "why": ("partition: each eligible child takes a share; a son given a title of your tier (held, or created "
                    "by the law from counties your realm holds) leaves the realm with it and its counties"),
            "next": SPLIT_LEVERS}
    if prev is not None and psp.get("intact") is True and sp.get("intact") is False:
        rows["succession_turned_split"] = {
            "takers": takers, "leaves_realm": sp.get("leaves_realm") or [],
            "titles_gained_this_step": [t.get("key") for t in out_events.get("titles_gained") or []],
            "why": "more titles than one heir keeps under partition: each living eligible child takes a share",
            "next": SPLIT_LEVERS}
    return rows


def claims_ended(prev: dict, snap: dict, old: dict, new: dict) -> dict:
    """The claims held at the last read and not now: claims_lost, and (1080.12.26: claims_lost
    k_ireland in the step the AI created it for him) claims_now_held, a claim that ended because the title is now
    the ruler's."""
    succession = prev.get("actor_id") != snap.get("actor_id")
    gone = sorted(set(old) - set(new))
    held = _held_ids(snap)
    # At a succession, the claims that died with the predecessor say so
    out: dict = {"claims_lost": [dict(old[t], **({"lost_at_succession": {
        "ruler_before": prev.get("actor_id"), "why": "unpressed: it died with its holder"}
        if old[t].get("type") == "unpressed" else {"ruler_before": prev.get("actor_id"),
                                                  "why": "not held by the new ruler"}}
                                           if succession else {}))
                                 for t in gone if _tid(t) not in held]}
    now_held = [dict(old[t], why="you hold the title now: a claim on your own title ends")
                for t in gone if _tid(t) in held]
    if now_held:
        out["claims_now_held"] = now_held
    return out


def _held_ids(snap: dict) -> set:
    """The title ids the ruler holds at this read (snapshot `titles`, id -> key; ids may be str keys)."""
    return {_tid(k) for k in (snap.get("titles") or {}) if _tid(k) is not None} if isinstance(snap.get("titles"), dict) else set()


def _tid(v) -> Optional[int]:
    """A title id held as an int or as its decimal string (a snapshot that went through JSON)."""
    return int(v) if isinstance(v, str) and v.isdigit() else _i(v)


#: (actor id, was, now) -> the date of the read that first named a realm's growth: each baseline diffs
#: against its own last read, so the turn news and the seat's own observe can both name one change
_VJ_NAMED: dict = {}


#: An independent ruler of a lower tier who borders you may OFFER you his fealty (offer_fealty_interaction,
#: 00_character_interactions.txt: his send, your ruler's AI accepts or declines in 5-10 days); the veto hook counts
#: only your ruler's own sends and the proposals mod holds only offer_vassalization, so this one is never read
FEALTY_OFFER_UNREAD = ("not read either: a lower-tier independent neighbour's offer of fealty to your ruler "
                       "(offer_fealty_interaction), his send, which your ruler's AI accepts")


def vassals_joined(prev: dict, snap: dict, out: dict) -> Optional[dict]:
    """The realm grew by counties no title gained and no war ended explains: a ruler became your vassal.
    The cause the reads give: your ruler's AI sent offers of vassalage in this step (the bridge's census of the AI's
    interactions for a watched ruler), else unread."""
    a, b = prev.get("realm_counties"), snap.get("realm_counties")
    if not (isinstance(a, int) and isinstance(b, int)) or b <= a or prev.get("actor_id") != snap.get("actor_id"):
        return None
    if [t for t in out.get("titles_gained") or [] if str(t.get("key") or "").startswith("c_")] or [
            w for w in out.get("wars_ended") or [] if isinstance(w, dict) and w.get("outcome") == "attacker_victory"
            and w.get("my_side") == "attacker"]:
        return None
    row: dict = {"realm_counties": {"was": a, "now": b}}
    sent = lambda s: sum(_i(x.get("count")) or 0 for x in s.get("ai_vassal_offers_sent") or [])  # noqa: E731
    if isinstance(snap.get("ai_vassal_offers_sent"), list) and sent(snap) > sent(prev):
        last = max((str(x.get("last") or "") for x in snap["ai_vassal_offers_sent"]), default=None)
        row["cause"] = "your_ai_offered_vassalage"
        row["how"] = (f"your ruler's AI sent {sent(snap) - sent(prev)} offer(s) of vassalage in this step (last "
                      f"{last}); a ruler who accepts becomes your vassal with no war (offer_vassalization_interaction)")
    else:
        row["cause_unread"] = ("no title of yours was gained and no war of yours was won; the AI's offers of vassalage "
                               "for your ruler are counted only while the bridge's veto hook watches him")
        # The game's other road
        # with no war, which no read records
        row["or_his_offer"] = FEALTY_OFFER_UNREAD
    # (8 -> 9 named in the turn news of 1070.11.28 and again in the seat's observe of
    # 1071.07.26, whose last observe was 1070.05.23): the same change named before says when
    k = (snap.get("actor_id"), a, b)
    first = _VJ_NAMED.get(k)
    if first is not None and first != snap.get("date"):
        row["named_before"] = {"date": first, "why": ("the same change, named in an earlier read; this one diffs "
                                                      f"against your read of {prev.get('date')}")}
    else:
        _VJ_NAMED[k] = snap.get("date")
    return row


#: The most counties one news diff reads the cause of (two reads each)
LEFT_REALM_READS = 4
#: What no read names when a vassal keeps his county and his realm changes
LIEGE_CHANGE_UNREAD = ("he kept it and is no longer in your realm: his liege changed (independence, a faction's "
                       "demand, another ruler's war on him, or his own offer of fealty); which one no read names")


async def counties_left_realm(gw, snap: dict, out: dict, left: list) -> Optional[list]:
    """A recorded case (1095.01: c_oriel "realm 36303 -> 36358, holder 64380 -> 56065" was only a
    map_changes row, and the seat wrote that it did not look into how): a county of your realm that a vassal held and
    that is in another realm now, with its cause from the reads there are: your own titles lost are `titles_lost`;
    a new holder from the county's title history (inheritance, conquest, ...: its newest entry, when it names him,
    and whom it came from), the old holder's character read (dead or alive), and a war of yours that ended in this
    diff; the same holder with a new realm is a liege change, its cause not read."""
    mine = {_tid(r.get("title_id")) for r in out.get("titles_lost") or []}
    rows = []
    for x in left[:LEFT_REALM_READS]:
        tid = _i(x.get("title_id"))
        if tid in mine:
            continue
        was, now = _i(x.get("holder_was")), _i(x.get("holder_now"))
        row = {"county": x.get("county"), "holder": f"{was} -> {now}", "realm_now": x.get("realm_now")}
        if was and was == now:
            row["how"] = LIEGE_CHANGE_UNREAD
        else:
            hist = _d(await gw.read("query_title_history", {"title_id": tid, "limit": 2})) if tid else {}
            ents = [e for e in hist.get("entries") or [] if isinstance(e, dict)]
            old = _d(await gw.read("query_character", {"character_id": was})) if was else {}
            dead = " (dead)" if old.get("alive") is False else ""
            if ents and _i(ents[0].get("char_id")) == now:
                raw = _i(ents[0].get("event_type_raw"))
                kind = HISTORY_TYPES.get(raw, f"history type {raw} (not named)")
                row["how"] = (f"{kind}: to {now} on {ents[0].get('date')}, from your vassal {was}{dead} (the game's "
                              f"title history); {now} is in realm {x.get('realm_now')}, not yours")
                row["history_type"] = raw
            else:
                row["how"] = (f"not named: the county's title history did not name {now}; your vassal {was}"
                              f"{dead or (' (alive)' if old.get('alive') is True else '')} held it")
        ended = [w.get("war_id") for w in out.get("wars_ended") or [] if isinstance(w, dict) and _war_lost(w)]
        if ended:
            row["wars_you_lost_in_this_step"] = ended
        rows.append(row)
    return rows or None


async def threat_rows(gw, snap: dict, baseline: str) -> Optional[dict]:
    """Patch D: attackers_new, a ruler who can now declare on you with a casus belli he could
    not at the last threats read (at most once in THREAT_NEWS_DAYS game days)."""
    me = snap.get("actor_id")
    if not _i(me):
        return None
    key = (baseline, me)
    last = _THREAT_READ.get(key)
    if last is not None and (_days(last, snap.get("date")) or 0) < THREAT_NEWS_DAYS:
        return None
    # each baseline diffs against its own last read (a live read: the environment's turn news read first and
    # the seat's observe, diffing against that shared read, never saw the new casus belli)
    before = _THREAT_SEEN.get(key)
    try:
        res = await _threats.read(gw, me)
    except Exception as e:  # noqa: BLE001  a read fault must not cost the news block
        return {"unread": f"the threats read failed: {type(e).__name__}"}
    _THREAT_READ[key] = snap.get("date")
    _THREAT_SEEN[key] = dict(_threats._LAST.get(me) or {})
    fresh = _threats.new_attackers(me, before)
    rows = {a["id"]: a for a in res.get("attackers") or [] if isinstance(a, dict) and _i(a.get("id"))}
    if before is None:
        # The first read (turn one, a new ruler) names who can make you his vassal NOW (a seated game: Munster
        # could from 1070 and took Murchad by an offer in 1075; the news named him only when his casus belli
        # was new, and only by name)
        fresh = [{"id": a, "cbs": list(r.get("can_declare") or [])} for a, r in rows.items()
                 if "vassalization_cb" in (r.get("can_declare") or [])
                 or (r.get("vassalization") or {}).get("valid_now") is True]
    if not fresh:
        if before is None:
            # (the first read omitted the row when nobody could vassalize; absence read as unread)
            return {"attackers": "none", "neighbours_read": len(rows)}
        return None
    you = res.get("you") or {}
    out = []
    for a in fresh:
        r = rows.get(a["id"]) or {}
        # Who and how strong against you in the row itself (seven rows pointed at inspect threats in one recorded
        # run and the seat never read it)
        out.append(dict(a, name=r.get("name"), strength=r.get("strength"), x_yours=r.get("x_yours"),
                        **({"already_able": True} if before is None else {})))
    row = {"attackers": out, "your_strength": you.get("strength"),
           "vassalization": (res.get("vassalization") or {}).get("verdict"),
           "next": {"tool": "inspect", "args": {"subject": "threats"}}}
    if any("vassalization_cb" in (a.get("cbs") or []) or (rows.get(a["id"], {}).get("vassalization") or {})
           .get("valid_now") is True for a in out):
        row["by_offer"] = {"why": "a higher-tier neighbour's AI may also OFFER vassalage and your ruler's AI may "
                                  "accept it at once (with no war and no letter)", "closes_it": VASSAL_OFFER_VETO}
    return row


#: character id -> the date of the first read that found him dead (the bridge reads no date of death)
_DEATHS_SEEN: dict = {}


def note_dead(cid: Any, date: Any) -> None:
    """Record the first read that found `cid` dead, on `date`."""
    c = _i(cid)
    if c and date and c not in _DEATHS_SEEN:
        _DEATHS_SEEN[c] = str(date)


def death_seen(cid: Any) -> Optional[str]:
    """The date a read of the news first found `cid` dead, else None."""
    return _DEATHS_SEEN.get(_i(cid))


async def heir_change_why(gw, was: Any, snap: dict) -> dict:
    """A recorded case (succession_plan named Brian 36303 primary heir at 1082; the news of 1085.04.20 said
    only `heir_changed` Brian -> Dungal, and the record could not tell a death from a wrong forecast): why the last
    primary heir is no longer it, from one read of him: he died, he is now the ruler, or he lives and the line
    passed him by. {"why": ...}; a read that did not answer is said to be unread."""
    pid = _i((was or {}).get("id")) if isinstance(was, dict) else None
    if not pid:
        return {}
    name = (was or {}).get("name") or pid
    if pid == _i(snap.get("actor_id")):
        return {"why": f"{name} ({pid}) is now your ruler"}
    c = _d(await gw.read("query_character", {"character_id": pid}))
    if c.get("alive") is False:
        note_dead(pid, snap.get("date"))
        return {"why": f"{name} ({pid}) died"}
    if c.get("alive") is True:
        return {"why": f"{name} ({pid}) is alive and is no longer first in the line (the game's own heir read)"}
    return {"why_unread": f"query_character {pid} did not say whether he lives"}


async def wars_ended(gw, prev: dict, snap: dict, old: dict, new: dict) -> list:
    """The wars of the ruler that ended between two reads: those the last read listed and this one does not,
    each with its history record; and those one of your acts ended in the window."""
    gone = sorted(set(old) - set(new))
    hist = await gw.read_many([("query_war_history", {"war_id": x}) for x in gone]) if gone else []
    ended = []
    for x, h in zip(gone, hist):
        row = dict({k: v for k, v in old[x].items() if k not in ("is_attacker", "cb_type", "attackers")}, my_side=_side(old[x]))
        row["_gold_years"] = _gold_years(old[x].get("cb_type"))
        row["_cb"] = old[x].get("cb_type")      # The prestige price of its ending
        # (a seated game, 1070.11.09: the Athlone war's history read returned war 42's record first, and the
        # county the war won was then unattributed): the record of THIS war, never the list's first
        hw = _d(h).get("wars") if isinstance(_d(h).get("wars"), list) else []
        rec = next((r for r in hw if isinstance(r, dict) and _i(r.get("war_id")) == _i(x)), None)
        if rec is None and len(hw) == 1 and isinstance(hw[0], dict) and "war_id" not in hw[0]:
            rec = hw[0]    # a record that names no war (never one naming ANOTHER war)
        if isinstance(rec, dict):
            row.update(outcome=rec.get("outcome"), end_date=rec.get("end_date"))
            row["_titles"] = [t for t in (rec.get("target_titles") or []) if _i(t) is not None]
        else:
            row.update(outcome=None, outcome_unread="query_war_history did not list this war")
        ended.append(row)
    # (a test run: a war declared and surrendered inside one observe's window, the war was in
    # neither list, so that observe said "no war of yours ended" while the news read named it): a war one of
    # your acts ended in this window is read from its history even when the last read never listed it. A war
    # the AI began and ended inside one window is still unseen (no act names it).
    mine = sorted({x["war_id"] for x in _war_end_acts(prev, snap) if _i(x.get("war_id"))} - set(old) - set(new))
    for x, h in zip(mine, await gw.read_many([("query_war_history", {"war_id": x}) for x in mine]) if mine else []):
        hw = _d(h).get("wars") if isinstance(_d(h).get("wars"), list) else []
        rec = next((r for r in hw if isinstance(r, dict) and _i(r.get("war_id")) == _i(x)), None)
        other = listed_elsewhere(snap, x, prev)
        row = {"war_id": x, "name": (rec or {}).get("name"), "my_side": None, "seen": "ended by your act; " + (
            f"begun and ended after this read's last one ({prev.get('date')}); another read listed it on {other}"
            if other else "begun and ended between two reads")}
        if isinstance(rec, dict):
            me = _i(snap.get("actor_id"))
            row.update(attacker_id=rec.get("attacker_id"), defender_id=rec.get("defender_id"),
                       outcome=rec.get("outcome"), end_date=rec.get("end_date"))
            row["my_side"] = ("attacker" if _i(rec.get("attacker_id")) == me else
                              "defender" if _i(rec.get("defender_id")) == me else None)
            row["_cb"] = rec.get("cb_type") or rec.get("cb")
            row["_titles"] = [t for t in (rec.get("target_titles") or []) if _i(t) is not None]
        else:
            row.update(outcome=None, outcome_unread="query_war_history did not list this war")
        if _declared_by_you(prev, snap, x) is not None:
            _say_declared(row, prev, snap)
            row["seen"] = "ended by your act; " + row["seen"]
        ended.append(row)
    # (a test run, step 3: the Dublin war, declared at the turn and lost inside the step, had no row; the purse
    # then read -357.55 prestige "unexplained" and ruled "no war of yours ended" out): every war the game's war
    # history ended inside this window with your ruler as a war leader, whichever list saw it
    ended += await wars_ended_in_window(gw, prev, snap, {_i(r.get("war_id")) for r in ended} | set(new))
    return ended


#: The most recent ended wars read per side (the bridge's war history, newest first)
WINDOW_WARS_READ = 16
WINDOW_SEEN = "begun and ended between two reads (the game's war history for this window)"


async def wars_ended_in_window(gw, prev: dict, snap: dict, known: set) -> list:
    """The wars the game's war history (query_war_history, every war end the bridge saw) ended between the last
    read and this one with the ruler as attacker or defender leader, not already in `known`. A window whose dates did
    not read gives none."""
    me = _i(snap.get("actor_id"))
    lo, hi = _ordinal_of(prev.get("date")), _ordinal_of(snap.get("date"))
    if not me or lo is None or hi is None or not hasattr(gw, "read_many"):
        return []
    reads = await gw.read_many([("query_war_history", {"attacker_id": me, "limit": WINDOW_WARS_READ}),
                                ("query_war_history", {"defender_id": me, "limit": WINDOW_WARS_READ})])
    out, seen = [], set(known)
    for h in reads:
        for rec in (_d(h).get("wars") or []) if isinstance(_d(h).get("wars"), list) else []:
            x = _i((rec or {}).get("war_id")) if isinstance(rec, dict) else None
            end = _ordinal_of(rec.get("end_date")) if x is not None else None
            if x is None or x in seen or end is None or not lo < end <= hi:
                continue
            seen.add(x)
            row = {"war_id": x, "name": rec.get("war_name") or rec.get("name"), "seen": WINDOW_SEEN,
                   "attacker_id": rec.get("attacker_id"), "defender_id": rec.get("defender_id"),
                   "outcome": rec.get("outcome"), "end_date": rec.get("end_date"),
                   "my_side": ("attacker" if _i(rec.get("attacker_id")) == me else
                               "defender" if _i(rec.get("defender_id")) == me else None)}
            row["_cb"] = rec.get("cb_type") or rec.get("cb")
            row["_gold_years"] = _gold_years(row["_cb"])
            row["_titles"] = [t for t in (rec.get("target_titles") or []) if _i(t) is not None]
            other = listed_elsewhere(snap, x, prev)
            if other:
                row["seen"] = (f"begun and ended after this read's last one ({prev.get('date')}); another read "
                               f"listed it on {other}")
            _say_declared(row, prev, snap)
            out.append(row)
    return out


def _declared_by_you(prev: dict, snap: dict, war_id) -> Optional[dict]:
    """The declaration this server sent for the ruler between the two reads that started war `war_id`, or None."""
    if _i(war_id) is None:
        return None
    return next((x for x in reversed(acts_in_step(prev, snap))
                 if str(x.get("what") or "").startswith("declared war") and _i(x.get("war_id")) == _i(war_id)), None)


def listed_elsewhere(snap: dict, war_id: Any, prev: dict) -> Optional[str]:
    """The date another read (another baseline: the turn's news, or the seat's observe) first listed war `war_id`
    after this read's last one, or None when no read did."""
    d = (_WARS_LISTED.get(snap.get("actor_id")) or {}).get(_i(war_id))
    lo = _ordinal_of(prev.get("date"))
    return d if d and (lo is None or (_ordinal_of(d) or -1) >= lo) else None


def _say_declared(row: dict, prev: dict, snap: dict) -> None:
    """A war this read did not list that your own declaration started: `seen` says so, with the day you declared it,
    and whether another read listed it in between (then it was news there, not here)."""
    d = _declared_by_you(prev, snap, row.get("war_id"))
    if d is not None:
        row["declared_by"] = "you"
        row["declared"] = d.get("date")
        other = listed_elsewhere(snap, row.get("war_id"), prev)
        row["seen"] = (f"declared by you on {d.get('date')} and ended on {row.get('end_date') or '(unread)'}, "
                       f"{row.get('outcome') or 'outcome unread'}, "
                       + (f"after this read's last one ({prev.get('date')}); another read listed it on {other}"
                          if other else "before any read listed it"))


async def we_win_rows(gw, snap: dict) -> list:
    """Each war whose total, as the bridge's war-score cache reads it, reached what we_win needs for your
    side, with the end_war call. (1076.05.12: a Peasant Uprising read war_score_yours 100 from the
    cache while the game's own check, the same day, read 0-25 and refused we_win): a row stands only when the game's own
    reading (end_war's read-only listing) opens we_win."""
    import redesign as R
    ws = snap.get("war_scores") or {}
    won = [x for x, s in sorted(ws.items()) if isinstance(s, int) and s >= R.END_WAR_WE_WIN_SCORE]
    rows = []
    for x in won:
        w = (snap.get("wars") or {}).get(x) or {}
        enemy = w.get("defender_id") if w.get("is_attacker") else w.get("attacker_id")
        reading = await R.end_war_open_terms(gw, _i(snap.get("actor_id")), _i(enemy))
        if not isinstance(reading, dict) or "we_win" not in (reading.get("open_terms") or []):
            continue
        rows.append({"war_id": x, "name": w.get("name"), "war_score_yours": ws[x],
                     "next_call": {"tool": "do", "args": {"intent": "end_war", "args": {
                         "enemy_id": enemy, "terms": "we_win"}}}})
    return rows


async def block(gw, requested_id: int, snap: dict, *, offers_served: bool,
                fabricate_within: Optional[str] = None, baseline: str = "observe") -> dict[str, Any]:
    """The news block for this observe, and the snapshot kept for the next. `baseline`
    names whose last read it diffs against (BASELINES); only that one moves."""
    prev = _LAST.get((baseline, requested_id))
    shown = _SHOWN.get((baseline, requested_id))
    if (baseline == "turn" and prev is not None and shown is not None and snap.get("date")
            and prev.get("date") == snap.get("date") and shown[0] == snap.get("date")):
        # (a test run, pre-flight: observe(depth="news") at a turn's start read nothing):
        # the environment reads this baseline when the step ends, to write the turn's opening
        # message, on the same server the seat calls; the seat's first read then diffed a
        # zero-day interval. No game day has passed since: the same news again, and the
        # baseline stays where it was (what the seat does today shows in the next step's)
        return dict(shown[1], read_again={"date": snap.get("date"), "detail": (
            "the news since the last step, read again: no game day has passed since it was "
            "first read (the turn's opening observation holds the same)")})
    _LAST[(baseline, requested_id)] = snap
    if snap.get("actor_id") and isinstance(snap.get("wars"), dict):
        seen_w = _WARS_LISTED.setdefault(snap["actor_id"], {})
        for w in snap["wars"]:
            if _i(w) is not None:
                seen_w.setdefault(_i(w), snap.get("date"))
    out: dict[str, Any] = {
        "since": prev.get("date") if prev else None,
        "offers": ("incoming_proposals" if offers_served else
                   "not served (no proposal mod in this game): the game's AI answers offers"),
    }
    # A claim-letter rule's amount is kept
    # at the letter's price as read now
    fl = await _seat_events.refresh_letter_floors(gw, snap.get("actor_id"), snap.get("chaplain_learning"))
    if fl:
        out["letter_rule_raised"] = fl
    idl = idle(snap)
    if idl:
        out["idle"] = idl
    if _task_goal.peaceful() and snap.get("actor_id"):
        # (1072-1073: the game made the son diarch, which blocked disinherit and vows on him,
        # and no row said so): the diarch by the game's check of the dismiss interactions, under a line task only
        council_rows = [{"holder_id": c.get("holder_id"), "vacant": c.get("vacant")}
                        for c in (snap.get("council") or {}).values() if isinstance(c, dict)]
        snap["diarch"] = await _diarchy.read(gw, snap["actor_id"], {"children": snap.get("family") or []},
                                             council_rows)
        if prev is None and isinstance(snap["diarch"], dict) and snap["diarch"].get("id") is not None:
            out["diarch_now"] = _diarchy.news_row({"id": None}, snap["diarch"])
        else:
            dr = _diarchy.news_row((prev or {}).get("diarch"), snap["diarch"])
            if dr:
                out["diarch_changed"] = dr
    if isinstance(fabricate_within, str) and fabricate_within.startswith("k_") and snap.get("actor_id"):
        # The kingdom's counties in the realm at each read, for the counties a succession took away
        kc = await gw.read("query_dejure_progress", {"character_id": snap["actor_id"], "title_key": fabricate_within,
                                                     "verbose": 1})
        rows_ = kc.get("counties") if isinstance(kc, dict) else None
        snap["kingdom_counties"] = ({str(_i(c.get("county_id"))): _i(c.get("holder_id")) for c in rows_
                                     if isinstance(c, dict) and c.get("you_control") is True}
                                    if isinstance(rows_, list) else None)
        if isinstance(snap["kingdom_counties"], dict):
            _KINGDOM_COUNTIES_BY[snap["actor_id"]] = (snap.get("date"), dict(snap["kingdom_counties"]))
    if prev is None:
        out["first_observe"] = True
        out["note"] = "the first observe: nothing to compare yet"
        # What the game's AI spends for the ruler, and the lever for each (a fact, once).
        # With the turn-one notes beside it (they say what each spend cost and what stops it),
        # the block keeps its calls and drops the prose the notes carry
        # Served once per server (the first observation of an episode: in an environment, the turn's opening;
        # the seat's own first observe then keeps under 8 KB without a second copy)
        t1 = turn_one_notes()
        if t1 is not None and not _TURN_ONE_SERVED:
            out["turn_one_notes"] = t1
            _TURN_ONE_SERVED.append(snap.get("date"))
        out["ai_spends"] = ai_spends(levers_only=t1 is not None)
        #
        # the spending kinds not vetoed, and the one call that sets them all
        su = _seat_events.spending_unset(snap.get("ai_vetoes")) if isinstance(snap.get("ai_vetoes"), list) else None
        if su:
            out["ai_veto_spending"] = su
        vu = veto_unset(snap)
        if vu:
            # the notes say why each spend matters: the rows keep their facts and the call
            out["ai_veto_unset"] = ({k: {x: y for x, y in r.items() if x != "why"} for k, r in vu.items()}
                                    if t1 is not None and isinstance(vu, dict) else vu)
        await _map_view.news_part(gw, snap.get("actor_id"), snap.get("date"), baseline)   # The baseline
        # (a test run: 32095's point waited, his first news read was this baseline and said nothing)
        await _seat_events.enrich_news(gw, snap.get("actor_id"), out, snap.get("ai_vetoes"))
        out.update(split_rows(None, snap, {}))
        out.update(heir_dynasty_rows(snap))
        # The turn-one note sends the seat to this row for the culture veto (it applies to a ruler
        # whose culture is not his capital's only), so the first observation carries it too
        cc = await culture_conversion_ahead(gw, snap)
        if cc:
            out["culture_conversion_ahead"] = cc
        # The baseline of who can declare on you; its first read names who can make you a vassal now
        tr = await threat_rows(gw, snap, baseline)
        if tr and not tr.get("unread"):
            out["vassalizers_now"] = tr
        # A vassal already in a faction at the first read (the turn's opening) is told so now
        out.update(_faction_letters.member_news({}, snap))
        _task_goal.news(out)
        _SHOWN[(baseline, requested_id)] = (snap.get("date"), out)
        return out
    if prev.get("actor_id") != snap.get("actor_id"):
        out["ruler_changed"] = {"from": prev.get("actor_id"), "to": snap.get("actor_id"),
                                "next": {"tool": "session", "args": {"action": "whoami"}}}
        age = snap.get("age")
        if isinstance(age, (int, float)) and not isinstance(age, bool) and age < ADULT_AGE:
            # (1078.02: Echrad, 2, ruled under a regency to 1091; no row said what changed)
            out["ruler_changed"]["child_ruler"] = await child_ruler_facts(gw, snap)
        carried = await carry_vetoes(gw, requested_id, prev, snap)
        if carried is not None:
            out["ruler_changed"]["ai_veto_carried"] = carried
        rules = await carry_event_rules(gw, requested_id, prev, snap)
        if rules is not None:
            out["ruler_changed"]["event_rules_carried"] = rules
        # Item e (a seated game 1068.06: Brian lost the vassalization casus belli, k_ireland rose 325 -> 425,
        # and the perk note was turn one's only): what the new ruler can do that his predecessor could not
        out["ruler_changed"]["briefing"] = await ruler_change_briefing(gw, prev, snap, fabricate_within)
        a_, b_ = prev.get("kingdom_counties"), snap.get("kingdom_counties")
        a_date = prev.get("date")
        late = _KINGDOM_COUNTIES_BY.get(prev.get("actor_id"))
        if late and (_ordinal_of(late[0]) or -1) > (_ordinal_of(a_date) or -1):
            a_, a_date = late[1], late[0]          # the late ruler's newest read, by whichever baseline
        if isinstance(a_, dict) and isinstance(b_, dict) and set(a_) - set(b_):
            gone = sorted(set(a_) - set(b_), key=int)
            # The counties of the kingdom that left the realm at the succession, with who holds them now
            now = await gw.read_many([("query_title_hierarchy", {"title_id": int(c)}) for c in gone])
            out["ruler_changed"]["counties_left_realm"] = {
                "kingdom": fabricate_within, "was": len(a_), "was_read": a_date, "now": len(b_),
                "counties": [{"title_id": int(c), "key": _d(_d(h).get("title")).get("key"),
                              "holder_id": _i(_d(_d(h).get("title")).get("holder_id"))} for c, h in zip(gone, now)],
                "why": ("partition: the late ruler's land is divided among his eligible children, and confederate "
                        "partition creates duchies for younger sons, who leave the realm with their counties")}
    unread = []

    def both(k):
        if prev.get(k) is None or snap.get(k) is None:
            unread.append(k)
            return None
        return prev[k], snap[k]

    c = both("claims")
    if c is not None:
        old, new = c
        gained = []
        for t in sorted(set(new) - set(old)):
            war: dict = {}
            nxt = await _claim_next(gw, snap.get("actor_id"), new[t], war)
            gained.append(dict(new[t], next=nxt,
                               **({"on_your_death": CLAIM_DIES} if new[t].get("type") == "unpressed" else {}),
                               **claim_facts(new[t], snap, war)))
        out["claims_gained"] = gained
        out.update(claims_ended(prev, snap, old, new))
    w = both("wars")
    if w is not None:
        old, new = w
        out["wars_started"] = [dict({k: v for k, v in new[x].items() if k not in ("is_attacker", "cb_type", "attackers")},
                                    my_side=_side(new[x]),
                                    next={"tool": "inspect", "args": {"subject": "war_score", "war_id": x}},
                                    # (a seated game 1090: an eradicate_house_cb war read target_titles [])
                                    **_takes_all(new[x]))
                               for x in sorted(set(new) - set(old))]
        out["wars_ended"] = await wars_ended(gw, prev, snap, old, new)
        joined = await _allies_joined(gw, snap.get("actor_id"), old, new, prev.get("date"), snap.get("date"))
        if joined:
            out["allies_joined"] = joined
    if prev.get("liege_id") != snap.get("liege_id") and None not in (prev.get("liege_id"), snap.get("liege_id")):
        # The cause is named after the seat events are read (liege_cause)
        out["liege_changed"] = {"from": prev["liege_id"], "to": snap["liege_id"]}
    dh = dynasty_head_changed(prev, snap)
    if dh:
        out["dynasty_head_changed"] = dh
    ph, nh = (prev.get("heir") or {}).get("id"), (snap.get("heir") or {}).get("id")
    if ph != nh:
        out["heir_changed"] = {"from": prev.get("heir"), "to": snap.get("heir"),
                               **(await heir_change_why(gw, prev.get("heir"), snap)),
                               "next": {"tool": "succession_plan", "args": {}}}
    k = both("council")
    if k is not None:
        old, new = k
        changed = []
        fab = fabrication_of(snap.get("actor_id"))
        for seat in sorted(set(old) | set(new)):
            a, b = old.get(seat), new.get(seat)
            if a != b:
                row = {"seat": seat, "before": a, "now": b,
                       "next": {"tool": "inspect", "args": {"subject": "council"}}}
                if (a or {}).get("task") == "task_fabricate_claim" and (b or {}).get("task") != "task_fabricate_claim":
                    done = bool(fab) and fab.get("title_id") in (snap.get("claims") or {})
                    if fab and str(fab.get("seat") or "") in seat:
                        row["fabrication_left"] = {"title_id": fab["title_id"], "title_key": fab.get("title_key"),
                                                   "since": fab.get("since"), "claim_held": done}
                    if fab and str(fab.get("seat") or "") in seat and not done \
                            and reverts_of(snap.get("actor_id"), fab.get("title_id")) < REVERT_STOP:
                        # The claim is not held: the call that sets it again
                        row["next"] = {"tool": "do", "args": {"intent": "council_task", "args": {
                            "seat": fab.get("seat") or "court_chaplain", "task": FABRICATE_TASK,
                            "target_title_id": fab["title_id"]}}}
                    else:
                        row["next"] = {"tool": "do", "args": {"action_class": "fabricate_targets",
                                                              **({"args": {"within": fabricate_within}}
                                                                 if fabricate_within else {})}}
                changed.append(row)
        out["council_changed"] = changed
    # The fabrication's state (running, on what, since when; or stopped)
    fab = fabrication_of(snap.get("actor_id"))
    if fab and fab.get("title_id") in (snap.get("claims") or {}):
        _FABRICATING.pop(snap.get("actor_id"), None)        # done: the claim is held
    fs = fabrication_status(snap.get("actor_id"), snap.get("council_tasks"), snap.get("date"))
    se = _seat_events.news_part(prev, snap)
    declined = [e for e in se.get("events_answered") or [] if e.get("event") in CLAIM_LETTERS
                and _declined(e)]
    if fs is not None:
        if fs.get("running") and declined:
            # (a test run, pre-flight: the letter declined 1067.11.24, the news then said
            # running, then overdue): the claim was not taken, and the task started again
            d = declined[-1]
            fs["claim_declined"] = {"date": d.get("date"), "event": d.get("event"), "applied": d.get("applied"),
                                    "rule": d.get("rule"), "detail": CLAIM_DECLINED}
            seen = _FAB_CHAPLAIN.get(snap.get("actor_id"))
            if seen is not None:
                seen["since"] = d.get("date")
            else:
                _FAB_CHAPLAIN[snap.get("actor_id")] = {"province_id": fs.get("target_province_id"),
                                                       "holder_id": None, "since": d.get("date")}
        if fs.get("running"):
            await _fabrication_timing(gw, snap, prev, fs)
        elif fs.get("reverted") and fs["reverted"]["times"] >= REVERT_STOP:
            # Past REVERT_STOP the counties with their checks, instead of the same call
            fs["next_call"] = {"tool": "do", "args": {"action_class": "fabricate_targets",
                                                      **({"args": {"within": fabricate_within}}
                                                         if fabricate_within else {})}}
        if fs.get("running"):
            # (four fabricated claims died with Murchad in 1080)
            fs["claim_it_makes"] = {"type": "unpressed", "on_your_death": CLAIM_DIES,
                                    **{k: snap.get(k) for k in ("age", "health") if snap.get(k) is not None},
                                    **letter_facts(snap)}
        out["fabrication"] = fs
        if fs.get("overdue") or fs.get("chaplain_changed"):
            out.setdefault("idle", {})["fabrication_overdue" if fs.get("overdue") else "fabrication_reset"] = {
                **({"months_elapsed": fs["expected"]["months_elapsed"],
                    "months_at_most": fs["expected"]["months_at_most"]} if fs.get("expected") else {}),
                **({"chaplain_changed": fs["chaplain_changed"]} if fs.get("chaplain_changed") else {}),
                "next": {"tool": "do", "args": {"action_class": "fabricate_targets",
                                                **({"args": {"within": fabricate_within}} if fabricate_within else {})}}}
    # A war you attack in whose loss price moved 10% or more from its first read
    moved = _loss_price_moved(snap)
    if moved:
        out["loss_price_moved"] = moved
    # A war whose total reached what we_win needs for your side
    ww = await we_win_rows(gw, snap)
    if ww:
        out["wars_at_we_win"] = ww
    sc = both("schemes")
    if sc is not None:
        old, new = sc
        out["schemes_ended"] = [dict(old[x], outcome=None,
                                     outcome_unread="no read gives a scheme's outcome once it leaves the list",
                                     next={"tool": "inspect", "args": {"subject": "opinion",
                                                                       "target_id": old[x].get("target_id")}})
                                for x in sorted(set(old) - set(new))]
    ti = both("titles")
    if ti is not None:
        old, new = ti
        by_war = {t: r for r in out.get("wars_ended") or [] for t in r.get("_titles") or []}
        # A war that ended in an earlier diff
        # still names the titles it won for a year
        mem = _WAR_TITLES.setdefault(snap.get("actor_id"), {})
        for t, r in list(by_war.items()):
            mem[t] = {k: r.get(k) for k in ("war_id", "outcome", "end_date", "my_side")}
        for t, r in list(mem.items()):
            if (_days(r.get("end_date"), snap.get("date")) or 0) > 365:
                mem.pop(t, None)
            elif t not in by_war:
                by_war[t] = r

        def _how(t, lost=False):
            r = by_war.get(t)
            # (a test run, steps 6 and 20: c_nantes and c_lyon listed LOST "by" the wars that had won them a
            # step earlier, when they went to a vassal): a war is the cause of a title lost only when the seat's
            # side lost it, and of a title gained only when it did not
            if r is not None and r.get("my_side") is not None and _war_lost(r) != lost:
                r = None
            return ({"how": f"war {r.get('war_id')} ended {r.get('outcome')}"} if r else
                    {"how": None, "how_unread": "no war in this diff targeted it: inheritance, grant "
                                                "or creation read the same"})
        gained = [dict({"title_id": t, "key": new[t]}, **_how(t)) for t in sorted(set(new) - set(old))]
        # (a test run, step 20: c_rennes gained how null; the seat had declared its claim war in the same step
        # and the war started and ended between two reads, so no wars_ended row named it): the seat's own
        # declaration on that title in the step, its war no longer running
        running = {str(x) for x in (snap.get("wars") or {})}
        lo, hi = _ordinal_of(prev.get("date")), _ordinal_of(snap.get("date"))
        for g in gained:
            if g.get("how") is not None or lo is None or hi is None:
                continue
            decl = next((x for x in reversed(acts_in_step(prev, snap))
                         if str(x.get("what") or "").startswith("declared war")
                         and _i(x.get("war_title_id")) == _i(g.get("title_id")) and str(x.get("war_id")) not in running),
                        None)
            if decl is not None:
                g.pop("how_unread", None)
                g.update(how=f"war {decl.get('war_id')} you declared for it on {decl.get('date')} ended between two "
                             "reads (in no wars_ended row) and the title came with it", by="you")
        if prev.get("actor_id") != snap.get("actor_id"):
            # A new ruler's
            # titles that his predecessor did not hold came to him at the succession or were his before it
            for g in gained:
                if g.get("how") is None:
                    g.pop("how_unread", None)
                    g.update(how=f"at the succession: your ruler is now {snap.get('actor_id')} (was "
                                 f"{prev.get('actor_id')}); he inherited it or held it before")
        # A barony gained in the same diff as its de-jure county came with it
        # (live 1068.09.15: b_athlone with c_athlone read how null)
        parents = snap.get("title_parents") or {}
        came = {g["key"]: g for g in gained if g.get("how") and str(g.get("key") or "").startswith("c_")}
        for g in gained:
            county = came.get(parents.get(g["title_id"]))
            if g.get("how") is None and str(g.get("key") or "").startswith("b_") and county:
                g.pop("how_unread", None)
                g.update(how=county["how"], with_county=county["key"])
        for g in gained:
            # (d_ulster and k_ireland, the seat's own creations, read "how": null while the purse
            # named them): a title the seat's own mint_title applied in this step, or the AI's recorded creation
            k = str(g.get("key") or "")
            if g.get("how") is not None or k[:2] not in ("d_", "k_", "e_"):
                continue
            mine = own_act_in_step(prev, snap, "mint_title", k)
            acted = next((a for a in se.get("ai_acted") or [] if isinstance(a, dict)
                          and a.get("what") == "create_title" and a.get("key") == k), None)
            if mine is not None:
                g.pop("how_unread", None)
                g.update(how=f"created by you (do mint_title, sent {mine.get('date')})", by="you")
            elif acted is not None:
                g.pop("how_unread", None)
                g.update(how=f"created for you by the game's AI on {acted.get('date')} (ai_acted)", by="ai")
        # (a test run, step 57, 1090.09.07: d_ulster, c_ulster, c_oriel and four baronies gained "how unknown,
        # no_record_in_step"; no war, act or succession of the seat's in the step): the game's own title history names
        # how each came (an inheritance from whom, a creation, a conquest), the newest entry being the ruler's
        reads, hist_tried = 0, set()
        counties_in = {str(g.get("key")) for g in gained if str(g.get("key") or "").startswith("c_")}
        for g in sorted(gained, key=lambda x: "ekdcb".find(str(x.get("key") or "x")[:1]) % 6):
            if g.get("how") is not None or reads >= GAINED_HISTORY_READS \
                    or parents.get(g.get("title_id")) in counties_in:      # a barony goes with its county below
                continue
            reads += 1
            hist_tried.add(g.get("title_id"))
            h = await gained_from_history(gw, snap.get("actor_id"), g.get("title_id"))
            if h:
                g.pop("how_unread", None)
                g.update(h)
        came = {g["key"]: g for g in gained if g.get("how") and str(g.get("key") or "").startswith("c_")}
        for g in gained:
            county = came.get(parents.get(g["title_id"]))
            if g.get("how") is None and str(g.get("key") or "").startswith("b_") and county:
                g.pop("how_unread", None)
                g.update(how=county["how"], with_county=county["key"])
        barony_in_held_county(gained, new, parents, snap, frozenset(hist_tried))
        out["titles_gained"] = gained
        out["titles_lost"] = [dict({"title_id": t, "key": old[t]}, **_how(t, lost=True))
                              for t in sorted(set(old) - set(new))]
        rs = await resync_goal_chain(gw, snap, [str(t.get("key") or "") for t in gained + out["titles_lost"]])
        if rs is not None:
            out["ai_veto_goal_chain"] = rs
    # A title
    # whose holder is now in your realm was granted, not lost; the realm still counts it
    for r in out.get("titles_lost") or []:
        if r.get("how") is None and not str(r.get("key") or "").startswith("b_"):
            how = await _lost_to(gw, snap.get("actor_id"), r.get("title_id"), prev.get("liege_id"),
                                 prev.get("actor_id") if prev.get("actor_id") != snap.get("actor_id") else None)
            if how:
                r.pop("how_unread", None)
                r.update(how)
    # A barony lost
    # in the same diff as its de jure county went with it
    _lp = prev.get("title_parents") or {}
    _went = {r["key"]: r for r in out.get("titles_lost") or [] if r.get("how") and str(r.get("key") or "").startswith("c_")}
    for r in out.get("titles_lost") or []:
        c = _went.get(_lp.get(r.get("title_id")))
        if r.get("how") is None and str(r.get("key") or "").startswith("b_") and c:
            r.pop("how_unread", None)
            r.update(how=c["how"], with_county=c["key"])
    granted_by_ai(prev, snap, out.get("titles_lost") or [])
    # the answer to an ask the target decides (vows, a call to war), from the events the game answered, his
    # traits and the war lists; read before the purse, which counts an ally's joining
    aa = await asks_answered(gw, prev, snap, se.get("events_answered") or [], out)
    if aa:
        out["asks_answered"] = aa
        joined = await _joined_by_letter(gw, prev, snap, aa, out)
        if joined:
            out["allies_joined"] = (out.get("allies_joined") or []) + joined
    # a call your ruler's AI sent (ai_acted ally_call) that his acceptance letter answered: the ally joined, and the
    # game charged the war's attacker for him as for a call of yours
    joined_ai = await _joined_by_ai_call(gw, prev, snap, se, out)
    if joined_ai:
        out["allies_joined"] = (out.get("allies_joined") or []) + joined_ai
    # Prestige, piety, gold that moved, with the acts this server knows the cost of,
    # and what else this diff saw that moves them (claims, wars, laws, titles, income)
    if prev.get("actor_id") != snap.get("actor_id"):
        # A new ruler's purse is his own; it starts a new baseline here
        out["purse"] = {"new_ruler": {"from": prev.get("actor_id"), "to": snap.get("actor_id"),
                                      **{k: (round(snap[k], 2) if isinstance(snap.get(k), (int, float)) else None)
                                         for k in ("gold", "prestige", "piety")},
                                      "predecessor_last_read": {k: (round(prev[k], 2) if _isnum(prev.get(k)) else None)
                                                                for k in ("gold", "prestige", "piety")},
                                      "note": ("a new ruler: these are his own gold, prestige and piety, a new "
                                               "baseline; the change from his predecessor's is not a gain or a loss")}}
    else:
        for key in ("prestige", "piety", "gold"):
            ch = _resource_change(prev, snap, key, dict(out, events_answered=se.get("events_answered"),
                                                         ai_acted=se.get("ai_acted"), ai_vetoed=se.get("ai_vetoed")))
            if ch is not None:
                out.setdefault("purse", {})[key] = ch
    # Legitimacy moved, and with it the claim-war price's multiplier
    lg = legitimacy_change(prev, snap)
    if lg and prev.get("actor_id") == snap.get("actor_id"):
        out.setdefault("purse", {})["legitimacy"] = lg
    # The events the game answered for a watched ruler; the reserve's buckets
    if se.get("budget"):
        out.setdefault("purse", {})["budget"] = se.pop("budget")
    out.update(se)
    if "liege_changed" in out:
        out["liege_changed"].update(liege_cause(out["liege_changed"], out, prev, snap))
    note_lost_wars(snap.get("actor_id"), out)
    # The prestige of each ally call the AI sent or was stopped from sending; the perk points waiting
    await _seat_events.enrich_news(gw, snap.get("actor_id"), out, snap.get("ai_vetoes"))
    # The factions aimed at the ruler (1079.11.13: the AI granted a populist demand; no read warned of it)
    out.update(_faction_letters.news_part(prev, snap))
    out.update(_faction_letters.member_news(prev, snap))
    # Two recorded runs: -1,000 prestige when the AI converted Domnall to Dublin's culture
    fd = free_decisions_ahead(snap)
    if fd:
        out["free_decisions_ahead"] = fd
    cc = await culture_conversion_ahead(gw, snap)
    if cc:
        out["culture_conversion_ahead"] = cc
    g = (out.get("purse") or {}).get("gold")
    if isinstance(g, dict) and isinstance(g.get("unexplained"), (int, float)) and g["unexplained"] < -GOLD_CAUSE_MIN:
        # (a test run, pre-flight 1067.11.27: -226.9 at a claim's arrival, -142.6 unexplained)
        built = await constructions_started(gw, snap.get("actor_id"), prev.get("date"), snap.get("date"))
        ruled = []
        if built:
            g["constructions_started"] = built
            g["construction_note"] = CONSTRUCTION_NOTE
            est = [b["price_estimate"] for b in built if isinstance(b.get("price_estimate"), (int, float))]
            if est:
                # The started constructions' price, beside what the purse could not explain
                g["constructions_expected"] = round(sum(est), 2)
            join_constructions(out.get("ai_acted") or [], built, snap.get("actor_id"))
            count_constructions_in_step(g, built, snap.get("actor_id"), out.get("ai_acted") or [])
            if any(b.get("ordered_by") == snap.get("actor_id") for b in built):
                # The AI's buildings (it built for -151 and -167 in one game): the lever is the construction veto
                g["lever"] = ai_spend_veto("construction")
        elif built is not None:
            ruled.append("no construction started in your counties in this step")
        # Mercenaries under contract to you that
        # this server did not hire
        mercs = await mercs_hired_for(gw, snap.get("actor_id"))
        own = {_i(x.get("company_id")) for x in _ACTS.get(snap.get("actor_id")) or [] if x.get("company_id")}
        # A hire the bridge recorded in this step (ai_acted) is priced even when the company
        # was named before (company 536 named 1091.12, under contract again 1093, not counted)
        hired_now = {_i(a.get("company_id")) for a in out.get("ai_acted") or []
                     if a.get("what") == "mercenary" and _i(a.get("company_id")) is not None}
        if mercs:
            ai = [m for m in mercs if m["company_id"] not in own]
            new = [m for m in ai if m["company_id"] not in _MERC_SEEN.setdefault(snap.get("actor_id"), set())
                   or m["company_id"] in hired_now]
            _MERC_SEEN[snap.get("actor_id")].update(m["company_id"] for m in ai)
            if ai:
                g["mercenaries_hired_for_you"] = ai
                g["mercenary_note"] = MERC_AI_NOTE
                g["lever"] = ai_spend_veto("mercenary")
                est = [m["price_estimate"] for m in new if isinstance(m.get("price_estimate"), (int, float))]
                if est:
                    g["mercenaries_expected"] = -round(sum(est), 2)
                by_id = {m["company_id"]: m for m in ai}
                for a in out.get("ai_acted") or []:
                    m = by_id.get(_i(a.get("company_id"))) if a.get("what") == "mercenary" else None
                    if m and isinstance(m.get("price_estimate"), (int, float)):
                        a["price_estimate"] = m["price_estimate"]
                        a["label"] = m.get("label")
                if hired_now:
                    g["hired_by_the_ai_in_this_step"] = sorted(hired_now)
                    g["mercenary_veto"] = ai_veto_call("mercenary")
                    count_hires_in_step(g, ai, hired_now)
        elif mercs is not None:
            ruled.append("no mercenary company is under contract to you")
        if ruled:
            g["ruled_out"] = ruled
        g["not_read"] = [x for x in GOLD_NOT_READ if mercs is None or "mercenaries" not in x]
        un = _seat_events.unclassified_in_step(prev, snap)
        if un:
            # The AI's posts for your ruler of classes no veto kind classifies (the bridge's census)
            g["ai_posts_not_classified"] = un
    elif isinstance(g, dict) and isinstance(g.get("unexplained"), (int, float)) and g["unexplained"] > GOLD_CAUSE_MIN:
        # A rise names what was ruled out
        ev = {k: out.get(k) for k in ("wars_ended", "titles_gained", "claims_gained")}
        g["ruled_out"] = (["no war of yours ended"] if not ev["wars_ended"] else []) + \
                         (["no title gained"] if not ev["titles_gained"] else []) + \
                         ([f"not the head of faith's gold: its accept letter {HOF_GOLD_LETTER} is not among this "
                           "step's answered events"] if letters_read(snap) and _letter_in_step(
                             {"events_answered": se.get("events_answered")}, HOF_GOLD_LETTER) is None else [])
        g["not_read"] = list(GOLD_RISE_NOT_READ)
    for r in out.get("wars_ended") or []:
        r.pop("_titles", None)
        r.pop("_gold_years", None)
        r.pop("_cb", None)
    # A split ahead near the death, a succession that turned split, a new attacker
    out.update(split_rows(prev, snap, out))
    out.update(heir_dynasty_rows(snap))
    tr = await threat_rows(gw, snap, baseline)
    if tr and tr.get("attackers") != "none":
        out["attackers_new"] = tr
    vj = vassals_joined(prev, snap, out)
    if vj:
        out["vassals_joined"] = vj
    # The region's holders that changed since the last news, with the map read to take
    mc = await _map_view.news_part(gw, snap.get("actor_id"), snap.get("date"), baseline)
    left = (mc or {}).pop("_left", None)
    if mc:
        out["map_changes"] = mc
    if left:
        cl = await counties_left_realm(gw, snap, out, left)
        if cl:
            out["counties_left_realm"] = cl
    # An unset veto whose spend can come now (after the ruler change's carry-over)
    vu = veto_unset(snap)
    if vu:
        out["ai_veto_unset"] = vu
    if unread:
        out["unread"] = {"sections": unread,
                         "detail": "these reads did not answer in this observe or the last one; "
                                   "their changes are unknown, not none"}
    title_how_unknown(out)
    _task_goal.news(out)
    _SHOWN[(baseline, requested_id)] = (snap.get("date"), out)
    return out


#: The keys compact() drops: notes a seat reads once in observe's news
_COMPACT_DROP = ("offers", "note")


def _compact_seat(r: dict) -> dict:
    """A council_changed row: the seat's task before and now when the holder stayed."""
    out = {x: r[x] for x in ("seat", "before", "now", "fabrication_left", "next") if x in r}
    a, b = r.get("before"), r.get("now")
    if isinstance(a, dict) and isinstance(b, dict) and a.get("holder_id") == b.get("holder_id") \
            and not a.get("vacant") and not b.get("vacant"):
        out.update(before=a.get("task"), now=b.get("task"))
    return out


def compact(news: Any) -> Any:
    """The news block made small for a per-turn hand-off: empty sections and the
    once-read notes dropped, a fabrication row cut to its facts. Nothing that
    changed is dropped; a block with nothing in it says `nothing_new`."""
    if not isinstance(news, dict):
        return news
    out = {}
    for k, v in news.items():
        if k in _COMPACT_DROP or v in ([], {}, None):
            continue
        if k == "fabrication" and isinstance(v, dict) and v.get("running") is True:
            v = {x: v[x] for x in ("running", "target", "target_province_id", "since", "days", "progress",
                                   "days_left_measured", "expected", "overdue", "chaplain_changed", "claim_it_makes",
                                   "claim_declined", "letter_price") if x in v}
            if isinstance(v.get("letter_price"), dict):
                v["letter_price"] = {x: y for x, y in v["letter_price"].items() if x != "rule"}
            if isinstance(v.get("expected"), dict):
                # the rule's sentence is read once in observe; the per-turn hand-off keeps the numbers
                v["expected"] = {x: y for x, y in v["expected"].items() if x != "rule"}
        if k == "council_changed" and isinstance(v, list):
            v = [_compact_seat(r) for r in v if isinstance(r, dict)]
        if k == "titles_gained" or k == "titles_lost":
            # (a build-proof run turn 13 b_slemish: the cause reached the seat, its source and county did not)
            v = [{x: r[x] for x in ("title_id", "key", "how", "how_reason", "with_county", "inside_county",
                                    "barony_source") if x in r} for r in v]
        out[k] = v
    purse = out.get("purse")
    quiet_purse = isinstance(purse, dict) and all(
        isinstance(r, dict) and set(r) <= {"was", "now", "change"} and abs(r.get("change") or 0) < 1
        for r in purse.values())
    quiet = {"since", "first_observe", "idle", "ai_spends", "turn_one_notes"}    # Notes are not news
    if (out.get("vassalizers_now") or {}).get("attackers") == "none":
        quiet = quiet | {"vassalizers_now"}            # "nobody can" is a reading, not news
    if set(out) <= quiet or (set(out) <= quiet | {"purse"} and quiet_purse):
        out["nothing_new"] = True
    return out


#: liege id -> name, read once per server (a liege's name does not change)
_NAMES: dict = {}


async def liege_name(gw, liege_id: int) -> Optional[str]:
    """The liege's name: query_character once per liege id, cached. None when the
    read did not name him (never a key or an id in its place)."""
    if liege_id in _NAMES:
        return _NAMES[liege_id]
    got = _d(await gw.read("query_character", {"character_id": liege_id}))
    name = got.get("name") if isinstance(got.get("name"), str) and got.get("name") else None
    if name is not None:
        _NAMES[liege_id] = name
    return name
