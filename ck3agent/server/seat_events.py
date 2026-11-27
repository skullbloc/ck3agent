"""The seat's own events (route E2) and gold reserve (route P3).

Events. The game's AI answers an AI ruler's event inside the turn, in the same call
that fires it (re/findings/re_events_purse.md §1): nothing waits for an answer. So a
seat answers IN ADVANCE: `do(intent="event_rule")` sets, per event key, a standing
rule the bridge's event-option hook applies (bridge/include/event_rules.h):
`option` (always that option, by key or index), `decline` (the other option of a
two-option event) or `pay_if` (the named option when gold/prestige/piety covers
`amount`, else the other). No rule = the game's AI picks, unchanged. Every pick the
game makes for a watched ruler is recorded (event, options, the AI's own pick, the
option applied, gold at the pick) and shown in observe's news as `events_answered`.

Gold reserve. `do(intent="gold_reserve", args={"amount": N})` sets the ruler's AI
reserved-gold maximum (the game's `set_reserved_gold_maximum`) and, with
`move: true`, moves the gold held now into it (`move_budget_gold`). Income fills the
reserve first; the AI's budget-aware spending sees the other buckets. Forced charges
(an event option's price, a title's price, upkeep) still take reserved gold, last.

Windows: the event-option hook is refused there (the hook-target review), so
set_event_rule answers event_hook_not_installed.

AI veto. The game's AI posts four of its decisions for an AI-run ruler as
commands through one dispatch funnel (re/findings/re_ai_veto.md): creating a title,
changing a law, hiring mercenaries, starting a construction. `do(intent="ai_veto",
args={"op": "set", "what": "create_title"})` makes the bridge drop that kind for the
seat's ruler before the game runs it; `clear` lets it through again. Every such post for a
controlled ruler is recorded with his gold at that moment and, for titles and laws, the
price the game reads for him; news shows the dropped ones (`ai_vetoed`, with the call that
does it yourself) and the ones that went through (`ai_acted`, with their price).

Two more kinds. `ally_call`: the AI's call_ally_interaction for the ruler (each ally who
joins a war you attack in costs prestige by his tier); the bridge tells its own sends (the
seat's do call_ally) from the AI's by the dispatch's return address and never drops them.
`perk`: the AI's lifestyle perk pick, so the points accumulate for do(intent="lifestyle",
args={"perk": key}) (the bridge's unlock_perk: the ruler's own point, the game's check).

And `decision`: the AI's decisions for the ruler (CExecuteDecisionCommand, posted by the
game's Decisions pass and its decision goal). In both run-eight arms the AI took
convert_to_local_culture_decision for the seat's ruler at 1,000 prestige. `keys` names the
decisions to drop (at most 8); none named drops every decision, which also drops the free
stress-relief decisions (stress_loss_*: 14 in the game's files, none with a cost) the AI uses to
keep its ruler's stress down, so the toolkit's own warning names the key. The seat's own
do take_decision is never dropped (the bridge marks its own post). Each row is priced from the
decision's cost block in the files (event_prices.decision_price), `unknown` where it is computed.
"""
from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Optional

#: the ops and answers the bridge's set_event_rule takes (factions.cpp do_set_event_rule)
EVENT_RULE_OPS = ("set", "clear", "watch", "list")
EVENT_ANSWERS = ("option", "decline", "pay_if")
EVENT_CURRENCIES = ("gold", "prestige", "piety")
#: picks shown by op=list (the bridge keeps 64)
LIST_PICKS = 10
#: the bridge's refusal codes and their gates (the shared table resolves the rest)
REFUSALS = {
    "event_rules_disabled": "experimental_write_disabled",
    "event_hook_not_installed": "bridge_fault",
    "event_required": "missing_required_argument",
    "option_required": "missing_required_argument",
    "amount_required": "missing_required_argument",
    "bad_event_key": "invalid_argument",
    "bad_option_key": "invalid_argument",
    "bad_option_index": "invalid_argument",
    "bad_currency": "invalid_argument",
    "bad_amount": "invalid_argument",
    "answer_must_be_option_decline_or_pay_if": "invalid_argument",
    "op_must_be_set_clear_watch_or_unwatch": "invalid_argument",
    "option_and_option_index_both_given": "invalid_argument",
    "too_many_seated": "watchlist_full",
    "too_many_rules": "watchlist_full",
    "no_rule_for_event": "already_in_that_state",
    "event_not_found": "name_not_found",
    "needs_two_options": "invalid_argument",     # Decline/pay_if on a 3+ option event
    "bad_reserve_amount": "invalid_argument",
    "bad_move": "invalid_argument",
    "no_ai_budget": "engine_unreachable",
    "no_core_data": "engine_unreachable",
}
# The bridge's set_ai_veto refusals (redesign._RAW_GATE mirrors them)
REFUSALS.update({
    "ai_veto_disabled": "experimental_write_disabled",
    "ai_veto_hook_not_installed": "bridge_fault",
    "what_required": "missing_required_argument",
    "what_must_be_create_title_law_mercenary_or_construction": "invalid_argument",
    "op_must_be_set_or_clear": "invalid_argument",
    "too_many_rulers": "watchlist_full",
    "not_a_controlled_character": "actor_not_claimed",
    "what_must_be_a_veto_kind": "invalid_argument",
    # A kind whose command vtable has no Windows anchor (legacy, faction); never on Linux
    "not_on_windows": "not_on_this_platform",
    "call_ally_interaction_unresolved": "engine_unreachable",
    "offer_vassalization_interaction_unresolved": "engine_unreachable",
    "declare_war_interaction_unresolved": "engine_unreachable",
    "call_in_interactions_unresolved": "engine_unreachable",
    "bad_spare_title_ids": "invalid_argument",
    "spare_keys_only_with_set_activity": "invalid_argument",
    "bad_spare_keys": "invalid_argument",
    "too_many_spare_titles": "invalid_argument",
    "keys_only_with_set_decision": "invalid_argument",
    "bad_decision_keys": "invalid_argument",
    "unknown_decision_key": "invalid_argument",
    "keys_would_narrow_every_decision": "invalid_argument",
    "too_many_decision_keys": "invalid_argument",
})
#: the ops the seat sends, and the kinds the bridge's table holds (bridge/include/ai_veto.h)
AI_VETO_OPS = ("set", "clear", "list")
AI_VETO_WHATS = ("create_title", "law", "mercenary", "construction", "ally_call", "perk", "decision", "vassal_offer",
                 "men_at_arms", "war", "court_type", "faction", "legacy", "call_in", "activity")
#: (the seat set six vetoes on turn one and left out mercenary, which the notes listed; the AI hired a
#: company for about 149 gold in 1093 and the run ended 75 gold short of the kingdom): one call sets the kinds whose
#: drop costs the seat nothing but the AI's own pick. Each is something the game's AI does for the ruler that the seat
#: can do itself, and every Munster and Dublin run's evidence shows it spent gold or prestige, or (vassal_offer) gave
#: the realm away: law (crown authority, -140 to -502 prestige), mercenary (149-193 gold), construction (132-167
#: gold), create_title (a duchy into debt), men_at_arms (a mangonel regiment), vassal_offer (an
#: answered offer makes the ruler a vassal).
#: The interactions by which the game's AI gives a ruler's titles away (00_grant_titles_interaction.txt)
AI_GRANT_INTERACTIONS = ("grant_titles_interaction", "grant_vassal_interaction")
#: (at West Francia 867 the AI changed the court type for 500 prestige on the day it passed
#: crown_authority_1 for 792; prestige 986 -> -305 in three games): court_type joins the preset
#: (a seated run, West Francia 867: the AI bought two dynasty legacies for Charles, 867.04 and 885.10,
#: renown 1,578 -> 240; renown is what disinherit, denounce and legitimize cost): legacy joins the preset. Unlike the
#: other kinds the seat cannot buy a legacy itself (no call posts one): with the preset set none is bought until the
#: seat clears it (do ai_veto op=clear what=legacy), and the AI then picks the legacy
#: (1070.05.20: the AI took take_vow_of_poverty_decision for Murchad at -86 gold, -20% income for
#: the 18 years of debt that followed; the preset left decisions out): decision joins the preset, narrowed to the
#: decisions that cost gold (their cost block names gold: the bridge reads it at the post, gold_cost) and the two
#: decisions in the game's files that cost income and nothing else (SPENDING_DECISION_KEYS); the free stress-relief
#: decisions and the rest still pass
#: (1075.10-1076.02 and 1085: about 280 gold spent by activities the game's AI started for Brian, a
#: pilgrimage among them, while the kingdom waited only on gold): activity joins the preset. Every activity type in the
#: game's files (common/activities/activity_types, 1.18) has a gold cost but the gruesome festival (prestige) and the
#: nomad's hunt (herd); the seat's own host_activity goes through the same AI pass, so the type it names is spared
AI_VETO_PRESETS = {"spending": ("law", "mercenary", "construction", "men_at_arms", "create_title", "vassal_offer",
                                "court_type", "legacy", "decision", "activity")}
#: The decisions whose effect adds a modifier that lowers income (common/decisions, common/modifiers; 1.18):
#: each with what it costs, for the news row of a dropped or taken one
SPENDING_DECISION_KEYS = {
    "take_vow_of_poverty_decision": "monthly income -20% for life (vow_of_poverty_modifier, 00_religion_modifiers.txt)",
    "provide_for_the_poor_oath_decision": "monthly income -5% (oath_provide_for_the_poor, its modifier file)",
    # No cost block, so the gold rule cannot
    # see it; its effect fires epidemic_events.0100 / natural_disaster.0110, whose one option costs the prestige
    "isolate_family_decision": ("-350 prestige and -50 legitimacy (epidemic_events.0100), then isolating_modifier for "
                                "5 years: no monthly prestige, diplomacy -3, legitimacy loss +30% (06_ce1_modifiers.txt)"),
    # The two stress-relief decisions that pay gold: no cost block, so the gold rule cannot see them; each fires its
    # on_action's event, whose one option takes medium_gold_value (6 months of income, at least 50), and the game's
    # AI takes it whenever stress is 50 or more, every 3 years
    "stress_loss_improvident_decision": ("medium_gold_value gold, 6 months of income, at least 50 "
                                         "(stress_trait_coping_decisions.4501, by its on_action), every 3 years"),
    "stress_loss_profligate_decision": ("medium_gold_value gold, 6 months of income, at least 50 "
                                        "(stress_trait_coping_decisions.4001, by its on_action), every 3 years"),
}
SPENDING_DECISIONS = ("the decisions whose cost block names gold, and those that cost income, prestige or gold with no "
                      "cost block: " + ", ".join(SPENDING_DECISION_KEYS) + "; the stress relief that pays nothing "
                      "still passes")
#: The kinds the preset leaves out, and why (the reply and the news say it)
AI_VETO_LEFT_OUT = {
    "ally_call": "drops the AI's calls in your defensive wars too, which cost nothing (a defender's call has "
                 "prestige_if_joined 0); it saves prestige only in wars you attack",
    "perk": "spends lifestyle points, not gold or prestige; held points wait for do lifestyle perk=<key>",
    # A war is not spending; in a conquest goal the AI's own wars may be welcome
    "war": "drops the wars your ruler's AI declares on its own; your own declare_war is never dropped",
    # A faction costs nothing; a faction he leads declares its war as his (the Lorraine goal)
    "faction": "drops your ruler's AI joining or starting a faction; your own do faction is never dropped",
    # (an ally's two overseas wars, 1076-79 and 1082-85, joined by the AI, drained the purse)
    "call_in": "drops every call to war another ruler sends yours (an ally's, a kinsman's), defensive ones too; "
               "joined, the war's raised armies cost upkeep (news call_in names each call)",
}
#: vassal_offer is ANOTHER ruler's offer of vassalage to yours (offer_vassalization_interaction has no
#: reply delay: your ruler's AI answers inside the sender's send, so no event reaches the seat); the veto
#: drops the offer before it is answered
VASSAL_OFFER_SENT_BY = ("offered by another ruler's AI; your ruler's AI answered it at once (no letter, no "
                        "event); set the veto and the offer never reaches him")
#: The decision veto's key list (bridge/include/ai_veto.h kMaxDecisionKeys, key characters)
DECISION_KEYS_MAX = 8
_DECISION_KEY_RE = re.compile(r"^[a-z0-9_]{1,63}$")
#: attempts shown by op=list (the bridge keeps 128 for every watched ruler)
LIST_ATTEMPTS = 10
#: what the seat does instead, per kind (the toolkit's own calls)
AI_VETO_YOURSELF = {
    "create_title": lambda key: {"tool": "do", "args": {"intent": "mint_title", "args": {"title_key": key}}},
    "law": lambda key: {"tool": "do", "args": {"intent": "change_law", "args": {"law": key}}},
    "perk": lambda key: {"tool": "do", "args": {"intent": "lifestyle", "args": {"perk": key}}},
    "decision": lambda key: {"tool": "do", "args": {"intent": "take_decision", "args": {"decision": key}}},
    "men_at_arms": lambda key: {"tool": "do", "args": {"intent": "men_at_arms", "args": {"action": "recruit",
                                                                                         "maa_type": key}}},
    # The three types host_activity puts to the AI (activities.ACTIVITIES); another type has no call
    "activity": lambda key: ({"tool": "do", "args": {"intent": "host_activity", "args": {"activity": key[9:]}}}
                             if key in ("activity_feast", "activity_hunt", "activity_pilgrimage") else None),
}
#: The bridge's cap on spared activity types per ruler (ai_veto.h kMaxActivitySpares)
ACTIVITY_SPARES_MAX = 4
ACTIVITY_SPARED = ("the activity veto lets {key} through: you put it to your ruler's AI (host_activity goes through "
                   "the same pass the veto watches); every other activity its AI starts is dropped")


async def activity_spare_params(gw, me: int, add: Optional[str] = None) -> dict:
    """The activity types the activity veto spares now (read back), with `add`, as set_ai_veto's spare_keys; {}
    when none. A set of the activity kind without them would forget the seat's own host_activity."""
    R = _R()
    cur = _veto_ruler(_d(R._unwrap_reply(await gw.read("query_ai_veto", {"character_id": me}))), me) or {}
    keys = [k for k in cur.get("activity_spare_keys") or [] if isinstance(k, str) and k]
    if add and add not in keys:
        keys = (keys + [add])[-ACTIVITY_SPARES_MAX:]
    return {"spare_keys": ",".join(keys)} if keys else {}


