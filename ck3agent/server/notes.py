"""The shared notes: short game facts and tool-use lessons from earlier seated games,
shipped with the toolkit and served to every seat through discover(what='notes').

ONE DEFINITION. The notes live in `notes.json` beside this file, one entry per note.
This module is the only reader: the schema and its limits (`problems`), the content
version (`version`), the on/off switch read at boot (`boot`), and the replies
(`index_reply`, `topic_reply`). mcp_server.py serves them; test_notes.py checks the
file against `problems` and the limits below.

HOW A SEAT LEARNS THEY EXIST. A seat sees the tool list whatever client it runs in
(a client in front of the server may forward the tools and drop the server's
instructions), so the pointer is in the tool list: discover's `what` enum carries
`notes` and its `topic` parameter enumerates the topics. The server instructions
carry the same one-line index for a client that shows them. One other reply carries
notes: the first news of a server (a game's first observation, and the seat's first
observe) carries the turn-one notes (`turn_one` in the file: the notes a seat needs before
its first act, at most TURN_ONE_MAX bytes), because a seat may open no topic at all.

VERSION. `version` is the first 12 hex digits of the sha256 of the notes' canonical
JSON (sorted keys, no whitespace): any change to any field of any note, or to the
topics, changes it. It is in every notes reply, in session(action='health') and in
the server instructions, so a run can record which notes its seats had.

SETS. The file's notes are a pool. The default set, served when CK3_NOTES_SET is unset, is
the pool without the notes that are one start's (`default_drop`), with the file's `turn_one`:
general game rules and tool use only, true of any start. A task names a set (`sets` in the
file) through CK3_NOTES_SET at boot: the set drops the pool's notes that are another start's
(`drop`), adds its own (`add`, the same schema) and has its own `turn_one` (`ireland1066` is
the pool whole, the notes the Ireland tasks are played with). `derive` makes the served
document; every limit applies to each set as it is served, and its version is that
document's. An unknown set refuses the boot (typed).

OFF SWITCH. CK3_NOTES=off at boot turns the notes off (for a comparison with and
without them): `notes` leaves discover's enum, `topic` leaves its schema, the
instructions carry no notes line, and discover(what='notes') is refused like any
unknown subject. Unset or `on` is on. Any other value refuses the boot (typed).
"""
from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping, Optional

NOTES_FILE = Path(__file__).with_name("notes.json")
ENV_VAR = "CK3_NOTES"
SET_ENV = "CK3_NOTES_SET"
SET_KEYS = ("about", "drop", "add", "turn_one")
#: the pool's notes the default set leaves out (the notes of one start)
DEFAULT_DROP = "default_drop"
SET_RE = re.compile(r"^[a-z0-9]+$")
#: A set as served, counted per topic as the JSON list of its rendered notes, the sizes summed
SET_TOTAL_MAX = 7416
FORMAT = 1
KINDS = ("game_rule", "tool_use", "observed")
FIELDS = ("id", "topic", "kind", "scope", "text", "provenance", "added")

