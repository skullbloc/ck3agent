"""ck-zgh4 deploy-51 — no seat-facing advisory names a verb the seat cannot call.

THIS FILE REPLACES ck-4b41's VERSION, AND KEEPS ITS FINDING. ck-4b41 was right
about the bridge and I am not overturning that: `disable_native_ai=false` undoes
NOTHING, because `do_set_controlled_character` disables under
`if (disable_ai && ...)` with no else-branch, so passing false merely skips the
disable and never toggles the AI back on. Its own test is kept below.

WHAT IT MISSED IS REACHABILITY. Having found that `set_character_ai enabled=true`
is what re-enables the per-character tick, it named that call to the SEAT — as
`FIX: do(intent='set_character_ai', enabled=True)`. There is no such intent.
`set_character_ai` is a BRIDGE action the SERVER can issue; it is not one of the
16 registered tools and not one of `do`'s 20 intents. So ck-4b41 replaced one
remedy that could not work with another that could not work, and the toolchain
designer counted 26 seat calls to the unknown verb.

WHAT REPLACED IT. The server already re-asserts the gate itself on every
`set_phase` call, so the seat needs no verb for it; and the two remedies left in
seat-facing text — `session(action='release')` and `set_phase` — are both on the
surface. `expansion_engine_health`, which carried the FIX string, was already
unreachable after item (1) removed its two result sites, and is deleted.
"""

import inspect
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")


def _seat_facing_text() -> str:
    """Every advisory/warning string a SEAT can receive, from the impls that
    emit them. Source-read, and that is a stated limit: it over-collects (a
    string in a comment counts) which is the SAFE direction for a
    must-not-appear row, and it would under-collect a string built at runtime.
    The driven rows in test_set_phase_per_seat.py cover the set_phase result
    itself."""
    out = []
    for name in ("set_phase_impl", "_session_impl_branches"):
        fn = getattr(redesign, name, None)
        if fn is not None:
            out.append(inspect.getsource(fn))
    return "\n".join(out)


#: Verbs that are NOT on the seat surface. A seat that follows text naming one
#: of these gets `unknown_verb`. Derived from the registered tool list and
#: `do`'s intent enum, not hand-maintained.
def _uncallable_examples():
    import gen_tool_reference as gtr          # noqa: PLC0415
    defs = gtr.collect_mcp_tools(gtr._import_tools_module())
    tools = {t["name"] for t in defs}
    intents = set(gtr._enum_for(defs, "do", "intent"))
    return tools, intents


def test_set_character_ai_IS_NOT_on_the_seat_surface():
    """The premise of the whole file, asserted rather than assumed — if this ever
    becomes callable, the rows below should be revisited, not silently kept."""
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
    tools, intents = _uncallable_examples()
    assert "set_character_ai" not in tools
    assert "set_character_ai" not in intents
    # A TRIPWIRE THAT IS ARM-NEUTRAL. My first version asserted `len(tools) == 16`
    # and the full suite red at 18: another test in the same run sets the board
    # arm, so `board_post`/`board_read` register and the count is 18 in arm B and
    # 16 everywhere else. A premise that depends on the ARM is the one thing a
    # test in this repo must not have, because the arm IS the manipulation — so
    # the board tools are subtracted using the producer's own authority rather
    # than a count being pinned.
    import registered_prompts as rp                          # noqa: PLC0415
    board = rp.board_tool_names()
    # ck-zgh4, AMENDMENT 6: 16 -> 15, execute_console_command off the advertised
    # surface. Asserted BY NAME as well as by count, because 16 -> 15 is
    # satisfied by any tool leaving, and because "15" already meant something
    # else in this tree — the count actually REGISTERED via @mcp.tool() while
    # 16 were advertised. Both now read 15, so the count alone no longer
    # distinguishes "amendment 6 applied" from "the old registered/advertised
    # gap". The name does.
    assert "execute_console_command" not in tools, (
        "execute_console_command is back on the advertised surface; amendment 6 "
        "removed it, and this file's premise was re-confirmed against its absence")
    assert len(tools - board) == 15 and len(intents) == 20, (
        f"surface moved: {len(tools - board)} non-board tools, {len(intents)} "
        f"intents (board tools present: {sorted(tools & board) or 'none'}) — "
        "re-check this file's premise before trusting its rows")


def test_NO_seat_facing_advisory_tells_the_seat_to_CALL_set_character_ai():
    """The defect. Not 'never mentions it' — the text may warn ABOUT it — but it
    must never be offered as something the seat should call."""
    txt = _seat_facing_text()
    for bad in ("do(intent='set_character_ai'",
                'do(intent="set_character_ai"',
                "call set_character_ai",
                "set_character_ai enabled=True is the only way back"):
        assert bad not in txt, f"seat-facing text still prescribes: {bad!r}"


def test_the_REMEDIES_the_text_DOES_name_are_all_callable():
    txt = _seat_facing_text()
    assert "session(action='release')" in txt, (
        "release is on the surface and is the remedy that works mid-war")
    assert "set_phase" in txt


def test_ck4b41_BRIDGE_FINDING_IS_KEPT():
    """A reader who remembers the old advice needs it CONTRADICTED by name, not
    merely omitted — that was ck-4b41's own argument and it still holds."""
    txt = _seat_facing_text()
    assert "no else-branch" in txt
    assert "does NOT undo it" in txt or "does not undo it" in txt


def test_the_advisory_that_carried_the_FIX_string_IS_GONE():
    assert not hasattr(redesign, "expansion_engine_health"), (
        "expansion_engine_health returned the prescriptive advisory list and the "
        "uncallable FIX; item (1) made it unreachable and item (5) deleted it")