def call_ally_call(war_id, ally_id) -> dict:
    """The seat's own call of an ally the AI's call was dropped for."""
    return {"tool": "do", "args": {"intent": "call_ally", "args": {"war_id": war_id, "ally_id": ally_id}}}
AI_VETO_APPLIES = ("from now on, every time the game's AI sends this for your ruler, the bridge drops it before "
                   "the game runs it (as the game drops a refused command; nothing is paid). You can still do it "
                   "yourself: do mint_title for a title, do change_law for a law, do call_ally for an ally (your "
                   "own call is never dropped), do lifestyle perk=<key> for a perk, do take_decision for a "
                   "decision, do men_at_arms for a regiment, declare_war for a war. news ai_vetoed lists each "
                   "attempt with the price and "
                   "your gold then")
#: What a decision veto without keys also drops (the game's files: 14 stress_loss_* decisions, no cost)
DECISION_ALL_NOTE = ("no keys: every decision of the game's AI for your ruler is dropped, the free stress-relief "
                     "ones (stress_loss_*) too; keys=[...] drops only those")
#: What a dropped perk pick leaves (re_lifestyle_perks.md §3: the AI spends a point some weeks after it appears: live)
PERK_VETO_NOTE = ("the game's AI spends a perk point some weeks after it appears; with this veto the points stay "
                  "for you: do lifestyle perk=<key> (query lifestyle lists unlockable_now)")
#: What a dropped legacy leaves (no call of the toolkit buys one)
LEGACY_VETO_NOTE = ("the renown stays with your dynasty (disinherit, denounce and legitimize are paid from it); no "
                    "call buys a legacy yourself: do ai_veto op=clear what=legacy lets the game's AI buy its pick")
#: What the bridge does not price for these two kinds (the news prices a hired company
#: from its own read where it can: mercenary_price_estimate)
ACTED_PRICE_NOTE = {
    "mercenary": "the bridge does not read a hire's price; gold_at_attempt is your gold when the AI sent it",
    "construction": "the bridge does not read a building's price; gold_at_attempt is your gold when the AI sent it",
    "men_at_arms": "the regiment type was not read; gold_at_attempt is your gold when the AI sent it",
    "war": "the casus belli the AI picked is not read; target_id is the ruler it declared on",
    "faction": "nothing is paid; a faction he leads declares its war as his",
    "activity": ("the game charges an activity's gold when it starts; the purse reads it from gold_at_attempt to the "
                 "next read (in_this_step)"),
}
#: A court type's price is its cost block in the game's files (1.18.3.1: prestige add = 500 for each
#: of the seven types); the bridge names the type, the toolkit reads the price
COURT_TYPES_FILE = ("common", "court_types", "00_court_types.txt")


def court_type_price(key: Any) -> Optional[dict]:
    """{"prestige": N} from the court type's `cost = { prestige = { add = N } }` in the game's files; None unread."""
    root = game_dir()
    p = root.joinpath(*COURT_TYPES_FILE) if root else None
    if not isinstance(key, str) or not key or p is None or not p.is_file():
        return None
    text = p.read_text(encoding="utf-8-sig", errors="replace")
    m = re.search(rf"(?m)^{re.escape(key)}\s*=\s*\{{", text)
    if not m:
        return None
    nxt = re.search(r"(?m)^[a-z_]+\s*=\s*\{", text[m.end():])
    body = text[m.end(): m.end() + nxt.start()] if nxt else text[m.end():]
    c = re.search(r"cost\s*=\s*\{\s*prestige\s*=\s*\{\s*add\s*=\s*(-?\d+(?:\.\d+)?)", body)
    return {"prestige": float(c.group(1))} if c else None
#: (0869.05.07: the spending veto dropped the AI's creation of k_sweden at 152.73 gold, the title the
#: task counts when the game's AI creates it for the seat): the goal title, named by whoever starts this server (a
#: harness, from its task's target title), is never dropped by the create_title veto: the bridge lets that one title's
#: creation through (set_ai_veto spare_title_id). Unset: no title is spared, as before.
GOAL_TITLE_ENV = "CK3_GOAL_TITLE"
GOAL_SPARED = ("the game's AI may still create {key}, your goal title, for you: the create_title veto never drops it "
               "(every other title's creation is dropped)")
#: (1071.05-1072.05: the AI's d_leinster dropped three times at 191, 191 and 169 gold, the duchy
#: k_ireland's tier rule needed; it was never bought): the titles the goal's creation still needs are spared too
CHAIN_SPARED = ("the game's AI may still create {key}, your goal title, and {keys}, which its creation still needs "
                "({rule}): the create_title veto never drops them (every other title's creation is dropped)")
CHAIN_NOT_SPARED = ("this bridge spares one title only: the AI's creation of {keys}, which {goal} still needs, is "
                    "dropped (do mint_title creates them yourself)")
CHAIN_DROPPED = ("{key} is a title {goal}, your goal title, still needs ({rule}); this bridge's veto dropped the "
                 "AI's creation of it: do mint_title creates it yourself")
GOAL_DROPPED = ("the game's AI tried to create {key}, your goal title, for you and the create_title veto dropped it "
                "(this bridge does not spare it); do mint_title creates it yourself")
GOAL_PASSED = "the game's AI for your ruler: {key} is your goal title, which the create_title veto never drops"
GOAL_NOT_SPARED = ("this bridge cannot spare {key}, your goal title: the create_title veto drops the AI's creation of "
                   "it too (do mint_title creates it yourself)")


def goal_title() -> Optional[str]:
    """The goal title's key (CK3_GOAL_TITLE), or None."""
    k = (os.environ.get(GOAL_TITLE_ENV) or "").strip()
    return k if re.fullmatch(r"[a-z]_[a-z0-9_]+", k) else None


#: The bridge's cap on spared titles per ruler (ai_veto.h kMaxSpareTitles): the goal and 7 of its chain
SPARE_TITLES_MAX = 8
#: The last chain this server spared, per ruler id ({"key", "ids"}): the news re-sends it when it changes
SPARED_CHAIN: dict = {}
#: Every title this server has spared as the goal's chain, per ruler id (a post that passed while spared is
#: named so even after the chain was sent again: the creation itself meets the rule and empties the chain)
SPARED_EVER: dict = {}
CHAIN_PASSED = ("the game's AI for your ruler: {key} is a title {goal}, your goal title, needed (the create_title veto "
                "spared it)")


async def _goal_spare(gw, me: int = 0) -> Optional[dict]:
    """{key, title_id} of the goal title to spare, {key, unread} when its id did not read, None when none.
    With `chain` [{title_key, title_id}]: the titles the goal's creation still needs (redesign.goal_chain:
    the game's own tier rule, the goal's de-jure children of that tier not held), and `rule` its words."""
    key = goal_title()
    if key is None:
        return None
    import title_refs as _trefs  # noqa: PLC0415
    tid, _row, blk = await _trefs.resolve(gw, key)
    if not tid or blk is not None:
        return {"key": key, "unread": "its title id did not read"}
    out: dict = {"key": key, "title_id": tid, "chain": []}
    if me:
        ch = await _R().goal_chain(gw, me, key)
        out["chain"] = [{"title_key": t["title_key"], "title_id": t["title_id"]}
                        for t in ch.get("titles") or []][:SPARE_TITLES_MAX - 1]
        if ch.get("option"):
            o = ch["option"]
            out["rule"] = f"the game's tier rule asks {o['need']} {o['tier']} titles, you hold {o['have']}"
        if ch.get("unread"):
            out["chain_unread"] = ch["unread"]
    return out


def spare_params(spare: Optional[dict]) -> dict:
    """The set_ai_veto params that spare the goal title and its chain ({} when nothing is spared)."""
    if not (spare or {}).get("title_id"):
        return {}
    ids = [t["title_id"] for t in spare.get("chain") or [] if t.get("title_id")]
    return {"spare_title_id": spare["title_id"], **({"spare_title_ids": ",".join(str(i) for i in ids)} if ids else {})}


def _goal_spare_result(spare: Optional[dict], ruler: Optional[dict], me: int = 0) -> Optional[dict]:
    """The reply's words on the goal title, from the readback (spare_title_id); and its chain
    (spare_title_ids)."""
    if spare is None:
        return None
    got = (ruler or {}).get("spare_title_id")
    if not (spare.get("title_id") and got == spare["title_id"]):
        return {"title_key": spare["key"], "spared": False, "why": GOAL_NOT_SPARED.format(key=spare["key"]),
                **({"unread": spare["unread"]} if spare.get("unread") else {})}
    out = {"title_key": spare["key"], "title_id": got, "spared": True, "why": GOAL_SPARED.format(key=spare["key"])}
    chain = spare.get("chain") or []
    if chain:
        ids = (ruler or {}).get("spare_title_ids")
        back = set(ids) if isinstance(ids, list) else set()
        yes = [t["title_key"] for t in chain if t["title_id"] in back]
        no = [t["title_key"] for t in chain if t["title_id"] not in back]
        out["chain"] = {"spared": yes, **({"not_spared": no} if no else {})}
        if yes:
            out["why"] = CHAIN_SPARED.format(key=spare["key"], keys=", ".join(yes), rule=spare.get("rule") or "its tier rule")
        if no:
            out["chain"]["why"] = CHAIN_NOT_SPARED.format(keys=", ".join(no), goal=spare["key"])
        if me and yes:
            SPARED_CHAIN[me] = {"key": spare["key"], "ids": sorted(t["title_id"] for t in chain if t["title_id"] in back),
                                "keys": yes, "rule": spare.get("rule")}
            SPARED_EVER.setdefault(me, set()).update(yes)
    elif me:
        SPARED_CHAIN[me] = {"key": spare["key"], "ids": [], "keys": [], "rule": None}
    if spare.get("chain_unread"):
        out["chain_unread"] = spare["chain_unread"]
    return out


def chain_title_of(key: Any, me: Any) -> Optional[dict]:
    """The spared-chain record when `key` is a title the goal's creation still needs (as last spared)."""
    c = SPARED_CHAIN.get(me) if me else None
    return c if c and isinstance(key, str) and key in (c.get("keys") or []) else None


RESERVE_MAX = 100000      # script_templates.h kReserveMax
RESERVE_NOTE = ("income fills the reserve first, up to the maximum; the AI's budget-aware spending "
                "sees the other buckets. Forced charges (an event option's price, a title's price, "
                "upkeep) still take reserved gold, last. Not a stop on the AI's purchases (it can hire a "
                "company while the reserve is set above the gold held); do ai_veto what=spending drops them")


def _R():
    import redesign  # noqa: PLC0415 — redesign imports this module
    return redesign


def _d(x) -> dict:
    return x if isinstance(x, dict) else {}


def _int(v) -> Optional[int]:
    if isinstance(v, bool):
        return None
    if isinstance(v, int):
        return v
    if isinstance(v, str) and v.strip().lstrip("-").isdigit():
        return int(v.strip())
    return None


