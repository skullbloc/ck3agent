"""The seats' notepads (pre-registration v4 draft sections 3 and 7; lane v4note).

ONE DEFINITION. Everything the notepad means lives here: the two tools' names,
the pads, the conditions-block keys and their environment transcription, the
bounds, the write rule, the verdict vocabulary, the log's record shape, where
the log lives, the replay that turns the log into the pads, and what a bundle
seals. The readers are:

  * `mcp_server.py` — registers `note_read` / `note_write` only when `boot()`
    says the seat's arm declares a pad, and serves both through `note_read_attempt()` and
    `note_write_attempt()`;
  * `scripts/registered_launch.py` — parses the keys, adds the two tools to
    the arm's expected surface (`Declaration.expected_surface`, which the cell's
    surface gate and witness grade through `seat_surface.declared`) and
    transcribes the arm's pads into the seat's environment
    (`transcription_for`);
  * `scripts/dry_cell.py` — transcribes the same into each cell seat's
    environment, and seals the log and the final pads into the bundle
    (`seal`);
  * `scripts/episode_bundle.py` — the bundle's artefact census (`LOG_NAME`,
    `log_required`).

THE TWO PADS. `private`: one per seat, read and written by that seat only
(arms K1 and COUNCIL). `council`: one per episode, read and written by every
seat (arm COUNCIL only). A seat names a pad by its KIND (`private` or
`council`), never by another seat's label: there is no address under which a
seat can reach another seat's private pad. A seat whose arm did not declare the
pad it asks for is refused by name (`notepad_pad_not_declared`).

A PAD BELONGS TO THE SEAT, NOT THE CHARACTER. The pad's key is the seat label
(`private:K`, `private:D3`) and the episode id; no character id enters it. A
rebind after a death changes nothing, and a resumed seat (a new server process)
replays the same log and sees the same pad.

THE LOG IS THE PADS. One append-only file per episode under the INSTANCE's
telemetry directory (the channel ledger's layout, for the channel's reasons:
the seats do not know the bundle root). Every read and every write attempt is
one record, refused or not, stamped with the seat, the cycle, the game date,
the tick and the bytes. The pads are the REPLAY of the applied writes
(`pads()`); each applied write carries the sha256 of the pad it produced, and
the replay checks it, so a log that does not reproduce its own pads raises
rather than being read.

THE WRITE RULE (concurrent council writes lose nothing). Every operation runs
under one exclusive lock over the whole read-decide-append, so two seats never
decide against the same state. `append` adds the text to the pad as it stands
at the lock: two appends both land, in lock order. `replace` must name the
`version` the writer last read; when the pad has moved since (another seat
wrote), it is refused `notepad_version_stale` and nothing is replaced: no seat
ever overwrites text it did not see. The rule is the same for both pads.

THE CYCLE COMES FROM THE CYCLE AUTHORITY. The cell's one schedule
(`cycle_cadence.cell_schedule`, start + k x cadence on the game calendar) at
the bridge's game date read at the call, never a per-process counter that a
resume restarts at zero (the v3reg2 msg_send quota defect: a resumed seat's
sends were stamped cycle 0). When there is no schedule or the date cannot be
read, the operation is refused `notepad_cycle_unknown` and logged with
`cycle: null`, never stamped with a guess.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import re
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Optional, Sequence

try:                                    # imported as ck3agent.server.notepad (scripts)
    from . import instance as _instance
    from . import cycle_cadence as _cc
    from .channel import ENV_ARM, ENV_SEAT, ENV_EPISODE_ID, KING, is_duke  # noqa: F401
except ImportError:                     # imported flat as `notepad` (the server)
    import instance as _instance        # type: ignore[no-redef]
    import cycle_cadence as _cc         # type: ignore[no-redef]
    from channel import ENV_ARM, ENV_SEAT, ENV_EPISODE_ID, KING, is_duke  # type: ignore[no-redef]  # noqa: F401


# ═══════════════════════════════════════════════════════════════════════════
# 1. THE VOCABULARY
# ═══════════════════════════════════════════════════════════════════════════

#: The tools' names on the wire (msg_send's noun_verb form).
TOOL_READ = "note_read"
TOOL_WRITE = "note_write"
TOOLS = (TOOL_READ, TOOL_WRITE)

#: The pads, by kind.
PAD_PRIVATE = "private"
PAD_COUNCIL = "council"
PADS = (PAD_PRIVATE, PAD_COUNCIL)

#: What an arm may declare (`notepad.pads.<arm>`), ONE spelling each: no pad,
#: the private pad (K1), both (COUNCIL). A council pad without private pads is
#: no arm of the design and is refused as an invalid declaration.
DECL_NONE = "none"
DECL_PRIVATE = "private"
DECL_BOTH = "private,council"
DECLARATIONS = {DECL_NONE: (), DECL_PRIVATE: (PAD_PRIVATE,), DECL_BOTH: (PAD_PRIVATE, PAD_COUNCIL)}

#: The write modes. No default: a write names its mode.
MODE_REPLACE = "replace"
MODE_APPEND = "append"
MODES = (MODE_REPLACE, MODE_APPEND)

#: Conditions-block keys. `KEY_PADS_PREFIX + <arm>` per arm; the three bounds;
#: the list of tools the notepad arms add to the held-constant 13.
KEY_PADS_PREFIX = "notepad.pads."
KEY_PRIVATE_MAX = "notepad.private_max_bytes"
KEY_COUNCIL_MAX = "notepad.council_max_bytes"
KEY_WRITE_MAX = "notepad.max_write_bytes"
KEY_BOUNDS = (KEY_PRIVATE_MAX, KEY_COUNCIL_MAX, KEY_WRITE_MAX)
KEY_ARMS_ADD = "tool_surface.notepad_arms_add"

#: The bounds this lane proposes for the v4 registration (the owner rules them;
#: nothing reads these as a default — a seat boots only with declared bounds).
#: private 4096 bytes: about 1k tokens a read, seven of v3's 600-byte messages;
#: council 8192: twice that for up to six writers; a write 1024: a pad takes
#: four full writes to fill, so one write cannot replace a plan wholesale by
#: accident of length. Recorded for the hand-back and the tests' fixtures.
PROPOSED_BOUNDS = {KEY_PRIVATE_MAX: "4096", KEY_COUNCIL_MAX: "8192", KEY_WRITE_MAX: "1024"}

#: The environment transcription (`transcription_for`), read at boot (`boot`).
ENV_PADS = "CK3_NOTEPAD_PADS"
ENV_PRIVATE_MAX = "CK3_NOTEPAD_PRIVATE_MAX_BYTES"
ENV_COUNCIL_MAX = "CK3_NOTEPAD_COUNCIL_MAX_BYTES"
ENV_WRITE_MAX = "CK3_NOTEPAD_MAX_WRITE_BYTES"
TRANSCRIPTION_KEYS = (ENV_PADS, ENV_PRIVATE_MAX, ENV_COUNCIL_MAX, ENV_WRITE_MAX)
#: block key -> env key, for the bounds
BOUND_ENV = {KEY_PRIVATE_MAX: ENV_PRIVATE_MAX, KEY_COUNCIL_MAX: ENV_COUNCIL_MAX,
             KEY_WRITE_MAX: ENV_WRITE_MAX}

#: Verdicts (the log's, and the gate a refused call carries), in decision order.
V_APPLIED = "applied"
V_PAD_UNKNOWN = "notepad_pad_unknown"
V_PAD_NOT_DECLARED = "notepad_pad_not_declared"
V_MODE_INVALID = "notepad_mode_invalid"
V_CYCLE_UNKNOWN = "notepad_cycle_unknown"
V_WRITE_TOO_LONG = "notepad_write_too_long"
V_VERSION_REQUIRED = "notepad_version_required"
V_VERSION_STALE = "notepad_version_stale"
V_PAD_FULL = "notepad_pad_full"
REFUSALS = (V_PAD_UNKNOWN, V_PAD_NOT_DECLARED, V_MODE_INVALID, V_CYCLE_UNKNOWN,
            V_WRITE_TOO_LONG, V_VERSION_REQUIRED, V_VERSION_STALE, V_PAD_FULL)
#: The tool-call refusal before any log (no seat label, no episode id).
GATE_UNAVAILABLE = "notepad_unavailable"

#: Boot refusals (typed).
R_PADS_INVALID = "notepad_pads_invalid"
R_BOUNDS_UNDECLARED = "notepad_bounds_undeclared"
R_SEAT_UNKNOWN = "notepad_seat_unknown"
#: Not a boot refusal (the surface is an arm property; the label a session's):
#: a call from a process with no seat label is refused by this name.
R_SEAT_UNDECLARED = "notepad_seat_undeclared"

KIND_READ = "read"
KIND_WRITE = "write"

#: The sealed artefacts' names in a bundle.
LOG_NAME = "notepad_log.jsonl"
PADS_DIR = "notepads"
PRODUCER = "v4note"

_POSINT_RE = re.compile(r"^[1-9][0-9]*$")


class NotepadBootRefusal(RuntimeError):
    """A seat that must not boot. `reason` is one of the R_* codes."""

    def __init__(self, reason: str, detail: str):
        self.reason = reason
        self.detail = detail
        super().__init__(f"{reason}: {detail}")

    def as_dict(self) -> dict:
        return {"reason": self.reason, "detail": self.detail}


# ═══════════════════════════════════════════════════════════════════════════
# 2. THE DECLARATION AT BOOT
# ═══════════════════════════════════════════════════════════════════════════

def valid_seat(label: str) -> bool:
    return label == KING or is_duke(label)


@dataclass(frozen=True)
class NotepadConfig:
    """What the environment transcription declared for THIS seat's process."""
    pads: tuple[str, ...] = ()
    seat: str = ""
    private_max_bytes: int = 0
    council_max_bytes: int = 0
    max_write_bytes: int = 0

    @property
    def enabled(self) -> bool:
        return bool(self.pads)

    def max_bytes(self, pad: str) -> int:
        return self.council_max_bytes if pad == PAD_COUNCIL else self.private_max_bytes

    def seat_refusal(self) -> Optional[str]:
        return None if self.seat else R_SEAT_UNDECLARED

    def as_dict(self) -> dict:
        return {"pads": list(self.pads), "seat": self.seat,
                "private_max_bytes": self.private_max_bytes,
                "council_max_bytes": self.council_max_bytes,
                "max_write_bytes": self.max_write_bytes, "enabled": self.enabled}


