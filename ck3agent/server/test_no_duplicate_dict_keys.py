"""No dict literal in the server names a key twice (cleanup 2026.11, L12 item 8).

RAW_REASON_TO_GATE carried "no_contract_with_that_vassal" twice (lane l6:
target_not_assignable; lane l10: no_such_contract): Python keeps the later one
silently, so the earlier line read as a rule and was not one. The census walks
every dict literal in every server module (fails OPEN: a module that does not
parse is a failure, not a skip).
"""
import ast
from pathlib import Path

import redesign

HERE = Path(__file__).parent


def _dupes(src: str) -> list:
    out = []
    for n in ast.walk(ast.parse(src)):
        if isinstance(n, ast.Dict):
            keys = [k.value for k in n.keys if isinstance(k, ast.Constant)]
            out += [(n.lineno, k) for k in sorted({k for k in keys if keys.count(k) > 1}, key=str)]
    return out


def test_no_server_dict_literal_repeats_a_key():
    mods = sorted(p for p in HERE.glob("*.py"))
    assert len(mods) > 20, "the census found too few modules (fail OPEN)"
    found = {p.name: d for p in mods if (d := _dupes(p.read_text()))}
    assert not found, found


def test_no_raw_reason_appears_twice_in_raw_reason_to_gate():
    tree = ast.parse((HERE / "redesign.py").read_text())
    node = next(n.value for n in ast.walk(tree)
                if isinstance(n, ast.AnnAssign) and getattr(n.target, "id", "") == "RAW_REASON_TO_GATE")
    keys = [k.value for k in node.keys]
    assert len(keys) == len(set(keys)) == len(redesign.RAW_REASON_TO_GATE)


def test_no_contract_with_that_vassal_is_no_such_contract():
    """The bridge emits it when the liege holds no contract with that vassal
    (action_handlers.cpp do_set_obligation_level): not retryable."""
    assert redesign.RAW_REASON_TO_GATE["no_contract_with_that_vassal"] == "no_such_contract"


def test_control_the_census_sees_a_repeated_key():
    assert _dupes('X = {"a": 1, "b": 2, "a": 3}') == [(1, "a")]
