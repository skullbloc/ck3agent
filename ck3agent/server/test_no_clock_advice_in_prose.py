"""No agent-facing prose tells the agent to run the clock for an answer (L13 FF2, witness R7).

The owner's rule: every tool works whether the game is paused or running, and
only session(pause|unpause|speed|wait) moves the clock, on the player's call.
The bridge's reply strings are guarded by test_inline_safety_static (row F2);
this is the same guard for the prose the agent and its author read as doctrine:
ck3agent/knowledge, ck3agent/boot, ck3agent/*.md, release/overlay and docs/*.md
(the top level; docs/reports, re/ and ck3agent/docs/notes are history and not
scanned). An agent that follows "call it UNPAUSED" moves the player's clock on
false advice, which defeats the rule by instruction.

A hit is a line matching one of ADVICE. ALLOWED names each line that may match
and why (a sentence that describes the OLD behaviour as gone); a CHANGELOG is
scanned only above its first released version, the rest being history.
"""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

#: Advice to run the clock (or to wait for a tick) to get an answer or a write.
ADVICE = tuple(re.compile(p, re.I) for p in (
    r"\bcall(ed)?\s+(it\s+)?unpaused\b",
    r"\bneeds?\s+(the\s+game\s+)?(to\s+be\s+)?\**unpaused\b",
    r"\bmust\s+be\s+(running|unpaused)\s+for\b",
    r"\bunpause\s+(briefly|first|to\s+(get|read|let|drain))\b",
    r"\b(re-?query|verify|leave\s+\S+)\s+unpaused\b",
    r"\bleave\s+(the\s+game\s+)?\**unpaused\b[^.]{0,40}\bdrain",
    r"\bpaused[- ]gated\b",
    r"\bpaused\s+it\s+blocks\b",
    r"\bdrains?\s+on\s+the\s+(game\s+)?tick\b",
    r"\bneeds?\s+(the\s+game\s+to\s+run\s+)?a\s+tick\b",
    r"\b(wait|waits)\s+for\s+a\s+tick\b",
    r"\bruns?\s+for\s+a\s+tick\b",
    r"\bon\s+an\s+unpaused\s+tick\b",
    r"\bonly\s+execute\s+on\s+an?\s+unpaused\b",
    r"\bneeds?\s+the\s+(game(\'s)?\s+)?tick\b",
    r"\bgame\s+paused\s+or\s+tick\b",
)) + (
    # the doctrine's shouted advice ("UNPAUSED, at peace", "call UNPAUSED"):
    # case-sensitive, so a sentence about the game running unpaused does not match
    re.compile(r"\bUNPAUSED\b"),
)

#: (path, a substring of the line): a line allowed to match, with why.
ALLOWED: dict[tuple[str, str], str] = {
    ("ck3agent/DESIGN-tool-consolidation.md", "then unpause to let time advance"):
        "a design record quoting the pre-2026 session docstring; letting time pass is the "
        "player's tempo, not a readback",
    ("docs/agent-character-contract.md", "told the agent to unpause first. That is history."):
        "names the old advice as history",
    ("docs/campaign-bug-backlog.md", "single-flight, needs unpaused, ≥6s"):
        "a dated bug backlog (the 2026-06-14 campaign) recording the behaviour of its time",
    ("docs/campaign-bug-backlog.md", "Applies on a game tick — poll to confirm."):
        "a dated bug backlog (the 2026-06-14 campaign) quoting the docstring of its time",
    ("docs/campaign-bug-backlog.md", "Reads need PAUSED / writes"):
        "a dated bug backlog (the 2026-06-14 campaign) recording the behaviour of its time",
    ("docs/live-session-findings.md", "paused-gated tier-2 `query_character`"):
        "a dated finding (L25, fixed) describing the pre-L13 tier-2 gate",
    ("docs/live-session-findings.md", "this human-vs-agent game runs mostly UNPAUSED"):
        "a dated finding (L6, fixed) describing a game the human ran unpaused",
}


#: The research tree has release/overlay; the public export lays the overlay on
#: its root and has none. Each scans the prose IT ships (witness m-k: the export
#: scanned neither its protocol nor docs/reference or docs/porting).
RESEARCH = (REPO / "release" / "overlay").is_dir()


