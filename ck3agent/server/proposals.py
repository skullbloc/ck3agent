"""Incoming proposals, the server side (design: docs/design/incoming-proposals.md).

An AI ruler's offer to a seated character (a marriage, an alliance, a call to
war, a peace offer, vassalization) is held in the game by the
`ck3agent_proposals` mod and answered by the seat. This module is everything the
server means by that:

  * THE NAMES (the contract's server half): classes, answers, `by` values, the
    debug.log words, template names and outcomes. `FIRST_SET` in
    scripts/proposal_overrides.py and the design's first-set table are checked
    against `KIND_CLASS` (scripts/test_proposals_server.py); the bridge header
    and scripts/proposal_mod.py mirror these names on their side.
  * THE OBSERVE BLOCK (`block`): the seat's own offers as the game lists them
    through the `proposal_list` template, what was answered without the seat
    since its last turn (the game's RESOLVED / VOID lines), its answer policy
    per class, and the hand-back at the turn boundary.
  * THE TWO INTENTS (`answer`, `set_policy`), each refused before anything is
    sent when the mod is not declared, the id is not one this server listed
    for this actor, or an argument is not one of the closed vocabulary.
  * THE LEDGER: `proposals.jsonl`, one row per offer event, per seat
    (`LEDGER_ENV`), each row carrying the seat's session id; the server
    re-reads it at start, because every turn of `ck3agent run` is a new server
    process.

Declared or not is the experiment's (`[episode].proposal_mod`), transcribed into
the seat's environment as `MOD_ENV=1` by scripts/dry_cell.py. Not declared:
`observe` carries no block, no read is made, and both intents refuse
`proposal_mod_not_declared` (a typed state, never "no offers").
"""
from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Mapping, Optional

try:                                    # imported as ck3agent.server.proposals (scripts)
    from . import cycle_cadence as _cc
except ImportError:                     # imported flat as `proposals` (the server)
    import cycle_cadence as _cc         # type: ignore[no-redef]


def _r():
    import redesign  # noqa: PLC0415 — redesign imports this module
    return redesign


# ═══════════════════════════════════════════════════════════════════════════
# 1. THE VOCABULARY (the design's contract section; mirror-checked)
# ═══════════════════════════════════════════════════════════════════════════

#: The seat's environment: the declaration, the ledger path, the session id.
MOD_ENV = "CK3_PROPOSAL_MOD"
MOD_DECLARED = "1"
LEDGER_ENV = "CK3_SEAT_PROPOSALS_OUT"
SESSION_ID_ENV = "CK3_SESSION_ID"          # seat_lineage.SESSION_ID_ENV (checked by test)
#: `<dir>/<session>.proposals.jsonl` while the seat runs; `proposals.jsonl` in the bundle.
SINK_SUFFIX = ".proposals.jsonl"
BUNDLE_NAME = "proposals.jsonl"

#: Application families (how an answer is carried out in the game).
FAMILY_RESEND = "resend"
FAMILY_JOIN_WAR = "join_war"
FAMILY_PEACE = "peace"

#: THE CLASSES. One named constant each; `call_to_war` and `peace` depend on
#: live rows (the hold may not work for them): if those rows show it
#: cannot, delete the constant and its CLASS_SPECS row, nothing else.
CLASS_MARRIAGE = "marriage"
CLASS_ALLIANCE = "alliance"
CLASS_CALL_TO_WAR = "call_to_war"
CLASS_PEACE = "peace"
CLASS_VASSALAGE = "vassalage"

ANSWER_ACCEPT = "accept"
ANSWER_DECLINE = "decline"
ANSWER_NATIVE = "native"
ANSWER_ASK_ME = "ask_me"
#: What a seat may answer one offer with.
ANSWERS = (ANSWER_ACCEPT, ANSWER_DECLINE, ANSWER_NATIVE)
#: A class's standing answer, as the mod stores it (1-4).
POLICY_ANSWERS = (ANSWER_ACCEPT, ANSWER_DECLINE, ANSWER_ASK_ME, ANSWER_NATIVE)
POLICY_CODES = {a: i + 1 for i, a in enumerate(POLICY_ANSWERS)}
#: Absent policy = this, for every class (the mod's trigger mirrors it).
DEFAULT_POLICY = ANSWER_ASK_ME


@dataclass(frozen=True)
class ClassSpec:
    family: str
    kinds: tuple            # the interactions of the class (FIRST_SET's keys)
    lapse_default: str      # the answer silence gets (design "Timing")


CLASS_SPECS: dict[str, ClassSpec] = {
    CLASS_MARRIAGE: ClassSpec(FAMILY_RESEND, ("arrange_marriage_interaction",), ANSWER_NATIVE),
    CLASS_ALLIANCE: ClassSpec(FAMILY_RESEND, ("negotiate_alliance_interaction",), ANSWER_NATIVE),
    CLASS_CALL_TO_WAR: ClassSpec(FAMILY_JOIN_WAR, ("call_ally_interaction",), ANSWER_ACCEPT),
    CLASS_PEACE: ClassSpec(FAMILY_PEACE, ("end_war_attacker_victory_interaction",
                                          "end_war_attacker_white_peace_interaction",
                                          "end_war_attacker_defeat_interaction"), ANSWER_DECLINE),
    CLASS_VASSALAGE: ClassSpec(FAMILY_RESEND, ("offer_vassalization_interaction",), ANSWER_NATIVE),
}
CLASSES = tuple(CLASS_SPECS)
KIND_CLASS = {k: c for c, s in CLASS_SPECS.items() for k in s.kinds}
FAMILY = {k: CLASS_SPECS[c].family for k, c in KIND_CLASS.items()}
#: `native` (hand the offer back to the game's AI) is a re-send: only the resend family has one.
NATIVE_SUPPORTED = frozenset(c for c, s in CLASS_SPECS.items() if s.family == FAMILY_RESEND)
#: A peace kind's winner, read from the seat's side into `you_win` / `white_peace` / `you_lose`.
PEACE_WINNER = {"end_war_attacker_victory_interaction": "attacker",
                "end_war_attacker_white_peace_interaction": None,
                "end_war_attacker_defeat_interaction": "defender"}

#: Who answered (RESOLVED's `<by>` and every ledger row's `by`).
BY_SEAT = "seat"
BY_RULE = "rule"
BY_BOUNDARY = "boundary"
BY_LAPSE = "lapse"
BY_VOID = "void"
BY_CLASSIFIER = "classifier"
BY_VALUES = (BY_SEAT, BY_RULE, BY_BOUNDARY, BY_LAPSE, BY_VOID, BY_CLASSIFIER)
#: The `by` a template call may carry (the rest are the game's own).
TEMPLATE_BY = (BY_SEAT, BY_BOUNDARY, BY_CLASSIFIER)

