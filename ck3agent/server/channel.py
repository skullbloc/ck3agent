"""The inter-seat message channel (pre-registration v3 section 5.2; Study 0 lane s0-channel).

ONE DEFINITION. Everything the channel means lives here and nothing else spells
it a second time: the conditions-block keys (section 9), their environment
transcription, the topology rule (who may write to whom), the bounds (section
7.6), the verdict vocabulary, the ledger's record shapes, the cycle rule, and
where the ledger lives. The readers are:

  * `mcp_server.py` — registers `msg_send` only when `boot()` says the seat's
    arm has a topology other than `none`, writes send attempts through
    `attempt()`, and delivers inboxes inside `observe` through `deliver()`;
  * `ck3agent run` (release/overlay/ck3agent/cli.py) — delivers the inbox at
    the start of a resumed turn through `deliver(via="resume_prompt")` and
    `prompt_block()`;
  * `scripts/registered_launch.py` — parses the keys and transcribes the
    launched arm's topology into the child environment (`transcription_for`);
  * `scripts/episode_bundle.py` / `scripts/dry_cell.py` — declare and
    collect the ledger as a sealed artefact (`LEDGER_NAME`, `ledger_path`);
  * `scripts/channel_ledger.py` — the section 4.2 process measures, pure
    functions over `materialise()`.

THE TOPOLOGY IS KEYED ON SEATS, NOT CHARACTER IDS (section 7.5): a seat is a
lineage, so a rebind after a death changes nothing in who may write to whom.
The channel never needs a character id and never reads one.

THE SERVER PROCESSES SHARE NOTHING BY DEFAULT. N seats are N MCP server
processes on one host; the ledger is one append-only file under the INSTANCE's
telemetry directory (`instance.resolve(...).telemetry_dir`, namespaced by
`CK3_INSTANCE_ID`), under the EPISODE's id, guarded by an fcntl lock. Every
process that knows the instance and the episode sees the same ledger, and the
driver adopts that one file into the bundle at seal. Chosen over writing into
the bundle directory directly because the seats do not know the bundle root
and must not be told it (a seat's world is not the harness's), and over a
socket because a file with a lock needs no fifth process to be alive.

APPEND-ONLY, TWO RECORD KINDS. A `send` record is one row per send attempt; a
`receipt` record is the read receipt the recipient's first `observe` after
delivery stamps. The spec's "one row per send attempt with ... the read
receipt" is the MATERIALISED view (`materialise()`), computed by every reader
from the two kinds, because stamping a receipt onto an existing line would
mean rewriting an append-only file.

THE FIXTURE CANNOT FORGE A DELIVERY. `deliver()` is the only writer of receipt
records and the only reader that returns an inbox, and it has two callers: the
server's `observe` (`via="observe"`) and, since lane v4sees, `ck3agent run` at
the start of a resumed turn (`via="resume_prompt"`, the text in the prompt). A
test that claims a message was read has called one of the two.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import re
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping, Optional, Sequence

try:                                    # imported as ck3agent.server.channel (scripts)
    from . import instance as _instance
    from .cycle_cadence import CycleCounter, wait_completed  # noqa: F401
    from . import cycle_cadence as _cc
except ImportError:                     # imported flat as `channel` (the server)
    import instance as _instance        # type: ignore[no-redef]
    from cycle_cadence import CycleCounter, wait_completed  # type: ignore[no-redef]  # noqa: F401
    import cycle_cadence as _cc         # type: ignore[no-redef]


# ═══════════════════════════════════════════════════════════════════════════
# 1. THE VOCABULARY
# ═══════════════════════════════════════════════════════════════════════════

#: The tool's name on the wire (section 5.1: "the 13 and the one").
TOOL_NAME = "msg_send"

#: The topologies (v4 §3: COUNCIL's complete topology, or none; v3's star is
#: deleted with the arms that used it). `none`: the tool is not registered.
TOPOLOGY_COMPLETE = "complete"
TOPOLOGY_NONE = "none"
TOPOLOGIES = (TOPOLOGY_COMPLETE, TOPOLOGY_NONE)

#: Seat labels (section 3): the king and the dukes D1..Dn.
KING = "K"
_DUKE_RE = re.compile(r"^D[1-9][0-9]*$")

#: Conditions-block keys (section 9). `KEY_TOPOLOGY_PREFIX + <arm>` per arm.
KEY_TOPOLOGY_PREFIX = "channel.topology."
KEY_QUOTA = "channel.quota_per_cycle"
KEY_MAX_BYTES = "channel.max_text_bytes"
#: Section 2.3 / 9: n, from which the seat label set K, D1..Dn is derived.
#: Owned by the census lane's block; read here because the channel needs n.
KEY_N_DUKES = "census.n_dukes"
#: Section 5.1: the one tool the channel arms add to the held-constant 13.
KEY_ARMS_ADD = "tool_surface.channel_arms_add"

#: The environment transcription the launcher establishes for a seat's process
#: (`registered_launch.channel_env`) and the server reads at boot (`boot`).
ENV_ARM = "CK3_EPISODE_ARM"
ENV_TOPOLOGY = "CK3_CHANNEL_TOPOLOGY"
ENV_QUOTA = "CK3_CHANNEL_QUOTA_PER_CYCLE"
ENV_MAX_BYTES = "CK3_CHANNEL_MAX_TEXT_BYTES"
ENV_SEATS = "CK3_CHANNEL_SEATS"            # comma-separated seat labels
#: Per SESSION, not per arm: which seat this server process is.
ENV_SEAT = "CK3_SEAT_LABEL"
#: The episode the ledger is namespaced by. Set per session by the driver after
#: the bundle opens; when absent the server asks the bridge (get_tick_info's
#: pipeline.episode_id, the same id `actions.jsonl` rows carry).
ENV_EPISODE_ID = "CK3_EPISODE_ID"
#: Every arm-level transcription variable, in the order the record lists them.
TRANSCRIPTION_KEYS = (ENV_TOPOLOGY, ENV_QUOTA, ENV_MAX_BYTES, ENV_SEATS)

#: The ledger's verdicts (section 5.2), and the order they are decided in:
#: a recipient the table does not allow is refused before the text is looked
#: at; an over-long text is refused before the quota is counted, because a send
#: that delivered nothing does not spend the quota; the quota is counted over
#: DELIVERED sends of this sender in this cycle.
V_DELIVERED = "delivered"
V_TOPOLOGY_DENIED = "topology_denied"
V_TEXT_TOO_LONG = "text_too_long"
V_QUOTA_EXCEEDED = "quota_exceeded"
VERDICTS = (V_DELIVERED, V_TOPOLOGY_DENIED, V_QUOTA_EXCEEDED, V_TEXT_TOO_LONG)

#: Boot refusals (typed, named by the leg). A seat whose arm has no topology
#: key is a refusal at boot, never a default to `complete` or `none`.
R_TOPOLOGY_UNDECLARED = "channel_topology_undeclared"
R_TOPOLOGY_INVALID = "channel_topology_invalid"
R_SEATS_UNDECLARED = "channel_seats_undeclared"
#: A seat label that is set and is not one of the declared seats: a boot refusal.
R_SEAT_UNKNOWN = "channel_seat_unknown"
#: No seat label at all. NOT a boot refusal: the tool surface is an ARM property
#: (the launcher's fresh-interpreter probe and the bundle's open witness ask
#: "what does this arm present?" with no session in hand), and the label is a
#: SESSION property the driver gives each seat. A send or a delivery from a
#: process with no label is refused by this name (`GATE_UNAVAILABLE`, `W_UNREAD`),
#: and the driver refuses to START a seat without one under a topology other
#: than `none`, so no episode seat ever runs present-and-refused.
R_SEAT_UNDECLARED = "channel_seat_undeclared"
R_BOUNDS_UNDECLARED = "channel_bounds_undeclared"
#: The tool-call refusal (a `blocked.gate`, NOT a ledger verdict) when the
#: ledger cannot be reached: no episode id from the environment or the bridge.
#: Not a verdict because nothing was attempted against any ledger.
GATE_UNAVAILABLE = "channel_unavailable"
#: The tool-call refusal (a `blocked.gate`, NOT a ledger verdict) when the
#: cycle authority cannot say which cycle the send falls in (`send_cycle`):
#: the cell's schedule is unreadable or the game date at the send is. Never a
#: default: the v3reg2 block stamped every send before a resumed session's
#: first wait "cycle 0" (the session-local counter's start), and 18 of its 19
#: quota refusals were false (seat-review.md problem 3).
R_CYCLE_UNKNOWN = "cycle_unknown"
#: Why `observe` could not deliver (a warning code beside an ABSENT `messages`).
W_UNREAD = "channel_unread"

#: Record kinds and the read-receipt absence.
KIND_SEND = "send"
KIND_RECEIPT = "receipt"
UNREAD = "unread"

#: The sealed artefact's name in the bundle.
LEDGER_NAME = "channel_ledger.jsonl"
LEDGER_PRODUCER = "s0-channel"

_POSINT_RE = re.compile(r"^[1-9][0-9]*$")


class ChannelBootRefusal(RuntimeError):
    """A seat that must not boot. `reason` is one of the R_* codes."""

    def __init__(self, reason: str, detail: str):
        self.reason = reason
        self.detail = detail
        super().__init__(f"{reason}: {detail}")

    def as_dict(self) -> dict:
        return {"reason": self.reason, "detail": self.detail}


# ═══════════════════════════════════════════════════════════════════════════
# 2. THE RULES (pure)
# ═══════════════════════════════════════════════════════════════════════════

def seat_labels(n_dukes: int) -> list[str]:
    """K, D1..Dn — section 3's seats for n dukes."""
    n = int(n_dukes)
    if n < 1:
        raise ValueError(f"n_dukes must be >= 1, got {n_dukes!r}")
    return [KING] + [f"D{i}" for i in range(1, n + 1)]


