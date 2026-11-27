"""When the ruler may die: a band from his age, his health read and the game's own death check, plus a
death rate outside that check fitted to the ruler deaths in our recorded runs.
Pure.

The game's rule (common/defines/00_defines.txt, NOldAge, CK3 1.18):
  * a character whose health is below DIE_HEALTH_TRESHOLD 3.0 has a chance to die every month, growing
    quadratically to DIE_HEALTH_CHANCE_ZERO 0.25 at health 0 ("This DIE values are used with a quadratic
    chance"): here 0.25 x ((3 - health) / 3)^2;
  * from LOWER_HEALTH_START_AGE 25 a yearly roll lowers health by LOWER_HEALTH_AMOUNT 0.125 with
    chance LOWER_HEALTH_BASE_CHANCE 0.075 + LOWER_HEALTH_YEARLY_INCREASE 0.022 per year past 25.

That check alone is not how rulers die in our runs. The fit enumerated every seat-ruler death in the
recorded trajectories (the recorded games, plus the Karling run): 27 deaths with a
health read before them. The check alone put 26 of the 27 deaths BEFORE its own 25 % year (Charles of
West Francia, 867: "16.8 years", died 8.4 years later). The rest of the dying is outside the check:
illness (common/on_action/health_on_actions.txt: disease_outbreak_pulse, yearly_health_pulse ->
harm.9501, infirmity), wounds and battle, schemes, events. They cannot be read off the game files as
one number, so they enter as a monthly rate OUTSIDE_MONTHLY_AT_40 x exp(OUTSIDE_AGE_RATE x (age - 40)),
fitted by maximum likelihood to those deaths and to the rulers who outlived their runs (censored).
The calibration over the same rows is pinned in test_death_estimate_and_faction_threat.py: from each dying ruler's first read,
6 of 27 died before the band's 25 % year, 13 before 50 %, 22 before 75 %, 26 before 90 %. Adding
the Lorraine run's two rulers and the Francia re-run's Charles (30): 6, 14, 24 and 29. Not refit: the
rulers read at 60 or older died before p25 at 5 of 6 reads, but those are 4 rulers (3 of them before p25
at their first read past 60, 1 not); at 55 or older the first reads of 6 rulers put 1 before p25. Too few
and too correlated to move the curve (test_succession_line_reads.py states the counts). Adding the two seated Munster runs'
rows (Murchad died 18.85 and 22.29 years after the start: both inside the band, between p25 and p90): from the 32
dying rulers' first reads 6, 14, 25 and 31 died before p25/p50/p75/p90. Not refit: a fit over the same rows puts
its optimum at the grid's edge (a 0.0025, b 0.06, log-likelihood +2.7 on first reads), overshoots the quantiles
(10, 21, 28, 32 of 32) and leaves fewer survivors past p25 (12 of 30 against 16). The "4-5 years" the seats read for
15 years is this curve's 1-in-4 year for a ruler of 40-50 in health 3.9-4.2, which a near-constant monthly rate
keeps near-constant: a band's lower edge, not a countdown.

It is a band, not a date, and the seat is told so: most of the recorded rulers are two characters
(Murchad of Munster and Murchad of Dublin, 1066) replayed many times, so the outside rate is a rough
average of warring rulers, not a law.
"""
from __future__ import annotations

import math
from typing import Any, Optional

DIE_HEALTH_THRESHOLD = 3.0
DIE_HEALTH_CHANCE_ZERO = 0.25
LOWER_HEALTH_START_AGE = 25
LOWER_HEALTH_BASE_CHANCE = 0.075
LOWER_HEALTH_YEARLY_INCREASE = 0.022
LOWER_HEALTH_AMOUNT = 0.125
HORIZON_YEARS = 60

#: the death rate outside the game's health check (illness, battle, schemes, events), per month, at age
#: 40, growing by OUTSIDE_AGE_RATE a year of age. Fitted by maximum likelihood over the
#: recorded ruler deaths and survivors (fixtures/ruler_deaths.json; grid a 0.0025-0.006, b 0-0.06).
OUTSIDE_MONTHLY_AT_40 = 0.0045
OUTSIDE_AGE_RATE = 0.03

