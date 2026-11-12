"""The seats' guide, served page by page (pre-registration v4 draft section 6; lane v4instr).

WHY A TOOL. A seat is locked down (no file tools, slash commands off:
ck3bench/runner/seat_surface.py), so strategy context cannot be skills or
files it opens. The harness serves it like the notepad: one tool,
`guide_read(topic)`, registered only when the seat's environment declares a
guide. One page per call; a page may name deeper pages; an unknown topic
returns the index. The seat's play file carries only the index (one line per
topic saying WHEN to open it), so the context grows only with what the seat
asks for (progressive disclosure).

ONE DEFINITION. Everything the guide means lives here: the tool's name, the
conditions-block keys and their environment transcription, the manifest's
shape and its checks, the index's rendering, the play file each role boots
with, the fetch log's record shape and where it lives, and what a bundle
seals. The readers are:

  * `mcp_server.py`: boots the guide (`boot`), registers `guide_read` only
    when it is declared, serves it through `read_attempt`;
  * `scripts/registered_launch.py`: parses the keys, adds the tool to every
    arm's expected surface when the block declares a guide, transcribes the
    declaration (`transcription_for`) and compares the launch with the
    bundle's open stamp (`cross_check`);
  * `scripts/dry_cell.py`: transcribes the same into each seat's environment
    and seals the fetch log (`seal`) into the bundle;
  * `ck3bench/runner/cell.py`: writes each seat's play file from the role
    (`role_of`, `Guide.play_text`) and witnesses its sha256 beside the
    manifest's (`play_witness`).

THE CONTENT IS IN-REPO, NOT SHIPPED: a directory of topic pages and a
`manifest.json` naming each page's file, sha256, bytes, when to open it, and
the deeper pages it names, plus each role's play file. The manifest's sha256
is the registered condition (`guide.manifest_sha256`); the cell hands the
directory and that sha to every seat (`CK3_GUIDE_DIR`,
`CK3_GUIDE_MANIFEST_SHA256`), and the server refuses to boot when the
manifest's bytes are not the declared sha, or a page's bytes are not the
manifest's. One declaration for the whole block: every seated arm gets the
same content (there is no per-arm line to differ).

EVERY FETCH IS LOGGED: one append-only file per episode under the instance's
telemetry directory (the notepad's layout), one record per call: the seat,
the cycle from the cycle authority (`cycle_cadence.episode_cycle`, via
`notepad.cycle_at`'s rule; `null` with the reason when it cannot be read,
never a guess), the game date, the topic asked and the topic served, the
bytes and the page's sha256. A page whose bytes changed since boot is
refused `guide_page_changed`, never served.
"""
from __future__ import annotations

import fcntl
import hashlib
import json
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping, Optional, Sequence

try:                                    # imported as ck3agent.server.guide (scripts)
    from . import instance as _instance
    from . import cycle_cadence as _cc
    from .channel import ENV_SEAT, ENV_EPISODE_ID, KING, is_duke
except ImportError:                     # imported flat as `guide` (the server)
    import instance as _instance        # type: ignore[no-redef]
    import cycle_cadence as _cc         # type: ignore[no-redef]
    from channel import ENV_SEAT, ENV_EPISODE_ID, KING, is_duke  # type: ignore[no-redef]


# ═══════════════════════════════════════════════════════════════════════════
# 1. THE VOCABULARY
# ═══════════════════════════════════════════════════════════════════════════

TOOL = "guide_read"
TOOLS = (TOOL,)
INDEX_TOPIC = "index"

#: Conditions-block keys: the manifest's sha256 (one line for the block) and the
#: tool the guide adds to the held-constant 13.
KEY_MANIFEST_SHA = "guide.manifest_sha256"
KEY_ARMS_ADD = "tool_surface.guide_arms_add"

#: The environment transcription (`transcription_for`), read at boot (`boot`).
ENV_DIR = "CK3_GUIDE_DIR"
ENV_MANIFEST_SHA = "CK3_GUIDE_MANIFEST_SHA256"
TRANSCRIPTION_KEYS = (ENV_DIR, ENV_MANIFEST_SHA)