#: Limits, in UTF-8 bytes of what a seat is served (`render`) unless named otherwise.
TEXT_MAX = 240             # one note's text: one to three lines
PROVENANCE_MAX = 240
SCOPE_MAX = 40
TOPIC_ABOUT_MAX = 60
TOPIC_REPLY_MAX = 1536     # one topic's rendered notes
TOTAL_MAX = 8192           # every note rendered
INDEX_LINE_MAX = 300       # the instructions line
TURN_ONE_MAX = 1700        # the turn-one notes the first observation carries, rendered
TURN_ONE_LEAD = "the rest: discover(what='notes', topic=...)"
ID_RE = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
TOPIC_RE = re.compile(r"^[a-z]+$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

#: Strings no public note may carry: a host path, an e-mail, a URL, a tracking id,
#: or the names of the machines and lanes the runs were made on. The release export
#: checks the shipped file again with its own list (release/test_export.py).
PRIVATE_RE = re.compile(r"/home/|/mnt/|/run/user|/tmp/|[A-Za-z]:\\\\|@|https?://|\bck-[a-z0-9]{2,14}\b"
                        r"|\bnode\d*\b|\bworkstation\b|\bowner\b|\bsubscription\b|\benv[a-z]+\d+\b"
                        r"|\blane\b|\bbead\b", re.I)


class NotesBootRefusal(Exception):
    def __init__(self, code: str, detail: str):
        super().__init__(f"{code}: {detail}")
        self.code, self.detail = code, detail

    def as_dict(self) -> dict:
        return {"code": self.code, "detail": self.detail}


def _b(s: str) -> int:
    return len(s.encode("utf-8"))


def render(note: Mapping[str, Any]) -> str:
    """One note as a seat reads it: the text, then its kind and scope."""
    return f"{note['text']} [{note['kind']}; {note['scope']}]"


def derive(doc: Mapping[str, Any], name: Optional[str]) -> dict:
    """The document a server serves for set `name` (None: the default set, the file's notes without
    `default_drop`, with the file's turn_one)."""
    base = {k: v for k, v in doc.items() if k not in ("sets", DEFAULT_DROP)}
    if name is None:
        drop = set(doc.get(DEFAULT_DROP) or [])
        return dict(base, notes=[n for n in base.get("notes") or [] if not (isinstance(n, dict) and n.get("id") in drop)])
    s = (doc.get("sets") or {})[name]
    drop = set(s.get("drop") or [])
    return dict(base, notes=[n for n in base.get("notes") or [] if not (isinstance(n, dict) and n.get("id") in drop)]
                + list(s.get("add") or []), turn_one=list(s.get("turn_one") or []))


def set_bytes(doc: Mapping[str, Any]) -> int:
    """A served set's size against SET_TOTAL_MAX: per topic, the JSON list of its notes as rendered, summed."""
    return sum(_b(json.dumps([render(n) for n in doc.get("notes") or [] if isinstance(n, dict) and n.get("topic") == t],
                             ensure_ascii=False)) for t in doc.get("topics") or {})


def _set_problems(doc: Mapping[str, Any]) -> list[str]:
    sets = doc.get("sets")
    if sets is None:
        return []
    if not isinstance(sets, dict) or not sets:
        return ["sets is not a non-empty object"]
    out: list[str] = []
    ids = {n.get("id") for n in doc.get("notes") or [] if isinstance(n, dict)}
    for name, s in sets.items():
        where = f"set {name!r}"
        if not SET_RE.match(str(name)):
            out.append(f"{where}: name is not lower-case letters and digits")
            continue
        if not isinstance(s, dict) or tuple(sorted(s)) != tuple(sorted(SET_KEYS)):
            out.append(f"{where}: keys {sorted(s) if isinstance(s, dict) else s!r}, want {sorted(SET_KEYS)}")
            continue
        if not isinstance(s["about"], str) or not s["about"] or PRIVATE_RE.search(s["about"]):
            out.append(f"{where}: about is empty or private-looking")
        if not isinstance(s["drop"], list) or not isinstance(s["add"], list):
            out.append(f"{where}: drop and add are lists")
            continue
        out += [f"{where}: drops {x!r}, not a note" for x in s["drop"] if x not in ids]
        out += [f"{where}: adds {n.get('id')!r}, an id the file has" for n in s["add"]
                if isinstance(n, dict) and n.get("id") in ids]
        d = derive(doc, name)
        out += [f"{where}: {p}" for p in problems(d)]
        if d.get("turn_one") is None or not s["turn_one"]:
            out.append(f"{where}: turn_one is empty")
        if set_bytes(d) > SET_TOTAL_MAX:
            out.append(f"{where}: {set_bytes(d)} bytes served, over {SET_TOTAL_MAX}")
    return out


def problems(doc: Any) -> list[str]:
    """Everything wrong with a parsed notes file; [] when it may be served."""
    if not isinstance(doc, dict):
        return ["the file is not a JSON object"]
    out: list[str] = _set_problems(doc)
    dd = doc.get(DEFAULT_DROP)
    if dd is not None:
        ids = {n.get("id") for n in doc.get("notes") or [] if isinstance(n, dict)}
        if not isinstance(dd, list) or not all(isinstance(x, str) for x in dd):
            out.append(f"{DEFAULT_DROP} is not a list of note ids")
        else:
            out += [f"{DEFAULT_DROP} names {x!r}, not a note" for x in dd if x not in ids]
            out += [f"turn_one names {x!r}, which {DEFAULT_DROP} leaves out" for x in doc.get("turn_one") or []
                    if x in dd]
            if not out:
                out += [f"the default set: {p}" for p in problems(derive(doc, None))]
    if doc.get("format") != FORMAT:
        out.append(f"format is {doc.get('format')!r}, this reader knows {FORMAT}")
    topics, notes = doc.get("topics"), doc.get("notes")
    if not isinstance(topics, dict) or not topics:
        return out + ["topics is not a non-empty object"]
    if not isinstance(notes, list) or not notes:
        return out + ["notes is not a non-empty list"]
    for t, about in topics.items():
        if not TOPIC_RE.match(str(t)):
            out.append(f"topic {t!r}: not lower-case letters")
        if not isinstance(about, str) or not about or _b(about) > TOPIC_ABOUT_MAX:
            out.append(f"topic {t!r}: about is empty or over {TOPIC_ABOUT_MAX} bytes")
    seen: set = set()
    for i, n in enumerate(notes):
        where = f"note {i}"
        if not isinstance(n, dict):
            out.append(f"{where}: not an object")
            continue
        where = f"note {n.get('id', i)!r}"
        if tuple(sorted(n)) != tuple(sorted(FIELDS)):
            out.append(f"{where}: fields {sorted(n)}, want {sorted(FIELDS)}")
            continue
        if not all(isinstance(n[f], str) and n[f].strip() for f in FIELDS):
            out.append(f"{where}: every field must be a non-empty string")
            continue
        if not ID_RE.match(n["id"]):
            out.append(f"{where}: id is not lower-case words joined by '-'")
        if n["id"] in seen:
            out.append(f"{where}: id repeated")
        seen.add(n["id"])
        if n["topic"] not in topics:
            out.append(f"{where}: topic {n['topic']!r} is not in topics")
        if n["kind"] not in KINDS:
            out.append(f"{where}: kind {n['kind']!r} is not one of {KINDS}")
        if not DATE_RE.match(n["added"]):
            out.append(f"{where}: added is not YYYY-MM-DD")
        for f, cap in (("text", TEXT_MAX), ("provenance", PROVENANCE_MAX), ("scope", SCOPE_MAX)):
            if _b(n[f]) > cap:
                out.append(f"{where}: {f} is {_b(n[f])} bytes, over {cap}")
        if "\n" in n["text"]:
            out.append(f"{where}: text has a line break")
        for f in FIELDS:
            m = PRIVATE_RE.search(n[f])
            if m:
                out.append(f"{where}: {f} carries a private-looking string {m.group(0)!r}")
    for f in ("about",):
        m = PRIVATE_RE.search(str(doc.get(f, "")))
        if m:
            out.append(f"{f} carries a private-looking string {m.group(0)!r}")
    for t in topics:
        rendered = [render(n) for n in notes if isinstance(n, dict) and n.get("topic") == t
                    and all(isinstance(n.get(f), str) for f in ("text", "kind", "scope"))]
        if not rendered:
            out.append(f"topic {t!r} has no note")
        elif _b(json.dumps(rendered, ensure_ascii=False)) > TOPIC_REPLY_MAX:
            out.append(f"topic {t!r}: {_b(json.dumps(rendered, ensure_ascii=False))} bytes rendered, "
                       f"over {TOPIC_REPLY_MAX}")
    t1 = doc.get("turn_one")
    if t1 is not None:
        ids = {n.get("id") for n in notes if isinstance(n, dict)}
        if not isinstance(t1, list) or not t1 or not all(isinstance(x, str) for x in t1):
            out.append("turn_one is not a non-empty list of note ids")
        else:
            out += [f"turn_one names {x!r}, not a note" for x in t1 if x not in ids]
            if len(set(t1)) != len(t1):
                out.append("turn_one repeats a note")
            size = _b(json.dumps(turn_one_texts(doc), ensure_ascii=False))
            if size > TURN_ONE_MAX:
                out.append(f"turn_one renders {size} bytes, over {TURN_ONE_MAX}")
    if not out and set_bytes(derive(doc, None)) > SET_TOTAL_MAX:
        out.append(f"the default set is {set_bytes(derive(doc, None))} bytes served, over {SET_TOTAL_MAX}")
    if not out:
        total = sum(_b(render(n)) for n in notes)
        if total > TOTAL_MAX:
            out.append(f"all notes rendered are {total} bytes, over {TOTAL_MAX}")
    return out


def turn_one_texts(doc: Mapping[str, Any]) -> list:
    """The turn-one notes' texts, in turn_one's order (seats opened zero to two
    topics a run; these ride in the first observation instead)."""
    by = {n.get("id"): n for n in doc.get("notes") or [] if isinstance(n, dict)}
    return [by[x]["text"] for x in doc.get("turn_one") or [] if x in by and isinstance(by[x].get("text"), str)]


def version(doc: Mapping[str, Any]) -> str:
    body = {"format": doc.get("format"), "topics": doc.get("topics"), "notes": doc.get("notes")}
    if doc.get("turn_one") is not None:
        body["turn_one"] = doc.get("turn_one")
    canon = json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(canon.encode("utf-8")).hexdigest()[:12]


def load(path: Path = NOTES_FILE) -> dict:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise NotesBootRefusal("notes_unreadable", f"{path.name}: {type(e).__name__}: {e}") from None
    bad = problems(doc)
    if bad:
        raise NotesBootRefusal("notes_invalid", f"{path.name}: " + "; ".join(bad[:5]))
    return doc


@dataclass
class Notes:
    enabled: bool
    doc: dict = field(default_factory=dict)
    version: Optional[str] = None
    set_name: Optional[str] = None

    @property
    def topics(self) -> list[str]:
        return list(self.doc.get("topics") or {}) if self.enabled else []

    def status(self) -> dict:
        """What session(action='health') reports."""
        if not self.enabled:
            return {"state": "off", "switch": f"{ENV_VAR}=off"}
        out = {"state": "on", "version": self.version, "notes": len(self.doc["notes"])}
        return dict(out, set=self.set_name) if self.set_name else out      # the default set's reply as it was

    def instructions_line(self) -> str:
        if not self.enabled:
            return ""
        return (f"Notes (version {self.version}): game rules and tool use, by topic: "
                + ", ".join(self.topics) + ". Read one with discover(what='notes', topic=...).\n")

    def index_reply(self) -> dict:
        return {"version": self.version,
                "topics": {t: f"{a} ({sum(1 for n in self.doc['notes'] if n['topic'] == t)})"
                           for t, a in self.doc["topics"].items()},
                "read": "discover(what='notes', topic=<topic>)"}

    def turn_one_reply(self) -> Optional[dict]:
        """The first observation's notes (news `turn_one_notes`), or None when the notes are off
        or the file names none."""
        if not self.enabled or not self.doc.get("turn_one"):
            return None
        return {"version": self.version, "read": TURN_ONE_LEAD, "notes": turn_one_texts(self.doc)}

    def topic_reply(self, topic: str) -> Optional[dict]:
        """The topic's notes, or None for a topic that is not one of these."""
        if topic not in self.doc["topics"]:
            return None
        return {"version": self.version, "topic": topic,
                "notes": [render(n) for n in self.doc["notes"] if n["topic"] == topic]}


#: The notes the running server serves (mcp_server sets it at boot); None outside a server.
SERVED: Optional[Notes] = None


def boot(environ: Mapping[str, str], path: Path = NOTES_FILE) -> Notes:
    """The notes this server serves, from the environment's switch and the file."""
    raw = environ.get(ENV_VAR)
    val = (raw or "").strip().lower()
    if val == "off":
        return Notes(False)
    if val not in ("", "on"):
        raise NotesBootRefusal("notes_switch_invalid",
                               f"{ENV_VAR}={raw!r}: the switch takes 'on' or 'off' (unset is on)")
    doc = load(path)
    name = (environ.get(SET_ENV) or "").strip() or None
    if name is not None and name not in (doc.get("sets") or {}):
        raise NotesBootRefusal("notes_set_unknown", f"{SET_ENV}={name!r}: the file's sets are "
                                                    f"{sorted(doc.get('sets') or {})}")
    served = derive(doc, name)
    return Notes(True, served, version(served), name)