def _files() -> list[Path]:
    out: list[Path] = []
    if RESEARCH:
        for d in ("ck3agent/knowledge", "ck3agent/boot", "release/overlay"):
            out += sorted((REPO / d).rglob("*.md"))
        out += sorted((REPO / "ck3agent").glob("*.md"))
        out += sorted((REPO / "docs").glob("*.md"))
        out += sorted((REPO / "docs" / "design").glob("*.md"))
    else:
        out += sorted((REPO / "ck3agent").rglob("*.md"))
        out += sorted((REPO / "docs").rglob("*.md"))
        out += sorted(REPO.glob("*.md"))
    return sorted(set(out))


def _scanned_lines(path: Path):
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if path.name == "CHANGELOG.md":
        # history below the first released version
        for i, line in enumerate(text):
            if re.match(r"^## \d{4}\.\d+\.\d+ — \d{4}-\d{2}-\d{2}", line):
                text = text[:i]
                break
    return enumerate(text, 1)


def test_the_census_reads_the_doctrine():
    """Fail OPEN: a glob that found nothing would green the row below. The floor
    and the named files are per tree: the research tree's doctrine, or the public
    export's shipped protocol, reference and porting docs."""
    names = {str(p.relative_to(REPO)) for p in _files()}
    if RESEARCH:
        must = ("ck3agent/knowledge/britannia-campaign.md", "ck3agent/knowledge/levers.md",
                "ck3agent/CLAUDE.md", "ck3agent/README.md",
                "release/overlay/ck3agent/protocol/CLAUDE.md", "release/overlay/CHANGELOG.md",
                "docs/design/multi-agent-control.md")
        floor = 30
    else:
        must = ("ck3agent/protocol/CLAUDE.md", "CHANGELOG.md",
                "docs/reference/bridge-contract.md", "docs/porting/p7-windows-vm-runbook.md")
        floor = 15
    for m in must:
        assert m in names, m
    assert len(names) > floor, len(names)


def test_no_prose_advises_running_the_clock_for_an_answer():
    hits = []
    for p in _files():
        rel = str(p.relative_to(REPO))
        for n, line in _scanned_lines(p):
            if any(rx.search(line) for rx in ADVICE):
                if any(rel == ap and sub in line for (ap, sub) in ALLOWED):
                    continue
                hits.append(f"{rel}:{n}: {line.strip()[:160]}")
    assert not hits, "\n".join(hits)


def test_every_allowed_line_still_exists_and_still_matches():
    """An allowance for a line that is gone, or that no longer matches, is dead."""
    for (rel, sub), why in ALLOWED.items():
        assert why, (rel, sub)
        lines = [l for _, l in _scanned_lines(REPO / rel)]
        hit = [l for l in lines if sub in l]
        assert hit, f"{rel}: allowed line {sub!r} is gone"
        assert any(rx.search(l) for l in hit for rx in ADVICE), f"{rel}: {sub!r} no longer matches"


def test_CONTROL_the_patterns_catch_the_witness_lines():
    """The phrasings witness R7 found (now fixed) are each caught."""
    for line in ("⚠ **CALL IT UNPAUSED** (drains on the game tick;",
                 "paused it blocks). Alliance flags",
                 "Caveat: the CB/validity discovers need **UNPAUSED** + serial",
                 "for those, unpause briefly and re-pause after.",
                 "NEVER 5 — it outruns paused-gated reads",
                 "the bounded engine-check budget didn't reach it (re-query UNPAUSED).",
                 '"dispatched/queued" and only EXECUTE on an unpaused tick.',
                 "Needs the game to run a tick.",
                 "the deferred CB queries and writes need the game tick.",
                 "| `timeout` | Game paused or tick budget exhausted before confirmation |",
                 "(or `culture=\"anglo_saxon\"`). UNPAUSED. Returns",
                 "answers the whole list in one call, while the clock runs for a tick"):
        assert any(rx.search(line) for rx in ADVICE), line
    for fine in ("End every cycle with the game unpaused at speed 1",
                 "Every tool works with the game paused or running",
                 "A loaded save stays paused until you unpause it"):
        assert not any(rx.search(fine) for rx in ADVICE), fine