def is_duke(label: str) -> bool:
    return bool(_DUKE_RE.match(label or ""))


def allowed(topology: str, sender: str, recipient: str, seats: Sequence[str]) -> bool:
    """Section 5.2's table: may `sender` write to `recipient`?

    complete: any seat to any OTHER seat. none: nobody. A recipient that is not a seat (a label outside
    the set, the sender itself, a typo) is outside the table: `topology_denied`.
    There is no broadcast address.
    """
    if sender not in seats or recipient not in seats or sender == recipient:
        return False
    return topology == TOPOLOGY_COMPLETE


def text_bytes(text: str) -> int:
    return len(text.encode("utf-8"))


def text_sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


#: THE CYCLE RULE (v3 §5.2 / §7.2: ONE cycle, not two) lives in
#: `cycle_cadence`: under the registered cadence (CK3_CYCLE_GAME_DAYS) a cycle
#: ends when the game date has advanced ≥ the cadence since it began, across
#: the seat's `session(action="wait")` calls; without one, every wait that
#: waited closes a cycle. The quota (`attempt`) is keyed on the index this
#: counter reports, so a seat waiting one day per call does not reset its
#: quota per call. Re-exported here (the import at the top) so the channel's
#: readers name one class.
#:
#: THE CYCLE A SEND OR A RECEIPT IS STAMPED WITH comes from `send_cycle`, not
#: from the counter directly. In a cell (`cycle_cadence.in_cell`) the authority
#: is the cell's schedule over the game date (`CellSchedule.cycle_index`, the
#: index every seat's wait reports), because the counter is per PROCESS: a
#: resumed session is a new server whose counter starts at 0 and moves only at
#: its first wait, so every send before that wait landed in cycle 0's bucket.


