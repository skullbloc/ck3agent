"""L14 FB2 (witness N-15): the struct-docs renderer strips only the confidence
grade its badge shows. It had stripped every confidence word in a field's
comment, so "LOW (was MEDIUM): ..." rendered "(was ): ..." and "R8V-1
confirmed via X" rendered "R8V-1  via X" (re/docs/structs, `make struct-docs`)."""
import importlib.util
from pathlib import Path

import pytest

GEN = Path(__file__).resolve().parents[2] / "scripts" / "gen_struct_docs.py"
if not GEN.is_file():
    pytest.skip(f"no struct-docs generator in this layout ({GEN})", allow_module_level=True)

_spec = importlib.util.spec_from_file_location("gen_struct_docs", GEN)
G = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(G)


def _row(comment: str) -> str:
    conf = G.RE_CONFIDENCE.search(comment).group(1).upper()
    ns = G.Namespace(name="t", section_title="t",
                     fields=[G.Field(name="F", value="0x10", comment=comment, confidence=conf, line_no=1)])
    row, = [l for l in G.generate_doc(ns, {}).splitlines() if "`F`" in l]
    return row


def test_a_regrade_keeps_the_grade_it_was():
    row = _row("LOW (was MEDIUM): NOT a string on a CFaith")
    assert "`LOW`" in row and "(was MEDIUM): NOT a string on a CFaith" in row, row


def test_a_later_confidence_word_is_description():
    row = _row("HIGH: int16, birth date index (R8V-1 confirmed via FUN_0674c0c0)")
    assert "`HIGH`" in row and "(R8V-1 confirmed via FUN_0674c0c0)" in row, row
    assert "| int16" in row      # the badge's own grade is still stripped