def _posint(environ: Mapping[str, str], key: str) -> int:
    raw = (environ.get(key) or "").strip()
    if not _POSINT_RE.match(raw):
        raise NotepadBootRefusal(
            R_BOUNDS_UNDECLARED,
            f"{key} is {raw!r}; the notepad's bounds must be declared as positive "
            "decimal integers, never defaulted")
    return int(raw)


def boot(environ: Mapping[str, str]) -> Optional[NotepadConfig]:
    """The seat's notepad declaration: None when the environment carries no
    `CK3_NOTEPAD_PADS` (the notepad is not declared: no tool is registered, and
    the seat surface's pin refuses a seat whose arm declared one and was not
    shown it); disabled under `none`; else the pads and all three bounds, which
    must be declared. A seat label, when set, must be a seat (K, D1..Dn)."""
    raw = environ.get(ENV_PADS)
    if raw is None or not raw.strip():
        return None
    decl = raw.strip().lower()
    if decl not in DECLARATIONS:
        raise NotepadBootRefusal(
            R_PADS_INVALID, f"{ENV_PADS}={raw!r} is not one of {list(DECLARATIONS)}")
    if decl == DECL_NONE:
        return NotepadConfig()
    seat = (environ.get(ENV_SEAT) or "").strip()
    if seat and not valid_seat(seat):
        raise NotepadBootRefusal(R_SEAT_UNKNOWN, f"{ENV_SEAT}={seat!r} is not a seat (K, D1..Dn)")
    return NotepadConfig(pads=DECLARATIONS[decl], seat=seat,
                         private_max_bytes=_posint(environ, ENV_PRIVATE_MAX),
                         council_max_bytes=_posint(environ, ENV_COUNCIL_MAX),
                         max_write_bytes=_posint(environ, ENV_WRITE_MAX))