def send_cycle(environ: Mapping[str, str], game_date: Any,
               counter: CycleCounter) -> tuple[Optional[int], Optional[str]]:
    """(cycle, None), or (None, why) when the authority cannot answer.

    A cell seat (CK3_EPISODE_HORIZON_START set): the cell schedule's index of
    `game_date` (the bridge's clock read at the call), the same for every seat
    and every session of the episode. A schedule that does not parse, or a date
    that does not read, is (None, why): the caller refuses `cycle_unknown`
    rather than stamp a guess. Not a cell (free play, no schedule): the
    process's own counter, the published rule."""
    cycle, why = _cc.episode_cycle(environ, game_date)
    if why == _cc.CYCLE_NOT_A_CELL:
        return counter.cycle, None
    return cycle, why


# ═══════════════════════════════════════════════════════════════════════════
# 3. THE DECLARATION AT BOOT
# ═══════════════════════════════════════════════════════════════════════════

@dataclass(frozen=True)
class ChannelConfig:
    """What the environment transcription declared for THIS seat's process."""
    arm: str
    topology: str
    seats: tuple[str, ...] = ()
    seat: str = ""
    quota_per_cycle: int = 0
    max_text_bytes: int = 0

    @property
    def enabled(self) -> bool:
        return self.topology != TOPOLOGY_NONE

    def seat_refusal(self) -> Optional[str]:
        """Why this process cannot act as a seat: no label (R_SEAT_UNDECLARED),
        or None when it can."""
        return None if self.seat else R_SEAT_UNDECLARED

    def as_dict(self) -> dict:
        return {"arm": self.arm, "topology": self.topology, "seats": list(self.seats),
                "seat": self.seat, "quota_per_cycle": self.quota_per_cycle,
                "max_text_bytes": self.max_text_bytes, "enabled": self.enabled}