#: Where the experiment's guide lives in the research tree (the cell and the
#: launcher resolve it against their repository root).
REPO_GUIDE_REL = "scripts/prompts/v4-1066"
MANIFEST_NAME = "manifest.json"
MANIFEST_FORMAT = 1
#: A page's bound: about 1.5k tokens of English prose.
PAGE_MAX_BYTES = 6000

#: The roles a seat plays, and how a seat label selects one.
ROLE_KING = "king"
ROLE_DUKE = "duke"
ROLES = (ROLE_KING, ROLE_DUKE)
#: The two files the manifest declares per role: the play file (the seat's
#: play-dir CLAUDE.md) and the goal text (the seat's `ck3agent run --goal`).
ROLE_FILE_KINDS = ("play", "goal")

#: Boot refusals (typed).
R_SHA_UNDECLARED = "guide_manifest_sha_undeclared"
R_DIR_UNDECLARED = "guide_dir_undeclared"
R_MANIFEST_UNREADABLE = "guide_manifest_unreadable"
R_MANIFEST_SHA = "guide_manifest_sha_mismatch"
R_MANIFEST_INVALID = "guide_manifest_invalid"
R_PAGE_SHA = "guide_page_sha_mismatch"
R_PAGE_TOO_LONG = "guide_page_too_long"
R_LINK_UNKNOWN = "guide_link_unknown"
R_PAGE_UNREACHABLE = "guide_page_unreachable"
BOOT_REFUSALS = (R_SHA_UNDECLARED, R_DIR_UNDECLARED, R_MANIFEST_UNREADABLE, R_MANIFEST_SHA,
                 R_MANIFEST_INVALID, R_PAGE_SHA, R_PAGE_TOO_LONG, R_LINK_UNKNOWN, R_PAGE_UNREACHABLE)

#: A fetch's verdicts.
V_PAGE = "page"                       # the topic asked for
V_INDEX = "index"                     # the index, asked for by name
V_INDEX_UNKNOWN = "index_for_unknown_topic"   # an unknown topic: the index instead
V_PAGE_CHANGED = "guide_page_changed"         # refused: the bytes moved since boot
SERVED = (V_PAGE, V_INDEX, V_INDEX_UNKNOWN)
#: The tool-call refusal before any log (no declaration, no seat, no episode).
GATE_UNAVAILABLE = "guide_unavailable"

#: The sealed artefact's name in a bundle.
LOG_NAME = "guide_log.jsonl"
PRODUCER = "v4instr"

_TOPIC_RE = re.compile(r"^[a-z][a-z0-9_]*$")
_SHA_RE = re.compile(r"^[0-9a-f]{64}$")


class GuideBootRefusal(RuntimeError):
    """A seat that must not boot. `reason` is one of BOOT_REFUSALS."""

    def __init__(self, reason: str, detail: str):
        self.reason = reason
        self.detail = detail
        super().__init__(f"{reason}: {detail}")

    def as_dict(self) -> dict:
        return {"reason": self.reason, "detail": self.detail}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_text(text: str) -> str:
    return sha256_bytes(text.encode("utf-8"))


def role_of(label: str) -> Optional[str]:
    """The role a seat label plays: K the king, D1..Dn a duke, else None."""
    if label == KING:
        return ROLE_KING
    if is_duke(label):
        return ROLE_DUKE
    return None


# ═══════════════════════════════════════════════════════════════════════════
# 2. THE MANIFEST
# ═══════════════════════════════════════════════════════════════════════════

@dataclass(frozen=True)
class Page:
    topic: str
    file: str
    sha256: str
    bytes: int
    when: Optional[str]              # None: a deeper page, reached by a link
    links: tuple[str, ...] = ()