#: debug.log words (query_script_log proposal_lines).
WORD_HELD = "CK3AGENT_PROPOSAL_HELD"
WORD_RESOLVED = "CK3AGENT_PROPOSAL_RESOLVED"
WORD_VOID = "CK3AGENT_PROPOSAL_VOID"
WORDS = (WORD_HELD, WORD_RESOLVED, WORD_VOID)
#: The fields of each word's line, in order (HELD's are pinned by this lane:
#: docs/design/incoming-proposals.md, "Contract changes (QSRV)").
HELD_FIELDS = ("id", "kind", "class", "actor_id", "recipient_id")
RESOLVED_FIELDS = ("id", "answer", "by", "rule")
VOID_FIELDS = ("id", "reason")

#: Bridge templates (script_action).
T_SEAT = "proposal_seat"
T_POLICY = "proposal_policy"
T_LIST = "proposal_list"
T_ANSWER = "proposal_answer"
TEMPLATES = (T_SEAT, T_POLICY, T_LIST, T_ANSWER)

#: proposal_answer's outcomes and refusal reasons.
OUT_ACCEPT = "applied_accept"
OUT_DECLINE = "applied_decline"
OUT_HANDED_BACK = "handed_back"
OUT_REFUSED = "refused"
OUTCOME_REASONS = ("not_found", "already_resolved", "native_not_supported", "void_sender_dead",
                   "void_subject_dead", "void_war_ended", "void_recipient_dead", "invalid_now",
                   "void_reason_unread")
#: The reasons an offer is void (the VOID line's `<reason>`, and an answer to a
#: void offer's outcome_reason): the game voided it, or it failed the
#: interaction's own conditions immediately before an answer (invalid_now).
VOID_REASONS = ("void_sender_dead", "void_subject_dead", "void_war_ended", "void_recipient_dead",
                "invalid_now", "void_reason_unread")

#: Timing (design "Timing" and "The surface").
MARKER_REFRESH_DAYS = 300
HARD_LAPSE_DAYS = 400
ADULT_AGE = 16

#: The server's refusals (gate, raw_reason). The gates are GATE_TABLE's.
G_NOT_DECLARED = "proposal_mod_not_declared"
G_NOT_YOURS = "not_your_proposal"
G_CLOSED = "proposal_closed"
G_NATIVE = "native_not_supported"
G_CLASS = "class_not_supported"
R_BAD_ARG = "bad_proposal_argument"
R_NOT_LOADED = "proposal_mod_not_loaded"
R_CHECK_UNREAD = "proposal_mod_check_unread"

#: The block's `unread` reasons (never shown as "no offers").
U_CHECK = R_CHECK_UNREAD
U_NOT_LOADED = R_NOT_LOADED
U_LIST_FAILED = "proposal_list_failed"
U_LIST_INCOMPLETE = "proposal_list_incomplete"
U_LIST_INCONSISTENT = "proposal_list_inconsistent"

#: Ledger row kinds: the design's offer events, then this lane's bookkeeping
#: (what the next process of the same seat re-reads).
L_HELD, L_SHOWN, L_ANSWERED, L_HANDED_BACK, L_VOID, L_LAPSED = (
    "held", "shown", "answered", "handed_back", "void", "lapsed")
L_REFUSED = "answer_refused"
L_OPENED, L_LISTED, L_REPORTED = "opened", "listed", "reported"
L_MARKED, L_UNMARKED, L_POLICY = "seat_marked", "seat_unmarked", "policy_set"
LEDGER_KINDS = (L_HELD, L_SHOWN, L_ANSWERED, L_HANDED_BACK, L_VOID, L_LAPSED, L_REFUSED,
                L_OPENED, L_LISTED, L_REPORTED, L_MARKED, L_UNMARKED, L_POLICY)
#: Ledger states the block states.
LEDGER_RECORDING = "recording"
LEDGER_UNRECORDED = "unrecorded"

ANSWER_WITH = "do(intent='answer_proposal', args={'proposal_id': %d, 'answer': %s})"
SET_WITH = ("do(intent='set_policy', args={'policy': 'proposals', 'class': <one of "
            + ", ".join(CLASSES) + ">, 'answer': 'accept'|'decline'|'ask_me'|'native'})")
SILENCE = ("an offer you have not answered by the first observe of your next turn is answered "
           "with its default_at_lapse; ask_me offers wait for you, accept/decline/native classes "
           "are answered by the game without you")


# ═══════════════════════════════════════════════════════════════════════════
# 2. THE DECLARATION
# ═══════════════════════════════════════════════════════════════════════════

def declared(environ: Optional[Mapping[str, str]] = None) -> bool:
    """True only when the seat's environment says `MOD_ENV=1` (the cell writes
    it when the experiment declares `[episode].proposal_mod`)."""
    env = os.environ if environ is None else environ
    return str(env.get(MOD_ENV) or "").strip() == MOD_DECLARED


def sink_path(log_dir, session_id: str) -> Optional[Path]:
    """`<log_dir>/<session_id>.proposals.jsonl`; None without a directory."""
    if not log_dir:
        return None
    return Path(os.path.expanduser(str(log_dir))) / f"{session_id}{SINK_SUFFIX}"


# ═══════════════════════════════════════════════════════════════════════════
# 3. THE STATE AND ITS LEDGER
# ═══════════════════════════════════════════════════════════════════════════

def _line_key(ln: Mapping[str, Any]) -> str:
    return "|".join([str(ln.get("word"))] + [str(f) for f in (ln.get("fields") or [])]
                    + [str(ln.get("date"))])


def _fields(ln: Mapping[str, Any], names: tuple) -> dict:
    f = list(ln.get("fields") or [])
    return {n: (f[i] if i < len(f) else None) for i, n in enumerate(names)}


def _int(v) -> Optional[int]:
    try:
        i = int(v)
    except (TypeError, ValueError):
        return None
    return i if i > 0 else None