# ── the game's event files ────────────────────────────────────────────────────
def game_dir() -> Optional[Path]:
    """The game's `game/` folder: CK3_GAME_DIR, else the Steam default; None when
    neither has an events folder."""
    for p in (os.environ.get("CK3_GAME_DIR"),
              str(Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")):
        if p and (Path(p) / "events").is_dir():
            return Path(p)
    return None


_EVENT_CACHE: dict = {}


def event_in_files(event: str) -> tuple[Optional[bool], str]:
    """(True, file) when an events/*.txt defines `event = {`; (False, "") when the
    files were read and none does; (None, why) when they could not be read."""
    if event in _EVENT_CACHE:
        return _EVENT_CACHE[event]
    root = game_dir()
    if root is None:
        return None, "the game's event files are not readable here (set CK3_GAME_DIR to the game/ folder)"
    pat = re.compile(r"^\s*" + re.escape(event) + r"\s*=\s*\{", re.M)
    try:
        for f in sorted((root / "events").rglob("*.txt")):
            if pat.search(f.read_text(encoding="utf-8-sig", errors="replace")):
                _EVENT_CACHE[event] = (True, str(f.relative_to(root)))
                return _EVENT_CACHE[event]
    except OSError as e:
        return None, f"the game's event files could not be read ({type(e).__name__})"
    _EVENT_CACHE[event] = (False, "")
    return _EVENT_CACHE[event]


# ── reply shapes ──────────────────────────────────────────────────────────────
def _opt(ref: Any) -> Any:
    """{index, key} -> the key, else the index; None stays None."""
    if not isinstance(ref, dict):
        return None
    return ref.get("key") if ref.get("key") else ref.get("index")


def pick_row(p: dict) -> dict:
    """One recorded pick as the seat reads it, with the call that sets a rule for it."""
    row = {"date": p.get("date"), "event": p.get("event"), "options": p.get("options") or [],
           "ai_would_pick": _opt(p.get("ai_pick")), "applied": _opt(p.get("applied")),
           "rule": p.get("rule")}
    if p.get("rule"):
        row["why"] = p.get("why")
        if "covered" in p:
            row["covered"] = p.get("covered")
        by = p.get("answered_by")
        if by in ("ai", "event_rule_else"):
            # The rule's option was not taken; say which was
            # wanted, why not, and what was applied by whom (never under the rule's name)
            row["rule_wanted"] = _opt(p.get("wanted"))
            row["applied_by"] = ("the game's AI (your rule's else: ai)" if by == "ai"
                                 else "your rule's else option")
        if p.get("covered") is False and str(p.get("rule") or "").startswith("pay_if") \
                and _FLOORED.get((p.get("character_id"), p.get("event"))) is not None:
            # An always-pay rule declined the letter at fire time: the gold did not cover it
            row["payment_refused"] = (f"your always-pay rule did not pay: the gold "
                                      f"({round(float(p['gold']), 2) if isinstance(p.get('gold'), (int, float)) else 'unread'})"
                                      f" was below the letter's {_FLOORED[(p.get('character_id'), p.get('event'))]}, so "
                                      "it took the free option instead of paying into debt; args.debt=true pays at "
                                      "any gold")
    if p.get("by_faction_demand"):
        row["by"] = "faction_demand_response"
    if isinstance(p.get("gold"), (int, float)):
        row["gold_at_pick"] = round(float(p["gold"]), 2)
    cheaper = _priced_pick(row)
    if not p.get("rule") and p.get("event") and len(row["options"]) >= 2:   # one option: nothing to choose
        # (1079.11.13: the populist letter's call named its first option, an
        # acceptance): a faction's demand letter names its refuse option, and says what it does
        import faction_letters as FL  # noqa: PLC0415 — faction_letters imports this module
        refuse = [i for i, k in enumerate(row["options"]) if FL.is_refuse_key(k)]
        first = refuse[0] if len(refuse) == 1 else 0
        row["next"] = {"tool": "do", "args": {"intent": "event_rule", "args": {
            "op": "set", "event": p["event"], "answer": "option",
            "option": (row["options"][first] or first)}}}
        if len(refuse) == 1:
            row["refusing"] = FL.REFUSE_MEANS
        elif family_of(p["event"]) in GUARD_FAMILIES and isinstance(row.get("applied"), str) \
                and option_pays_gold(p["event"], row["applied"]):
            # (a seated game: three stress events, each a new key, each answered by the game's AI with the
            # option that takes gold; the call offered set the event's first option, which reveals the ruler's secrets):
            # an answer that took gold is said so, and the call guards the event's whole family before the next fires
            row["applied_takes_gold"] = True
            gc = guard_call(p["event"])
            if gc is not None:
                row["next"] = gc
                row["next_does"] = ("one rule on each event of " + str(family_of(p["event"])) + " whose options "
                                    "take gold, before the next fires: " + FAMILY_RULE)
        elif cheaper is not None:
            # The rule that
            # takes the option costing the least prestige next time this event fires
            row["next"]["args"]["args"]["option"] = cheaper
    return row


def rule_answer_row(row: dict) -> dict:
    """A pick_row a rule of yours decided, as the news' short row: who answered (your rule; its else option; the
    game's AI when the rule's else is ai) and with which option."""
    out = {"date": row.get("date"), "event": row.get("event"), "applied": row.get("applied")}
    if row.get("rule_wanted") is None:
        out["by"] = "your rule"
    else:
        out["by"] = row.get("applied_by") or "the game's AI"
        out["rule_wanted"] = row.get("rule_wanted")
        out["why"] = row.get("why")
    return out


def _priced_pick(row: dict) -> Optional[str]:
    """The prestige the applied option gives or takes (from the event's file), and, when it
    takes prestige, the option of the same event that takes the least (None when no other is cheaper or
    the files do not say). Unconditional amounts only: an amount inside a condition is said as such."""
    import event_prices as _ep  # noqa: PLC0415
    ev, applied, opts = row.get("event"), row.get("applied"), row.get("options") or []
    if not isinstance(ev, str) or not isinstance(applied, str):
        return None
    p = _ep.option_prestige(ev, applied)
    if not p or p.get("unread"):
        return None
    if isinstance(p.get("amount"), (int, float)):
        row["prestige"] = p["amount"]
    if p.get("if_conditions_held") is not None:
        row["prestige_if_its_conditions_held"] = p["if_conditions_held"]
    if not (isinstance(p.get("amount"), (int, float)) and p["amount"] < 0):
        return None
    best, best_amount = None, p["amount"]
    for o in opts:
        if not isinstance(o, str) or o == applied:
            continue
        q = _ep.option_prestige(ev, o)
        if q is not None and q.get("unread"):
            continue
        amount = (q or {}).get("amount", 0) if not (q or {}).get("if_conditions_held") else None
        if isinstance(amount, (int, float)) and amount > best_amount:
            best, best_amount = o, amount
    if best is not None:
        row["cheaper_option"] = {"option": best, "prestige": best_amount,
                                 "basis": ("add_prestige in each option of the event's file (unconditional); prestige only: "
                                          "its gold and other effects are not compared")}
    return best


def _rules_of(raw: dict, me: int) -> Optional[list]:
    for s in raw.get("seats") or []:
        if isinstance(s, dict) and s.get("character_id") == me:
            return [r for r in s.get("rules") or [] if isinstance(r, dict)]
    return None


def _picks_of(raw: dict, me: int) -> list:
    return [p for p in raw.get("picks") or [] if isinstance(p, dict) and p.get("character_id") == me]


#: (a gold rule on court_chaplain_task.0301 left its 3-option variant 0302 to the AI, which paid
#: 90 gold at 48.85 on 1078.11.06): events that are one letter in several variants. A rule set on one is set on each,
#: the option matched by what it does (option_effects), never by its index
EVENT_VARIANTS = (("court_chaplain_task.0301", "court_chaplain_task.0302"),)
#: an option's statements that are not what it does (its name, the AI's weights, tooltips, conditional branches)
_NOT_EFFECTS = frozenset({"name", "ai_chance", "custom_tooltip", "trigger", "show_as_tooltip", "flavor",
                          "stress_impact", "exclusive", "highlight_portrait", "clicksound", "fallback",
                          "show_as_unavailable", "if", "else", "else_if", "random", "random_list", "hidden_effect",
                          "trigger_event"})


def option_effects(event: str) -> Optional[list]:
    """[(option key, its unconditional effects as a sorted tuple)] in file order; None when the files cannot say.
    An option with no effect (a decline) has an empty tuple."""
    import faction_letters as FL  # noqa: PLC0415
    found, where = event_in_files(event)
    root = game_dir()
    if not found or root is None:
        return None
    try:
        tree = FL._parsed(root / where)
    except OSError:
        return None
    for k, v in tree:
        if k == event and isinstance(v, list):
            return [(FL._name_key(o), tuple(sorted((kk, FL._flat(vv) if isinstance(vv, list) else str(vv))
                                                   for kk, vv in o if kk not in _NOT_EFFECTS)))
                    for k2, o in v if k2 == "option" and isinstance(o, list)]
    return None


#: (a test run, 1073.05.27: pay_if gold>=87 on court_chaplain_task.0302 with else=0302.b, the duchy claim
#: letter, accepted with no warning; it paid 87 at 27.53 gold and 87 at -91.79): a statement in an option's body that
#: takes gold. The whole body is read (its `if` branches and hidden effects too): an option that pays only sometimes
#: still counts as paying
_PAYS_GOLD = re.compile(r"(?:remove|pay|charge|spend)\w*gold|\badd_gold\s*=\s*(?:-|\{[^{}]*?(?:multiply\s*=\s*-|"
                        r"subtract\s*=|value\s*=\s*-))")


def option_bodies(event: str) -> Optional[list]:
    """[(option key, its whole body as script text)] in file order; None when the files cannot say."""
    import faction_letters as FL  # noqa: PLC0415
    found, where = event_in_files(event)
    root = game_dir()
    if not found or root is None:
        return None
    try:
        tree = FL._parsed(root / where)
    except OSError:
        return None
    for k, v in tree:
        if k == event and isinstance(v, list):
            import event_prices as _ep  # noqa: PLC0415
            return [(_ep.option_key(o), FL._flat(o)) for k2, o in v if k2 == "option" and isinstance(o, list)]
    return None


def option_pays_gold(event: str, option: Any) -> Optional[bool]:
    """Whether `option` (its key, or its index) of `event` takes gold anywhere in its body; None when the files
    cannot say or the event has no such option."""
    bodies = option_bodies(event)
    if bodies is None:
        return None
    oi = _int(option)
    if oi is not None:
        body = bodies[oi][1] if 0 <= oi < len(bodies) else None
    else:
        body = next((b for k, b in bodies if k == option), None)
    return None if body is None else bool(_PAYS_GOLD.search(body))


def free_option(event: str) -> Optional[str]:
    """The one option of `event` that does nothing (pays nothing, takes nothing: a decline), else None."""
    eff = option_effects(event) or []
    free = [k for k, e in eff if not e and k]
    return free[0] if len(free) == 1 else None


def same_effect_option(event: str, option: Any, other: str) -> Optional[str]:
    """The option of `other` that does what `option` of `event` does (its unconditional effects), else None."""
    mine = dict(option_effects(event) or [])
    if not isinstance(option, str) or option not in mine:
        return None
    hits = [k for k, e in option_effects(other) or [] if e == mine[option] and k]
    return hits[0] if len(hits) == 1 else None


def variants_of(event: str) -> tuple:
    return next((tuple(e for e in fam if e != event) for fam in EVENT_VARIANTS if event in fam), ())


def variant_rule(event: str, other: str, rule: dict) -> Optional[dict]:
    """The rule `rule` (set_event_rule's params for `event`) as it reads on the variant `other`, its options
    matched by effect; None when an option has no match there."""
    out = {k: v for k, v in rule.items() if k not in ("event", "option", "option_index", "else")}
    out["event"] = other
    ans = rule.get("answer")
    if ans == "decline":
        # the other option of a two-option letter is its one free option: on the variant, that option by name
        fo = free_option(other)
        named = rule.get("option")
        if fo is None or (named is not None and named == free_option(event)):
            return None
        return dict(out, answer="option", option=fo)
    opt = rule.get("option")
    if opt is None and "option_index" in rule:
        eff = option_effects(event) or []
        oi = rule["option_index"]
        opt = eff[oi][0] if isinstance(oi, int) and 0 <= oi < len(eff) else None
    if opt is None and ans == "pay_if":
        eff = option_effects(event) or []
        opt = eff[0][0] if eff else None
    m = same_effect_option(event, opt, other)
    if m is None:
        return None
    out["option"] = m
    if ans == "pay_if":
        els = rule.get("else")
        e2 = same_effect_option(event, els, other) if els else free_option(other)
        if e2 is None:
            return None
        if len(option_effects(other) or []) != 2 or els:
            out["else"] = e2
    elif rule.get("else"):
        e2 = same_effect_option(event, rule["else"], other)
        if e2:
            out["else"] = e2
    return out


#: What an always-pay claim-letter rule is set as, and why
ALWAYS_PAY_FLOOR = ("an always-pay letter rule pays only while the gold covers the letter: set as pay_if at the "
                    "letter's price, the free option when the gold is short (never into debt); args.debt=true "
                    "pays at any gold")
DECLINE_AS_FREE = "decline on a letter of three or more options: its one option that does nothing"
#: (ruler, event) -> the amount an always-pay rule was floored at (pick_row says so when it declines)
_FLOORED: dict = {}


async def _pays_claim_letter(event: str, option: Any) -> bool:
    """`option` of a chaplain's claim letter takes gold (its body, by the event file; the files unread, the
    first option, as letter_cost_warning reads it)."""
    import news as _N  # noqa: PLC0415
    if event not in _N.CLAIM_LETTERS:
        return False
    fp = option_pays_gold(event, option)
    return fp if fp is not None else ((isinstance(option, str) and option.endswith(".a")) or _int(option) == 0)


# ── a guard over an event family (event="<namespace>.*") ──────────────────────
#: The bridge answers by event key only, so a rule protects gold only after its event has fired once and been
#: named. A family rule sets, before any of them fires, one rule on each event of a namespace whose options include
#: one that takes gold and one that does not (in a seated game the stress events stress_threshold.1001, .2311 and
#: .1721 each fired once, each a new key, and the game's AI answered each with the option that pays gold)
FAMILY_RE = re.compile(r"^([a-z][a-z0-9_]*)\.\*$")
#: the bridge's rule table holds 32 rules a ruler (event_rules.h kMaxRulesPerChar)
RULES_MAX = 32
FAMILY_ANSWERS = ("decline",)
#: The families whose gold-paying answer the news meets with the family guard's call: the stress events, whose
#: gold options buy nothing but a coping trait (any namespace can be guarded by the call itself)
GUARD_FAMILIES = ("stress_threshold.*",)
FAMILY_RULE = ("on each event of the family with an option that takes gold and one that does not: the option that "
               "does not with the highest base weight the game's AI gives it (its file's ai_chance base), else the "
               "event's option that is always offered and takes no gold; when no free option is always offered, "
               "else the free one offered unless a condition holds (its trigger a NOT or NAND), and failing that "
               "the free one of the next highest weight. An option is named only by a key no other option of the "
               "event shares. An event with no option taking gold gets no rule, and a rule of yours already on an "
               "event is kept. The rule replaces the AI's pick on every firing, also when it would not have paid: "
               "the bridge answers by event, not by what an option costs")
FAMILY_NOT_COVERED = ("an event the game's AI answers when neither named option is offered (the AI's pick then "
                      "stands, gold or not): `exposed` names each event where that can happen")
#: what one rule can name (bridge/include/event_rules.h Rule: option_key and one else_key)
RULE_HOLDS = ("a rule names one option and one else (the bridge's rule format holds no longer list): an event's "
              "other free options cannot be named after them")
#: why an event of the family gets no rule (guard_analysis)
NOT_GUARDED_WHY = {
    "no_options": "the event has no option: it asks nothing",
    "no_gold_option": "no option takes gold",
    "every_option_takes_gold": "every option takes gold: no free option to name",
    "no_free_option_of_its_own_key": "its free options share their key with another option, so none can be named",
    "keys_unread": "an option's key is not readable in the file",
}
EXPOSED_WHEN = ("neither the rule's option nor its else is offered: the game's AI picks, and these options take "
                "gold")
_NAMESPACE_EVENTS: dict = {}


def namespace_events(ns: str) -> Optional[list]:
    """The events of namespace `ns` the game's events/ files define (`ns.<id> = {`), in file order; None when the
    files are not readable."""
    if ns in _NAMESPACE_EVENTS:
        return _NAMESPACE_EVENTS[ns]
    root = game_dir()
    if root is None:
        return None
    pat = re.compile(r"^(" + re.escape(ns) + r"\.[a-z0-9_]+)\s*=\s*\{", re.M)
    out: list = []
    try:
        for f in sorted((root / "events").rglob("*.txt")):
            for m in pat.finditer(f.read_text(encoding="utf-8-sig", errors="replace")):
                if m.group(1) not in out:
                    out.append(m.group(1))
                    _EVENT_CACHE.setdefault(m.group(1), (True, str(f.relative_to(root))))
    except OSError:
        return None
    _NAMESPACE_EVENTS[ns] = out
    return out


def _ai_base(opt: list) -> float:
    """The `base` of an option's ai_chance (`ai_chance = N` or `{ base = N ... }`); 0 when it states none."""
    for k, v in opt:
        if k == "ai_chance":
            if isinstance(v, str):
                return float(v) if re.fullmatch(r"-?\d+(?:\.\d+)?", v) else 0.0
            b = next((x for kk, x in v if kk == "base" and isinstance(x, str)), None) if isinstance(v, list) else None
            return float(b) if b and re.fullmatch(r"-?\d+(?:\.\d+)?", b) else 0.0
    return 0.0


def offered_when(opt: list) -> str:
    """When the game offers an option, by its own `trigger`: "always" (none), "unless" (every statement of it a
    NOT or a NAND: offered unless a condition holds), "if" (offered only when its condition holds)."""
    trig = [v for k, v in opt if k == "trigger"]
    if not trig:
        return "always"
    t = trig[0]
    if isinstance(t, list) and t and all(k in ("NOT", "NAND") for k, _v in t):
        return "unless"
    return "if"


def guard_analysis(event: str) -> tuple:
    """(choice, why): choice {event, option, else?, takes_gold, exposed?} when the event has an option that takes
    gold (its whole body, option_pays_gold) and a free one with a key of its own; else (None, a NOT_GUARDED_WHY
    code), or (None, None) when the files cannot say."""
    import event_prices as _ep  # noqa: PLC0415
    import faction_letters as FL  # noqa: PLC0415
    found, where = event_in_files(event)
    root = game_dir()
    if not found or root is None:
        return None, None
    try:
        tree = FL._parsed(root / where)
    except OSError:
        return None, None
    body = next((v for k, v in tree if k == event and isinstance(v, list)), None)
    opts = [v for k, v in body or [] if k == "option" and isinstance(v, list)]
    if not opts:
        return None, "no_options"
    keys = [_ep.option_key(o) for o in opts]
    if None in keys:
        return None, "keys_unread"
    pays = [bool(_PAYS_GOLD.search(FL._flat(o))) for o in opts]
    if not any(pays):
        return None, "no_gold_option"
    if all(pays):
        return None, "every_option_takes_gold"
    # The bridge takes the first option whose key matches (event_rules.h decide): a key two options share names
    # only the first, so a free option is named only by a key of its own
    free = [i for i in range(len(opts)) if not pays[i] and keys.count(keys[i]) == 1]
    if not free:
        return None, "no_free_option_of_its_own_key"
    when = {i: offered_when(opts[i]) for i in free}
    by_weight = sorted(free, key=lambda i: (-_ai_base(opts[i]), i))
    always = [i for i in free if when[i] == "always"]
    unless = [i for i in by_weight if when[i] == "unless"]
    fallback = always[0] if always else (unless[0] if unless else None)
    rest = [i for i in by_weight if i != fallback]
    best = rest[0] if rest else fallback
    els = fallback if fallback is not None and fallback != best else (rest[1] if len(rest) > 1 else None)
    out = {"event": event, "option": keys[best], "takes_gold": [k for k, p in zip(keys, pays) if p]}
    if els is not None:
        out["else"] = keys[els]
    if not any(when[i] == "always" for i in (best, els) if i is not None):
        # No named option is always offered: the AI's pick can stand, and the gold it can take is said
        ex: dict = {"when": EXPOSED_WHEN,
                    "named_offered": {keys[i]: (when[i] if when[i] == "always" else
                                                f"{when[i]}: {FL._flat(next(v for k, v in opts[i] if k == 'trigger'))[:160]}")
                                      for i in (best, els) if i is not None},
                    "gold_options": []}
        for i, p in enumerate(pays):
            if p:
                g = _ep.option_gold_taken(event, i)
                ex["gold_options"].append({"option": keys[i], "takes": (
                    g.get("gold") if isinstance((g or {}).get("gold"), (int, float)) else
                    (g or {}).get("rule") or (g or {}).get("unknown") or "gold, by a statement the file computes")})
        out["exposed"] = ex
    return out, None


def gold_guard_choice(event: str) -> Optional[dict]:
    """For one event: {event, option, else?, takes_gold: [keys], exposed?} (guard_analysis); None when no rule
    is set on it."""
    return guard_analysis(event)[0]


async def do_event_family(gw, a: dict, me: int, actor, ns: str, refuse) -> dict:
    """op=set answer=decline on `<ns>.*`: a gold guard on each event of the namespace (gold_guard_choice); op=clear:
    the rules this table holds for the namespace's events. Refused whole, nothing set, when the table cannot hold
    them."""
    R = _R()
    op = str(a.get("op") or "set")
    if op not in ("set", "clear"):
        return refuse("family_op", f"an event family ({ns}.*) takes op=set or op=clear", ["op"])
    events = namespace_events(ns)
    if events is None:
        return refuse("event_files_unread", "the game's event files are not readable here (set CK3_GAME_DIR to the "
                                            "game/ folder): a family rule is read from them", ["event"])
    if not events:
        return refuse("event_not_found", f"no file under the game's events/ defines an event of {ns}", ["event"])
    back = _d(R._unwrap_reply(await gw.read("query_event_rules", {"character_id": me})))
    # a ruler with no table yet holds no rule (the bridge makes his table at the first set)
    have = (_rules_of(back, me) or []) if back.get("success") is True else None
    if have is None:
        return R.envelope("do", ok=False, actor=actor, game=gw.game(),
                          blocked=R.why_blocked("read_timeout", summary="query_event_rules did not answer: the "
                                                                        "rules held now are needed first",
                                                raw_reason=str(back.get("reason") or "no_reply")),
                          bridge_health=gw.health)
    held = {str(r.get("event")) for r in have}
    if op == "clear":
        mine = [e for e in events if e in held]
        rows = []
        for e in mine:
            w = _d(R._unwrap_reply(await gw.write("set_event_rule", {"character_id": me, "op": "clear", "event": e})))
            rows.append({"event": e, "cleared": w.get("success") is True,
                         **({} if w.get("success") is True else {"refused": w.get("reason") or "no_reply"})})
        return R.envelope("do", verdict="applied", actor=actor, game=gw.game(), bridge_health=gw.health,
                          result={"intent": "event_rule", "op": "clear", "family": f"{ns}.*", "cleared": rows})
    if a.get("answer") not in FAMILY_ANSWERS or a.get("option") is not None or a.get("else") not in (None, "", "ai"):
        return refuse("family_answer", f"a family rule ({ns}.*) is answer=decline with no option and no else: it "
                                       "names, on each event, the option that takes no gold (" + FAMILY_RULE + ")",
                      ["answer"])
    analysed = [(e,) + guard_analysis(e) for e in events]
    choices = [c for _e, c, _w in analysed if c]
    unguarded: dict = {}
    for e, c, w in analysed:
        if c is None:
            unguarded.setdefault(w or "files_unread", []).append(e)
    new = [c for c in choices if c["event"] not in held]
    kept = [c["event"] for c in choices if c["event"] in held]
    if len(held) + len(new) > RULES_MAX:
        return refuse("too_many_rules", f"{len(new)} rules for {ns}.* beside the {len(held)} you hold: the table "
                                        f"holds {RULES_MAX} a ruler. Nothing was set; clear some (op=clear)", ["event"],
                      result={"intent": "event_rule", "family": f"{ns}.*", "would_set": len(new), "held": len(held)})
    rows = []
    for c in new:
        p = {"character_id": me, "op": "set", "event": c["event"], "answer": "option", "option": c["option"],
             **({"else": c["else"]} if c.get("else") else {})}
        w = _d(R._unwrap_reply(await gw.write("set_event_rule", p)))
        rows.append({"event": c["event"], "option": c["option"], **({"else": c["else"]} if c.get("else") else {}),
                     "takes_gold": c["takes_gold"], **({"exposed": c["exposed"]} if c.get("exposed") else {}),
                     "set": w.get("success") is True,
                     **({} if w.get("success") is True else {"refused": w.get("reason") or "no_reply"})})
    back = _d(R._unwrap_reply(await gw.read("query_event_rules", {"character_id": me})))
    now = _rules_of(back, me) if back.get("success") is True else None
    if now is not None:
        got = {str(r.get("event")) for r in now}
        for r in rows:
            r["read_back"] = r["event"] in got
    result = {"intent": "event_rule", "op": "set", "family": f"{ns}.*", "events_in_files": len(events),
              "guarded": rows, "rule": FAMILY_RULE, "rule_holds": RULE_HOLDS, "not_covered": FAMILY_NOT_COVERED,
              "exposed": [c["event"] for c in choices if c.get("exposed")],
              **({"kept_yours": kept} if kept else {}),
              "no_gold_option": len(unguarded.get("no_gold_option") or []),
              "not_guarded": {"count": len(events) - len(choices), "of": len(events),
                              "events": {w: {"why": NOT_GUARDED_WHY.get(w, "the event's file was not readable"),
                                             "events": es} for w, es in unguarded.items()}},
              "readback": {"via": "query_event_rules", "read": now is not None}}
    bad = [r["event"] for r in rows if not r["set"] or r.get("read_back") is False]
    if bad:
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                          blocked=R.write_blocked(gate="write_not_applied", raw_reason="readback_disagrees",
                                                  summary=f"{len(bad)} of the family's rules did not hold: "
                                                          + ", ".join(bad[:4])),
                          bridge_health=gw.health)
    return R.envelope("do", verdict="applied", actor=actor, game=gw.game(), result=result, bridge_health=gw.health)


def family_of(event: Any) -> Optional[str]:
    """The family call's event for an event whose namespace has a gold guard to set (`stress_threshold.*`), else
    None."""
    if not isinstance(event, str) or "." not in event:
        return None
    return event.split(".", 1)[0] + ".*"


def guard_call(event: Any) -> Optional[dict]:
    """The call that sets the gold guard on `event`'s family, when the event's options include one that takes gold
    and one that does not; else None."""
    fam = family_of(event)
    if fam is None or gold_guard_choice(event) is None:
        return None
    return {"tool": "do", "args": {"intent": "event_rule", "args": {"op": "set", "event": fam, "answer": "decline"}}}


#: What the spending preset leaves to the event rules, and the call that guards the family that pays gold most: a
#: stress event (stress_threshold.*) offers a coping trait that takes 50 gold or more (improvident, profligate:
#: medium_gold_value at the least, whose floor is medium_gold_min_value 50);
#: once the ruler has the trait the game's AI weighs that option +200 at every later one, and the trait's decision
#: (in the preset now) pays again every 3 years
SPENDING_EVENTS_ROW = {
    "not_covered": ("the game's AI answers your ruler's events itself: a veto drops a command, and an event's "
                    "option is no command. Each stress event (stress_threshold.*) can offer a coping trait that "
                    "takes 50 gold or more at once (improvident, profligate), and with that trait the AI picks it at "
                    "every later stress event"),
    "guard": {"tool": "do", "args": {"intent": "event_rule", "args": {"op": "set", "event": "stress_threshold.*",
                                                                     "answer": "decline"}}},
    "guard_does": "one rule on each stress event whose options take gold, before any fires: " + FAMILY_RULE,
}


VARIANT_UNMATCHED = "no option of this variant does what the rule's option does: not set; set it by its own key"
VARIANT_ELSE_PAYS = "on this variant the rule's else option pays gold: not set; set it with the free option as else"


# ── do(intent="event_rule") ───────────────────────────────────────────────────
async def do_event_rule(gw, a: dict, character_id: int, actor) -> dict:
    """op=set {event, answer, option, else, currency, amount}; op=clear {event}; op=watch
    (record every pick, no rule); op=list (the rules and the last picks)."""
    R = _R()
    me = character_id or R._as_int((actor or {}).get("id"))
    op = str(a.get("op") or ("set" if a.get("answer") else "list"))
    event = str(a.get("event") or "")
    answer = a.get("answer")
    option = a.get("option")
    currency = a.get("currency")
    amount = a.get("amount")
    else_ = a.get("else")    # "ai" (default) or a second option, when the rule's cannot be taken

    def refuse(code: str, summary: str, fields=None, result=None):
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                          blocked=R.write_blocked(gate=REFUSALS.get(code, "invalid_argument"), raw_reason=code,
                                                  summary=summary,
                                                  fix={"rejected_fields": fields} if fields else None),
                          bridge_health=gw.health)

    if op not in EVENT_RULE_OPS:
        return refuse("op_must_be_set_clear_watch_or_unwatch", f"args.op is one of {', '.join(EVENT_RULE_OPS)}",
                      ["op"])
    fam = FAMILY_RE.match(event)
    if fam and op in ("set", "clear"):
        # event="<namespace>.*": a gold guard on each of the namespace's events, set before any of them fires
        return await do_event_family(gw, a, me, actor, fam.group(1), refuse)
    if op == "list":
        raw = _d(R._unwrap_reply(await gw.read("query_event_rules", {"character_id": me})))
        if raw.get("success") is not True:
            return R.envelope("do", ok=False, actor=actor, game=gw.game(),
                              blocked=R.why_blocked("read_timeout", summary="query_event_rules did not answer",
                                                    raw_reason=str(raw.get("reason") or "no_reply")),
                              bridge_health=gw.health)
        rules = _rules_of(raw, me)
        picks = _picks_of(raw, me)
        return R.envelope("do", actor=actor, game=gw.game(), bridge_health=gw.health, result={
            "intent": "event_rule", "op": "list", "watched": rules is not None, "rules": rules or [],
            "picks": [pick_row(p) for p in picks[-LIST_PICKS:]], "picks_total": len(picks),
            "hook_installed": raw.get("hook_installed"),
            **({"disabled_by_env": True} if raw.get("disabled_by_env") else {})})
    params: dict = {"character_id": me, "op": op}
    checked = None
    floored_at = None
    if op in ("set", "clear"):
        if not event:
            return refuse("event_required", "args.event is the event's key (court_chaplain_task.0301)", ["event"])
        params["event"] = event
    if op == "set":
        if answer not in EVENT_ANSWERS:
            return refuse("answer_must_be_option_decline_or_pay_if",
                          f"args.answer is one of {', '.join(EVENT_ANSWERS)}", ["answer"])
        params["answer"] = answer
        oi = _int(option)
        if oi is not None:
            params["option_index"] = oi
        elif isinstance(option, str) and option:
            params["option"] = option
        elif option is not None:
            return refuse("bad_option_key", "args.option is the option's key or its index (0 = the first)",
                          ["option"])
        if answer == "pay_if":
            if currency is not None:
                params["currency"] = currency
            if _int(amount) is None:
                return refuse("amount_required", "pay_if needs args.amount (whole gold, prestige or piety)",
                              ["amount"])
            params["amount"] = _int(amount)
        if else_ not in (None, "", "ai"):
            if not isinstance(else_, str) or else_ == option:
                return refuse("bad_else_option_key", "args.else is 'ai' (the default) or another option's key",
                              ["else"])
            params["else"] = else_
        found, where = event_in_files(event)
        if found is False:
            return refuse("event_not_found", f"no file under the game's events/ defines {event}", ["event"])
        checked = {"in_game_files": found, ("file" if found else "why"): where}
        if found and option is not None:
            # (a seated game, step 16: a rule 0302 -> 0301.a answered "applied"; 0301.a is not an option of
            # 0302): an option the event's file does not have is refused, naming the event's options
            import faction_letters as FL  # noqa: PLC0415
            opts, _why = FL.letter_options(event)
            keys = [o.get("key") for o in opts or []]
            # a key is refused only when the file names every option (a stress event's options are named by
            # the game at fire time, stress_threshold.1731.e.journaller.gain: the file cannot say); an index
            # past the last option is refused whenever the options were read
            if opts is not None and (not (oi is not None and 0 <= oi < len(opts)) if oi is not None
                                     else (None not in keys and option not in keys)):
                return refuse("option_not_in_event",
                              f"{option} is not an option of {event}; its options: "
                              + ", ".join(f"{o['index']}={o.get('key')}" for o in opts) + ". Nothing was set",
                              ["option"], result={"intent": "event_rule", "options": opts})
        if found and isinstance(option, str) and option:
            # Whether the game offers the option, as far as its file says
            import faction_letters as FL  # noqa: PLC0415
            off = FL.option_offered(event, option)
            if off.get("offered") != "always":
                checked["option_offered"] = dict(off, rule=FL.OFFER_RULE)
        if found and else_ not in (None, "", "ai"):
            # The else is checked against the event's options whether or not the rule names its own option: an
            # else the event does not have never matches at fire time, and the game's AI then picks (a pay_if rule
            # with else=decline and no option let the AI pay a three-option claim letter)
            import faction_letters as FL  # noqa: PLC0415
            opts, _ = FL.letter_options(event)
            keys = [o.get("key") for o in opts or []]
            if opts is not None and None not in keys and else_ not in keys:
                fo = free_option(event)
                return refuse("bad_else_option_key",
                              f"{else_} is not an option of {event}, so it would never be taken and the game's AI "
                              "would pick instead; its options: "
                              + ", ".join(f"{o['index']}={o.get('key')}" for o in opts) + ". Nothing was set"
                              + (f"; the option that pays nothing: else={fo}" if fo else ""), ["else"],
                              result={"intent": "event_rule", "options": opts, **({"instead": {
                                  "tool": "do", "args": {"intent": "event_rule", "args": dict(
                                      {k: a[k] for k in a if k != "else"}, op="set", **{"else": fo})}}}
                                  if fo else {})})
        if found and answer == "option" and option is not None and a.get("debt") is not True \
                and await _pays_claim_letter(event, option):
            # (1090.11.06: an always-pay rule on 0301.a paid the 108-gold letter at 38.19 gold, -68
            # after; the tool had warned at set-up, and nothing refused it at fire time): an always-pay claim-letter
            # rule pays only from a purse that covers the letter, unless args.debt is true. It is set as pay_if at
            # the letter's price with the free option as its else, so the bridge declines at fire time when the gold
            # is short (event_rules.h compares the gold with the amount then), and refresh_letter_floors keeps the
            # amount at the price as the chaplain's learning moves
            import news as _N  # noqa: PLC0415
            lv = await _N.chaplain_learning(gw, await gw.read("query_council", {"character_id": me}))
            cost = letter_price_high(lv, option)
            fo = free_option(event)
            if cost is None or fo is None:
                return refuse("letter_floor_unread",
                              "an always-pay letter rule pays only from a purse that covers the letter, and "
                              + ("the chaplain's learning (the letter's price) was not read" if cost is None else
                                 f"{event} has no option that pays nothing to take when the gold is short")
                              + f". Nothing was set; args.debt=true pays at any gold, into debt ({DEBT_FORBIDS})",
                              ["answer"])
            amount = int(-(-cost // 1))
            answer, else_ = "pay_if", fo
            params.update(answer="pay_if", currency="gold", amount=amount, **{"else": fo})
            checked["always_pay_floor"] = {"set_as": f"pay_if gold>={amount} else={fo}", "letter_costs": cost,
                                           "why": ALWAYS_PAY_FLOOR}
            floored_at = amount
        elif found and answer == "option" and a.get("debt") is True:
            checked["debt"] = "set: this rule pays at any gold, into debt too"
        if found and answer == "decline":
            # (`0302 decline` refused needs_two_options; 0302 has three options and the 0301
            # rule set just before had already put 0302 on its free option): decline on a letter of three or more
            # options takes the one option that does nothing, by what each option does
            fo = free_option(event)
            import faction_letters as FL  # noqa: PLC0415
            opts3, _ = FL.letter_options(event)
            if fo is not None and opts3 is not None and len(opts3) > 2:
                answer = "option"
                params.update(answer="option", option=fo)
                params.pop("option_index", None)
                checked["decline_as"] = {"option": fo, "why": DECLINE_AS_FREE}
        if found and answer in ("decline", "pay_if"):
            # Decline and pay_if choose between exactly two options; on an event
            # with more the bridge keeps the AI's pick at every firing (needs_two_options)
            import faction_letters as FL  # noqa: PLC0415
            opts, _ = FL.letter_options(event)
            fo = free_option(event) if answer == "pay_if" and opts is not None and len(opts) > 2 else None
            if fo is not None and else_ in (None, "", "ai") and fo != option:
                # pay_if on a letter of three or more options: when the gold does not cover it, the option that
                # does nothing (the decline), found by what each option does
                else_ = params["else"] = fo
                checked["else_by_effect"] = fo
            elif answer == "pay_if" and opts is not None and len(opts) > 2 and else_ not in (None, "", "ai"):
                pass                          # The named else option is taken when the amount is not covered
            elif opts is not None and len(opts) != 2:
                rk = FL.refuse_key(opts)
                return refuse("needs_two_options",
                              f"{event} has {len(opts)} options; {answer} chooses between two, so the game's AI "
                              "would keep answering it. Name the option: answer=option, option="
                              + (rk if rk else "<its key or index>")
                              + (f" ({FL.REFUSE_MEANS})" if rk else ""),
                              ["answer"], result={"intent": "event_rule", "options": opts})
        if found and answer == "pay_if" and else_ not in (None, "", "ai") and option_pays_gold(event, else_):
            # The else is taken
            # exactly when the gold does not cover the amount, so an else that pays gold pays from a purse below it
            fo = free_option(event)
            return refuse("else_pays_gold",
                          f"{else_} pays gold too: a pay_if rule takes its else when the gold is below {amount}, so "
                          f"it would pay from less, into debt ({DEBT_FORBIDS}). Nothing was set"
                          + (f"; the option that pays nothing: else={fo}" if fo else ""), ["else"],
                          result={"intent": "event_rule", **({"instead": {"tool": "do", "args": {
                              "intent": "event_rule", "args": dict({k: a[k] for k in a if k != "else"}, op="set",
                                                                   **{"else": fo})}}} if fo else {})})
    if op == "set":
        # (a seated game, step 16, 1071: an always-pay rule paid the Athlone letter at 9 gold, -104, and every
        # declaration stayed refused into 1079): a rule that pays a claim letter while the gold held does not cover
        # it is refused; its `instead` pays only from a purse that does (pay_if at the letter's price)
        cw = await letter_cost_warning(gw, me, event, answer, option, amount)
        if cw and not (answer == "option" and a.get("debt") is True):     # Debt accepted, said in cost_warning
            frm = (f"whenever the gold is at least {cw['pays_from']}" if "pays_from" in cw
                   else f"from {cw['gold']} gold")
            return refuse("pays_letter_into_debt",
                          f"this rule pays the next claim letter ({cw['each_letter']} gold) {frm}: "
                          f"{cw['debt']}. Nothing was set; cost_warning.instead pays only while the gold covers it",
                          ["answer", "amount"], result={"intent": "event_rule", "cost_warning": cw})
    raw = _d(R._unwrap_reply(await gw.write("set_event_rule", params)))
    if raw.get("success") is not True:
        code = str(raw.get("reason") or "no_reply")
        return refuse(code, f"the bridge refused: {code}", result={"intent": "event_rule", "op": op, "raw": raw})
    variants = []
    if op in ("set", "clear"):
        for ev in (event,) + variants_of(event):
            # The rule now set on the letter is the floored always-pay one, or not
            if floored_at is None:
                _FLOORED.pop((me, ev), None)
            else:
                _FLOORED[(me, ev)] = floored_at
        # The same rule on each variant of the event, its options matched by effect
        for other in variants_of(event):
            vr = variant_rule(event, other, params) if op == "set" else dict(params, event=other)
            if vr is None:
                variants.append({"event": other, "set": False, "why": VARIANT_UNMATCHED})
                continue
            if vr.get("answer") == "pay_if" and vr.get("else") and option_pays_gold(other, vr["else"]):
                variants.append({"event": other, "set": False, "why": VARIANT_ELSE_PAYS})
                continue
            vraw = _d(R._unwrap_reply(await gw.write("set_event_rule", vr)))
            variants.append({"event": other, "set": vraw.get("success") is True,
                             **{k: vr[k] for k in ("answer", "option", "else", "amount") if k in vr},
                             **({} if vraw.get("success") is True else {"refused": vraw.get("reason") or "no_reply"})})
    back = _d(R._unwrap_reply(await gw.read("query_event_rules", {"character_id": me})))
    rules = _rules_of(back, me) if back.get("success") is True else None
    result = {"intent": "event_rule", "op": op, "rules": rules if rules is not None else raw.get("rules"),
              "readback": {"via": "query_event_rules", "read": rules is not None}}
    if checked is not None:
        result["event_check"] = checked
    if variants:
        result["variants"] = variants
    if op == "set":
        result["applies_to"] = ("every time the game fires this event for your ruler, from now on until "
                                "changed; if the rule's option cannot be taken: "
                                + (f"{else_} when the game offers it, else " if else_ not in (None, "", "ai") else "")
                                + "the game's AI picks, and news names it the AI's")
        cw = await letter_cost_warning(gw, me, event, answer, option, amount)
        if cw:
            result["cost_warning"] = cw
        lf = await letter_facts(gw, me, event, answer, option, rules)
        if lf:
            result["claim_letter"] = lf
        oe = named_option_effects(event, params.get("option", option), params.get("else"),
                                  params.get("option_index"))
        if oe:
            result["option_effects"] = oe
        here = [r for r in rules or [] if r.get("event") == event and r.get("answer") == answer]
        if rules is not None and not here:
            return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                              blocked=R.write_blocked(gate="write_not_applied", raw_reason="readback_disagrees",
                                                      summary="the bridge's table does not hold the rule just set"),
                              bridge_health=gw.health)
    return R.envelope("do", verdict="applied", actor=actor, game=gw.game(), result=result,
                      bridge_health=gw.health)


def named_option_effects(event: str, option: Any, else_: Any = None, index: Any = None) -> Optional[dict]:
    """{option key: what it does, read from the event's file and the scripted effects it calls
    (event_prices.option_effects_brief), ..., "note"} for each option a rule names (its option, its else); None when
    it names none. Fact only: the effects as the files set them, or which could not be read."""
    import event_prices as _ep  # noqa: PLC0415
    import faction_letters as FL  # noqa: PLC0415
    names: list = []
    oi = _int(index) if index is not None else _int(option)
    if oi is not None:
        opts, _why = FL.letter_options(event)
        key = (opts[oi].get("key") if opts and 0 <= oi < len(opts) else None)
        names.append(key if key else oi)
    elif isinstance(option, str) and option:
        names.append(option)
    if isinstance(else_, str) and else_ not in ("", "ai") and else_ not in names:
        names.append(else_)
    if not names:
        return None
    out: dict = {str(n): _ep.option_effects_brief(event, n) for n in names}
    out["note"] = _ep.OPTION_EFFECTS_NOTE
    return out


async def letter_facts(gw, me: int, event: str, answer: Any, option: Any, rules) -> Optional[dict]:
    """A recorded case (claim letters cost about 402 gold in one test run and 381 in another, most unused; and a
    rule on 0301 did not cover a 0302 letter): at the call that sets a rule on a chaplain's claim letter, what one
    letter costs, how long the claim lasts, how many months of income that is, and the gold the realm's next title
    still needs; and the letter's other event when no rule covers it. None for another event."""
    import news as _N  # noqa: PLC0415
    if event not in _N.CLAIM_LETTERS:
        return None
    R = _R()
    out: dict = {}
    have = {str(r.get("event")) for r in rules or [] if isinstance(r, dict)}
    other = [e for e in _N.CLAIM_LETTERS if e != event and e not in have]
    if other:
        calls = _N.letter_rule_calls(None)
        out["not_covered"] = {e: calls[e] for e in other}
        out["why"] = ("the chaplain's letter comes as either event; a rule on one does not answer the other "
                      "(the AI pays a 0302 letter, into debt, under a rule set only on 0301)")
    pays = answer == "pay_if" or (answer == "option" and option not in (_N.CLAIM_DECLINE_OPTION, 1, "1"))
    if not pays:
        return out or None
    purse = _d((await R.ruler_purse(gw, me)) or {})
    gold, inc = purse.get("gold"), purse.get("monthly_income")
    lv = await _N.chaplain_learning(gw, await gw.read("query_council", {"character_id": me}))
    pr = _N.claim_letter_price(lv)
    if pr is None:
        out["each_letter"] = "unread: the chaplain's learning was not read"
        return out
    cost = pr.get("gold", pr.get("gold_high"))
    duchy = option == _N.CLAIM_DUCHY_OPTION
    if duchy:
        cost = _N.claim_duchy_gold_cost(lv)
    out["each_letter"] = {"gold": cost, "basis": _N.CLAIM_DUCHY_BASIS if duchy else _N.CLAIM_PRICE_RULE[:120]}
    out["claim_lasts"] = "until your ruler's death unless pressed: " + _N.CLAIM_DIES
    if isinstance(inc, (int, float)) and inc > 0:
        out["months_of_income"] = round(cost / inc, 1)
    for kt, v in (await R.next_title_short(gw, me, gold, cost) or {}).items():
        out[kt] = {"price": v["price"], "gold": v["gold"], "short_now": v["short_now"],
                   "short_after_one_letter": v["short_after"]}
    return out


#: (a test run, turn 9, 1069.10.02: the claim letter's rule set to "option .a" (always pay) at
#: gold 0.56; the Ossory letter came at 0.89 and left -86.11; in debt most of 1069-1074, which held a war 14 months)
DEBT_FORBIDS = "the game refuses every war declaration while gold is below 0"


async def letter_cost_warning(gw, me: int, event: str, answer: Any, option: Any, amount: Any):
    """The claim letter's cost at the time a rule that pays it is set, when the gold held does not cover it:
    {each_letter, gold, monthly_income, after_one_letter, debt, months_to_repay}; None for another event, a
    rule that does not pay, one whose floor covers the price, or a purse that did not read."""
    import news as _N  # noqa: PLC0415
    if event not in _N.CLAIM_LETTERS:          # 0302's county letter (.a) is priced as 0301's
        return None
    if answer == "option":
        # An option pays when its body takes gold (the duchy letter 0302.b does); the files unread, the old
        # rule (the first option, .a)
        fp = option_pays_gold(event, option)
        pays = fp if fp is not None else ((isinstance(option, str) and option.endswith(".a")) or _int(option) == 0)
    else:
        pays = answer == "pay_if"
    if not pays:
        return None
    R = _R()
    purse = _d((await R.ruler_purse(gw, me)) or {})
    gold, inc = purse.get("gold"), purse.get("monthly_income")
    lv = await _N.chaplain_learning(gw, await gw.read("query_council", {"character_id": me}))
    cost = letter_price_high(lv, option)
    if not isinstance(gold, (int, float)) or cost is None:
        return None
    floor = _int(amount) if answer == "pay_if" else None
    if floor is not None and floor >= cost:
        return None                   # pay_if pays only from a purse that covers it
    if floor is None and gold >= cost:
        return None
    # A pay_if floor below the price pays
    # into debt from any gold between the two, whatever the purse holds now
    after = round((floor if floor is not None else gold) - cost, 2)
    out = {"each_letter": cost, "price_basis": _N.CLAIM_PRICE_RULE[:120], "gold": round(gold, 2),
           "monthly_income": inc, "after_one_letter": after, "debt": DEBT_FORBIDS}
    if floor is not None:
        out["pays_from"] = floor
    if isinstance(inc, (int, float)) and inc > 0:
        out["months_to_repay"] = round(-after / inc, 1)
    paying = option if isinstance(option, str) and option else event + ".a"
    out["instead"] = {"tool": "do", "args": {"intent": "event_rule", "args": {
        "op": "set", "event": event, "answer": "pay_if", "option": paying, "currency": "gold",
        "amount": int(cost) + 1}}}
    return out


def letter_price_high(learning: Any, option: Any) -> Optional[float]:
    """The most gold the chaplain's letter can charge for `option` at this learning: the duchy letter
    (court_chaplain_task.0302.b, 7 x (40 - learning), at least 80-100) or the county letter (claim_letter_price, its
    upper bound where the random minimum binds). None without the learning."""
    import news as _N  # noqa: PLC0415
    pr = _N.claim_letter_price(learning)
    if pr is None:
        return None
    if option == _N.CLAIM_DUCHY_OPTION:
        return float(max((40 - learning) * 7, 100))
    return float(pr.get("gold", pr.get("gold_high")))


#: What the row of a raised floor says
FLOOR_RAISED = ("the letter's price moved above the rule's amount (the chaplain's learning changed): the amount is "
                "raised to the price, so the rule never pays a letter the gold does not cover")


async def refresh_letter_floors(gw, me: int, learning: Any) -> Optional[list]:
    """A recorded case (a test run, a07: a gold>=78 rule set at a 78-gold letter paid a 90-gold one at 83.54 two years later): at
    each read, every pay_if gold rule of the ruler on a claim letter whose amount is below the letter's price now is set
    again at that price. The rows of the rules raised, or None (none, or nothing read)."""
    import news as _N  # noqa: PLC0415
    if not me or not isinstance(learning, (int, float)) or isinstance(learning, bool):
        return None
    raw = _d(_R()._unwrap_reply(await gw.read("query_event_rules", {"character_id": me})))
    rules = _rules_of(raw, me) if raw.get("success") is True else None
    out = []
    for r in rules or []:
        if r.get("event") not in _N.CLAIM_LETTERS or r.get("answer") != "pay_if" \
                or r.get("currency") not in (None, "gold") or _int(r.get("amount")) is None:
            continue
        cost = letter_price_high(learning, r.get("option"))
        if cost is None or _int(r["amount"]) >= cost:
            continue
        need = int(-(-cost // 1))
        params = {"character_id": me, "op": "set", "event": r["event"], "answer": "pay_if", "currency": "gold",
                  "amount": need}
        if _int(r.get("option")) is not None:
            params["option_index"] = _int(r["option"])
        elif r.get("option"):
            params["option"] = r["option"]
        if r.get("else") not in (None, "", "ai"):
            params["else"] = r["else"]
        w = _d(_R()._unwrap_reply(await gw.write("set_event_rule", params)))
        out.append({"event": r["event"], "amount_was": _int(r["amount"]), "amount_now": need, "letter_costs": cost,
                    "set": w.get("success") is True, **({"why": FLOOR_RAISED} if not out else {})})
    return out or None


# ── do(intent="gold_reserve") ─────────────────────────────────────────────────
def budget_row(raw: Any) -> Optional[dict]:
    """query_ai_budget's reply as the purse shows it, or None when it did not read."""
    raw = _d(raw)
    b = raw.get("buckets")
    if raw.get("success") is not True or not isinstance(b, dict):
        return None
    row = {k: round(float(b[k]), 2) for k in ("reserved", "war_chest", "long_term", "short_term")
           if isinstance(b.get(k), (int, float))}
    row["reserved_max"] = raw.get("reserved_max")
    if raw.get("buckets_sum_to_gold") is False:
        row["layout_check"] = "the buckets do not sum to the gold field: the read is suspect"
    return row


async def debt_row(gw, me: int) -> dict:
    """{"debt": sentence} when the ruler's gold is below 0, else {} (also when unread)."""
    import declare_gates as _dg  # noqa: PLC0415
    p = _dg.purse_of(await gw.read("query_character", {"character_id": me})) or {}
    g = p.get("gold")
    return {"debt": _dg.debt_words(g, p.get("monthly_income"))} if isinstance(g, (int, float)) and g < 0 else {}


async def do_gold_reserve(gw, a: dict, character_id: int, actor) -> dict:
    """args {amount (whole gold, 0 releases), move}: no amount = read the buckets."""
    R = _R()
    me = character_id or R._as_int((actor or {}).get("id"))
    amount = a.get("amount")
    move = a.get("move", False)
    before = budget_row(await gw.read("query_ai_budget", {"character_id": me}))
    debt = await debt_row(gw, me)
    if amount is None:
        return R.envelope("do", actor=actor, game=gw.game(), bridge_health=gw.health,
                          result={"intent": "gold_reserve", "budget": before, **debt,
                                  **({} if before else {"budget_unread": "query_ai_budget did not answer"}),
                                  "note": RESERVE_NOTE})
    n = _int(amount)
    if n is None or n < 0 or n > RESERVE_MAX or not isinstance(move, bool):
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                          blocked=R.write_blocked(gate="invalid_argument", raw_reason="bad_reserve_amount",
                                                  summary=f"args.amount is whole gold 0..{RESERVE_MAX}, "
                                                          "args.move true or false",
                                                  fix={"rejected_fields": ["amount", "move"]}),
                          bridge_health=gw.health)
    sent = await R._script_write(gw, "gold_reserve", {"actor_id": me, "amount": n, "move": move})

    async def landed(_gw, _b):
        got = budget_row(await gw.read("query_ai_budget", {"character_id": me}))
        # A read whose buckets do not sum to the gold field is suspect (on Windows the budget
        # layout is the Linux one, unmeasured), so it cannot say the maximum did NOT change: unread
        if got is None or "layout_check" in got:
            return R.UNREAD
        return got if got.get("reserved_max") == n else None
    v = await R.script_verdict(gw, sent, landed=landed, before=None, what="the reserved-gold maximum",
                               guard_gate="write_not_applied", guard_reason="reserve_not_set",
                               error_gate="engine_refused")
    v["result"] = dict(v["result"], intent="gold_reserve", budget_before=before, note=RESERVE_NOTE, **debt)
    return R._script_envelope("do", v, actor, gw)


# ── do(intent="ai_veto") ──────────────────────────────────────────────────────
def price_of(rec: dict) -> Optional[dict]:
    """A record's price, its non-zero currencies only ({"gold": 212.5}); None when not read."""
    p = rec.get("price")
    if rec.get("priced") != "yes" or not isinstance(p, dict):
        return None
    return {k: round(float(p[k]), 2) for k in ("gold", "prestige", "piety", "renown")
            if isinstance(p.get(k), (int, float)) and abs(float(p[k])) >= 0.005}


def attempt_row(rec: dict) -> dict:
    """One recorded AI post as the seat reads it."""
    row = {"date": rec.get("date"), "what": rec.get("what"), "dropped": rec.get("dropped") is True}
    if rec.get("key"):
        row["key"] = rec["key"]
    elif rec.get("decision_key"):
        row["key"] = rec["decision_key"]
    for k in ("title_id", "company_id", "laamp_id", "location_id", "ally_id", "war_id", "offered_by", "regiment",
              "regiment_id", "target_id", "faction_post", "faction_id", "recipient_id", "called_by"):
        if k in rec:
            row[k] = rec[k]
    pr = price_of(rec)
    if pr is None and rec.get("what") == "court_type":
        pr = court_type_price(rec.get("key"))
        if pr is not None:
            row["price_source"] = "/".join(COURT_TYPES_FILE) + " cost"
    if pr is not None:
        row["price"] = pr
    elif rec.get("priced") == "no":
        row["price_unread"] = rec.get("unpriced_why") or "not read"
    else:
        row["price_unread"] = "pending: priced on the next game day"
    if isinstance(rec.get("gold"), (int, float)):
        row["gold_at_attempt"] = round(float(rec["gold"]), 2)
        row["prestige_at_attempt"] = round(float(rec.get("prestige") or 0), 2)
    return row


def _ep_decision_keys():
    import event_prices as _ep  # noqa: PLC0415
    return _ep.decision_keys()


def _veto_ruler(raw: dict, me: int) -> Optional[dict]:
    for r in raw.get("rulers") or []:
        if isinstance(r, dict) and r.get("character_id") == me:
            return r
    return None


def _veto_records(raw: dict, me: int) -> list:
    return [r for r in raw.get("records") or [] if isinstance(r, dict) and r.get("character_id") == me]


def _listed(rec: dict, me: int) -> dict:
    """An attempt as op=list shows it; a decision priced from its files as in the news (not
    decision_cost_in_files, the bridge's word for "the toolkit prices it")."""
    row = attempt_row(rec)
    decision_row_price(row, {"actor_id": me})
    return row


async def _ai_veto_list(gw, me: int, actor) -> dict:
    """op=list: the vetoes, the counts, the last attempts (a read only)."""
    R = _R()
    back = _d(R._unwrap_reply(await gw.read("query_ai_veto", {"character_id": me})))
    if back.get("success") is not True:
        return R.envelope("do", ok=False, actor=actor, game=gw.game(),
                          blocked=R.why_blocked("read_timeout", summary="query_ai_veto did not answer",
                                                raw_reason=str(back.get("reason") or "no_reply")),
                          bridge_health=gw.health)
    ruler = _veto_ruler(back, me) or {}
    recs = _veto_records(back, me)
    result = {"intent": "ai_veto", "op": "list", "vetoes": list(ruler.get("vetoes") or []),
              "dropped": ruler.get("dropped") or {}, "passed": ruler.get("passed") or {},
              "attempts": [_listed(r, me) for r in recs[-LIST_ATTEMPTS:]], "attempts_held": len(recs),
              "hook_installed": back.get("hook_installed")}
    if "decision" in result["vetoes"]:
        result["decision_keys"] = list(ruler.get("decision_keys") or []) or "every decision"
    if back.get("disabled_by_env"):
        result["disabled_by_env"] = True
    return R.envelope("do", actor=actor, game=gw.game(), bridge_health=gw.health, result=result)


GOLD_RULE_ABSENT = ("this bridge did not read back the gold rule: only the decisions named in keys are dropped, not "
                    "those that cost gold")


def spending_unset(vetoes) -> Optional[dict]:
    """The preset's kinds not vetoed now and the one call that sets them; None when every one is set,
    {"unread": ...} when the vetoes were not read."""
    if not isinstance(vetoes, list):
        return {"unread": "query_ai_veto did not answer: which kinds are vetoed is not known"}
    left = [w for w in AI_VETO_PRESETS["spending"] if w not in vetoes]
    if not left:
        return None
    return {"not_vetoed": left, "one_call": {"tool": "do", "args": {"intent": "ai_veto", "args": {
        "op": "set", "what": "spending"}}}}


async def _ai_veto_preset(gw, a: dict, me: int, op: str, preset: str, actor, refuse) -> dict:
    """Op=set|clear what=spending: each kind of the preset written to the bridge, then the table read back
    once. Applied only when every kind reads as asked; otherwise rejected, naming the kinds that did not take."""
    R = _R()
    if a.get("keys") not in (None, []):
        return refuse("keys_only_with_set_decision", "args.keys goes with op=set what=decision only", ["keys"])
    kinds = AI_VETO_PRESETS[preset]
    refused: dict = {}
    spare = await _goal_spare(gw, me) if op == "set" and "create_title" in kinds else None
    dec_keys = None
    for w in kinds:
        params = {"character_id": me, "op": op, "what": w}
        if w == "create_title":
            params.update(spare_params(spare))                               # The goal and its chain pass
        if w == "activity" and op == "set":
            params.update(await activity_spare_params(gw, me))               # The seat's own host_activity
        if w == "decision" and op == "set":
            # The decisions that cost gold (the bridge's gold rule) and those that cut income, added to the keys
            # the seat holds; a decision veto that drops every decision already is left as it is
            cur = _veto_ruler(_d(R._unwrap_reply(await gw.read("query_ai_veto", {"character_id": me}))), me) or {}
            had = list(cur.get("decision_keys") or [])
            if "decision" in (cur.get("vetoes") or []) and not had and not cur.get("decision_gold_cost"):
                continue
            dec_keys = had + [k for k in SPENDING_DECISION_KEYS if k not in had]
            params.update(keys=",".join(dec_keys[:DECISION_KEYS_MAX]), gold_cost=1)
        raw = _d(R._unwrap_reply(await gw.write("set_ai_veto", params)))
        if raw.get("success") is not True:
            refused[w] = str(raw.get("reason") or "no_reply")
    back = _d(R._unwrap_reply(await gw.read("query_ai_veto", {"character_id": me})))
    ruler = _veto_ruler(back, me) if back.get("success") is True else None
    vetoes = list((ruler or {}).get("vetoes") or [])
    result = {"intent": "ai_veto", "op": op, "what": preset, "kinds": list(kinds), "vetoes": vetoes,
              "left_out": dict(AI_VETO_LEFT_OUT), "readback": {"via": "query_ai_veto", "read": ruler is not None}}
    if op == "set":
        result["applies_to"] = AI_VETO_APPLIES
        gs = _goal_spare_result(spare, ruler, me)
        if gs is not None:
            result["goal_title"] = gs
        if ruler is not None and "decision" in kinds:
            result["decisions"] = {"drops": SPENDING_DECISIONS, "keys": list(ruler.get("decision_keys") or []),
                                   "by_gold_cost": ruler.get("decision_gold_cost") is True}
            if dec_keys is not None and ruler.get("decision_gold_cost") is not True:
                result["decisions"]["gold_cost_unread"] = GOLD_RULE_ABSENT
        result["events"] = SPENDING_EVENTS_ROW
    if refused:
        result["refused"] = refused
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                          blocked=R.write_blocked(gate=REFUSALS.get(next(iter(refused.values())), "invalid_argument"),
                                                  raw_reason=next(iter(refused.values())),
                                                  summary="the bridge refused " + ", ".join(
                                                      f"{w} ({c})" for w, c in refused.items())
                                                  + "; the other kinds were written"),
                          bridge_health=gw.health)
    if ruler is None:
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                          blocked=R.write_blocked(gate="write_unconfirmed", raw_reason="readback_absent",
                                                  summary="query_ai_veto did not answer after the writes"),
                          bridge_health=gw.health)
    wrong = [w for w in kinds if (w in vetoes) != (op == "set")]
    if wrong:
        result["not_as_asked"] = wrong
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                          blocked=R.write_blocked(gate="write_not_applied", raw_reason="readback_disagrees",
                                                  summary="the bridge's veto table does not read as just set for "
                                                          + ", ".join(wrong)),
                          bridge_health=gw.health)
    return R.envelope("do", verdict="applied", actor=actor, game=gw.game(), result=result, bridge_health=gw.health)