def _posint(environ: Mapping[str, str], key: str) -> int:
    raw = (environ.get(key) or "").strip()
    if not _POSINT_RE.match(raw):
        raise ChannelBootRefusal(
            R_BOUNDS_UNDECLARED,
            f"{key} is {raw!r}; the channel bounds (section 7.6) must be declared "
            "as positive decimal integers, never defaulted")
    return int(raw)


def boot(environ: Mapping[str, str]) -> Optional[ChannelConfig]:
    """The seat's channel declaration, or None in free play, or a refusal.

    No `CK3_EPISODE_ARM`: free play, no channel, nothing registered. An arm
    with no `CK3_CHANNEL_TOPOLOGY`: a refusal (`channel_topology_undeclared`),
    because a seat whose arm has no key must not boot with a guessed one.
    `none`: declared off, the tool is absent. Otherwise the seats and both
    bounds must be declared; the seat label may be absent (see
    R_SEAT_UNDECLARED) but, when set, must be one of the seats.
    """
    arm = (environ.get(ENV_ARM) or "").strip().lower()
    if not arm:
        return None
    raw = environ.get(ENV_TOPOLOGY)
    if raw is None or not raw.strip():
        raise ChannelBootRefusal(
            R_TOPOLOGY_UNDECLARED,
            f"arm {arm!r} boots with no {ENV_TOPOLOGY}: the conditions block "
            f"declares no {KEY_TOPOLOGY_PREFIX}{arm}, or the launcher did not "
            "transcribe it. A missing topology is a refusal, never `complete` "
            "or `none`.")
    topology = raw.strip().lower()
    if topology not in TOPOLOGIES:
        raise ChannelBootRefusal(
            R_TOPOLOGY_INVALID,
            f"{ENV_TOPOLOGY}={raw!r} is not one of {list(TOPOLOGIES)}")
    if topology == TOPOLOGY_NONE:
        return ChannelConfig(arm=arm, topology=topology)
    seats_raw = (environ.get(ENV_SEATS) or "").strip()
    seats = tuple(s.strip() for s in seats_raw.split(",") if s.strip())
    if not seats or KING not in seats or any(s != KING and not is_duke(s) for s in seats):
        raise ChannelBootRefusal(
            R_SEATS_UNDECLARED,
            f"{ENV_SEATS}={seats_raw!r} does not name the seats K, D1..Dn")
    seat = (environ.get(ENV_SEAT) or "").strip()
    if seat and seat not in seats:
        raise ChannelBootRefusal(
            R_SEAT_UNKNOWN,
            f"{ENV_SEAT}={seat!r} is not one of the declared seats {list(seats)}")
    return ChannelConfig(arm=arm, topology=topology, seats=seats, seat=seat,
                         quota_per_cycle=_posint(environ, ENV_QUOTA),
                         max_text_bytes=_posint(environ, ENV_MAX_BYTES))


