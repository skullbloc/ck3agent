"""plan_war's empty_reason names what actually excluded the rows (L14 finding 2).

The evaluator's two empty plans (eval_b_play, both clock modes) answered
`empty_reason.gate: "target_allied"`, "allied/truced ... re-check next reign",
while the excluded rows said otherwise. The fixtures are those rows verbatim
(gate, raw_reason, summary), built through the same mappers plan_war uses.
"""
import redesign as R


def _row(name, blocked):
    return {"target_name": name, "blocked": blocked}


# 12037 (ally_free_only False): holder_not_independent (unmapped then) + claim_not_pressed
EXCLUDED_12037 = [
    _row("Ælfgar", R.map_raw_reason("holder_not_independent", summary="holder_not_independent")),
    _row("Comgal", R.map_raw_reason("claim_not_pressed", summary="claim_not_pressed")),
]
# 9460: one row, the ruler is 76
EXCLUDED_9460 = [
    _row("Cerball", R.why_blocked("succession_straddle",
                                  summary="ruler is 76 (>= 62): the war is expected to cross his succession")),
]


def test_holder_not_independent_is_a_typed_gate_with_words():
    b = R.map_raw_reason("holder_not_independent")
    assert b["gate"] == "target_not_independent", b
    assert "vassal" in b["why"], b


def test_12037_names_claim_not_pressed_and_every_gate_with_its_count():
    e = R.plan_war_empty_reason(EXCLUDED_12037, ally_free_only=False, exclude_straddling=True)
    assert e["gate"] == "claim_not_pressed", e
    assert e["measured"]["by_gate"] == {"target_not_independent": 1, "claim_not_pressed": 1}, e
    assert e["measured"]["excluded"] == 2
    assert "1 of 2" in e["summary"] and "target_not_independent x1" in e["summary"], e["summary"]
    assert "allied/truced" not in e["summary"]
    assert "fix" not in e                     # no plan_war parameter admits these rows


def test_9460_names_the_straddle_and_the_parameter_that_admits_it():
    e = R.plan_war_empty_reason(EXCLUDED_9460, ally_free_only=True, exclude_straddling=True)
    assert e["gate"] == "succession_straddle", e
    assert e["fix"]["succession_straddle"]["retry_with"] == {"exclude_straddling": False}, e
    assert "confirm_straddle=True" in e["fix"]["succession_straddle"]["then"], e
    assert e["retryable"] is True
    # already admitted: no fix to offer
    e = R.plan_war_empty_reason(EXCLUDED_9460, ally_free_only=True, exclude_straddling=False)
    assert "fix" not in e


def test_the_commonest_gate_wins_and_allied_rows_name_ally_free_only():
    rows = [_row("a", R.why_blocked("target_allied", summary="target pulls allies")),
            _row("b", R.why_blocked("target_allied", summary="target pulls allies")),
            _row("c", R.map_raw_reason("claim_not_pressed"))]
    e = R.plan_war_empty_reason(rows, ally_free_only=True, exclude_straddling=True)
    assert e["gate"] == "target_allied" and "2 of 3" in e["summary"], e
    assert e["fix"]["target_allied"]["retry_with"] == {"ally_free_only": False}


def test_nothing_enumerated_is_still_no_valid_targets():
    assert R.plan_war_empty_reason([], ally_free_only=True,
                                   exclude_straddling=True)["gate"] == "no_valid_targets"


def test_plan_war_impl_uses_it():
    """The one place: plan_war_impl builds empty_reason by this function."""
    import inspect
    src = inspect.getsource(R.plan_war_impl)
    assert "plan_war_empty_reason(excluded" in src
    assert "allied/truced/non-declarable" not in inspect.getsource(R)