def transcription_for(pads: str, *, private_max_bytes: Optional[str] = None,
                      council_max_bytes: Optional[str] = None,
                      max_write_bytes: Optional[str] = None) -> dict[str, str]:
    """The arm-level environment a launcher establishes from the block's values
    (verbatim strings). `none` carries only the declaration."""
    env = {ENV_PADS: pads}
    if pads != DECL_NONE:
        env[ENV_PRIVATE_MAX] = str(private_max_bytes)
        env[ENV_COUNCIL_MAX] = str(council_max_bytes)
        env[ENV_WRITE_MAX] = str(max_write_bytes)
    return env


def transcription_from_scalars(scalars: Mapping[str, str], arm: str) -> dict[str, str]:
    """The transcription for `arm` from a block's scalars; {} when the block
    declares no `notepad.pads.<arm>` line (the notepad is not declared for the
    arm, and the expected surface carries no notepad tool either)."""
    pads = scalars.get(f"{KEY_PADS_PREFIX}{arm}")
    if pads is None:
        return {}
    return transcription_for(pads, private_max_bytes=scalars.get(KEY_PRIVATE_MAX),
                             council_max_bytes=scalars.get(KEY_COUNCIL_MAX),
                             max_write_bytes=scalars.get(KEY_WRITE_MAX))


