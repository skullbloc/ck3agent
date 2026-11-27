"""partition_forecast: who takes land at a partition succession, and vassalization exposure.

The cases are the recorded runs' successions (their tool-call logs), with what each record does and does not show:

* a recorded game 1082.01.17 (recorded): Murchad 31506, a count under confederate partition and
  male preference, died; his primary heir was Culdub 66780, not one of his children (the
  1071.08.14 read of Murchad lists children [36124, 37111, 37528]); Donnchad 37528, a son,
  took Athlone and Leinster and was independent. The observe verdict had named the breaker
  Duinsech 15819 since 1071.06.13, and the simulation's heirs were 66780, 15819 and others.
  NOT recorded: whether 36124 and 37111 were dead at 1082.01. The fixture marks them dead
  because the recorded split gave them nothing; that is an inference, said here.
* two recorded games: the verdict named Enna 32805, the 6th entry of Diarmait's
  Leinster line at 1066.09.15 and not one of Murchad's children, as the breaker.
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import partition_forecast as PF  # noqa: E402

CONF = "confederate_partition_succession_law"
MALE_PREF = "male_preference_law"


def kid(cid, alive=True, female=False, traits=()):
    return {"id": cid, "alive": alive, "is_female": female, "traits": [{"name": t} for t in traits]}


def test_a_son_takes_land_and_leaves_the_realm():
    out = PF.forecast(ruler_tier=2, held_counties=5, succession_law=CONF, gender_law=MALE_PREF, primary_heir_id=66780,
                      children=[kid(36124, alive=False), kid(37111, alive=False), kid(37528)],
                      sim_heir_ids=[66780, 15819, 70552])
    assert out["takers"] == [66780, 37528]
    assert out["intact"] is False
    assert out["younger_shares"] == [{"id": 37528, "fate": "leaves_realm",
                                      "why": out["younger_shares"][0]["why"]}]
    assert out["primary_heir_is_child"] is False
    assert "primary_heir_note" in out
    assert out["simulation_disagrees"]["named_but_not_partition_heirs"] == [15819, 70552]
    assert out["dead_children_lines_unknown"] == [36124, 37111]


def test_a_brother_named_by_the_simulation_is_flagged():
    # a recorded game 1071.09.16: breaker 32805 (Murchad's brother); primary 36124 is a son.
    out = PF.forecast(ruler_tier=2, held_counties=5, succession_law=CONF, gender_law=MALE_PREF, primary_heir_id=36124,
                      children=[kid(36124), kid(37111), kid(37528)], sim_heir_ids=[36124, 32805])
    assert out["takers"] == [36124, 37111, 37528]
    assert out["simulation_disagrees"]["named_but_not_partition_heirs"] == [32805]
    assert [s["fate"] for s in out["younger_shares"]] == ["leaves_realm", "leaves_realm"]


@pytest.mark.parametrize("trait", ["devoted", "disinherited", "bastard", "order_member"])
def test_a_blocking_trait_removes_a_son(trait):
    out = PF.forecast(ruler_tier=2, held_counties=5, succession_law=CONF, gender_law=MALE_PREF, primary_heir_id=1,
                      children=[kid(1), kid(2, traits=[trait])])
    assert out["takers"] == [1]
    assert out["intact"] is True
    assert out["excluded_children"][0]["id"] == 2 and trait in out["excluded_children"][0]["why"]


def test_male_preference_daughters_only_without_sons():
    with_son = PF.eligible_children([kid(1), kid(2, female=True)], MALE_PREF)
    assert [c["id"] for c in with_son["eligible"]] == [1]
    no_son = PF.eligible_children([kid(2, female=True), kid(3, female=True)], MALE_PREF)
    assert [c["id"] for c in no_son["eligible"]] == [2, 3]
    assert [c["id"] for c in PF.eligible_children([kid(1), kid(2, female=True)], "equal_law")["eligible"]] == [1, 2]


def test_an_unread_child_is_refused_not_defaulted():
    with pytest.raises(PF.ForecastInputError):
        PF.eligible_children([{"id": 5, "alive": True}], MALE_PREF)
    with pytest.raises(PF.ForecastInputError):
        PF.eligible_children([kid(5)], "gavelkind")
    with pytest.raises(PF.ForecastInputError):
        PF.forecast(ruler_tier=2, held_counties=5, succession_law="", gender_law=MALE_PREF, primary_heir_id=1,
                    children=[kid(1)])


def test_a_duke_under_confederate_partition_may_lose_a_created_duchy():
    out = PF.forecast(ruler_tier=3, held_counties=4, succession_law=CONF, gender_law=MALE_PREF, primary_heir_id=1,
                      children=[kid(1), kid(2)])
    assert out["younger_shares"][0]["fate"] == "may_leave_realm"
    assert "creates free titles" in out["younger_shares"][0]["why"]


def test_single_heir_law_is_not_a_partition():
    out = PF.forecast(ruler_tier=2, held_counties=5, succession_law="single_heir_succession_law", gender_law=MALE_PREF,
                      primary_heir_id=1, children=[kid(1), kid(2)])
    assert out["partition"] is False and out["takers"] == [1]


def test_vassalization_limit_and_exposure():
    assert PF.vassalization_size_limit([]) == 4
    assert PF.vassalization_size_limit(["innovation_casus_belli"]) == 7
    assert PF.vassalization_size_limit(PF.VASSALIZE_SIZE_INNOVATIONS) == 16
    # a recorded game 1084.01: a 3-county count against a king who declared the CB
    v = PF.vassalization_exposure(my_tier=2, my_sub_realm_counties=3, attacker_tier=4,
                                  attacker_innovations=["innovation_casus_belli"], attacker_unlocked=True)
    assert v["verdict"] == "open" and v["counties_to_close"] == 5
    big = PF.vassalization_exposure(my_tier=3, my_sub_realm_counties=8, attacker_tier=4,
                                    attacker_innovations=["innovation_casus_belli"], attacker_unlocked=True)
    assert big["verdict"] == "closed" and big["counties_to_close"] is None
    king = PF.vassalization_exposure(my_tier=4, my_sub_realm_counties=3, attacker_tier=4,
                                     attacker_innovations=[], attacker_unlocked=True)
    assert king["verdict"] == "closed"
    unread = PF.vassalization_exposure(my_tier=2, my_sub_realm_counties=3, attacker_tier=4,
                                       attacker_innovations=[], attacker_unlocked=None)
    assert unread["verdict"] == "open_if_unlocked"
    with pytest.raises(PF.ForecastInputError):
        PF.vassalization_exposure(my_tier=2, my_sub_realm_counties=None, attacker_tier=4,
                                  attacker_innovations=[], attacker_unlocked=True)
