"""The 13 tools' descriptions and per-argument texts.

ONE place for what a seat reads about each tool. A seat sees only the tools'
names, descriptions and input schemas, so each text says what the code does and
nothing else, and every number in it is a code constant interpolated here (no
literal digits: test_tool_surface_listing.py checks every number in every registered
description against these constants and the constants against the code that
owns them). Closed value sets are schema enums, not prose.

The session text has two forms: with CK3_CLOCK_OWNER=env the environment owns the
clock and the clock paragraph is not served (redesign.clock_owned_by_env).
"""
from __future__ import annotations

import army_orders as _army
import personality_axes as _pa
import redesign as R
import tools as T

QUERY_BATCH_MAX = 5          # mcp_server.QUERY_BATCH_MAX is this value (checked)

ENVELOPE = ("Every reply: {ok, tool, result, blocked, warnings[], errors[], bridge_health, game: "
            "{date, tick, paused, speed}, actor}; a write adds verdict (applied | rejected | pending) "
            "and pending {intent_id, drains_when}. ok=false carries blocked {gate, summary, retryable, "
            "fix?, measured?}: the gate names why, fix what to send instead. errors[] rows are reads "
            "that failed (that section is unread, never empty).")

UNITS = ("Units: gold, prestige and piety are the game's own numbers (monthly_income is gold per game "
         "month); military_strength is the game's soldier-strength figure, comparable between "
         f"realms; war_score runs {R.WAR_SCORE_MIN} to {R.WAR_SCORE_MAX}, positive favours the "
         "attacker (favored_side names the side, my_side yours); game.date is the game date "
         "(Y.M.D), game.tick the bridge's frame counter.")

TITLE_REF = ("A title argument (title_id, cb_title_id, target_title_id, location_id) takes a key ('c_dublin') or an id; a key is answered with result.titles_named [{title_key, title_id, "
             "name}].")

OBSERVE = (
    "Your situation, first every cycle. brief: de_jure (your held titles' de-jure county counts; "
    "next_title: the title above them and every requirement to create it), standing (gold, debt, stress, strongest faction %, succession ETA, "
    "at_war), wars (each war, your side, war_score when the game has computed it), money, military "
    "(strength, levies, men-at-arms), liege, council (seat, holder, task), succession (the verdict "
    "succession_plan gives), posture (the personality genome), news (what changed since your last "
    "observe: claims, wars started and ended, schemes ended, titles gained and lost, ruler, liege, "
    "heir and council changes, fabrication, wars at we_win, purse moves (what priced them), "
    "idle (what nothing uses), each with a `next` call; offers are incoming_proposals when served), "
    "since (records naming your realm; withheld counts the rest) and goal. Any other value is refused. "
    f"Which tool: query, one raw read per domain (up to {QUERY_BATCH_MAX}); inspect, a typed or "
    "composed subject; discover, whether one action would pass; do, ready calls and acts; plan_war, "
    "cbs, declare_war for wars; succession_plan; set_phase a posture, steer single axes; session. "
    + ENVELOPE + " " + UNITS)

QUERY = (
    "One bridge read per domain, its reply under result (domains=[...]: up to "
    f"{QUERY_BATCH_MAX}, each under its own key; one that failed is an errors[] row, ok false). "
    "inspect: composed subjects; observe: the cycle's view. Each argument applies to the "
    "domains it names. characters: the character browser (filter, sort, court_of, "
    f"female; default {T.CHARACTERS_DEFAULT_ROWS} rows); marriage_candidates: landed unmarried "
    f"rulers in range (default {T.MARRIAGE_DEFAULT_ROWS}), rows paired with you or your child; "
    "neighbors: a county's neighbours, or with title_id 0 your capital's plus bordering_realms; "
    f"conquest_targets: {T.CONQUEST_TARGETS_DEFAULT_ROWS} counties outside your realm, conquerable "
    "first, each with declare_args; buildings: your title_id's slots and what "
    "can be built, priced; the feeds (war_declarations, scheme_inits, council_tasks, "
    "event_log) take since_tick (the previous reply's current_tick) and limit. A read the "
    "bridge declined is refused under its reason's gate. "
    f"Over {T.QUERY_REPLY_KB} KB rows are cut (truncated). "
    "title_id takes a key ('c_dublin') or an id; a key is echoed in result.titles_named "
    "[{title_key, title_id, name}].")