def adds_tools(pads: Optional[str]) -> bool:
    """Does an arm declared with `pads` present the two notepad tools?"""
    return pads is not None and pads != DECL_NONE


def declared_block(environ: Mapping[str, str]) -> dict:
    """What a bundle records at OPEN about the notepad the seats ran under.
    Absence is `not_declared`, never `none`."""
    raw = environ.get(ENV_PADS)
    if raw is None or not raw.strip():
        return {"declared": False, "state": "not_declared", "pads": None}
    decl = raw.strip().lower()
    block: dict = {"declared": True, "state": "declared", "pads_declaration": decl,
                   "pads": list(DECLARATIONS.get(decl, ()))}
    if decl != DECL_NONE:
        block.update(private_max_bytes=environ.get(ENV_PRIVATE_MAX),
                     council_max_bytes=environ.get(ENV_COUNCIL_MAX),
                     max_write_bytes=environ.get(ENV_WRITE_MAX))
    return block


def log_required(summary: Mapping[str, Any]) -> bool:
    """Is the notepad log a REQUIRED artefact of this bundle? Keyed on the
    witness stamped at open (`summary["notepad"]`): declared, with a pad. A
    bundle with no block predates the lane: never a retroactive requirement."""
    blk = summary.get("notepad")
    if not isinstance(blk, Mapping) or not blk.get("declared"):
        return False
    return bool(blk.get("pads"))


# ═══════════════════════════════════════════════════════════════════════════
# 3. THE CYCLE (from the cycle authority)
# ═══════════════════════════════════════════════════════════════════════════

def cycle_at(game_date: Any, environ: Optional[Mapping[str, str]] = None) -> Optional[int]:
    """The episode's cycle at `game_date` by the cell's one schedule
    (`cycle_cadence.episode_cycle`, the one derivation `channel.send_cycle`
    reads too), or None when there is no schedule or the date cannot be read.
    Never a per-process count."""
    return _cc.episode_cycle(os.environ if environ is None else environ, game_date)[0]


# ═══════════════════════════════════════════════════════════════════════════
# 4. THE LOG
# ═══════════════════════════════════════════════════════════════════════════

def log_path(episode_id: str, environ: Optional[Mapping[str, str]] = None) -> Path:
    """Where the episode's notepad log lives for THIS instance (the channel
    ledger's layout, beside it)."""
    if not episode_id or "/" in episode_id or episode_id in (".", ".."):
        raise ValueError(f"not an episode id: {episode_id!r}")
    paths = _instance.resolve(env=os.environ if environ is None else environ)
    return Path(paths.telemetry_dir) / "notepad" / episode_id / LOG_NAME


def utc_now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def nbytes(text: str) -> int:
    return len(text.encode("utf-8"))


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def pad_id(pad: str, seat: str) -> str:
    """The pad's key: `council`, or `private:<seat>`."""
    return PAD_COUNCIL if pad == PAD_COUNCIL else f"{PAD_PRIVATE}:{seat}"


def pad_file(pid: str) -> str:
    """The sealed file's name for a pad key (`council.txt`, `private-K.txt`)."""
    return pid.replace(":", "-") + ".txt"


@dataclass
class Pad:
    text: str = ""
    version: int = 0
    updated_by: Optional[str] = None
    updated_game_date: Any = None
    updated_cycle: Optional[int] = None

    def view(self) -> dict:
        return {"text": self.text, "version": self.version, "bytes": nbytes(self.text),
                "sha256": sha256_text(self.text), "updated_by": self.updated_by,
                "updated_game_date": self.updated_game_date, "updated_cycle": self.updated_cycle}


def read_records(path: os.PathLike | str) -> list[dict]:
    """Every record in a log file, in file order. A line that is not a JSON
    object raises: a log is evidence."""
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