async def do_ai_veto(gw, a: dict, character_id: int, actor) -> dict:
    """op=set|clear {what}; op=list (the vetoes, the counts, the last attempts)."""
    R = _R()
    me = character_id or R._as_int((actor or {}).get("id"))
    op = str(a.get("op") or ("set" if a.get("what") else "list"))
    what = a.get("what")
    if op == "list":
        return await _ai_veto_list(gw, me, actor)

    def refuse(code: str, summary: str, fields=None, result=None):
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                          blocked=R.write_blocked(gate=REFUSALS.get(code, "invalid_argument"), raw_reason=code,
                                                  summary=summary,
                                                  fix={"rejected_fields": fields} if fields else None),
                          bridge_health=gw.health)

    if op not in AI_VETO_OPS:
        return refuse("op_must_be_set_or_clear", f"args.op is one of {', '.join(AI_VETO_OPS)}", ["op"])
    if not what:
        return refuse("what_required", f"args.what is one of {', '.join(AI_VETO_WHATS)}", ["what"])
    if what in AI_VETO_PRESETS:
        return await _ai_veto_preset(gw, a, me, op, str(what), actor, refuse)
    if what not in AI_VETO_WHATS:
        return refuse("what_must_be_a_veto_kind",
                      f"args.what is one of {', '.join(AI_VETO_WHATS + tuple(AI_VETO_PRESETS))}", ["what"])
    keys = a.get("keys")
    if isinstance(keys, str):
        keys = [k.strip() for k in keys.split(",")]
    if keys is not None and keys != []:
        if what != "decision" or op != "set":
            return refuse("keys_only_with_set_decision", "args.keys goes with op=set what=decision only", ["keys"])
        if (not isinstance(keys, list) or len(keys) > DECISION_KEYS_MAX
                or not all(isinstance(k, str) and _DECISION_KEY_RE.match(k) for k in keys)):
            return refuse("bad_decision_keys", f"args.keys is a list of at most {DECISION_KEYS_MAX} decision keys "
                                               "(lower-case letters, digits, _)", ["keys"])
        # (a seated game, 1071.09.22: `convert_to_local_culture` was taken and the keyed culture veto lost for 25
        # years): every key must be a decision the game's files define; the nearest one is named
        known = _ep_decision_keys()
        if known is not None:
            bad = [k for k in keys if k not in known]
            if bad:
                import difflib  # noqa: PLC0415
                near = {k: (difflib.get_close_matches(k, sorted(known), n=1, cutoff=0.6) or [None])[0] for k in bad}
                return refuse("unknown_decision_key", "no decision file defines " + "; ".join(
                    f"{k} (nearest: {near[k]})" if near[k] else k for k in bad), ["keys"],
                    result={"intent": "ai_veto", "unknown": bad, "nearest": near})
    params = {"character_id": me, "op": op, "what": what}
    replace = a.get("replace") is True
    merged_from = None
    if keys and not replace:
        # Set ADDS to the keys already vetoed (replace: true rewrites the list)
        cur = _d(R._unwrap_reply(await gw.read("query_ai_veto", {"character_id": me})))
        cr = _veto_ruler(cur, me) if cur.get("success") is True else None
        if cr is None:
            return refuse("no_reply", "query_ai_veto did not answer: the keys already set are unknown, so "
                                      "nothing was changed (send replace: true to set exactly these)")
        had = list(cr.get("decision_keys") or [])
        if cr.get("decision_gold_cost") is True:
            params["gold_cost"] = 1               # The spending preset's gold rule stays with the keys added
        if "decision" in (cr.get("vetoes") or []) and not had and cr.get("decision_gold_cost") is not True:
            return refuse("keys_would_narrow_every_decision", "the decision veto drops every decision now; "
                          "send replace: true to drop only these", ["keys"])
        merged_from = had
        keys = had + [k for k in keys if k not in had]
        if len(keys) > DECISION_KEYS_MAX:
            return refuse("too_many_decision_keys", f"{len(keys)} keys with those already set {had}: at most "
                          f"{DECISION_KEYS_MAX}; send replace: true with the list to keep", ["keys"])
    if keys:
        params["keys"] = ",".join(keys)
    spare = await _goal_spare(gw, me) if what == "create_title" and op == "set" else None
    params.update(spare_params(spare))                                       # The goal and its chain pass
    if what == "activity" and op == "set":
        params.update(await activity_spare_params(gw, me))                   # The seat's own host_activity
    raw = _d(R._unwrap_reply(await gw.write("set_ai_veto", params)))
    if raw.get("success") is not True:
        code = str(raw.get("reason") or "no_reply")
        return refuse(code, f"the bridge refused: {code}", result={"intent": "ai_veto", "op": op, "raw": raw})
    back = _d(R._unwrap_reply(await gw.read("query_ai_veto", {"character_id": me})))
    ruler = _veto_ruler(back, me) if back.get("success") is True else None
    vetoes = list((ruler or {}).get("vetoes") or [])
    result = {"intent": "ai_veto", "op": op, "vetoes": vetoes,
              "dropped": (ruler or {}).get("dropped") or {}, "passed": (ruler or {}).get("passed") or {}}
    result["readback"] = {"via": "query_ai_veto", "read": ruler is not None}
    if op == "set":
        result["applies_to"] = AI_VETO_APPLIES
        gs = _goal_spare_result(spare, ruler, me)
        if gs is not None:
            result["goal_title"] = gs
    if what == "decision" and ruler is not None:
        got = list(ruler.get("decision_keys") or [])
        result["decision_keys"] = got
        if merged_from is not None:
            result["keys_added_to"] = merged_from
        if op == "set" and not keys:
            result["note"] = DECISION_ALL_NOTE
        if op == "set" and sorted(got) != sorted(keys or []):
            return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                              blocked=R.write_blocked(gate="write_not_applied", raw_reason="readback_disagrees",
                                                      summary="the bridge's decision keys do not read as just set"),
                              bridge_health=gw.health)
    if ruler is not None and (what in vetoes) != (op == "set"):
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                          blocked=R.write_blocked(gate="write_not_applied", raw_reason="readback_disagrees",
                                                  summary="the bridge's veto table does not read as just set"),
                          bridge_health=gw.health)
    return R.envelope("do", verdict="applied", actor=actor, game=gw.game(), result=result,
                      bridge_health=gw.health)