@dataclass
class Guide:
    """A loaded, checked guide: the manifest's bytes matched the declared sha,
    every page's bytes matched the manifest at load."""
    root: Path
    manifest_sha256: str
    name: str
    pages: dict[str, Page]
    play: dict[str, dict]
    #: role -> {file, sha256}: the seat's GOAL text (`ck3agent run --goal`),
    #: one per role beside the play file, declared by the same manifest
    goal: dict[str, dict] = field(default_factory=dict)
    texts: dict[str, str] = field(default_factory=dict)

    def index_text(self) -> str:
        return render_index(self.pages.values())

    def page_text(self, topic: str) -> tuple[Optional[str], Optional[str]]:
        """(text, None), or (None, why) when the page's bytes on disk are no
        longer the manifest's (re-read at every fetch, never cached)."""
        p = self.pages[topic]
        try:
            data = (self.root / p.file).read_bytes()
        except OSError as exc:
            return None, f"{p.file}: {exc}"
        if sha256_bytes(data) != p.sha256:
            return None, f"{p.file}: sha256 {sha256_bytes(data)} is not the manifest's {p.sha256}"
        return data.decode("utf-8"), None

    def play_text(self, role: str) -> tuple[str, str]:
        """(text, declared sha256) of the role's play file; raises
        GuideBootRefusal when the bytes are not the manifest's."""
        return self._role_text(self.play, "play", role)

    def goal_text(self, role: str) -> tuple[str, str]:
        """(text, declared sha256) of the role's goal text (the seat's
        `--goal`); raises GuideBootRefusal when the bytes are not the manifest's."""
        return self._role_text(self.goal, "goal", role)

    def role_entry(self, kind: str, role: str) -> dict:
        """The manifest's {file, sha256} for the role's `play` or `goal` file."""
        return dict((self.play if kind == "play" else self.goal).get(role) or {})

    def _role_text(self, table: Mapping[str, Any], kind: str, role: str) -> tuple[str, str]:
        entry = table.get(role)
        if not isinstance(entry, Mapping):
            raise GuideBootRefusal(R_MANIFEST_INVALID, f"the manifest declares no {kind} file for role {role!r}")
        data = (self.root / str(entry["file"])).read_bytes()
        if sha256_bytes(data) != entry["sha256"]:
            raise GuideBootRefusal(R_PAGE_SHA, f"{entry['file']}: sha256 {sha256_bytes(data)} is not the "
                                               f"manifest's {entry['sha256']}")
        return data.decode("utf-8"), str(entry["sha256"])


def render_index(pages: Iterable[Page]) -> str:
    """The index: one line per top-level topic (a page with a `when`), in
    manifest order, saying when to open it. The play files carry exactly this
    text (checked by test_guide)."""
    lines = [f"- `{p.topic}`: {p.when}" for p in pages if p.when]
    return "\n".join(lines) + "\n"


def _invalid(detail: str) -> GuideBootRefusal:
    return GuideBootRefusal(R_MANIFEST_INVALID, detail)