def pads(records: Iterable[Mapping[str, Any]]) -> dict[str, Pad]:
    """THE REPLAY: every pad the applied writes produced, keyed by pad id. Each
    applied write's `pad_sha256_after` is checked against the replay; a log
    that does not reproduce its own pads raises."""
    out: dict[str, Pad] = {}
    for rec in records:
        kind = rec.get("kind")
        if kind == KIND_READ:
            continue
        if kind != KIND_WRITE:
            raise ValueError(f"unknown notepad record kind {kind!r}")
        if rec.get("verdict") != V_APPLIED:
            continue
        pid = str(rec.get("pad_id"))
        p = out.setdefault(pid, Pad())
        text = rec.get("text")
        if not isinstance(text, str):
            raise ValueError(f"applied write {rec.get('op_id')!r} carries no text")
        p.text = text if rec.get("mode") == MODE_REPLACE else p.text + text
        p.version += 1
        p.updated_by, p.updated_game_date, p.updated_cycle = rec.get("seat"), rec.get("game_date"), rec.get("cycle")
        if rec.get("pad_sha256_after") != sha256_text(p.text) or rec.get("version_after") != p.version:
            raise ValueError(f"applied write {rec.get('op_id')!r} does not reproduce its pad "
                             f"{pid} (sha or version differs from the replay)")
    return out


class Store:
    """The append-only per-episode log, one file, one lock, N processes. Every
    operation is one exclusive critical section: replay, decide, append."""

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

    def records(self) -> list[dict]:
        if not self.path.exists():
            return []
        lock = self._locked(fcntl.LOCK_SH)
        try:
            return read_records(self.path)
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
            lock.close()

    def transact(self, decide) -> dict:
        """Under the exclusive lock: `decide(pads_now)` returns the record,
        which is appended (fsync'd) before the lock is released."""
        lock = self._locked(fcntl.LOCK_EX)
        try:
            now = pads(read_records(self.path)) if self.path.exists() else {}
            rec = {"episode_id": self.episode_id, "instance_id": self.instance_id}
            rec.update(decide(now))
            with self.path.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps(rec, sort_keys=True) + "\n")
                fh.flush()
                os.fsync(fh.fileno())
            return rec
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
            lock.close()


def _new_op_id() -> str:
    return hashlib.sha256(f"{time.time_ns()}:{os.getpid()}:{os.urandom(8).hex()}".encode()).hexdigest()[:32]


def _pad_refusal(cfg: NotepadConfig, pad: str) -> Optional[str]:
    if pad not in PADS:
        return V_PAD_UNKNOWN
    if pad not in cfg.pads:
        return V_PAD_NOT_DECLARED
    return None


def note_read_attempt(cfg: NotepadConfig, store: Store, *, pad: str, game_date: Any, tick: Any,
         cycle: Optional[int]) -> tuple[dict, Optional[dict]]:
    """One read: (its log record, the pad's view for the caller, or None when
    refused). Decision order: the pad (unknown, then not declared for this
    arm), then the cycle. The log keeps the read's bytes, version and sha256,
    never the text (the replay at that version is the text)."""
    pad = (pad or "").strip().lower()
    holder: dict = {}

    def decide(now: dict[str, Pad]) -> dict:
        verdict = _pad_refusal(cfg, pad) or (V_CYCLE_UNKNOWN if cycle is None else V_APPLIED)
        pid = pad_id(pad, cfg.seat) if pad in PADS else None
        rec = {"kind": KIND_READ, "op_id": _new_op_id(), "seat": cfg.seat, "pad": pad, "pad_id": pid,
               "cycle": cycle, "game_date": game_date, "tick": tick, "verdict": verdict,
               "utc": utc_now_iso()}
        if verdict == V_APPLIED:
            v = now.get(pid, Pad()).view()
            rec.update(bytes=v["bytes"], version=v["version"], pad_sha256=v["sha256"])
            holder["view"] = v
        return rec

    rec = store.transact(decide)
    return rec, holder.get("view")


