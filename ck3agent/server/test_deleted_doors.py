"""Doors deleted in cleanup 2026.11 L14 (lane S2) answer as unknown and send nothing.

Finding 7 (docs/cleanup-2026.11/evidence/l13/surface-eval.md): no read yielded an
event id, so `do(intent="respond_event")` had no input, and
`session(action="auto_events")` acts only while no client is connected, so never
for a caller of this server. Both are deleted with
`query(domain="event_probabilities")` (in test_query_tool.py's DELETED_DOMAINS).
L16 (ruling R3) deleted the bridge handlers behind them too (respond_event,
query_event_probabilities / event_probabilities, set_auto_respond_events:
scripts/test_bridge_action_census.py); the no-agent fallback stays.
"""
import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

R = pytest.importorskip("redesign")


class _Gw:
    """Records every write; reads answer a live character."""

    health = "ok"

    def __init__(self):
        self.writes = []

    async def write(self, action, params=None):
        self.writes.append(action)
        return {"success": True}

    async def read(self, action, params=None):
        if action == "query_character":
            return {"id": (params or {}).get("character_id"), "name": "X", "alive": True}
        return {}

    async def actor_gate(self):
        return None

    def game(self):
        return {"tick": 1, "date": "867.1.1", "paused": True, "speed": 1}


def _refused_unknown(env, gw):
    assert env.get("ok") is False, env
    assert (env.get("blocked") or {}).get("gate") == "unknown_verb", env
    assert gw.writes == [], gw.writes


#: (tool, door) -> why it went. A door added back reds here by name.
DELETED = {
    ("do", "respond_event"): "finding 7: no read yields an event id",
    ("session", "auto_events"): "finding 7: inert while any client is connected",
}


def test_the_deleted_doors_are_not_advertised():
    assert "respond_event" not in R.INTENTS
    assert "respond_event" not in R.INTENT_TO_ACT
    assert "auto_events" not in R.SESSION_ACTIONS


def test_do_respond_event_is_refused_unknown_and_sends_nothing():
    gw = _Gw()
    env = asyncio.run(R.do_impl(gw, intent="respond_event", character_id=10465,
                                args={"event_id": "x", "option": 0}))
    _refused_unknown(env, gw)


def test_session_auto_events_is_refused_unknown_and_writes_nothing():
    gw = _Gw()
    env = asyncio.run(R.session_impl(gw, "auto_events", enabled=True))
    _refused_unknown(env, gw)


# ── finding 8: one door per read, one succession vocabulary ──────────────────

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

#: The texts an agent is taught from (CLAUDE.md "What governs you"): the
#: doctrine, the boot sources, the shipped protocol, the agent's own docs, and
#: the server's prompts. History (ck3agent/docs/notes, docs/reports, the
#: CHANGELOG's released sections) is not taught and not scanned.
def _taught_texts():
    import glob
    globs = ["ck3agent/knowledge/*.md", "ck3agent/knowledge/seed-deltas/*.json",
             "ck3agent/boot/*.md", "ck3agent/boot/*.SEED", "ck3agent/CLAUDE.md",
             "ck3agent/SOUL*.md", "ck3agent/README.md",
             "release/overlay/ck3agent/protocol/*.md", "ck3agent/server/prompts.py",
             "ck3agent/protocol/*.md"]   # the public export's layout
    out = {}
    for g in globs:
        for p in glob.glob(os.path.join(REPO, g)):
            out[os.path.relpath(p, REPO)] = open(p, encoding="utf-8").read()
    # fail OPEN: a moved tree is a red, not a pass. The research tree has the
    # doctrine (>= 10 files); the public export ships the protocol and the prompts.
    research = os.path.isdir(os.path.join(REPO, "ck3agent", "knowledge"))
    assert len(out) >= (10 if research else 3), sorted(out)
    return out


#: A deleted door or a retired verdict field, as a taught text would spell it.
import re  # noqa: E402

DELETED_SPELLINGS = {
    "a deleted query domain": re.compile(
        # keyword (domain=/domains=) and positional (query("...")) spellings
        r"""(?:domains?\s*=\s*|\bquery\(\s*)\[?[^)\n]*['"](event_probabilities|active_wars|dejure|goal|"""
        r"""partition_preview|succession_preview|succession_simulation|debug_flags)['"]"""),
    "inspect(succession_preview)": re.compile(r"""succession_preview['"]"""),
    "a moved discover enumeration": re.compile(
        # keyword (what=) and positional (discover(x), discover("x")) spellings
        r"""(?:what\s*=\s*['"]|\bdiscover\(\s*(?:what\s*=\s*)?['"]?)"""
        r"""(hireable_mercenaries|fabricate_targets|joinable_factions)\b"""),
    "an event door": re.compile(r"""['"](respond_event|auto_events)['"]"""),
    "a retired succession field": re.compile(
        r"`(any_splits|scatters|scatter_breaker|passes_single_heir|succession_scatters|"
        r"is_partition)\b|\b(any_splits|scatter_breaker|succession_scatters)\b"),
}