# ── observe's news ─────────────────────────────────────────────────────────────
#: The rulers this server put in the bridge's event table to record their answered events
WATCHED_BY_SERVER: dict = {}


async def news_reads(gw, actor_id: int) -> tuple[Optional[dict], Optional[dict], Optional[dict]]:
    """(query_event_rules, query_ai_budget, query_ai_veto) for the news snapshot; None when unread.
    A ruler with no event table is watched (set_event_rule op=watch: the bridge records
    the game's picks for him, changes none), so the news can name every event the AI answers for him
    and its price; without it, events answered before the seat's first rule were never recorded
    (-300 prestige in four answered events read from telemetry, not from the news)."""
    if not actor_id:
        return None, None, None
    got = await gw.read_many([("query_event_rules", {"character_id": actor_id}),
                              ("query_ai_budget", {"character_id": actor_id}),
                              ("query_ai_veto", {"character_id": actor_id})])
    ev, bud, veto = (list(got) + [None, None, None])[:3]
    ev = ev if isinstance(ev, dict) and ev.get("success") is True else None
    if ev is not None and _rules_of(ev, actor_id) is None and actor_id not in WATCHED_BY_SERVER:
        w = await gw.write("set_event_rule", {"character_id": actor_id, "op": "watch"})
        WATCHED_BY_SERVER[actor_id] = isinstance(w, dict) and w.get("success") is True
        again = await gw.read("query_event_rules", {"character_id": actor_id})
        if isinstance(again, dict) and again.get("success") is True:
            ev = again
    bud = bud if isinstance(bud, dict) and bud.get("success") is True else None
    veto = veto if isinstance(veto, dict) and veto.get("success") is True else None
    return ev, bud, veto


