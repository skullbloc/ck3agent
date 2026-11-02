"""The AI personality axes: THE Python table, and the range the bridge accepts.

Bridge order (the engine's byte order at personality_ptr). It mirrors the
engine-backed part of bridge/include/personality_locks.h
(PERSONALITY_AXIS_NAMES[:PERSONALITY_ENGINE_AXES], PERSONALITY_AXIS_MIN/MAX);
test_steer_tool.py reads that header and checks this table. Every Python reader
imports it from here: steer's validation (through redesign), personality_nudge,
scripts/bridge_test_suite.py and re/coredump/bridge_emulator.py.

The bridge's table has a tenth name, dominance, with no engine byte and no
getter hook (personality.cpp): the game's AI never reads it. It is not an axis
here, so no tool accepts or writes it (L14 finding 5; neither the
pre-registration nor docs/pre-registration-conditions.md declares an axis list).
"""

AXES = (
    "honor", "greed", "rationality", "energy", "boldness",
    "zeal", "vengefulness", "compassion", "sociability",
)
AXIS_MIN = -100
AXIS_MAX = 100


def personality_refusal(values: dict) -> str | None:
    """Why a dict of axis writes cannot be written, or None when it can.

    THE check for every caller that writes axes (steer's `personality`,
    set_phase's `overrides`): a key that is not an axis, or a value that is not
    an integer from AXIS_MIN to AXIS_MAX. The bridge ignores an unknown key and
    reports success, so a refusal here is the only place the caller hears of it.
    """
    unknown = sorted(str(k) for k in values if k not in AXES)
    if unknown:
        return (f"not a personality axis: {', '.join(unknown)}; the axes are "
                f"{', '.join(AXES)}. Nothing was written.")
    bad = sorted(k for k, v in values.items()
                 if isinstance(v, bool) or not isinstance(v, int)
                 or not AXIS_MIN <= v <= AXIS_MAX)
    if bad:
        return ("axis values are integers from "
                f"{AXIS_MIN} to {AXIS_MAX}; got "
                + ", ".join(f"{k}={values[k]!r}" for k in bad)
                + ". Nothing was written.")
    return None