@dataclass
class SeatState:
    """What one seat's server knows of its offers, rebuilt from its ledger."""
    path: Optional[Path] = None
    session_id: Optional[str] = None
    loaded: bool = False
    opened: bool = False
    #: actor -> {proposal id -> list item}, the last list read for that actor
    last_list: dict = field(default_factory=dict)
    shown_turn: dict = field(default_factory=dict)        # proposal id -> turn first shown
    marked: dict = field(default_factory=dict)            # actor -> game date the marker was set
    primary: Optional[int] = None
    policies: dict = field(default_factory=dict)          # actor -> class -> {answer, set_at, token}
    held: dict = field(default_factory=dict)              # proposal id -> HELD fields + date
    seen: set = field(default_factory=set)                # line keys already in the ledger
    events: list = field(default_factory=list)            # answered-without-the-seat events
    reported: dict = field(default_factory=dict)          # line key -> turn first reported
    voided: dict = field(default_factory=dict)            # proposal id -> {reason, date} (VOID lines)
    #: proposal id -> {actor_id, class}: every offer this server saw addressed to one
    #: of its actors (HELD, shown, listed, answered). Never dropped on an answer, so
    #: the RESOLVED / VOID line that follows the answer is still known as this
    #: seat's (live qf2).
    offer_to: dict = field(default_factory=dict)
    offset: int = 0
    write_error: Optional[str] = None

    # -- the ledger -------------------------------------------------------
    def ledger_state(self) -> str:
        if self.path is None:
            return f"{LEDGER_UNRECORDED}: no {LEDGER_ENV} in the seat's environment"
        if self.write_error:
            return f"{LEDGER_UNRECORDED}: {self.write_error}"
        return LEDGER_RECORDING

    def write(self, kind: str, **row) -> dict:
        rec = {"kind": kind, "seat_session_id": self.session_id,
               "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), **row}
        self._apply(rec)
        if self.path is not None:
            try:
                self.path.parent.mkdir(parents=True, exist_ok=True)
                with self.path.open("a", encoding="utf-8") as fh:
                    fh.write(json.dumps(rec, sort_keys=True) + "\n")
            except OSError as exc:
                self.write_error = f"{type(exc).__name__}: {exc}"
        return rec

    def load(self) -> None:
        """Re-read this seat's ledger (once per process)."""
        if self.loaded:
            return
        self.loaded = True
        if self.path is None or not self.path.is_file():
            return
        try:
            text = self.path.read_text(encoding="utf-8")
        except OSError as exc:
            self.write_error = f"ledger unreadable at start: {exc}"
            return
        for line in text.splitlines():
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if isinstance(rec, dict):
                self._apply(rec)

    def _apply(self, rec: Mapping[str, Any]) -> None:
        """One row's effect on the state: the same for a row written now and
        a row re-read at start (one reader of the ledger's meaning)."""
        k, pid, actor = rec.get("kind"), _int(rec.get("proposal_id")), _int(rec.get("actor_id"))
        if rec.get("line_key"):
            self.seen.add(rec["line_key"])
        if pid and actor and k in (L_HELD, L_SHOWN, L_ANSWERED, L_HANDED_BACK, L_REFUSED):
            self.offer_to.setdefault(pid, {"actor_id": actor, "class": rec.get("class")})
        if k == L_LISTED and actor:
            for i in rec.get("items") or []:
                if isinstance(i, dict) and _int(i.get("id")):
                    self.offer_to.setdefault(_int(i["id"]), {"actor_id": actor, "class": i.get("class")})
        if k == L_HELD and pid:
            self.held[pid] = {"actor_id": _int(rec.get("from_id")), "recipient_id": actor,
                              "kind": rec.get("interaction"), "class": rec.get("class"),
                              "date": rec.get("game_date")}
        elif k == L_SHOWN and pid:
            self.shown_turn.setdefault(pid, rec.get("turn"))
        elif k == L_LISTED and actor:
            # the whole items: a later process answers from them (class, parties, war)
            self.last_list[actor] = {_int(i.get("id")): dict(i) for i in (rec.get("items") or [])
                                     if isinstance(i, dict) and _int(i.get("id"))}
        elif k in (L_ANSWERED, L_HANDED_BACK, L_VOID, L_LAPSED) and rec.get("source") == "line":
            self.events.append({"line_key": rec.get("line_key"), "id": pid, "class": rec.get("class"),
                                "answer": rec.get("answer"), "by": rec.get("by"),
                                "rule": rec.get("rule"), "date": rec.get("game_date"),
                                "actor_id": actor})
        elif k == L_REPORTED and rec.get("line_key"):
            self.reported.setdefault(rec["line_key"], rec.get("turn"))
        elif k == L_MARKED and actor:
            self.marked[actor] = rec.get("game_date")
            if rec.get("primary"):
                self.primary = actor
        elif k == L_UNMARKED and actor:
            self.marked.pop(actor, None)
        elif k == L_POLICY and actor and rec.get("class"):
            self.policies.setdefault(actor, {})[rec["class"]] = {
                "answer": rec.get("answer"), "set_at": rec.get("game_date"), "token": rec.get("action_token")}
        if k == L_VOID and pid:
            self.voided[pid] = {"reason": rec.get("rule"), "date": rec.get("game_date")}
        elif k == L_REFUSED and pid and rec.get("outcome_reason") in VOID_REASONS:
            # the answer's own check found it void: its VOID line may come later
            self.voided.setdefault(pid, {"reason": rec.get("outcome_reason"), "date": rec.get("game_date")})
        if k in (L_ANSWERED, L_HANDED_BACK, L_VOID, L_LAPSED) and pid:
            for lst in self.last_list.values():
                lst.pop(pid, None)

    def policy_view(self, actor: int) -> dict:
        mine = self.policies.get(actor) or {}
        return {c: ({"answer": mine[c]["answer"], "source": "set", "set_at": mine[c]["set_at"]}
                    if c in mine else {"answer": DEFAULT_POLICY, "source": "default", "set_at": None})
                for c in CLASSES}


_STATE: list = []


def state(environ: Optional[Mapping[str, str]] = None) -> SeatState:
    """THE seat's state for this process (created, then loaded from the ledger, once)."""
    if not _STATE:
        env = os.environ if environ is None else environ
        out = str(env.get(LEDGER_ENV) or "").strip()
        _STATE.append(SeatState(path=Path(os.path.expanduser(out)) if out else None,
                                session_id=str(env.get(SESSION_ID_ENV) or "").strip() or None))
    st = _STATE[0]
    st.load()
    return st


def reset() -> None:
    """Forget this process's state (tests; a new process does it by starting)."""
    _STATE.clear()


# ═══════════════════════════════════════════════════════════════════════════
# 4. SYSTEM 1: the seam a learned answerer plugs into (design, "the system-1 seam")
# ═══════════════════════════════════════════════════════════════════════════

def system1_none(offer: Mapping[str, Any], policies: Mapping[str, Any]
                 ) -> tuple[Optional[str], Optional[str], Optional[float]]:
    """v1: answers nothing; every ask_me offer reaches the seat."""
    return None, None, None


#: (offer, policies) -> (answer | None, rule id, confidence). A learned model
#: replaces this; its answers go out as proposal_answer with by=classifier.
SYSTEM1: list[Callable] = [system1_none]


# ═══════════════════════════════════════════════════════════════════════════
# 5. GAME READS AND TEMPLATE CALLS
# ═══════════════════════════════════════════════════════════════════════════

async def mod_check(gw, actor: int) -> tuple[Optional[bool], Optional[str]]:
    """(proposal_mod_enabled, unread why). (None, why) when the read did not
    come back or carried no reading: a read failure, never "not loaded"."""
    R = _r()
    q = R._unwrap_reply(await gw.read("query_script_log", {"character_id": actor}))
    en = q.get("proposal_mod_enabled")
    if isinstance(en, bool):
        return en, None
    return None, str(q.get("reason") or q.get("error") or "query_script_log carried no proposal_mod_enabled")


async def _template(gw, template: str, params: dict) -> dict:
    return await _r()._script_write(gw, template, params)


def _date(gw) -> Optional[str]:
    d = (gw.game() or {}).get("date")
    return d if isinstance(d, str) else None


def _turn(date: Optional[str], environ=None) -> tuple[Optional[int], Optional[str]]:
    return _cc.episode_cycle(os.environ if environ is None else environ, date)


def _days_between(a: Optional[str], b: Optional[str]) -> Optional[int]:
    oa, ob = _cc.ordinal(a), _cc.ordinal(b)
    return None if oa is None or ob is None else ob - oa


def _plus_days(d: Optional[str], n: int) -> Optional[str]:
    o = _cc.ordinal(d)
    return None if o is None else _cc.date_from_ordinal(o + n)


async def ensure_marker(gw, st: SeatState, actor: int, *, primary: bool) -> dict:
    """The seat marker on `actor`: set when this seat has no record of it, or
    the record is older than MARKER_REFRESH_DAYS (the mod's is a 400-day timed
    variable). When the seat's primary moved (a succession), the marker moves:
    the old primary's is cleared. Returns {seat_marked, marked_at, detail}."""
    now = _date(gw)
    out: dict = {}
    if primary and st.primary and st.primary != actor and st.primary in st.marked:
        old = st.primary
        sent = await _template(gw, T_SEAT, {"actor_id": old, "on": False})
        raw = sent["raw"]
        st.write(L_UNMARKED, actor_id=old, game_date=now, action_token=raw.get("token"),
                 guard=sent["guard"], reason=raw.get("reason"), moved_to=actor)
        out["moved_from"] = old
    age = _days_between(st.marked.get(actor), now) if actor in st.marked else None
    if actor in st.marked and age is not None and 0 <= age <= MARKER_REFRESH_DAYS:
        if primary:
            st.primary = actor
        return dict(out, seat_marked=True, marked_at=st.marked[actor])
    sent = await _template(gw, T_SEAT, {"actor_id": actor, "on": True})
    raw = sent["raw"]
    if raw.get("success") is True and sent["guard"] == "ran":
        st.write(L_MARKED, actor_id=actor, game_date=now, action_token=raw.get("token"), primary=primary)
        if primary:
            st.primary = actor
        return dict(out, seat_marked=True, marked_at=now)
    why = raw.get("reason") or raw.get("error") or f"guard {sent['guard']}"
    return dict(out, seat_marked=False, marked_at=None,
                detail=f"proposal_seat did not read the marker back ({why}); offers to you are not held")


async def read_lines(gw, st: SeatState, actors: set) -> Optional[str]:
    """Fold the game's HELD / RESOLVED / VOID lines since the last read into the
    ledger and the state. Lines about other seats' offers are left out. Returns
    None, or why the lines could not be read."""
    R = _r()
    q = R._unwrap_reply(await gw.read("query_script_log", {"proposal_lines": True,
                                                          "debug_log_offset": st.offset}))
    lines = q.get("lines")
    if not isinstance(lines, list):
        return str(q.get("reason") or q.get("error") or "query_script_log carried no lines")
    nxt = q.get("next_offset")
    for ln in lines:
        if not isinstance(ln, dict) or ln.get("word") not in WORDS:
            continue
        key = _line_key(ln)
        if key in st.seen:
            continue
        date = ln.get("date")
        if ln["word"] == WORD_HELD:
            f = _fields(ln, HELD_FIELDS)
            pid, to = _int(f["id"]), _int(f["recipient_id"])
            if pid and to in actors:
                st.write(L_HELD, proposal_id=pid, actor_id=to, from_id=_int(f["actor_id"]),
                         interaction=f["kind"], **{"class": f["class"]}, by=None, rule=None,
                         game_date=date, line_key=key, source="line")
            continue
        f = _fields(ln, RESOLVED_FIELDS if ln["word"] == WORD_RESOLVED else VOID_FIELDS)
        pid = _int(f["id"])
        h = st.held.get(pid) if pid else None
        known = st.offer_to.get(pid) if pid else None
        if h is None and known is None and not any(pid in lst for lst in st.last_list.values()):
            continue                                    # not an offer to this seat
        to = (h or {}).get("recipient_id") or (known or {}).get("actor_id") or next(
            (a for a, lst in st.last_list.items() if pid in lst), None)
        cls = (h or {}).get("class") or (known or {}).get("class")
        if ln["word"] == WORD_VOID:
            st.write(L_VOID, proposal_id=pid, actor_id=to, **{"class": cls}, answer=None, by=BY_VOID,
                     rule=f["reason"], game_date=date, line_key=key, source="line")
            continue
        by, answer = f["by"], f["answer"]
        kind = (L_LAPSED if by == BY_LAPSE else L_HANDED_BACK if answer == ANSWER_NATIVE else L_ANSWERED)
        extra = {}
        if by == BY_RULE and cls and to:
            extra["rule_set_by_token"] = ((st.policies.get(to) or {}).get(cls) or {}).get("token")
        st.write(kind, proposal_id=pid, actor_id=to, **{"class": cls}, answer=answer, by=by,
                 rule=f["rule"], game_date=date, line_key=key, source="line", **extra)
    if isinstance(nxt, int) and nxt >= 0:
        st.offset = nxt
    return None


async def list_offers(gw, actor: int) -> tuple[Optional[list], Optional[str], Optional[str]]:
    """(items, None, token) or (None, why, token): the proposal_list template.
    `list_complete` false is a refusal, never an empty list; an item that is
    not an offer to `actor` makes the whole list unread (never shown)."""
    sent = await _template(gw, T_LIST, {"actor_id": actor})
    raw = sent["raw"]
    tok = raw.get("token")
    if raw.get("success") is not True:
        return None, f"{U_LIST_FAILED}: {raw.get('reason') or raw.get('error') or 'no reply'}", tok
    if raw.get("list_complete") is not True:
        return None, U_LIST_INCOMPLETE, tok
    items = raw.get("proposals")
    if not isinstance(items, list):
        return None, U_LIST_INCOMPLETE, tok
    for it in items:
        pid = _int(it.get("id")) if isinstance(it, dict) else None
        if (pid is None or _int(it.get("recipient_id")) != actor
                or KIND_CLASS.get(it.get("kind")) != it.get("class")):
            return None, f"{U_LIST_INCONSISTENT}: item {it!r}", tok
    return items, None, tok


async def _name(gw, cid: Optional[int]) -> dict:
    if not cid:
        return {"id": None, "name": None}
    c = await _r()._read_dict(gw, "query_character", {"character_id": cid})
    return {"id": cid, "name": c.get("name") if isinstance(c.get("name"), str) else None,
            "age": c.get("age") if isinstance(c.get("age"), int) else None}


async def resolve_war(gw, item: Mapping[str, Any], seat: int, wars: Optional[list] = None
                      ) -> tuple[Optional[dict], Optional[str]]:
    """The war an offer is about, from the two parties' common wars (the list
    carries only its name). call_to_war: a war the sender fights and the seat
    does not; peace: a war the two lead on opposite sides. (war, None) or
    (None, why): `war_unread`, `war_not_found`, `war_ambiguous`."""
    R = _r()
    if wars is None:
        try:
            wars = await R._all_wars(gw)
        except Exception as exc:  # noqa: BLE001  (a read that failed is not "no war")
            return None, f"war_unread: {type(exc).__name__}"
    frm = _int(item.get("actor_id"))
    cls = item.get("class")
    found = []
    for w in wars:
        sides = {"attacker": set(w["attackers"]) | {w["attacker_id"]},
                 "defender": set(w["defenders"]) | {w["defender_id"]}}
        if cls == CLASS_CALL_TO_WAR:
            side = next((s for s, ids in sides.items() if frm in ids), None)
            if side and seat not in sides["attacker"] | sides["defender"]:
                found.append(dict(w, sender_side=side))
        elif cls == CLASS_PEACE:
            if {w["attacker_id"], w["defender_id"]} == {frm, seat}:
                found.append(dict(w, seat_side="attacker" if w["attacker_id"] == seat else "defender"))
    name = item.get("war_name")
    if name and len(found) > 1:
        found = [w for w in found if w.get("name") == name] or found
    if not found:
        return None, "war_not_found"
    if len(found) > 1:
        return None, "war_ambiguous"
    return found[0], None


async def offered(gw, item: Mapping[str, Any], seat: int, wars) -> dict:
    """The offer's terms, by class, from the seat's side (design, `offers[].offered`)."""
    cls, opts = item.get("class"), [o for o in (item.get("options") or []) if isinstance(o, str)]
    if cls == CLASS_MARRIAGE:
        theirs = await _name(gw, _int(item.get("secondary_actor_id")))
        yours = await _name(gw, _int(item.get("secondary_recipient_id")))
        ages = [x.get("age") for x in (theirs, yours)]
        betrothal = (None if any(a is None for a in ages) else any(a < ADULT_AGE for a in ages))
        return {"their_side": theirs, "your_side": yours, "betrothal": betrothal,
                "matrilineal": "matrilineal" in opts, "options": opts}
    if cls == CLASS_ALLIANCE:
        return {"options": opts}
    if cls == CLASS_VASSALAGE:
        return {"obligations": opts, "options": opts}
    w, why = await resolve_war(gw, item, seat, wars)
    war = ({"war_id": w["war_id"], "name": w.get("name")} if w else
           {"war_id": None, "name": item.get("war_name"), "unresolved": why})
    if cls == CLASS_CALL_TO_WAR:
        return {"war": war, "side": (w or {}).get("sender_side")}
    winner = PEACE_WINNER.get(item.get("kind"))
    if not w:
        terms = None
    elif winner is None:
        terms = "white_peace"
    else:
        terms = "you_win" if winner == w["seat_side"] else "you_lose"
    return {"war": war, "terms": terms}


def _answer_with(pid: int, cls: str) -> str:
    choices = ANSWERS if cls in NATIVE_SUPPORTED else (ANSWER_ACCEPT, ANSWER_DECLINE)
    return ANSWER_WITH % (pid, "|".join(f"'{a}'" for a in choices))


# ═══════════════════════════════════════════════════════════════════════════
# 6. THE OBSERVE BLOCK
# ═══════════════════════════════════════════════════════════════════════════

async def _send_answer(gw, st: SeatState, actor: int, item: Mapping[str, Any], answer: str, by: str,
                       rule: Optional[str], turn) -> dict:
    """proposal_answer, and its ledger row (the call's own; the game's RESOLVED
    line follows as a row of its own)."""
    pid = _int(item.get("id"))
    sent = await _template(gw, T_ANSWER, {"actor_id": actor, "proposal_id": pid, "answer": answer, "by": by})
    raw = sent["raw"]
    out = raw.get("outcome")
    kind = (L_HANDED_BACK if out == OUT_HANDED_BACK else L_ANSWERED if out in (OUT_ACCEPT, OUT_DECLINE)
            else L_REFUSED)
    st.write(kind, proposal_id=pid, actor_id=actor, **{"class": item.get("class")}, answer=answer, by=by,
             rule=rule, outcome=out, outcome_reason=raw.get("outcome_reason") or raw.get("reason"),
             game_date=_date(gw), action_token=raw.get("token"), source="call", turn=turn)
    return {"sent": sent, "outcome": out}


async def block(gw, actor: int, *, primary: bool, environ=None) -> dict:
    """observe's `incoming_proposals`. Call only when `declared()`."""
    st = state(environ)
    now = _date(gw)
    turn, turn_why = _turn(now, environ)
    if not st.opened:
        st.opened = True
        st.write(L_OPENED, actor_id=actor, game_date=now, pid=os.getpid())
    view: dict = {"mod_enabled": None, "seat_marked": None, "policies": st.policy_view(actor),
                  "offers": None, "answered_since_last_turn": [], "unread": None,
                  "turn": turn, "boundary": ("the first observe of each turn" if turn is not None
                                             else f"none: {turn_why}"),
                  "silence": SILENCE, "set_with": SET_WITH, "ledger": st.ledger_state()}
    enabled, why = await mod_check(gw, actor)
    view["mod_enabled"] = enabled
    if enabled is None:
        view["unread"] = f"{U_CHECK}: {why}"
        return view
    if enabled is not True:
        view["unread"] = U_NOT_LOADED
        return view
    mk = await ensure_marker(gw, st, actor, primary=primary)
    view["seat_marked"] = mk["seat_marked"]
    if mk.get("detail"):
        view["seat_marker_detail"] = mk["detail"]
    lines_why = await read_lines(gw, st, set(st.marked) | {actor})
    items, list_why, tok = await list_offers(gw, actor)
    if items is None:
        view["unread"] = list_why
    else:
        # the boundary: an offer shown in an earlier turn gets its default now
        keep = []
        for it in items:
            pid = _int(it["id"])
            shown = st.shown_turn.get(pid)
            if turn is not None and shown is not None and shown < turn:
                d = CLASS_SPECS[it["class"]].lapse_default
                await _send_answer(gw, st, actor, it, d, BY_BOUNDARY, f"lapse_default:{it['class']}={d}", turn)
                continue
            keep.append(it)
        # system 1: an ask_me offer the classifier answers never reaches the seat
        pol = st.policy_view(actor)
        rest = []
        for it in keep:
            ans, rule_id, conf = SYSTEM1[0](it, pol)
            if ans in ANSWERS and not (ans == ANSWER_NATIVE and it["class"] not in NATIVE_SUPPORTED):
                await _send_answer(gw, st, actor, it, ans, BY_CLASSIFIER, rule_id, turn)
                continue
            rest.append(it)
        wars = None                     # read once, by resolve_war, when a war offer needs it
        if any(it["class"] in (CLASS_CALL_TO_WAR, CLASS_PEACE) for it in rest):
            try:
                wars = await _r()._all_wars(gw)
            except Exception:  # noqa: BLE001  (resolve_war reads again and names the failure)
                wars = None
        offers = []
        for it in rest:
            pid, cls = _int(it["id"]), it["class"]
            if pid not in st.shown_turn:
                st.write(L_SHOWN, proposal_id=pid, actor_id=actor, **{"class": cls}, by=None, rule=None,
                         game_date=now, turn=turn, from_id=_int(it.get("actor_id")))
            h = st.held.get(pid) or {}
            arrived = h.get("date")
            age = it.get("age_days_min") if isinstance(it.get("age_days_min"), int) else None
            hard = (_plus_days(arrived, HARD_LAPSE_DAYS) if arrived
                    else _plus_days(now, HARD_LAPSE_DAYS - age) if age is not None else None)
            o = {"id": pid, "kind": it["kind"], "class": cls,
                 "from": {k: v for k, v in (await _name(gw, _int(it.get("actor_id")))).items() if k != "age"},
                 "offered": await offered(gw, it, actor, wars),
                 "arrived": arrived, "age_days_min": age,
                 "lapses": {"at_turn_after_shown": turn is not None, "hard_by": hard,
                            "hard_by_basis": "arrived" if arrived else "age_days_min" if age is not None
                            else None},
                 "default_at_lapse": CLASS_SPECS[cls].lapse_default,
                 "answer_with": _answer_with(pid, cls)}
            if not arrived:
                o["arrived_witness"] = "unwitnessed"
            if CLASS_SPECS[cls].lapse_default == ANSWER_NATIVE:
                o["note"] = ("handed back at lapse, the game's AI answers with the state of that day, "
                             "not of the day the offer arrived")
            offers.append(o)
        view["offers"] = offers
        if len(rest) < len(items):
            # this observe answered some itself: their RESOLVED lines are this turn's news
            lines_why = await read_lines(gw, st, set(st.marked) | {actor}) or lines_why
        st.write(L_LISTED, actor_id=actor, ids=sorted(_int(it["id"]) for it in rest),
                 items=[dict(it) for it in rest], game_date=now, turn=turn, action_token=tok)
    # what was answered without the seat since its last turn (the game's lines)
    shown = []
    for ev in st.events:
        if ev.get("actor_id") not in (actor, None) or ev.get("by") == BY_SEAT:
            continue
        if ev["line_key"] not in st.reported:
            st.write(L_REPORTED, line_key=ev["line_key"], proposal_id=ev["id"], actor_id=actor,
                     game_date=now, turn=turn)
        elif turn is None or st.reported[ev["line_key"]] != turn:
            continue                    # reported in an earlier turn (or once, outside a cell)
        shown.append({k: ev[k] for k in ("id", "class", "answer", "by", "rule", "date")})
    view["answered_since_last_turn"] = shown
    if lines_why:
        view["answered_since_last_turn_unread"] = lines_why
    view["policies"] = st.policy_view(actor)
    view["ledger"] = st.ledger_state()
    return view


# ═══════════════════════════════════════════════════════════════════════════
# 7. THE TWO INTENTS
# ═══════════════════════════════════════════════════════════════════════════

def _refuse(gw, actor, gate: str, raw_reason: str, summary: str, *, result=None, fix=None) -> dict:
    R = _r()
    return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                      blocked=R.write_blocked(gate=gate, raw_reason=raw_reason, summary=summary, fix=fix),
                      bridge_health=gw.health)