def parse_manifest(data: bytes) -> tuple[str, dict[str, Page], dict[str, dict], dict[str, dict]]:
    """(name, pages by topic in order, play by role, goal by role). Structural checks only."""
    try:
        m = json.loads(data.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise GuideBootRefusal(R_MANIFEST_UNREADABLE, f"{MANIFEST_NAME} is not JSON: {exc}")
    if not isinstance(m, dict) or m.get("format") != MANIFEST_FORMAT:
        raise _invalid(f"{MANIFEST_NAME} is not a format-{MANIFEST_FORMAT} manifest")
    pages: dict[str, Page] = {}
    for row in m.get("pages") or []:
        if not isinstance(row, dict):
            raise _invalid(f"a page row is not an object: {row!r}")
        t = row.get("topic")
        if not isinstance(t, str) or not _TOPIC_RE.match(t) or t == INDEX_TOPIC or t in pages:
            raise _invalid(f"topic {t!r} is not a fresh lowercase name (and `{INDEX_TOPIC}` is the index)")
        if not isinstance(row.get("file"), str) or not _SHA_RE.match(str(row.get("sha256"))):
            raise _invalid(f"page {t!r} names no file or no sha256")
        when = row.get("when")
        if when is not None and (not isinstance(when, str) or not when.strip() or "\n" in when):
            raise _invalid(f"page {t!r}: `when` is one line or absent")
        links = row.get("links") or []
        if not isinstance(links, list) or not all(isinstance(x, str) for x in links):
            raise _invalid(f"page {t!r}: `links` is a list of topics")
        pages[t] = Page(topic=t, file=row["file"], sha256=row["sha256"], bytes=int(row.get("bytes") or 0),
                        when=when, links=tuple(links))
    if not pages:
        raise _invalid("the manifest names no page")
    tables = {}
    for kind in ROLE_FILE_KINDS:
        t = m.get(kind) or {}
        if not isinstance(t, dict) or set(t) != set(ROLES) or not all(
                isinstance(v, dict) and isinstance(v.get("file"), str) and _SHA_RE.match(str(v.get("sha256")))
                for v in t.values()):
            raise _invalid(f"`{kind}` names a file and a sha256 for exactly the roles {list(ROLES)}")
        tables[kind] = t
    return str(m.get("name") or ""), pages, tables["play"], tables["goal"]


def load(root: os.PathLike | str, declared_sha256: str) -> Guide:
    """The guide at `root`, checked against the declared manifest sha256:
    the manifest's bytes, every page's bytes and bound, every link names a
    page and is named in the page's text, every deeper page is reachable."""
    root = Path(root)
    try:
        data = (root / MANIFEST_NAME).read_bytes()
    except OSError as exc:
        raise GuideBootRefusal(R_MANIFEST_UNREADABLE, f"{root / MANIFEST_NAME}: {exc}")
    got = sha256_bytes(data)
    if got != declared_sha256:
        raise GuideBootRefusal(R_MANIFEST_SHA, f"{root / MANIFEST_NAME} sha256 {got} is not the declared "
                                               f"{declared_sha256}")
    name, pages, play, goal = parse_manifest(data)
    texts = {}
    for p in pages.values():
        try:
            body = (root / p.file).read_bytes()
        except OSError as exc:
            raise GuideBootRefusal(R_PAGE_SHA, f"{p.file}: {exc}")
        if sha256_bytes(body) != p.sha256 or len(body) != p.bytes:
            raise GuideBootRefusal(R_PAGE_SHA, f"{p.file}: sha256 {sha256_bytes(body)} / {len(body)} bytes "
                                               f"is not the manifest's {p.sha256} / {p.bytes}")
        if len(body) > PAGE_MAX_BYTES:
            raise GuideBootRefusal(R_PAGE_TOO_LONG, f"{p.file}: {len(body)} bytes > {PAGE_MAX_BYTES}")
        texts[p.topic] = body.decode("utf-8")
        for link in p.links:
            if link not in pages or f"`{link}`" not in texts[p.topic]:
                raise GuideBootRefusal(R_LINK_UNKNOWN, f"page {p.topic!r} links {link!r}, which is not a "
                                                       "topic or is not named in its text as `topic`")
    linked = {x for p in pages.values() for x in p.links}
    orphans = [t for t, p in pages.items() if p.when is None and t not in linked]
    if orphans:
        raise GuideBootRefusal(R_PAGE_UNREACHABLE, f"deeper pages no page links: {orphans}")
    g = Guide(root=root, manifest_sha256=got, name=name, pages=pages, play=play, goal=goal, texts=texts)
    for role in ROLES:
        g.play_text(role)                   # the play files' bytes too
        g.goal_text(role)                   # and the goal texts'
    return g


def refresh(root: os.PathLike | str) -> bytes:
    """The manifest with every page's and play file's sha256 and bytes
    recomputed from disk (the author's step after editing a page:
    `python3 ck3agent/server/guide.py refresh scripts/prompts/v4-1066`).
    Returns the bytes; the caller writes them."""
    root = Path(root)
    m = json.loads((root / MANIFEST_NAME).read_text(encoding="utf-8"))
    for row in m["pages"]:
        body = (root / row["file"]).read_bytes()
        row["sha256"], row["bytes"] = sha256_bytes(body), len(body)
    for kind in ROLE_FILE_KINDS:
        for entry in m[kind].values():
            entry["sha256"] = sha256_bytes((root / entry["file"]).read_bytes())
    return (json.dumps(m, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


# ═══════════════════════════════════════════════════════════════════════════
# 3. THE DECLARATION AT BOOT
# ═══════════════════════════════════════════════════════════════════════════

@dataclass
class GuideConfig:
    guide: Guide
    seat: str = ""

    @property
    def enabled(self) -> bool:
        return True


def boot(environ: Mapping[str, str]) -> Optional[GuideConfig]:
    """The seat's guide: None when the environment declares none (neither key);
    else the loaded guide. Either key without the other, or anything `load`
    refuses, refuses the boot (typed)."""
    sha = (environ.get(ENV_MANIFEST_SHA) or "").strip()
    root = (environ.get(ENV_DIR) or "").strip()
    if not sha and not root:
        return None
    if not sha:
        raise GuideBootRefusal(R_SHA_UNDECLARED, f"{ENV_DIR} is set and {ENV_MANIFEST_SHA} is not: the "
                                                 "guide's content is declared by its sha, never assumed")
    if not root:
        raise GuideBootRefusal(R_DIR_UNDECLARED, f"{ENV_MANIFEST_SHA} is set and {ENV_DIR} is not")
    return GuideConfig(guide=load(root, sha), seat=(environ.get(ENV_SEAT) or "").strip())


def transcription_for(manifest_sha256: str, root: os.PathLike | str) -> dict[str, str]:
    """The seat environment a launcher establishes from the block's sha and
    the guide's directory."""
    return {ENV_DIR: str(root), ENV_MANIFEST_SHA: str(manifest_sha256)}


def transcription_from_scalars(scalars: Mapping[str, str], repo: os.PathLike | str) -> dict[str, str]:
    """The transcription from a block's scalars; {} when the block declares no guide."""
    sha = scalars.get(KEY_MANIFEST_SHA)
    if sha is None:
        return {}
    return transcription_for(sha, Path(repo) / REPO_GUIDE_REL)


def declared_block(environ: Mapping[str, str]) -> dict:
    """What a bundle records at OPEN about the guide the seats ran under.
    Absence is `not_declared`, never a default."""
    sha = (environ.get(ENV_MANIFEST_SHA) or "").strip()
    if not sha:
        return {"declared": False, "state": "not_declared", "manifest_sha256": None}
    return {"declared": True, "state": "declared", "manifest_sha256": sha,
            "dir": (environ.get(ENV_DIR) or "").strip() or None}


def log_required(summary: Mapping[str, Any]) -> bool:
    blk = summary.get("guide")
    return isinstance(blk, Mapping) and bool(blk.get("declared"))


def play_witness(guide: Guide, label: str, delivered_sha256: Optional[str]) -> dict:
    """The play-file row for one seat: the role, the manifest's file and sha
    (declared) beside the sha of the bytes the seat's play dir holds
    (delivered). `agreed` only when both were read and are equal."""
    role = role_of(label)
    entry = guide.play.get(role or "") or {}
    declared = entry.get("sha256")
    verdict = ("witness_absent" if delivered_sha256 is None or declared is None
               else "agreed" if delivered_sha256 == declared else "disagreed")
    return {"label": label, "role": role, "file": entry.get("file"), "declared_sha256": declared,
            "delivered_sha256": delivered_sha256, "verdict": verdict,
            "manifest_sha256": guide.manifest_sha256}


# ═══════════════════════════════════════════════════════════════════════════
# 4. THE FETCH AND ITS LOG
# ═══════════════════════════════════════════════════════════════════════════

def log_path(episode_id: str, environ: Optional[Mapping[str, str]] = None) -> Path:
    if not episode_id or "/" in episode_id or episode_id in (".", ".."):
        raise ValueError(f"not an episode id: {episode_id!r}")
    paths = _instance.resolve(env=os.environ if environ is None else environ)
    return Path(paths.telemetry_dir) / "guide" / episode_id / LOG_NAME


def cycle_at(game_date: Any, environ: Optional[Mapping[str, str]] = None) -> tuple[Optional[int], Optional[str]]:
    """The episode cycle by the one authority (`cycle_cadence.episode_cycle`)."""
    return _cc.episode_cycle(os.environ if environ is None else environ, game_date)


def utc_now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def append(path: os.PathLike | str, rec: Mapping[str, Any]) -> None:
    """One record, appended under an exclusive lock and fsync'd."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p.with_name(p.name + ".lock"), "a+") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            with p.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps(dict(rec), sort_keys=True) + "\n")
                fh.flush()
                os.fsync(fh.fileno())
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def read_records(path: os.PathLike | str) -> list[dict]:
    out = []
    with Path(path).open("r", encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            if not line.strip():
                continue
            rec = json.loads(line)
            if not isinstance(rec, dict):
                raise ValueError(f"{path}:{n}: record is not an object")
            out.append(rec)
    return out


def resolve(guide: Guide, topic: Any) -> tuple[str, str, Optional[str], Optional[str]]:
    """(verdict, topic served, text, why-refused) for a topic asked."""
    asked = str(topic or "").strip().lower()
    if asked in guide.pages:
        text, why = guide.page_text(asked)
        if text is None:
            return V_PAGE_CHANGED, asked, None, why
        return V_PAGE, asked, text, None
    text = guide.index_text()
    return (V_INDEX if asked == INDEX_TOPIC else V_INDEX_UNKNOWN), INDEX_TOPIC, text, None


def read_attempt(cfg: GuideConfig, *, topic: Any, game_date: Any, tick: Any,
                 cycle: Optional[int], cycle_why: Optional[str], episode_id: str,
                 instance_id: str = "", log: Optional[os.PathLike | str] = None) -> tuple[dict, Optional[str]]:
    """One fetch: (its log record, the text served or None). Logged before it
    returns, refused or not."""
    verdict, served, text, why = resolve(cfg.guide, topic)
    rec = {"kind": "read", "op_id": sha256_text(f"{time.time_ns()}:{os.getpid()}:{os.urandom(8).hex()}")[:32],
           "episode_id": episode_id, "instance_id": instance_id, "seat": cfg.seat,
           "cycle": cycle, "game_date": game_date, "tick": tick,
           "topic_asked": str(topic or ""), "topic_served": served, "verdict": verdict,
           "bytes": len(text.encode("utf-8")) if text is not None else 0,
           "page_sha256": sha256_text(text) if text is not None else None,
           "manifest_sha256": cfg.guide.manifest_sha256, "utc": utc_now_iso()}
    if cycle is None:
        rec["cycle_why"] = cycle_why
    if why:
        rec["detail"] = why
    append(log if log is not None else log_path(episode_id), rec)
    return rec, text


# ═══════════════════════════════════════════════════════════════════════════
# 5. THE SEAL
# ═══════════════════════════════════════════════════════════════════════════

def seal(records: Sequence[Mapping[str, Any]], *, seats: Sequence[str]) -> dict:
    """The report a bundle carries beside the copied log: per seat the fetches,
    the bytes and the topics; per topic the fetches; how many fetches were of
    an unknown topic, refused, or carried no cycle; and every manifest sha the
    log names (one, when the seats ran under one guide)."""
    per_seat = {s: {"fetches": 0, "bytes": 0, "topics": {}} for s in seats}
    per_topic: dict[str, int] = {}
    for r in records:
        s = per_seat.setdefault(str(r.get("seat")), {"fetches": 0, "bytes": 0, "topics": {}})
        s["fetches"] += 1
        s["bytes"] += int(r.get("bytes") or 0)
        t = str(r.get("topic_served"))
        s["topics"][t] = s["topics"].get(t, 0) + 1
        per_topic[t] = per_topic.get(t, 0) + 1
    return {"records": len(records), "seats": per_seat, "topics": per_topic,
            "unknown_topic": sum(1 for r in records if r.get("verdict") == V_INDEX_UNKNOWN),
            "refused": sum(1 for r in records if r.get("verdict") not in SERVED),
            "cycle_unknown": sum(1 for r in records if r.get("cycle") is None),
            "manifest_sha256": sorted({str(r.get("manifest_sha256")) for r in records})}


def _main(argv: Sequence[str]) -> int:
    if len(argv) == 2 and argv[0] in ("refresh", "check"):
        root = Path(argv[1])
        new = refresh(root)
        if argv[0] == "refresh":
            (root / MANIFEST_NAME).write_bytes(new)
        elif (root / MANIFEST_NAME).read_bytes() != new:
            print(f"{root / MANIFEST_NAME} is stale: run `refresh`", file=sys.stderr)
            return 1
        print(sha256_bytes(new))
        return 0
    print("usage: guide.py refresh|check <guide dir>   (prints the manifest's sha256)", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