def snapshot_part(actor_id, event_rules: Optional[dict], ai_budget: Optional[dict],
                  ai_veto: Optional[dict] = None) -> dict:
    """The snapshot keys news diffs: the watched flag, the picks, the budget."""
    if not isinstance(event_rules, dict):
        ev = {"event_watched": None, "event_picks": None, "event_rule_events": None}
    else:
        rules = _rules_of(event_rules, actor_id)
        ev = {"event_watched": rules is not None,
              "event_picks": _picks_of(event_rules, actor_id),
              # The events a rule of the seat's decides
              "event_rule_events": sorted({str(r.get("event")) for r in rules or [] if r.get("event")})}
    veto = ({"ai_veto_records": _veto_records(ai_veto, actor_id),
             "ai_vetoes": list((_veto_ruler(ai_veto, actor_id) or {}).get("vetoes") or []),
             # The decisions the decision veto names (carried to a new ruler with it)
             "ai_veto_decision_keys": list((_veto_ruler(ai_veto, actor_id) or {}).get("decision_keys") or [])}
            if isinstance(ai_veto, dict) else {"ai_veto_records": None, "ai_vetoes": None,
                                               "ai_veto_decision_keys": None})
    # (1070.03, 1071.04: Ulster and Athlone became the seat's vassals with no war and no row saying
    # how): the AI's offers of vassalage for this ruler, from the bridge's census of its interactions
    offers = None
    if isinstance(ai_veto, dict) and isinstance(ai_veto.get("interactions"), list):
        offers = [{"count": x.get("count"), "last": x.get("last")} for x in ai_veto["interactions"]
                  if isinstance(x, dict) and x.get("interaction") == "offer_vassalization_interaction"
                  and (x.get("character_id") in (None, actor_id))]
    # The AI's grants of titles for this ruler, from the same census (counted, never dropped)
    grants = None
    if isinstance(ai_veto, dict) and isinstance(ai_veto.get("interactions"), list):
        grants = [{"interaction": x.get("interaction"), "count": x.get("count"), "last": x.get("last")}
                  for x in ai_veto["interactions"]
                  if isinstance(x, dict) and x.get("interaction") in AI_GRANT_INTERACTIONS
                  and (x.get("character_id") in (None, actor_id))]
    # The AI's posts of command classes no veto kind classifies, by class (a newer bridge only)
    uncl = None
    if isinstance(ai_veto, dict) and isinstance(ai_veto.get("unclassified_commands"), list):
        uncl = [{"class": x.get("class"), "vtable_rva": x.get("vtable_rva"), "count": x.get("count"), "last": x.get("last")}
                for x in ai_veto["unclassified_commands"]
                if isinstance(x, dict) and (x.get("character_id") in (None, actor_id))]
    return {**ev, "ai_budget": budget_row(ai_budget), **veto, "ai_vassal_offers_sent": offers,
            "ai_grants_sent": grants, "ai_unclassified": uncl}