def not_declared(gw, actor) -> dict:
    return _refuse(gw, actor, G_NOT_DECLARED, G_NOT_DECLARED,
                   "this episode does not declare the proposal mod: no offer to you is held and "
                   "there is nothing to answer or set")


async def _mod_gate(gw, actor_id: int, actor) -> Optional[dict]:
    enabled, why = await mod_check(gw, actor_id)
    if enabled is None:
        return _refuse(gw, actor, "read_timeout", R_CHECK_UNREAD,
                       f"the proposal mod's presence could not be read ({why}); nothing was sent, ask again")
    if enabled is not True:
        return _refuse(gw, actor, "engine_unreachable", R_NOT_LOADED,
                       "the game has not logged the proposal mod enabled; nothing is held")
    return None


async def set_policy(gw, cls, answer, actor_id: int, actor) -> dict:
    """do(intent='set_policy', args={'policy': 'proposals', 'class', 'answer'})."""
    R = _r()
    if not declared():
        return not_declared(gw, actor)
    if not isinstance(cls, str) or not cls or answer not in POLICY_ANSWERS:
        return _refuse(gw, actor, "missing_required_argument", R_BAD_ARG,
                       f"args.class is one of {', '.join(CLASSES)} and args.answer one of "
                       f"{', '.join(POLICY_ANSWERS)}", fix={"rejected_fields": ["class", "answer"]})
    if cls not in CLASS_SPECS:
        return _refuse(gw, actor, G_CLASS, G_CLASS,
                       f"offers of class {cls!r} are answered by the game's AI; the classes you set "
                       f"are {', '.join(CLASSES)}")
    st = state()
    gate = await _mod_gate(gw, actor_id, actor)
    if gate:
        return gate
    await ensure_marker(gw, st, actor_id, primary=False)
    sent = await _template(gw, T_POLICY, {"actor_id": actor_id, "class": cls, "answer": answer})

    async def landed(_gw, _b):
        # the template's guard is the game's read-back of the variable
        return {"class": cls, "answer": answer} if sent["guard"] == "ran" else None
    v = await R.script_verdict(gw, sent, landed=landed, before=None,
                               what=f"the {cls} answer policy", guard_gate="write_not_applied",
                               guard_reason="proposal_policy_not_read_back", error_gate="engine_refused")
    if v["verdict"] == "applied":
        st.write(L_POLICY, actor_id=actor_id, **{"class": cls}, answer=answer, game_date=_date(gw),
                 action_token=sent["raw"].get("token"), by=None, rule=f"class_policy:{cls}={answer}")
    v["result"] = dict(v["result"], proposal_policies=st.policy_view(actor_id))
    return R._script_envelope("do", v, actor, gw)