def test_no_taught_text_names_a_deleted_door_or_field():
    hits = []
    for path, text in _taught_texts().items():
        for n, line in enumerate(text.splitlines(), 1):
            for what, rx in DELETED_SPELLINGS.items():
                if rx.search(line):
                    hits.append(f"{path}:{n} ({what}): {line.strip()[:120]}")
    assert not hits, "\n".join(hits)


def test_the_scan_sees_a_deleted_spelling():
    """Not a discriminating control: pins that each pattern matches the spelling it
    names, so the scan above cannot pass by matching nothing."""
    samples = {
        "a deleted query domain": ['query(domain="partition_preview")',
                                   'query("partition_preview")'],
        "inspect(succession_preview)": 'inspect(subject="succession_preview")',
        "a moved discover enumeration": ['discover(what="hireable_mercenaries")',
                                         # L14 FF1 M-4: the positional spelling SOUL.md taught
                                         'from `discover(fabricate_targets)`',
                                         'discover("joinable_factions")'],
        "an event door": 'do(intent="respond_event")',
        "a retired succession field": "read `any_splits` first",
    }
    for what, rx in DELETED_SPELLINGS.items():
        for sample in ([samples[what]] if isinstance(samples[what], str) else samples[what]):
            assert rx.search(sample), (what, sample)


# ── L14 FF2 N-3: a doctrine line keyed on passes_intact false alone ──────────
#
# The verdict has three states (succession_verdict.OUTCOMES): passes_intact is
# true, false, or null (undetermined). A taught line that acts only on
# `passes_intact:false` / `== false` takes no action on undetermined, which is
# the state a failed read now answers (N-MAJ-2). Such a line must also name the
# third state on the same line ("not true", "undetermined" or the
# `succession_undetermined` warning).

FALSE_KEYED = re.compile(r"passes_intact`?\s*(?:==|=|:|\bis\b)?\s*`?\s*false\b", re.I)
NAMES_THE_THIRD_STATE = re.compile(r"undetermined|not true", re.I)

#: Human-owned boot text. The owner ruled FA2's wording in L15 (lane E): the
#: four lines that acted on `passes_intact` false alone now name the third
#: state, so no taught line is licensed to stay false-keyed. Each entry is the
#: line as the owner's ruling wrote it; it must still be in its file.
OWNER_RULED_LINES = {
    ("ck3agent/boot/operator-directives.md",
     "a `passes_intact` that is not true (false or undetermined) → you MUST have the next war declared within TWO"),
    ("ck3agent/boot/operator-directives.md",
     "1 (`succession_plan()`). If `passes_intact` is not true (false, or undetermined: the "
     "`succession_undetermined` warning), enumerate every"),
    ("ck3agent/boot/operator-directives.md.SEED",
     "a `passes_intact` that is not true (false or undetermined) → you MUST have the next war declared within TWO"),
    ("ck3agent/boot/operator-directives.md.SEED",
     "If `passes_intact` is not true (false, or undetermined: the `succession_undetermined` "
     "warning), enumerate every `splits_away` title, read the"),
}


def _false_keyed_lines():
    for path, text in _taught_texts().items():
        for n, line in enumerate(text.splitlines(), 1):
            if FALSE_KEYED.search(line) and not NAMES_THE_THIRD_STATE.search(line):
                yield path, n, line


def test_no_taught_line_acts_on_passes_intact_false_alone():
    hits = [f"{p}:{n}: {line.strip()[:120]}" for p, n, line in _false_keyed_lines()]
    assert not hits, "\n".join(hits)


def test_the_owner_ruled_lines_are_in_the_boot_text():
    """The owner's wording is present verbatim (a revert to the false-keyed
    lines reds here and in the scan above)."""
    texts = _taught_texts()
    for path, line in OWNER_RULED_LINES:
        if path not in texts:
            continue            # the public export ships no boot text
        lines = {l.strip() for l in texts[path].splitlines()}
        assert line in lines, (path, line)


def test_the_false_keyed_scan_sees_the_spellings():
    """The red row the brief asked for: a doctrine line keyed on
    `passes_intact:false` alone is a hit, in each spelling the tree used, and the
    fixed wording is not."""
    for red in ("THE RULE: when `passes_intact:false` (or the `succession_not_intact` warning fires)",
                "- trigger: `observe().succession.passes_intact == false` OR the",
                "on passes_intact false act on the named splits_away cadets",
                "If `passes_intact==false`, enumerate every",
                # the four boot lines as they read before the owner's L15 ruling
                "a `passes_intact:false` → you MUST have the next war declared within TWO",
                "1 (`succession_plan()`). If `passes_intact==false`, enumerate every",
                "If `passes_intact==false`, enumerate every `splits_away` title, read the"):
        assert FALSE_KEYED.search(red) and not NAMES_THE_THIRD_STATE.search(red), red
    for green in ("when `passes_intact` is not true (false OR undetermined/null; or the",
                  "passes_intact:true guarantees only the PRIMARY KINGDOM TITLE",
                  *(line for _p, line in OWNER_RULED_LINES)):
        assert not (FALSE_KEYED.search(green) and not NAMES_THE_THIRD_STATE.search(green)), green
