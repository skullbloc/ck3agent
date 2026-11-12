"""Pushed warnings: what a seat is told without having to think to look (v4 draft section 6; lane v4instr).

`observe`'s `result.warnings` is a list of one-line items, each
`{"trigger", "topic", "line"}`: the trigger is an id of TRIGGERS (the
enumerated table below, the only producer), the topic a guide page
(`guide.py`) the line names. Every item comes from a mechanical rule over
reads the server makes; no item is written by hand for an episode, and the
table is the whole vocabulary (test_pushed_warnings checks both).

Not the envelope's `warnings` (diagnostics, `{code, detail}`): those say what
the server could not read; these say what the game state means for the seat.

THE TRIGGERS, each with its input:

  succession_splits        observe's succession verdict is `splits`
                           (succession_verdict.verdict, the one definition)
  succession_undetermined  the verdict is `undetermined`
  ruler_minor              query_character `age` (build_char_profile,
                           query_handlers.cpp:571) at least 1 and under
                           ADULT_AGE. Age 0 is not read as a minor: the
                           bridge writes 0 when its age call faults.
  vassal_over_demesne      a direct vassal (query_realm `vassals[].id`,
                           query_handlers.cpp:1605) whose own
                           query_realm `realm_total_counties` (:1540)
                           exceeds the ruler's `domain_size` (:1526, the
                           counties he holds himself)
  refusal_repeated         the seat's last action (ACTION_TOOLS) was refused
                           and had the same tool and arguments as the one
                           before it (ActionMemory)
  start_briefing           at the open (cycle index 0), each risk line of the
                           start briefing (scripts/start_history `brief`,
                           deterministic and hashed), computed by the cell and
                           handed over in BRIEFING_ENV with its sha256 in
                           BRIEFING_SHA_ENV; the topic is BRIEFING_TOPICS[check]
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Any, Mapping, Optional, Sequence

#: The adult age, MALE_ADULT_AGE = FEMALE_ADULT_AGE = 16 in the game's
#: common/defines/00_defines.txt (excerpted under
#: scripts/start_history/fixtures/game/common/defines/; checked by test).
ADULT_AGE = 16

TOPIC_SUCCESSION = "succession"
TOPIC_REGENCY = "regency"
TOPIC_VASSALS = "vassals"
TOPIC_REFUSALS = "refusals"
TOPIC_WARS = "wars"
TOPIC_START = "start"

#: THE TABLE: trigger id -> the guide topic its line names, and the line's form.
TRIGGERS: dict[str, dict[str, str]] = {
    "succession_splits": {
        "topic": TOPIC_SUCCESSION,
        "line": "Succession splits: {heirs} heirs take titles at the next death (first breaker {breaker}); "
                "R falls by what they take. Open guide `succession`."},
    "succession_undetermined": {
        "topic": TOPIC_SUCCESSION,
        "line": "Succession undetermined ({reason}): treat the next death as a risk, not as safe. "
                "Open guide `succession`."},
    "ruler_minor": {
        "topic": TOPIC_REGENCY,
        "line": "Your ruler is {age}, under the adult age {adult}: a regency governs. Open guide `regency`."},
    "vassal_over_demesne": {
        "topic": TOPIC_VASSALS,
        "line": "Vassal {name} ({vassal_id}) holds {counties} counties in his realm; your own demesne is "
                "{demesne}. Open guide `vassals`."},
    "refusal_repeated": {
        "topic": TOPIC_REFUSALS,
        "line": "Your last {tool} was refused ({gate}) with the same arguments as the one before. Read the "
                "reason and change the action; do not send it again unchanged. Open guide `refusals`."},
    "start_briefing": {
        "topic": "",              # per line: BRIEFING_TOPICS[check]
        "line": "Start briefing [{check}]: {text}. Open guide `{topic}`."},
}

#: scripts/start_history/briefing.py CHECKS -> the guide topic (mirror checked by test).
BRIEFING_TOPICS: dict[str, str] = {
    "partition_split": TOPIC_SUCCESSION,
    "partition_duchies": TOPIC_SUCCESSION,
    "no_child_heir": TOPIC_SUCCESSION,
    "minor_ruler": TOPIC_REGENCY,
    "overmighty_vassal": TOPIC_VASSALS,
    "war_at_start": TOPIC_WARS,
    "tributary": TOPIC_WARS,
    "ruler_dead_in_history": TOPIC_START,
}

#: The start briefing the cell hands every seat (one spelling: the cell imports these).
BRIEFING_ENV = "CK3_START_BRIEFING"
BRIEFING_SHA_ENV = "CK3_START_BRIEFING_SHA256"
#: The diagnostic (envelope warning code) when the handed briefing cannot be read.
W_BRIEFING_UNREAD = "start_briefing_unread"

#: The tools whose calls are actions (game writes the seat chooses).
ACTION_TOOLS = ("declare_war", "do", "steer", "set_phase")
#: How many direct vassals the vassal trigger reads (the imprison builder's cap).
VASSAL_READ_CAP = 40


def item(trigger: str, *, topic: Optional[str] = None, **fields: Any) -> dict:
    """One warning, from the table only (a trigger not in it raises)."""
    row = TRIGGERS[trigger]
    t = topic if topic is not None else row["topic"]
    return {"trigger": trigger, "topic": t, "line": row["line"].format(topic=t, **fields)}


def _int(v: Any) -> Optional[int]:
    return v if isinstance(v, int) and not isinstance(v, bool) else None


# ── the triggers over observe's own reads ──────────────────────────────────

def succession_items(succession: Any) -> list[dict]:
    if not isinstance(succession, Mapping):
        return []
    outcome = succession.get("outcome")
    if outcome == "splits":
        b = succession.get("breaker") if isinstance(succession.get("breaker"), Mapping) else {}
        who = b.get("name") or b.get("id") or "unnamed"
        return [item("succession_splits", heirs=succession.get("eligible_heir_count"), breaker=who)]
    if outcome == "undetermined":
        return [item("succession_undetermined", reason=succession.get("undetermined_reason") or "no reason read")]
    return []


def minor_items(character: Any) -> list[dict]:
    age = _int((character or {}).get("age")) if isinstance(character, Mapping) else None
    if age is None or age < 1 or age >= ADULT_AGE:
        return []
    return [item("ruler_minor", age=age, adult=ADULT_AGE)]


def vassal_ids(realm: Any) -> list[int]:
    """The direct vassals query_realm listed, at most VASSAL_READ_CAP."""
    if not isinstance(realm, Mapping):
        return []
    rows = realm.get("vassals") if isinstance(realm.get("vassals"), list) else []
    return [v["id"] for v in rows if isinstance(v, Mapping) and _int(v.get("id")) is not None][:VASSAL_READ_CAP]


def vassal_items(realm: Any, vassal_realms: Mapping[int, Any]) -> list[dict]:
    """A vassal whose realm counts more counties than the ruler's demesne."""
    demesne = _int(realm.get("domain_size")) if isinstance(realm, Mapping) else None
    if demesne is None:
        return []
    names = {v.get("id"): v.get("name") for v in (realm.get("vassals") or []) if isinstance(v, Mapping)}
    out = []
    for vid, reply in vassal_realms.items():
        n = _int(reply.get("realm_total_counties")) if isinstance(reply, Mapping) else None
        if n is not None and n > demesne:
            out.append((n, vid))
    return [item("vassal_over_demesne", name=names.get(vid) or "unnamed", vassal_id=vid, counties=n,
                 demesne=demesne) for n, vid in sorted(out, key=lambda x: (-x[0], x[1]))]