#: proposal_answer's refusal reasons -> (gate, how the seat reads it).
_OUTCOME_GATES = {
    "not_found": (G_NOT_YOURS, "the game finds no open offer with that id among yours"),
    "already_resolved": (G_CLOSED, "the offer was already answered"),
    "void_sender_dead": (G_CLOSED, "the sender died; the offer is void"),
    "void_subject_dead": (G_CLOSED, "a person the offer is about died or is no longer eligible"),
    "void_war_ended": (G_CLOSED, "the war the offer is about has ended"),
    "void_recipient_dead": (G_CLOSED, "you died; the offer is void"),
    "invalid_now": ("interaction_invalid", "the game's own conditions for this offer no longer hold (the game "
                                           "would not send it now): it is void, nothing was carried out"),
    "void_reason_unread": (G_CLOSED, "the offer is void; the game kept no reason for it"),
    "native_not_supported": (G_NATIVE, "this class cannot be handed back to the game's AI"),
}


async def _accept_landed(gw, item: Mapping[str, Any], seat: int):
    """The game state an accepted offer leaves (design, "Answer"): a dict when
    seen, None when read and not there, UNREAD when it could not be read."""
    R = _r()
    cls, frm = item.get("class"), _int(item.get("actor_id"))
    if cls == CLASS_MARRIAGE:
        mine = _int(item.get("secondary_recipient_id")) or seat
        theirs = _int(item.get("secondary_actor_id")) or frm
        c = await R._read_dict(gw, "query_character", {"character_id": mine})
        if "spouses" not in c and "betrothed" not in c:
            return R.UNREAD
        spouses = {_int(s.get("id")) for s in (c.get("spouses") or []) if isinstance(s, dict)}
        b = c.get("betrothed") if isinstance(c.get("betrothed"), dict) else None
        if theirs in spouses:
            return {"married": [mine, theirs]}
        if b and _int(b.get("id")) == theirs:
            return {"betrothed": [mine, theirs], "matrilineal": b.get("matrilineal")}
        return None
    if cls == CLASS_ALLIANCE:
        al = await R._read_dict(gw, "query_alliances", {"character_id": seat})
        if "alliances" not in al:
            return R.UNREAD
        allies = {_int(x.get("ally_id")) for x in (al.get("alliances") or []) if isinstance(x, dict)}
        return {"ally_id": frm} if frm in allies else None
    if cls == CLASS_VASSALAGE:
        c = await R._read_dict(gw, "query_character", {"character_id": seat})
        if "liege_id" not in c:
            return R.UNREAD
        return {"liege_id": frm} if _int(c.get("liege_id")) == frm else None
    war = item.get("_war")
    if not war:
        return R.UNREAD
    try:
        wars = await R._all_wars(gw)
    except Exception:  # noqa: BLE001
        return R.UNREAD
    w = R._war_pick(wars, war["war_id"])
    if cls == CLASS_PEACE:
        return {"war_ended": war["war_id"]} if w is None else None
    if w is None:
        return None
    side = w["attackers"] + [w["attacker_id"]] if war.get("sender_side") == "attacker" \
        else w["defenders"] + [w["defender_id"]]
    return {"war_id": war["war_id"], "joined_side": war.get("sender_side")} if seat in side else None


