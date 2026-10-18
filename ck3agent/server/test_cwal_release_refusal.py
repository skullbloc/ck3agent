"""ck-cwal — a control seat's LAST-character release is refused, and surfaced.

Progenitor ruling 2026-09-24: the bridge refuses it and the session tool
surfaces the refusal with its explanation. This is the TOOL half. The bridge
half (ck-760i) is not written yet; the tool half landing first is agreed, and
until the bridge enforces it the gate degrades to `refusal_reason_unknown` with
the raw reason verbatim, which is the safe direction.

THE CARVE-OUT CONTRACT IS NOW STATED, because it was answered from the handler
rather than inferred. ck-760i read bridge_init.cpp:1575 (2026-09-25): the direct
assignment there only refreshes the session's cached primary, while the CLAIM is
made in `swap_character`, which the handoff always goes through and which moves
the id between `claimed_character_ids` under one lock. A succession-seated
character is therefore an ordinary claim, indistinguishable from one made by
set_controlled_character, and `last_steered_character_id` is not a second
seating path — it only recovers WHICH corpse to run the handoff for.

AND ONE OF THE TWO CARVE-OUTS DISSOLVED. I had planned to exempt a handoff
corpse release; there is nothing to exempt, because the corpse is removed by
`swap_character` inside the handoff and never reaches do_release_character. So
the list is ONE, and it is stated positively: the refusal applies to every
release an agent can make, and the only thing it lets through is the switch's
own internal second half — which is not an agent's release either.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import redesign as rd                                            # noqa: E402

LITERAL = "last_claim_of_control_session"


class TheLiteralIsTheAGREEDONE(unittest.TestCase):

    def test_the_code_is_in_the_closed_set(self):
        """The two halves meet at exactly this string. ck-760i emits it; this
        recognises it. A mismatch routes a real refusal into
        `refusal_reason_unknown` — safe, and not the explanation the ruling
        asks for."""
        self.assertIn(LITERAL, rd.RELEASE_REFUSAL_REASONS)

    def test_every_member_of_the_set_has_a_summary(self):
        """A code in the set with no summary would surface the refusal with the
        generic unforeseen text, which is the failure this pairing prevents."""
        self.assertLessEqual(set(rd.RELEASE_REFUSAL_REASONS),
                             set(rd.RELEASE_REFUSAL_SUMMARIES))

    def test_the_summary_gives_the_RULED_explanation(self):
        """The ruling's own words: 'you steer; the AI runs regardless'. An
        agent told only that it cannot release will try again."""
        s = rd.RELEASE_REFUSAL_SUMMARIES[LITERAL]
        self.assertIn("you steer", s.lower())
        self.assertIn("ai runs regardless", s.lower())

    def test_the_summary_says_SWITCHING_IS_STILL_ALLOWED(self):
        """The ruling keeps switching legal. A refusal that did not say so
        would read as 'you are stuck with this character', which is a different
        and wrong contract."""
        s = rd.RELEASE_REFUSAL_SUMMARIES[LITERAL].lower()
        self.assertIn("switching", s)
        self.assertIn("still allowed", s)


class AnUnknownCodeStillDegradesSafely(unittest.TestCase):

    def test_an_unfamiliar_reason_is_NOT_swallowed(self):
        """This is why landing ahead of the bridge is safe: a code this client
        does not know comes back as refusal_reason_unknown with the raw reason
        carried verbatim, not as a success."""
        src = (Path(__file__).resolve().parent / "redesign.py").read_text()
        self.assertIn("refusal_reason_unknown", src)
        self.assertIn("raw_reason", src)

    def test_the_release_path_reads_success_THREE_VALUEDLY(self):
        """An absent `success` is unwitnessed, not success — the seat may still
        hold the character."""
        src = (Path(__file__).resolve().parent / "redesign.py").read_text()
        self.assertIn('"release_witnessed": res.get("success") is True', src)


class TheCarveOutContractIsStatedNow(unittest.TestCase):
    """This class REPLACES the row that kept the contract unstated. That row's
    job was to fail if anyone wrote the contract before ck-760i answered from
    the handler; the answer arrived 2026-09-25, so the row is discharged rather
    than deleted — these assert what was learned, in its place."""

    def test_an_INHERITED_claim_is_still_a_claim(self):
        """The operationally new fact, and the one an agent would guess wrong.
        A seat that came by its character through a succession holds an
        ordinary claim: `swap_character` moves the id between
        `claimed_character_ids` under one lock, so the roster cannot tell it
        from a set_controlled_character claim. An agent told only "your last
        character" might reasonably assume an inherited seat is a special case
        it can hand back. It is not."""
        s = rd.RELEASE_REFUSAL_SUMMARIES[LITERAL].lower()
        self.assertIn("succession", s)
        self.assertIn("inherited claim is a claim", s)

    def test_the_summary_still_does_not_mention_a_CORPSE_case(self):
        """Same assertion as the discharged row, for the OPPOSITE reason, which
        is why it is worth keeping rather than dropping. It used to mean "we do
        not know yet". It now means "there is no such case": the corpse is
        removed by `swap_character` inside the handoff and never reaches
        do_release_character, so there is no release request to exempt.
        Describing one would invent a contract."""
        s = rd.RELEASE_REFUSAL_SUMMARIES[LITERAL].lower()
        for word in ("corpse", "exempt", "carve"):
            self.assertNotIn(word, s, f"no such case exists (found {word!r})")

    def test_switching_is_still_offered_as_the_way_out(self):
        """The one carve-out is the switch's internal second half, which is not
        an agent's release — so what the agent is told is the ACTION, not the
        exemption: control the next character directly."""
        s = rd.RELEASE_REFUSAL_SUMMARIES[LITERAL].lower()
        self.assertIn("switching to another character is still allowed", s)
        self.assertIn("rather than releasing this one first", s)


if __name__ == "__main__":
    unittest.main()