SOURCE = ("the game's health death check (defines NOldAge: below health 3, 0.25 x ((3-h)/3)^2 a month; "
          "yearly health loss from 25) plus a death rate outside it, fitted to ruler deaths and survivors")
UNCERTAIN = ("a band, not a date: illness, battle and schemes kill at any health; by the fitted rates one ruler "
             "in four dies before p25 and one in ten lives past p90")
AGE_HEURISTIC = 65
POOR_HEALTH = 4.0
QUANTILES = (("p25", 0.25), ("p50", 0.5), ("p75", 0.75), ("p90", 0.9))


def monthly_death_chance(health: float) -> float:
    """The game's own monthly death check at this health (0 at 3.0 or more)."""
    if health >= DIE_HEALTH_THRESHOLD:
        return 0.0
    x = (DIE_HEALTH_THRESHOLD - max(0.0, health)) / DIE_HEALTH_THRESHOLD
    return DIE_HEALTH_CHANCE_ZERO * x * x


def outside_monthly(age: float) -> float:
    """The fitted monthly death rate outside the game's health check, at this age."""
    return OUTSIDE_MONTHLY_AT_40 * math.exp(OUTSIDE_AGE_RATE * (age - 40))


def _num(v) -> Optional[float]:
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def survival(age: float, health: float, months: int) -> list[float]:
    """Chance of being alive after 0..months months: the health path is the game's expected yearly
    loss; each month both the game's check and the outside rate apply."""
    alive, out = 1.0, [1.0]
    hk, ak = health, age
    for k in range(1, months + 1):
        lam = min(1.0, monthly_death_chance(hk) + outside_monthly(ak))
        alive *= 1.0 - lam
        out.append(alive)
        ak += 1.0 / 12
        if k % 12 == 0 and ak >= LOWER_HEALTH_START_AGE:
            p = min(1.0, LOWER_HEALTH_BASE_CHANCE + LOWER_HEALTH_YEARLY_INCREASE * (ak - LOWER_HEALTH_START_AGE))
            hk -= LOWER_HEALTH_AMOUNT * p
    return out


def estimate(age: Any, health: Any) -> Optional[dict]:
    """{health, monthly_death_chance_now (the game's check plus the outside rate, this month), years: {p25,
    p50, p75, p90} (None past the horizon), source, uncertain}: the years by which the chance he has died
    reaches 25/50/75/90 %.
    None when age or health is not read."""
    a, h = _num(age), _num(health)
    if a is None or h is None:
        return None
    s = survival(a, h, HORIZON_YEARS * 12)
    years: dict[str, Optional[float]] = {}
    for name, q in QUANTILES:
        years[name] = next((round(k / 12, 1) for k, v in enumerate(s) if v <= 1.0 - q), None)
    return {"health": round(h, 2),
            "monthly_death_chance_now": round(min(1.0, monthly_death_chance(h) + outside_monthly(a)), 4),
            "years": years, "source": SOURCE, "uncertain": UNCERTAIN}


def eta_years(age: Any, health: Any) -> tuple[Optional[int], Optional[str], Optional[dict]]:
    """(succession_eta_years, its basis, the estimate): the band's 25 % year (the earlier of it and
    65 minus age, which it nearly always is); the age heuristic alone (said so) when health is not read."""
    a = _num(age)
    if not a:
        return None, None, None
    heur = max(0, int(AGE_HEURISTIC - a))
    est = estimate(age, health)
    if est is None:
        return heur, "age_heuristic: 65 minus the ruler's age (his health was not read); uncertain", None
    p25 = est["years"]["p25"]
    if p25 is not None and (p25 < heur or a >= AGE_HEURISTIC):
        return int(p25), ("health: the year by which he has a 1-in-4 chance of having died (game check + "
                          "a fitted death rate); he can die sooner"), est
    return heur, ("the earlier of 65 minus age and the band's 25 % year (later here); he can die sooner"), est


def poor(age: Any, health: Any) -> bool:
    """The 'act now' condition for the split news row: 50 or older, or health below 4."""
    a, h = _num(age), _num(health)
    return bool((a is not None and a >= 50) or (h is not None and h < POOR_HEALTH))