async def _decline_landed(gw, st: SeatState, pid: int, seat: int):
    items, why, _tok = await list_offers(gw, seat)
    lines_why = await read_lines(gw, st, set(st.marked) | {seat})
    if items is None or lines_why:
        return _r().UNREAD
    if any(_int(i.get("id")) == pid for i in items):
        return None
    line = next((e for e in st.events if e.get("id") == pid and e.get("answer") == ANSWER_DECLINE), None)
    return {"proposal_id": pid, "gone_from_list": True, "resolved_line": line["date"]} if line else None


def _void_refusal(gw, st: SeatState, actor, actor_id: int, pid: int, cls, ans: str, reason: str,
                  date: Optional[str], *, sent: bool, base: Optional[dict] = None, raw=None) -> dict:
    """The typed refusal of an answer to a VOID offer: the gate of its reason,
    the reason as raw_reason, and `void` {reason, date, by} in the result. SENT
    says whether a template call reached the game (False: the server knew it was
    void and sent nothing)."""
    gate, words = _OUTCOME_GATES.get(reason, (G_CLOSED, f"the offer is void ({reason})"))
    if not sent:
        st.write(L_REFUSED, proposal_id=pid, actor_id=actor_id, **{"class": cls}, answer=ans, by=BY_SEAT,
                 rule=None, outcome=None, outcome_reason=reason, game_date=_date(gw), action_token=None,
                 source="server", turn=_turn(_date(gw))[0])
    (st.last_list.get(actor_id) or {}).pop(pid, None)
    result = dict(base or {"path": "script", "template": T_ANSWER, "token": None, "proposal_id": pid,
                           "class": cls, "answer": ans, "outcome": None},
                  void={"reason": reason, "date": date, "by": BY_VOID}, sent=sent)
    return _r().envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=result,
                         blocked=_r().write_blocked(gate=gate, raw_reason=reason, summary=words, raw=raw),
                         bridge_health=gw.health)