INSPECT = (
    "A typed read of one subject. title (title_key='k_scotland'): every de-jure county with its id, "
    "key, name, duchy, holder, the realm it is in (realm_ruler_id, name, strength), whether you "
    "control it, your claim on it and next_call (declare, fabricate or the gate); the de-jure duchies (who holds each, how many counties you "
    "control); the realms present; and the counties the title's creation needs. map: one line per "
    "county of a kingdom (title_key; default your primary title's) and its border, layers chosen "
    "(holders, casus belli, your wars' sieges and armies, strength, neighbours); ? = not read. heir_plan: each "
    "child and heir with the levers that exist for them. guardian_candidates: guardians for a child. "
    "threats: who can declare war on you now and with what (the game's check, rulers near you as "
    "attacker) and the vassalization rule. "
    "war_stance: each war's side, stance, occupation and a stall verdict. war_score: your wars, or "
    "war_id's. factions: as liege, each faction with its threat and the levers against its leader. "
    f"pending: the writes still queued and the outcomes of the last {R.PENDING_RING_SIZE} drained "
    f"(a {R.PENDING_RING_SIZE + 1}th drops the oldest); max_results lists the newest (default "
    f"{R.INSPECT_MAX_RESULTS_DEFAULT}, at most {R.PENDING_LIST_MAX}). marriage: the same "
    "rows. The other subjects are one bridge read each. " + TITLE_REF)

DISCOVER = (
    "Check one action before sending it, or list the names a do or declare_war call needs. "
    "interactions / decisions: the names (decisions in a do call come only from here); "
    "decisions=[fragments] filters the names; decisions_annotated / valid_decisions (decisions=[names]): "
    "each with its unmet requirements and cost; acceptance (interaction_type, target_id): the send's own "
    "checks and his answer, nothing sent; interaction_validity "
    "(interaction, target_id, cb, title): the game's own check; valid_cbs / valid_cbs_strict / "
    "war_validity: casus belli checks; war_targets: candidates judged by cbs's read; "
    "construction_check (building, location_id, slot): the build button's verdict; imprisonment: "
    "the grounds against target_id; targets (action, title_id for claim): the valid targets for one "
    "action; decision_why_blocked (decision_type): per-clause pass or fail; scheme_types; "
    "scheme_targets (scheme, target_id or candidates, max_results default "
    f"{R.SCHEME_TARGETS_DEFAULT}): whom a scheme can start against, by the game's check (a refused row lists the game's reasons, "
    "each {check, summary}). Lists with "
    "ready calls (mercenaries, fabricate-claim targets, factions) are do(action_class=...). "
    "actor_id or character_id is the actor (0 = yours). " + TITLE_REF)

#: Appended to DISCOVER while the shared notes are on (notes.py).
DISCOVER_NOTES = " notes (topic): game rules and tool use, by topic; no topic: the index."

DO = (
    "Act, or list what you can do. Enumerate: do(action_class=..., candidates=[...]) answers "
    "available and unavailable rows, each with its do_call and, when "
    "unavailable, blocked with the gate. decisions needs candidates (names from "
    "discover(what='decisions')); formable_titles needs candidates (title keys) and gives each one's requirements, met or not: de-jure counties held vs needed, the price vs your purse, "
    "the game's check and its words. fabricate_targets: args={'within': key} or "
    "candidates=[county keys] judges them. interactions: candidates are name fragments. "
    "income_levers: the writes that bring gold or prestige. Execute: do(intent=..., args={...}) runs one action. "
    "Every write answers verdict applied (read back), rejected (blocked: the gate, and the game's "
    "words in blocked.detail when given) or pending (pending.drains_when "
    "next_pass: the verdict was not read in time; clock_advances: it ran and its effect needs game "
    "days). inspect(subject='pending') reads a pending write; "
    "do(intent='cancel_pending', args={'request_id': <intent_id>}) withdraws one not yet run. "
    "Peace: make_peace offers white_peace, surrender (you lose, always taken) or enforce_demands "
    "(you win); end_war ends a war you lead against enemy_id with the casus belli's outcome "
    "(we_win, white_peace, we_yield = you lose); check=true reads the open terms and prices each "
    "(gold, prestige, truce). "
    # The sentence the build's switches make true (army_orders.py; off by default,
    # where it reads as before, byte for byte)
    + _army.do_sentence() +
    "appoint only swaps two seated councillors. "
    "event_rule: a standing answer to your ruler's event (decline = the other of two options; news "
    "events_answered). gold_reserve: reserved gold, not a stop on AI buying (ai_veto is). "
    "ai_veto: stops your ruler's AI acts (news ai_vetoed). "
    "A refused write is re-sent after a day or a write of yours. "
    + TITLE_REF)

DO_ARGS = ("Intent-specific keys; values after = are the closed set the code takes. An "
           "enumerate row's do_call carries them; a missing one is refused with "
           "fix.rejected_fields. ")

PLAN_WAR = (
    "Your best next wars, ranked: targets already declarable, ally-free and goal-advancing, each "
    "with declare_args for declare_war as is. Each row's casus belli passed the game's "
    "own check when read (precheck says when and what it does not cover); strength (yours and the "
    "target's, with allies, and their ratio), score (de-jure counties gained times that ratio), "
    "truce and affordability (the casus belli's base cost; the game prices it per target). Rows that "
    "fail are in excluded with their gate. A claim casus belli is offered on your own claim or on a "
    "courtier's pressed claim (claim_pressed). Empty: empty_reason names the commonest gate, and fix "
    "the argument that would admit those rows. goal_scoped false adds every duchy bordering your "
    "realm; toward (a title key) puts that title's duchies first. A refused declaration returns "
    "after a day or a write of yours.")