def transcription_for(topology: str, *, quota_per_cycle: Optional[str] = None,
                      max_text_bytes: Optional[str] = None,
                      n_dukes: Optional[str] = None) -> dict[str, str]:
    """The arm-level environment a launcher establishes from the block's values
    (strings, verbatim from the document). `none` carries only the topology."""
    env = {ENV_TOPOLOGY: topology}
    if topology != TOPOLOGY_NONE:
        env[ENV_QUOTA] = str(quota_per_cycle)
        env[ENV_MAX_BYTES] = str(max_text_bytes)
        env[ENV_SEATS] = ",".join(seat_labels(int(n_dukes)))
    return env


def transcription(environ: Mapping[str, str]) -> dict[str, str]:
    """The arm-level channel variables present in `environ` (a copy)."""
    return {k: environ[k] for k in TRANSCRIPTION_KEYS if environ.get(k)}


def declared_block(environ: Mapping[str, str]) -> dict:
    """What a bundle records at OPEN about the channel it ran under: the
    transcription as seen, or `declared: false` with the state named. Never a
    guess: a missing transcription is `not_declared`, not `none`."""
    raw = environ.get(ENV_TOPOLOGY)
    if raw is None or not raw.strip():
        return {"declared": False, "state": "not_declared", "topology": None}
    topology = raw.strip().lower()
    block: dict = {"declared": True, "state": "declared", "topology": topology}
    if topology != TOPOLOGY_NONE:
        block["quota_per_cycle"] = environ.get(ENV_QUOTA)
        block["max_text_bytes"] = environ.get(ENV_MAX_BYTES)
        block["seats"] = [s for s in (environ.get(ENV_SEATS) or "").split(",") if s]
    return block


def ledger_required(summary: Mapping[str, Any]) -> bool:
    """Is `channel_ledger.jsonl` a REQUIRED artefact of this bundle? Keyed on the
    witness stamped at open: declared, and a topology other than `none`. A
    bundle with no `channel` block predates the lane: a coverage statement,
    never a retroactive requirement."""
    blk = summary.get("channel")
    if not isinstance(blk, Mapping) or not blk.get("declared"):
        return False
    return blk.get("topology") not in (None, TOPOLOGY_NONE)


# ═══════════════════════════════════════════════════════════════════════════
# 4. THE LEDGER
# ═══════════════════════════════════════════════════════════════════════════

def ledger_path(episode_id: str, environ: Optional[Mapping[str, str]] = None) -> Path:
    """Where the episode's ledger lives for THIS instance: under the instance's
    telemetry dir (`CK3_INSTANCE_ID`-namespaced by `instance.resolve`), under
    the episode id. One definition; the server writes here, the driver adopts
    from here."""
    if not episode_id or "/" in episode_id or episode_id in (".", ".."):
        raise ValueError(f"not an episode id: {episode_id!r}")
    paths = _instance.resolve(env=os.environ if environ is None else environ)
    return Path(paths.telemetry_dir) / "channel" / episode_id / LEDGER_NAME


def utc_now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


class Ledger:
    """The append-only per-episode ledger, one file, one lock, N processes."""

    def __init__(self, path: os.PathLike | str, *, episode_id: str, instance_id: str = ""):
        self.path = Path(path)
        self.episode_id = episode_id
        self.instance_id = instance_id

    @property
    def lock_path(self) -> Path:
        return self.path.with_name(self.path.name + ".lock")

    def _locked(self, mode: int):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fh = open(self.lock_path, "a+")
        fcntl.flock(fh.fileno(), mode)
        return fh

    def append(self, record: Mapping[str, Any]) -> dict:
        out = {"episode_id": self.episode_id}
        out.update(record)
        lock = self._locked(fcntl.LOCK_EX)
        try:
            with self.path.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps(out, sort_keys=True) + "\n")
                fh.flush()
                os.fsync(fh.fileno())
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
            lock.close()
        return out

    def records(self) -> list[dict]:
        if not self.path.exists():
            return []
        lock = self._locked(fcntl.LOCK_SH)
        try:
            return read_records(self.path)
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
            lock.close()