async def answer(gw, pid_arg, ans, actor_id: int, actor) -> dict:
    """do(intent='answer_proposal', args={'proposal_id', 'answer'})."""
    R = _r()
    if not declared():
        return not_declared(gw, actor)
    pid = pid_arg if isinstance(pid_arg, int) and not isinstance(pid_arg, bool) and pid_arg >= 1 else None
    if pid is None or ans not in ANSWERS:
        return _refuse(gw, actor, "missing_required_argument", R_BAD_ARG,
                       f"args.proposal_id is an offer id from observe's incoming_proposals.offers and "
                       f"args.answer one of {', '.join(ANSWERS)}",
                       fix={"rejected_fields": ["proposal_id", "answer"]})
    st = state()
    item = (st.last_list.get(actor_id) or {}).get(pid)
    if item is None and pid in st.voided:
        v = st.voided[pid]
        return _void_refusal(gw, st, actor, actor_id, pid, (st.held.get(pid) or {}).get("class"), ans,
                             v["reason"] or "void_reason_unread", v["date"], sent=False)
    if item is None:
        return _refuse(gw, actor, G_NOT_YOURS, G_NOT_YOURS,
                       f"offer {pid} is not in the last list of your offers this server read "
                       "(observe() lists them); nothing was sent")
    cls = item.get("class")
    if ans == ANSWER_NATIVE and cls is not None and cls not in NATIVE_SUPPORTED:
        return _refuse(gw, actor, G_NATIVE, G_NATIVE,
                       f"a {cls} offer cannot be handed back to the game's AI; answer accept or decline")
    gate = await _mod_gate(gw, actor_id, actor)
    if gate:
        return gate
    # the game may have voided it since the list was read: its VOID line says so,
    # and nothing is sent (a read that fails leaves the check to the game's own)
    if not await read_lines(gw, st, set(st.marked) | {actor_id}) and pid in st.voided:
        v = st.voided[pid]
        return _void_refusal(gw, st, actor, actor_id, pid, cls, ans, v["reason"] or "void_reason_unread",
                             v["date"], sent=False)
    war = None
    if cls in (CLASS_CALL_TO_WAR, CLASS_PEACE) and ans == ANSWER_ACCEPT:
        war, _why = await resolve_war(gw, item, actor_id)
    turn, _ = _turn(_date(gw))
    res = await _send_answer(gw, st, actor_id, item, ans, BY_SEAT, None, turn)
    sent, out = res["sent"], res["outcome"]
    raw = sent["raw"]
    base = {"path": "script", "template": T_ANSWER, "token": raw.get("token"), "proposal_id": pid,
            "class": cls, "answer": ans, "outcome": out, "raw": raw}
    if raw.get("success") is not True and "success" in raw:
        reason = str(raw.get("reason") or raw.get("error") or "script_action_failed")
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=base,
                          blocked=R.write_blocked(reason, summary=f"{reason}: {raw.get('detail') or ''}"
                                                  .rstrip(": "), raw=raw), bridge_health=gw.health)
    if out == OUT_REFUSED:
        why = str(raw.get("outcome_reason") or "")
        if why == "not_found":
            # a void story ends on its next step: its VOID line names why it is gone
            await read_lines(gw, st, set(st.marked) | {actor_id})
            if pid in st.voided:
                v = st.voided[pid]
                return _void_refusal(gw, st, actor, actor_id, pid, cls, ans, v["reason"] or "void_reason_unread",
                                     v["date"], sent=True, base=base, raw=raw)
        if why in VOID_REASONS:
            v = st.voided.get(pid) or {}
            return _void_refusal(gw, st, actor, actor_id, pid, cls, ans, why, v.get("date") or _date(gw),
                                 sent=True, base=base, raw=dict(raw, engine_reason="\n".join(
                                     e for e in (raw.get("errors") or []) if isinstance(e, str))))
        gate, words = _OUTCOME_GATES.get(why, ("engine_refused", f"the game refused the answer ({why})"))
        errs = [e for e in (raw.get("errors") or []) if isinstance(e, str)]
        said = dict(raw, engine_reason="\n".join(errs))
        (st.last_list.get(actor_id) or {}).pop(pid, None)
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=base,
                          blocked=R.write_blocked(gate=gate, raw_reason=why or "refused", summary=words, raw=said),
                          bridge_health=gw.health)
    if out == OUT_HANDED_BACK:
        (st.last_list.get(actor_id) or {}).pop(pid, None)
        return R.envelope("do", verdict="pending", actor=actor, game=gw.game(), result=base,
                          pending=R.write_pending("readback", game_date_at_submit=_date(gw),
                                                  expected_change="the game's AI answers the re-sent offer",
                                                  raw_reason="handed_back",
                                                  why_pending="handed back: the game's AI answers it within "
                                                              "the interaction's reply days",
                                                  verify_with="observe() and the relation the offer was about"),
                          bridge_health=gw.health)
    if out == OUT_ACCEPT:
        got = await _accept_landed(gw, dict(item, _war=war), actor_id)
        what = f"the {cls} the offer creates"
    elif out == OUT_DECLINE:
        got = await _decline_landed(gw, st, pid, actor_id)
        what = "the offer gone from your list and its RESOLVED line"
    else:
        got, what = R.UNREAD, "the template's outcome"
    (st.last_list.get(actor_id) or {}).pop(pid, None)
    if isinstance(got, dict):
        return R.envelope("do", verdict="applied", result=R.applied_result(dict(base, readback=got)),
                          actor=actor, game=gw.game(), bridge_health=gw.health)
    if got is R.UNREAD:
        return R.envelope("do", verdict="pending", actor=actor, game=gw.game(), result=base,
                          pending=R.write_pending("readback", game_date_at_submit=_date(gw),
                                                  expected_change=what, raw_reason="write_unconfirmed",
                                                  why_pending=f"{what} could not be read yet",
                                                  verify_with="observe() before answering again"),
                          bridge_health=gw.health)
    return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(), result=base,
                      blocked=R.write_blocked(gate="write_not_applied", raw_reason="silent_no_op",
                                              summary=f"the game said {out} and {what} does not show it: "
                                                      "not applied", raw=raw),
                      bridge_health=gw.health)