DECLARE_WAR = (
    "Declare one war: pass a plan_war row's declare_args unchanged, or target_id, cb, cb_title_id "
    "(a key or an id) and claimant_id. The war list is read back: applied when the war is listed, "
    "rejected with the gate (debt, the claim war's price for this target and a truce are checked "
    "before sending, raised armies read; the game's own refusal names its failed condition), "
    "pending when it is not "
    "listed yet (read inspect(subject='wars') before declaring again). A war that would run across "
    f"your succession (you are {R.STRADDLE_AGE} or older) needs confirm_straddle=true.")

CBS = (
    "Every casus belli against one target (target_id, required), by the game's check: "
    "valid_options whole (family, scope, grants, tradeoffs, cost, declare_args for "
    "declare_war), best the first's cb; blocked_now: refused now by debt, "
    "cost, truce or raised armies; the rest counted by first unmet requirement "
    "(invalid_by_gate), whole with include_invalid.")

SUCCESSION_PLAN = (
    "Will your realm pass to your heir intact? outcome intact, splits (breaker: the heir who takes "
    "titles away) or undetermined (undetermined_reason); partition_forecast: who takes land (your "
    "eligible children) and whose share leaves the realm; the per-title map; death_estimate from "
    "health; levers with do calls: take_out_of_split (a son's vows or disinheritance, priced, the "
    "forecast after), designate_heir, disinherit, grant_title, revoke, title_law, marriage. "
    "observe's succession block is the same verdict.")

SET_PHASE = (
    "Write a named posture: every personality axis of the phase's genome (the phase description "
    "gives each phase's leading values), plus the native AI's tick re-enabled. phase null reads the "
    "genome and names the phase it matches (null when none). result.lock_state is the bridge's word "
    "on the write (live: held against the AI's rewrites). Nothing re-applies it after a succession. "
    "Use steer for single axes.")

STEER = (
    f"Read (no arguments) or write the {len(_pa.AXES)} personality axes that steer what the game's "
    f"AI chooses for this ruler: {', '.join(_pa.AXES)}, integers {_pa.AXIS_MIN} to {_pa.AXIS_MAX}. "
    "The axes you pass are written and held; the rest keep their values. Any other key or value is "
    "refused and nothing is written. A write answers applied with result.readback, rejected or "
    "pending. set_phase writes a whole named posture.")

PASS_TURN = "Choose not to act this cycle; the reason goes to the decision log."
#: Under an environment's clock, pass_turn does not end the turn.
PASS_TURN_CLOCKLESS = PASS_TURN + " It does not end your turn."

SESSION_CORE = (
    "Identity and liveness. whoami: the ruler you control (control can drift: confirm it is alive and "
    "landed); with none bound, ok=false with gate no_ruler_bound. control switches to another ruler after a succession (character_id required); add claims "
    "one more (the first stays primary: a call without character_id acts as it; with several, pass "
    "character_id on every call); release frees one. A character reserved for another seat is "
    "refused (reserved_for_another_seat). health: the bridge's liveness ('hung' = stop calling). "
    "experimental_flags lists the off-by-default engine flags; set_experimental (flag, enabled "
    "required) arms or disarms one.")

SESSION_CLOCK = (
    " The clock: only this tool changes it; every other tool works paused or running. speed "
    f"{R.SPEED_MIN} to {R.SPEED_MAX} (anything else is refused, at war by speed_refused_at_war); "
    "pause; unpause; wait unpauses, lets the clock run, pauses again and returns start_date and "
    f"end_date: seconds (1 to {R.WAIT_CAP_S}) or game_days (stops at the first day at or past "
    "start plus game_days). A registered cadence or horizon refuses a wait beyond it "
    "(cycle_game_days_over_cadence, horizon_reached); in a scheduled run every wait ends at the "
    "cycle's end and speed is refused (speed_registered).")

SESSION_CLOCKLESS = (
    " The environment running this seat owns the clock: end your turn to let game days pass "
    "(pause, unpause, speed and wait are not served: clock_owned_by_env).")


def session_text(clockless: bool) -> str:
    return SESSION_CORE + (SESSION_CLOCKLESS if clockless else SESSION_CLOCK)


def phase_meanings() -> str:
    """Each phase with its three highest-valued axes, read from PHASE_GENOME."""
    out = []
    for name, g in R.PHASE_GENOME.items():
        top = sorted(g.items(), key=lambda kv: -kv[1])[:3]
        out.append(f"{name} ({', '.join(f'{k} {v}' for k, v in top)})")
    return "The posture, or null to read: " + "; ".join(out) + "."