def unclassified_in_step(prev: dict, snap: dict) -> Optional[list]:
    """The command classes no veto kind classifies that the game's AI posted for the ruler since the
    previous read ({class, vtable, posts, last}), None when the bridge does not send the census."""
    now = snap.get("ai_unclassified")
    if not isinstance(now, list):
        return None
    before = {x.get("vtable_rva"): x.get("count") or 0 for x in prev.get("ai_unclassified") or [] if isinstance(x, dict)}
    return [{"class": x.get("class"), "vtable_rva": x.get("vtable_rva"), "posts": (x.get("count") or 0) - before.get(x.get("vtable_rva"), 0),
             "last": x.get("last")} for x in now
            if isinstance(x, dict) and (x.get("count") or 0) > before.get(x.get("vtable_rva"), 0)]


def new_veto_records(prev: dict, snap: dict) -> list:
    """The AI posts recorded since the previous read (by the bridge's sequence)."""
    recs = snap.get("ai_veto_records")
    if not isinstance(recs, list):
        return []
    last = max([r.get("seq") or 0 for r in prev.get("ai_veto_records") or [] if isinstance(r, dict)] or [0])
    return [r for r in recs if isinstance(r, dict) and (r.get("seq") or 0) > last]


def fired_words(fires: list) -> list:
    """One string per fired event: `epidemic_events.0100: .a -350 prestige` (`if its branch runs` when the
    effect fires it under a condition)."""
    out = []
    for f in fires or []:
        opts = f.get("options")
        if isinstance(opts, list):
            body = ", ".join("." + str(o.get("option") or "?").rsplit(".", 1)[-1] + f" {o.get('prestige')}"
                             for o in opts) + " prestige"
            gold = [f".{str(o.get('option') or '?').rsplit('.', 1)[-1]} {o.get('gold', o.get('gold_if_its_conditions_held'))}"
                    + (" if its conditions held" if "gold_if_its_conditions_held" in o else "")
                    for o in opts if "gold" in o or "gold_if_its_conditions_held" in o]
            if gold:
                body += "; gold " + ", ".join(gold)
        else:
            body = str(opts)
        out.append(f"{f.get('event')}: {body}" + (" (if its branch runs)" if f.get("conditional") else ""))
    return out