# ── the repeated refusal ────────────────────────────────────────────────────

def refused(envelope: Any) -> bool:
    """A reply that refused the action: `rejected`, or `ok: false` with a gate
    and no `pending` (a pending write is not a refusal)."""
    if not isinstance(envelope, Mapping):
        return False
    if envelope.get("verdict") == "pending":
        return False
    return envelope.get("verdict") == "rejected" or (envelope.get("ok") is False and bool(envelope.get("blocked")))


def args_key(tool: str, arguments: Mapping[str, Any]) -> str:
    return hashlib.sha256(json.dumps([tool, dict(arguments)], sort_keys=True, default=str)
                          .encode("utf-8")).hexdigest()


class ActionMemory:
    """The seat's action calls, newest last: in this process, and in a file
    when the seat and the episode are known, so a resumed session (a new
    server process) remembers the action before its first."""

    def __init__(self, path: Optional[os.PathLike | str] = None, keep: int = 8):
        self.path = Path(path) if path else None
        self.keep = keep
        self.rows: list[dict] = []
        if self.path is not None and self.path.is_file():
            try:
                lines = self.path.read_text(encoding="utf-8").splitlines()
                self.rows = [json.loads(x) for x in lines[-keep:] if x.strip()]
            except (OSError, ValueError):
                self.rows = []

    def record(self, tool: str, arguments: Mapping[str, Any], envelope: Any) -> dict:
        gate = (envelope.get("blocked") or {}).get("gate") if isinstance(envelope, Mapping) and \
            isinstance(envelope.get("blocked"), Mapping) else None
        row = {"tool": tool, "args_sha256": args_key(tool, arguments), "refused": refused(envelope),
               "gate": gate}
        self.rows = (self.rows + [row])[-self.keep:]
        if self.path is not None:
            try:
                self.path.parent.mkdir(parents=True, exist_ok=True)
                with self.path.open("a", encoding="utf-8") as fh:
                    fh.write(json.dumps(row, sort_keys=True) + "\n")
            except OSError:
                pass
        return row

    def items(self) -> list[dict]:
        if len(self.rows) < 2:
            return []
        last, before = self.rows[-1], self.rows[-2]
        if last["refused"] and last["args_sha256"] == before["args_sha256"]:
            return [item("refusal_repeated", tool=last["tool"], gate=last.get("gate") or "no gate named")]
        return []