def note_write_attempt(cfg: NotepadConfig, store: Store, *, pad: str, text: str, mode: str,
          version: Optional[int], game_date: Any, tick: Any, cycle: Optional[int]) -> dict:
    """One write attempt: decide the verdict against the pad as it stands under
    the lock, append the record, return it. Decision order: the pad, the mode,
    the cycle, the write's length, (replace) the version, the pad's bound.
    Nothing is ever truncated."""
    pad = (pad or "").strip().lower()
    mode = (mode or "").strip().lower()
    size = nbytes(text)

    def decide(now: dict[str, Pad]) -> dict:
        pid = pad_id(pad, cfg.seat) if pad in PADS else None
        cur = now.get(pid, Pad()) if pid else Pad()
        after = None
        verdict = _pad_refusal(cfg, pad)
        if verdict is None and mode not in MODES:
            verdict = V_MODE_INVALID
        if verdict is None and cycle is None:
            verdict = V_CYCLE_UNKNOWN
        if verdict is None and size > cfg.max_write_bytes:
            verdict = V_WRITE_TOO_LONG
        if verdict is None and mode == MODE_REPLACE:
            if version is None:
                verdict = V_VERSION_REQUIRED
            elif version != cur.version:
                verdict = V_VERSION_STALE
        if verdict is None:
            after = text if mode == MODE_REPLACE else cur.text + text
            if nbytes(after) > cfg.max_bytes(pad):
                verdict = V_PAD_FULL
        verdict = verdict or V_APPLIED
        rec = {"kind": KIND_WRITE, "op_id": _new_op_id(), "seat": cfg.seat, "pad": pad, "pad_id": pid,
               "mode": mode, "cycle": cycle, "game_date": game_date, "tick": tick,
               "bytes": size, "text_sha256": sha256_text(text), "version_seen": version,
               "version_before": cur.version, "pad_bytes_before": nbytes(cur.text),
               "verdict": verdict, "utc": utc_now_iso()}
        if verdict == V_APPLIED:
            rec.update(text=text, version_after=cur.version + 1, pad_bytes_after=nbytes(after),
                       pad_sha256_after=sha256_text(after))
        return rec

    return store.transact(decide)


# ═══════════════════════════════════════════════════════════════════════════
# 5. THE SEAL
# ═══════════════════════════════════════════════════════════════════════════

def expected_pad_ids(pads_declared: Sequence[str], seats: Sequence[str]) -> list[str]:
    """Every pad an episode with these pads and seats owns, written to or not."""
    out = [pad_id(PAD_PRIVATE, s) for s in seats] if PAD_PRIVATE in pads_declared else []
    if PAD_COUNCIL in pads_declared:
        out.append(PAD_COUNCIL)
    return out


def seal(records: Sequence[Mapping[str, Any]], *, pads_declared: Sequence[str],
         seats: Sequence[str]) -> tuple[dict[str, str], dict]:
    """(files, report): the final pads as files (`notepads/<pad>.txt`, every
    declared pad of every seat, an untouched one empty) and the report per pad
    and per seat. A pad the log holds that the declaration does not name is
    sealed too and listed under `undeclared_pads` (the log is the evidence)."""
    final = pads(records)
    ids = expected_pad_ids(pads_declared, seats)
    undeclared = sorted(set(final) - set(ids))
    files, per_pad = {}, {}
    for pid in ids + undeclared:
        p = final.get(pid, Pad())
        files[f"{PADS_DIR}/{pad_file(pid)}"] = p.text
        per_pad[pid] = {k: v for k, v in p.view().items() if k != "text"}
        per_pad[pid].update(
            writes_applied=sum(1 for r in records if r.get("kind") == KIND_WRITE
                               and r.get("pad_id") == pid and r.get("verdict") == V_APPLIED),
            writes_refused=sum(1 for r in records if r.get("kind") == KIND_WRITE
                               and r.get("pad_id") == pid and r.get("verdict") != V_APPLIED),
            reads=sum(1 for r in records if r.get("kind") == KIND_READ and r.get("pad_id") == pid
                      and r.get("verdict") == V_APPLIED))
    per_seat: dict[str, dict] = {}
    for r in records:
        s = per_seat.setdefault(str(r.get("seat")), {"reads": 0, "writes": 0, "refused": 0})
        if r.get("verdict") != V_APPLIED:
            s["refused"] += 1
        elif r.get("kind") == KIND_READ:
            s["reads"] += 1
        else:
            s["writes"] += 1
    return files, {"pads": per_pad, "seats": per_seat, "records": len(records),
                   "undeclared_pads": undeclared,
                   "cycle_unknown": sum(1 for r in records if r.get("verdict") == V_CYCLE_UNKNOWN)}