def decision_row_price(a: dict, snap: dict) -> None:
    """A decision row's price from the decision's cost block in the game's files (the bridge reads
    no decision price): `price` with the amounts the files state, `price_unknown` for a computed one."""
    import event_prices as _ep  # noqa: PLC0415
    key = a.get("key")
    if a.get("what") != "decision" or not key:
        return
    p = _ep.decision_price(key, snap.get("culture"), _ep.CAPITAL_CULTURE.get(snap.get("actor_id")),
                           snap.get("monthly_income"))
    a.pop("price_unread", None)
    if "unread" in p:
        a["price_unknown"] = p["unread"]
        return
    if p.get("free"):
        # A decision that costs
        # income says so
        cost = SPENDING_DECISION_KEYS.get(key)
        a["price"] = f"no gold now; {cost}" if cost else "free (no cost block, fires no event)"
        return
    if p.get("fires") and p.get("prestige") == 0 and "gold" not in p and key not in SPENDING_DECISION_KEYS:
        # its events take nothing the files name
        a["fires"] = fired_words(p["fires"])
        a["price"] = "free (no cost block; the events it fires take no prestige or gold)"
        a["price_basis"] = p["basis"]
        return
    if p.get("fires"):
        # The events its effect fires, each option's prestige (first level only)
        a["fires"] = fired_words(p["fires"])
        if key in SPENDING_DECISION_KEYS:
            a["effect"] = SPENDING_DECISION_KEYS[key]        # What it does to the ruler, in words
    skip = ("basis", "multipliers", "fires", "note", "gold_basis")
    known = {c: v for c, v in p.items() if c not in skip and isinstance(v, (int, float))}
    unknown = {c: v for c, v in p.items() if c not in skip and isinstance(v, str)}
    if known:
        a["price"] = known
    if unknown:
        a["price_unknown"] = unknown
    a["price_basis"] = p["basis"] + (f"; gold: {p['gold_basis']}" if p.get("gold_basis") else "")


#: (ally 36358's overseas wars 1076-79 and 1082-85, joined by Murchad's AI; the purse fell 136 -> 3.6
#: in the second): another ruler's call to war TO your ruler is answered by your ruler's AI inside the send
CALL_IN_SENT_BY = ("called by another ruler's AI into his war; your ruler's AI answered at once (no letter, no event); "
                   "do ai_veto what=call_in drops such calls before they reach him")
CALL_IN_DROPPED = ("the call never reached your ruler, so he did not join and no refusal was answered; do ai_veto "
                   "op=clear what=call_in lets the next call through")
#: The levy upkeep a month per man raised (news.GOLD_COST_PER_SOLDIER, 00_defines.txt)
CALL_IN_COST_BASIS = ("your raised armies now at 0.003 gold a man a month (00_defines.txt GOLD_COST_PER_SOLDIER); "
                      "men-at-arms' raised upkeep is not read")


def call_in_war(war_id: Any, snap: dict) -> dict:
    """The war a call is for, from the news's war list ({war, attacker_id, defender_id}), or why not."""
    w = (snap.get("wars") or {}).get(war_id) if isinstance(snap.get("wars"), dict) else None
    if not isinstance(w, dict):
        return {"war_unread": "the war is not in your wars read now (the call dropped, or the war not read)"}
    return {"war": w.get("name"), "attacker_id": w.get("attacker_id"), "defender_id": w.get("defender_id")}


def call_in_costs(snap: dict) -> dict:
    """What joining costs as read: the men raised now and their upkeep a month."""
    import news as _n  # noqa: PLC0415
    men = snap.get("armies_men")
    if not isinstance(men, int) or isinstance(men, bool):
        return {"unread": "your armies were not read"}
    return {"armies_raised_men": men, "upkeep_a_month": -round(men * _n.GOLD_COST_PER_SOLDIER, 2),
            "basis": CALL_IN_COST_BASIS}


def _news_creation_price(key: str, me: Any) -> Optional[dict]:
    """News._CREATION_PRICE's last read of a title's creation price for this ruler ({cost, currency, date})."""
    import news as _n  # noqa: PLC0415
    p = _n._CREATION_PRICE.get(key)
    if not isinstance(p, dict) or (me and p.get("actor_id") not in (None, me)):
        return None
    return {k: p[k] for k in ("cost", "currency", "date") if k in p}


def veto_news(prev: dict, snap: dict) -> dict:
    """ai_vetoed: the posts the bridge dropped since the last read, one row per (what, key) with the
    attempts, the price and the gold at the last one, and the call that does it yourself. ai_acted:
    the title and law posts that went through to the game, with their price."""
    out: dict = {}
    dropped: dict = {}
    acted = []
    for r in new_veto_records(prev, snap):
        if r.get("origin") not in (None, "game"):
            # A post the bridge made itself (the seat's own call_ally): not the AI's
            continue
        if r.get("dropped") is True:
            ally = r.get("what") == "ally_call"
            war = r.get("what") == "war"
            cin = r.get("what") == "call_in"
            k = (r.get("what"), (r.get("ally_id"), r.get("war_id")) if ally else r.get("target_id") if war else
                 (r.get("called_by"), r.get("war_id")) if cin else
                 (r.get("key") or r.get("title_id") or r.get("company_id") or r.get("location_id")))
            row = dropped.get(k)
            a = attempt_row(r)
            if row is None:
                row = dropped[k] = {"what": a["what"], **({"key": a["key"]} if "key" in a and not ally and not war
                                                          else {}),
                                    "attempts": 0, "first": a["date"]}
                if ally:
                    row.update(ally_id=a.get("ally_id"), war_id=a.get("war_id"),
                               next=call_ally_call(a.get("war_id"), a.get("ally_id")))
                mk = AI_VETO_YOURSELF.get(a["what"])
                if mk and a.get("key") and not ally and mk(a["key"]):
                    row["next"] = mk(a["key"])
                if a["what"] == "perk":
                    row["note"] = PERK_VETO_NOTE
                if a["what"] == "legacy":
                    row["note"] = LEGACY_VETO_NOTE
                if a["what"] == "create_title" and a.get("key") and a.get("key") == goal_title():
                    row["note"] = GOAL_DROPPED.format(key=a["key"])
                    row["goal_title"] = True
                elif a["what"] == "create_title" and chain_title_of(a.get("key"), snap.get("actor_id")):
                    c = chain_title_of(a.get("key"), snap.get("actor_id"))       # An old bridge's drop
                    row["note"] = CHAIN_DROPPED.format(key=a["key"], goal=c["key"], rule=c.get("rule") or "its tier rule")
                    row["goal_chain"] = True
                if a["what"] == "create_title":
                    # Every dropped creation names the title (its id when the key did not read)
                    row["title_id"] = a.get("title_id")
                    if not a.get("key"):
                        row["key_unread"] = "the bridge did not read the title's key; title_id names it"
                if cin:
                    row.update(called_by=a.get("called_by"), war_id=a.get("war_id"), note=CALL_IN_DROPPED,
                               **call_in_war(a.get("war_id"), snap))
                for x in ("company_id", "laamp_id", "target_id"):
                    # (a live check: three drops of company 58 read with no company named)
                    if x in a:
                        row[x] = a[x]
            row["attempts"] += 1
            row["last"] = a["date"]
            decision_row_price(a, snap)
            for x in ("price", "price_unread", "price_unknown", "price_basis", "gold_at_attempt", "prestige_at_attempt"):
                if x in a:
                    row[x] = a[x]
            if cin:
                row.pop("price_unread", None)
                row.pop("key", None)
            elif a["what"] == "create_title" and "price" not in row and a.get("key"):
                # A drop not yet priced by the bridge: the last creation price this server read for the title
                last = _news_creation_price(a["key"], snap.get("actor_id"))
                if last is not None:
                    row["price_last_read"] = last
        elif r.get("what") in AI_VETO_WHATS:
            # (1093: -212.9 gold unexplained beside a mercenary company the news named
            # but did not date): every kind the bridge records goes through here, mercenaries and
            # constructions too, each with the call that vetoes that kind
            a = attempt_row(r)
            a.pop("dropped", None)
            goal = a.get("what") == "create_title" and bool(a.get("key")) and a.get("key") == goal_title()
            chain = (a.get("what") == "create_title" and not goal and bool(a.get("key")) and goal_title() is not None
                     and a.get("key") in SPARED_EVER.get(snap.get("actor_id"), ()))
            # (live act, 1075.01.04: a pilgrimage the seat's host_activity put to the AI passed, spared, and read
            # "no veto set for it"): under the activity veto a passed activity is one the seat's host_activity spared
            spared = a.get("what") == "activity" and "activity" in (snap.get("ai_vetoes") or [])
            a["sent_by"] = (VASSAL_OFFER_SENT_BY if a.get("what") == "vassal_offer"
                            else ACTIVITY_SPARED.format(key=a.get("key") or "this type") if spared
                            else CALL_IN_SENT_BY if a.get("what") == "call_in"
                            else GOAL_PASSED.format(key=a["key"]) if goal       # Spared, not unvetoed
                            else CHAIN_PASSED.format(key=a["key"], goal=goal_title()) if chain
                            else "the game's AI for your ruler (no veto set for it)")
            if a.get("what") in ACTED_PRICE_NOTE and "price" not in a:
                a["price_unread"] = ACTED_PRICE_NOTE[a["what"]]
            decision_row_price(a, snap)
            a["veto"] = {"tool": "do", "args": {"intent": "ai_veto", "args": {
                "op": "set", "what": a["what"], **({"keys": [a["key"]]} if a.get("what") == "decision" and a.get("key")
                                                   else {})}}}
            if a.get("what") in ("ally_call", "vassal_offer", "call_in"):
                a.pop("price_unread", None)
                a.pop("key", None)
            if a.get("what") == "call_in":
                a.update(call_in_war(a.get("war_id"), snap), joining_costs=call_in_costs(snap))
            if a.get("what") == "war":
                a.pop("key", None)
            if goal or spared:
                a.pop("veto", None)
            if goal:
                a["goal_title"] = True
            if chain:
                a.pop("veto", None)
                a["goal_chain"] = True
            acted.append(a)
    if dropped:
        out["ai_vetoed"] = list(dropped.values())
    if acted:
        out["ai_acted"] = acted
        # Beside what the AI did, the spending kinds still not vetoed and the one call that sets them
        su = spending_unset(snap.get("ai_vetoes"))
        if su:
            out["ai_veto_spending"] = su
    return out


#: The prestige an ally's joining costs is the script's, by his primary title's tier and
#: your side of the war (redesign.call_ally_cost): read here for each ally_call row
ALLY_PRICE_WHEN = "if he accepts and joins: the game charges the war's attacker by the ally's tier"


async def enrich_news(gw, actor_id, out: dict, vetoes) -> None:
    """The prestige each ally call in ai_vetoed / ai_acted would cost (by the ally's tier and
    your side of that war), and, while the perk veto holds, the points that are waiting."""
    R = _R()
    rows = [r for k in ("ai_vetoed", "ai_acted") for r in out.get(k) or []
            if isinstance(r, dict) and r.get("what") == "ally_call"]
    wars = None
    for r in rows:
        if wars is None:
            wars = await R._all_wars(gw)
        w = R._war_pick(wars, r.get("war_id")) if r.get("war_id") is not None else None
        side = None
        if w is not None:
            side = "attacker" if w.get("attacker_id") == actor_id else (
                "defender" if w.get("defender_id") == actor_id else None)
        tier = R._tier_of(R._char_section(await gw.read("query_character", {"character_id": r.get("ally_id")})))
        c = R.call_ally_cost(tier or None, side)
        r["prestige_if_joined"] = c if c is not None else "unknown: " + (
            "the war was not found in the war list" if w is None else "his tier or your side was not read")
        r["when"] = ALLY_PRICE_WHEN
    if isinstance(vetoes, list) and "perk" in vetoes and actor_id:
        ls = _d(await gw.read("query_lifestyle", {"character_id": actor_id}))
        have = {x.get("lifestyle"): x.get("points") for x in ls.get("lifestyles") or []
                if isinstance(x, dict) and isinstance(x.get("points"), int) and x.get("points") > 0}
        if have:
            now = [p for p in ls.get("unlockable_now") or [] if isinstance(p, str)]
            row = {"points": have, "unlockable_now": now, "held_for_you": PERK_VETO_NOTE}
            if now:
                row["next"] = AI_VETO_YOURSELF["perk"](now[0])
            else:
                row["why_none"] = ("no perk of a lifestyle with a point has every parent held, or no focus is set: "
                                   "query lifestyle lists next_perks")
            out["perk_points"] = row


def news_part(prev: dict, snap: dict) -> dict:
    """events_answered: the picks recorded since the last read (watched rulers only);
    purse_budget: the four buckets when a reserve is set or holds gold."""
    out: dict = {}
    picks = snap.get("event_picks")
    if isinstance(picks, list) and snap.get("event_watched"):
        last = max([p.get("seq") or 0 for p in prev.get("event_picks") or [] if isinstance(p, dict)] or [0])
        new = [pick_row(p) for p in picks if isinstance(p, dict) and (p.get("seq") or 0) > last]
        if new:
            out["events_answered"] = new
            # The picks a rule of yours decided, one short row each, kept on the news' first page (reply_fit's
            # observe `keep`): events_answered is the first list a long reply cuts to a later page, and the purse
            # row of such a pick is there only when gold fell
            mine = [rule_answer_row(r) for r in new if r.get("rule")]
            if mine:
                out["rules_answered"] = mine
    elif picks is None and prev.get("event_watched"):
        out["events_answered_unread"] = "query_event_rules did not answer: the event picks are unknown, not none"
    bud = snap.get("ai_budget")
    if isinstance(bud, dict) and ((bud.get("reserved_max") or 0) > 0 or (bud.get("reserved") or 0) > 0):
        out["budget"] = bud
    out.update(veto_news(prev, snap))
    return out