def read_records(path: os.PathLike | str) -> list[dict]:
    """Every record in a ledger file, in file order. A line that is not a
    JSON object is a defect of the file and raises: a ledger is evidence."""
    out: list[dict] = []
    with Path(path).open("r", encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            if not line.strip():
                continue
            rec = json.loads(line)
            if not isinstance(rec, dict):
                raise ValueError(f"{path}:{n}: record is not an object")
            out.append(rec)
    return out


def materialise(records: Iterable[Mapping[str, Any]]) -> list[dict]:
    """One row per send attempt, in ledger order, with its read receipt
    (`read_game_date`, `read_tick`, `read_cycle`, `reader`) or `read: "unread"`.
    A receipt naming no send is a defect and raises."""
    rows: list[dict] = []
    by_id: dict[str, dict] = {}
    for rec in records:
        kind = rec.get("kind")
        if kind == KIND_SEND:
            row = dict(rec)
            row["read"] = UNREAD
            rows.append(row)
            by_id[str(rec.get("msg_id"))] = row
        elif kind == KIND_RECEIPT:
            row = by_id.get(str(rec.get("msg_id")))
            if row is None:
                raise ValueError(f"receipt for unknown msg_id {rec.get('msg_id')!r}")
            if row["read"] == UNREAD:          # the FIRST observe stamps; later ones never re-stamp
                row["read"] = "read"
                row["read_game_date"] = rec.get("read_game_date")
                row["read_tick"] = rec.get("read_tick")
                row["read_cycle"] = rec.get("read_cycle")
                row["reader"] = rec.get("reader")
                row["read_utc"] = rec.get("read_utc")
                # a receipt from before lane v4sees has no `via`: only observe delivered then
                row["read_via"] = rec.get("via") or VIA_OBSERVE
                if rec.get("turn") is not None:
                    row["read_turn"] = rec.get("turn")
        else:
            raise ValueError(f"unknown ledger record kind {kind!r}")
    return rows


def delivered_in_cycle(rows: Iterable[Mapping[str, Any]], sender: str, cycle: int) -> int:
    return sum(1 for r in rows
               if r.get("sender") == sender and r.get("cycle") == cycle
               and r.get("verdict") == V_DELIVERED)


def attempt(cfg: ChannelConfig, ledger: Ledger, *, to: str, text: str,
            game_date: Any, tick: Any, cycle: int) -> dict:
    """One send attempt: decide the verdict, append the row, return it.

    `game_date` and `tick` are the bridge's clock stamp at the send, read the
    way the server reads every envelope's game block (the fresh get_tick_info
    reading at tool entry). The decision order is the one `VERDICTS` documents.
    """
    recipient = (to or "").strip()
    nbytes = text_bytes(text)
    if not allowed(cfg.topology, cfg.seat, recipient, cfg.seats):
        verdict = V_TOPOLOGY_DENIED
    elif nbytes > cfg.max_text_bytes:
        verdict = V_TEXT_TOO_LONG
    elif delivered_in_cycle(materialise(ledger.records()), cfg.seat, cycle) >= cfg.quota_per_cycle:
        verdict = V_QUOTA_EXCEEDED
    else:
        verdict = V_DELIVERED
    row = {
        "kind": KIND_SEND,
        "msg_id": uuid.uuid4().hex,
        "instance_id": ledger.instance_id,
        "game_date": game_date,
        "tick": tick,
        "sender": cfg.seat,
        "recipient": recipient,
        "cycle": cycle,
        "bytes": nbytes,
        "text_sha256": text_sha256(text),
        "text": text,
        "verdict": verdict,
        "sent_utc": utc_now_iso(),
    }
    return ledger.append(row)


#: How a message reached its reader (the receipt's `via`; materialised `read_via`).
#: `observe`: inside the recipient's observe. `resume_prompt` (lane v4sees): at
#: the start of the recipient's resumed turn, in the prompt `ck3agent run` sends.
VIA_OBSERVE = "observe"
VIA_RESUME_PROMPT = "resume_prompt"
VIAS = (VIA_OBSERVE, VIA_RESUME_PROMPT)


def deliver(cfg: ChannelConfig, ledger: Ledger, *, game_date: Any, tick: Any,
            cycle: Optional[int], via: str = VIA_OBSERVE,
            turn: Optional[int] = None) -> list[dict]:
    """The recipient's inbox: every DELIVERED, UNREAD message addressed to this
    seat, in ledger order, whose edge the topology ALLOWS re-checked on the
    reader's side (`allowed(topology, sender, me)`; a row the sender's process
    stamped `delivered` under another table, or a hand-edited file, is not
    delivered by trusting the stamp — it stays unread in the ledger, visible to
    the readers); each is stamped read with this observe's clock and the
    recipient's cycle (`send_cycle`; None when the authority could not say, which
    the readers count as `read_cycle_unstamped`, never as cycle 0). The only
    writer of receipts.

    `via` names the path (VIAS): the same inbox, the same receipt, so a message
    is delivered ONCE whichever path reads it first; `turn` is the resumed
    turn's number when `via` is `resume_prompt`."""
    if via not in VIAS:
        raise ValueError(f"via {via!r} is not one of {VIAS}")
    inbox = [r for r in materialise(ledger.records())
             if r.get("recipient") == cfg.seat and r.get("verdict") == V_DELIVERED
             and r.get("read") == UNREAD
             and allowed(cfg.topology, str(r.get("sender") or ""), cfg.seat, cfg.seats)]
    out: list[dict] = []
    for r in inbox:
        ledger.append({
            "kind": KIND_RECEIPT, "msg_id": r["msg_id"], "reader": cfg.seat,
            "read_game_date": game_date, "read_tick": tick, "read_cycle": cycle,
            "read_utc": utc_now_iso(), "via": via,
            **({"turn": turn} if turn is not None else {}),
        })
        out.append({"sender": r.get("sender"), "game_date": r.get("game_date"),
                    "tick": r.get("tick"), "text": r.get("text"), "msg_id": r["msg_id"]})
    return out


# ═══════════════════════════════════════════════════════════════════════════
# 5. THE SEAT'S RUNTIME STATE (one per server process)
# ═══════════════════════════════════════════════════════════════════════════

@dataclass
class SeatState:
    """What a server process keeps between calls: its cycle counter and the
    episode id it resolved (from the environment, else the bridge)."""
    cycles: CycleCounter = field(default_factory=CycleCounter.from_env)
    episode_id: Optional[str] = None
    episode_id_source: Optional[str] = None

    def resolve_episode_id(self, environ: Mapping[str, str],
                           tick_info: Any) -> Optional[str]:
        """The environment's id wins; else the bridge's `pipeline.episode_id`
        (the id `actions.jsonl` rows carry). None when neither says."""
        env_id = (environ.get(ENV_EPISODE_ID) or "").strip()
        if env_id:
            self.episode_id, self.episode_id_source = env_id, "environment"
            return env_id
        if isinstance(tick_info, Mapping):
            pipe = tick_info.get("pipeline")
            if isinstance(pipe, Mapping):
                bid = pipe.get("episode_id")
                if isinstance(bid, str) and bid.strip():
                    self.episode_id, self.episode_id_source = bid.strip(), "bridge"
                    return self.episode_id
        return None


# ═══════════════════════════════════════════════════════════════════════════
# 6. DELIVERY IN THE RESUME PROMPT (lane v4sees)
# ═══════════════════════════════════════════════════════════════════════════

#: The heading of the block `ck3agent run` appends to a resumed turn's prompt.
PROMPT_HEADING = ("Messages delivered to you at the start of this turn (each is delivered once; "
                  "observe will not show them again):")


def prompt_block(messages: Sequence[Mapping[str, Any]]) -> str:
    """The text a resumed turn's prompt carries for `deliver()`'s inbox: each
    message's own text, verbatim, with its sender and the game date it was
    sent. Empty when there is nothing to deliver (the prompt is then the bare
    continuation text)."""
    if not messages:
        return ""
    lines = [PROMPT_HEADING]
    for m in messages:
        lines.append(f"- from {m.get('sender')} (sent {m.get('game_date')}): {m.get('text')}")
    return "\n".join(lines)