def memory_path(environ: Mapping[str, str], telemetry_dir: os.PathLike | str) -> Optional[Path]:
    """Where a seat's action memory lives: beside the guide log, per seat."""
    eid = (environ.get("CK3_EPISODE_ID") or "").strip()
    seat = (environ.get("CK3_SEAT_LABEL") or "").strip()
    if not eid or not seat or "/" in eid or "/" in seat:
        return None
    return Path(telemetry_dir) / "guide" / eid / f"actions-{seat}.jsonl"


# ── the start briefing ──────────────────────────────────────────────────────

def briefing_payload(brief: Mapping[str, Any], *, title: str, date: str) -> str:
    """The canonical text the cell hands over: the brief's own sha256 and, per
    risk, its check, subject and text (the facts stay in the bundle)."""
    lines = [{"check": r["check"], "subject": r.get("subject"), "text": r["text"]}
             for r in brief.get("risks") or []]
    return json.dumps({"brief_sha256": brief.get("sha256"), "title": title, "date": date, "lines": lines},
                      sort_keys=True, ensure_ascii=False)


def briefing_env(brief: Mapping[str, Any], *, title: str, date: str) -> dict[str, str]:
    payload = briefing_payload(brief, title=title, date=date)
    return {BRIEFING_ENV: payload, BRIEFING_SHA_ENV: hashlib.sha256(payload.encode("utf-8")).hexdigest()}


def read_briefing(environ: Mapping[str, str]) -> tuple[Optional[dict], Optional[str]]:
    """(payload, None); (None, None) when none was handed over; (None, why)
    when the payload does not match its sha or does not parse."""
    raw = environ.get(BRIEFING_ENV)
    if raw is None:
        return None, None
    want = (environ.get(BRIEFING_SHA_ENV) or "").strip()
    if hashlib.sha256(raw.encode("utf-8")).hexdigest() != want:
        return None, f"{BRIEFING_ENV} does not hash to {BRIEFING_SHA_ENV}"
    try:
        out = json.loads(raw)
    except ValueError as exc:
        return None, f"{BRIEFING_ENV} is not JSON: {exc}"
    if not isinstance(out, dict) or not isinstance(out.get("lines"), list):
        return None, f"{BRIEFING_ENV} carries no lines"
    return out, None


def briefing_items(payload: Optional[Mapping[str, Any]], cycle_index: Any) -> list[dict]:
    """The briefing's lines, at the open only (cycle index 0)."""
    if not payload or cycle_index != 0:
        return []
    out = []
    for ln in payload.get("lines") or []:
        check = ln.get("check")
        topic = BRIEFING_TOPICS.get(check)
        if topic is None:
            continue          # a check this table does not know names no topic: not invented
        out.append(item("start_briefing", topic=topic, check=check, text=str(ln.get("text") or "").rstrip(".")))
    return out


def compose(*, succession: Any, character: Any, realm: Any, vassal_realms: Mapping[int, Any],
            memory: Optional[ActionMemory], briefing: Optional[Mapping[str, Any]],
            cycle_index: Any) -> list[dict]:
    """Every warning for one observe, in a fixed order: the briefing, then the
    standing triggers."""
    return (briefing_items(briefing, cycle_index) + succession_items(succession) + minor_items(character)
            + vassal_items(realm, vassal_realms) + (memory.items() if memory is not None else []))


def triggers_of(items: Sequence[Mapping[str, Any]]) -> list[str]:
    return [i.get("trigger") for i in items]
