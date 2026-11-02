"""A dead actor is a dead actor (witness #8 minor 6).

Until the final fix a call from a dead character's seat answered
`actor_dead_or_stale` (class bridge, transient, retry "now", "re-pin control via
session(action='control')"), while session(control) on the same corpse answered
`character_dead` (retry never, "claim the heir, not the corpse"). The re-pin was
a dead end and the two refusals contradicted each other. Both now go through
`character_dead`, and a call about a war names the war's leaders as query_wars
reads them now.

The replies are real, from live867 run ff-g1 gl5 r1 (l5_war_outcomes, 0868.07.16,
tick 561): step 80, do(make_peace, character_id=11098, war_id=20) from the seat
of war 20's defender after he died, and step 82, query_wars(all, verbose) in
the same paused tick (request 1b4e2bc2-5604-4a30-b097-eb25f28a6da5), war 20's
row as the bridge sent it. The dead actor's query_character reply is the
actor block that step echoed (id, name, alive false).
"""
import asyncio
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import redesign as R  # noqa: E402

#: query_wars(all, verbose) war 20, ff-g1 gl5 r1 step 82 (verbatim row).
WAR_20 = {"war_id": 20, "name": "Mursiyyah Border Raid of Al-Karas", "active": True,
          "cb_type": "fp2_border_raid", "cb_family": "raid", "primary_attacker": 9917,
          "primary_defender": 13927, "target_titles": [7706], "attackers": [9917],
          "defenders": [13927], "battle_score": -100, "battle_count": 2,
          "ticking_score": -37, "war_score": -100, "war_score_source": "computed",
          "score_breakdown": {"imprisonment": 0, "battle": -100, "occupation": 0,
                              "ticking": -36, "landless": 0},
          "favored_side": "defender", "my_side": "unknown", "sieges": []}
#: The actor block step 80 echoed for 11098.
DEAD = {"id": 11098, "name": "al-Fadl", "alive": False}


class Gw:
    def __init__(self, wars):
        self.wars = wars
        self.reads = []
        self.health = "ok"

    async def actor_gate(self):
        # Several rulers held: an id-less call would be refused; these calls name 11098.
        return R.why_blocked("actor_required")

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return dict(DEAD)
        if action == "query_wars":
            return {"scope": "all_active", "wars": self.wars}
        raise AssertionError(f"a refused dead actor read {action}")

    async def write(self, action, params=None):
        raise AssertionError(f"a refused dead actor wrote {action}")

    def game(self):
        return {"tick": 561, "date": "0868.07.16", "paused": True, "speed": 3}


def _make_peace(gw, war_id=20):
    return asyncio.run(R.do_impl(gw, intent="make_peace", character_id=11098,
                                 args={"war_id": war_id, "terms": "enforce_demands"}))


def test_a_dead_actor_is_character_dead_and_never_retried():
    env = _make_peace(Gw([WAR_20]))
    b = env["blocked"]
    assert env["ok"] is False
    assert (b["gate"], b["retryable"], b["retry"]) == ("character_dead", False, "never"), b
    assert "re-pin" not in b["summary"], b["summary"]


def test_the_refusal_names_the_wars_leaders_as_read_now():
    b = _make_peace(Gw([WAR_20]))["blocked"]
    assert b["measured"] == {"war_id": 20, "attacker_id": 9917, "defender_id": 13927,
                             "dead_actor_side": None}
    assert "9917" in b["summary"] and "13927" in b["summary"], b["summary"]


def test_a_dead_actor_still_listed_on_a_side_is_pointed_at_that_sides_leader():
    war = dict(WAR_20, defenders=[13927, 11098])
    b = _make_peace(Gw([war]))["blocked"]
    assert b["measured"]["dead_actor_side"] == "defender"
    assert "defender leader now is 13927" in b["summary"], b["summary"]
    assert "character_id=13927" in b["summary"]


def test_a_war_that_is_gone_is_said_to_be_gone():
    b = _make_peace(Gw([]))["blocked"]
    assert b["gate"] == "character_dead"
    assert b["measured"] == {"war_id": 20, "war_listed": False}


def test_a_dead_actor_with_no_war_reads_nothing_else():
    gw = Gw([WAR_20])
    env = asyncio.run(R.do_impl(gw, intent="set_focus", character_id=11098, args={}))
    assert env["blocked"]["gate"] == "character_dead"
    assert [a for a, _ in gw.reads] == ["query_character"]


def test_the_bridges_dead_and_missing_actor_codes_map_to_the_one_gate():
    for raw in ("actor_not_alive", "owner_not_alive"):
        assert R.RAW_REASON_TO_GATE[raw] == "character_dead", raw
        assert R.map_raw_reason(raw)["gate"] == "character_dead", raw
        assert R.why_blocked("unmapped_refusal", raw_reason=raw)["retry"] == "never", raw
    for raw in ("actor_not_found", "owner_not_found"):
        assert R.RAW_REASON_TO_GATE[raw] == "character_not_found", raw


def test_no_gate_tells_a_dead_actor_to_retry():
    assert "actor_dead_or_stale" not in R.GATE_TABLE
    assert "actor_dead_or_stale" not in set(R.RAW_REASON_TO_GATE.values())


# ── witness #9 minor 5: a war read that failed is not a war that is gone, and
#    war 0 is a war ──

class FailedWarsGw(Gw):
    """query_wars answers what a failed read answers: the gateway's {} on a
    timeout, or the reader's fault reply."""

    def __init__(self, reply):
        super().__init__([])
        self.reply = reply

    async def read(self, action, params=None):
        if action == "query_wars":
            self.reads.append((action, dict(params or {})))
            return self.reply
        return await super().read(action, params)


def test_a_failed_war_read_is_said_to_have_failed_not_that_the_war_is_gone():
    for reply in ({}, None, {"error": "memory_access_fault"}, {"scope": "all_active"}):
        b = _make_peace(FailedWarsGw(reply))["blocked"]
        assert b["gate"] == "character_dead"
        assert b["measured"] == {"war_id": 20, "war_read": "failed"}, (reply, b["measured"])


def test_a_war_missing_from_a_degraded_list_is_not_said_to_be_gone():
    gw = FailedWarsGw({"scope": "all_active", "wars": [], "degraded": True, "fault_count": 2})
    assert _make_peace(gw)["blocked"]["measured"] == {"war_id": 20, "war_read": "degraded"}


def test_war_0_is_a_war():
    """war 0 is the first war the game made (re_war.md section 6), not "no war"."""
    war0 = dict(WAR_20, war_id=0)
    gw = Gw([war0])
    b = _make_peace(gw, war_id=0)["blocked"]
    assert [a for a, _ in gw.reads] == ["query_character", "query_wars"]
    assert b["measured"] == {"war_id": 0, "attacker_id": 9917, "defender_id": 13927,
                             "dead_actor_side": None}


def test_a_war_id_that_is_not_a_number_is_no_war():
    gw = Gw([WAR_20])
    env = asyncio.run(R.do_impl(gw, intent="make_peace", character_id=11098,
                                args={"war_id": "twenty", "terms": "enforce_demands"}))
    assert env["blocked"]["gate"] == "character_dead"
    assert "query_wars" not in [a for a, _ in gw.reads]


# ── witness #9 minor 7: the bridge's actor refusals, one reason per cause ──

REPO = HERE.parents[1]
BRIDGE = REPO / "bridge"
# L13 FF3 (witness X4): the one C++ tokenizer (bridge/scripts/cpp_strip.py).
sys.path.insert(0, str(BRIDGE / "scripts"))
import cpp_strip  # noqa: E402


def _bridge_text() -> str:
    files = sorted((BRIDGE / "src").glob("*.cpp")) + sorted((BRIDGE / "include").glob("*.h"))
    return "\n".join(p.read_text(errors="replace") for p in files)


def _actor_refusal_reasons() -> list:
    import re
    hdr = (BRIDGE / "include" / "action_dispatch_internal.h").read_text()
    i = hdr.index("inline const char* actor_refusal(void* actor)")
    body = hdr[i:hdr.index("\n}\n", i)]
    return re.findall(r'return\s+"(\w+)"', body)


def test_the_bridge_answers_a_missing_actor_and_a_dead_one_apart():
    import pytest
    if not (BRIDGE / "include" / "action_dispatch_internal.h").is_file():
        pytest.skip("no bridge sources in this layout")
    assert _actor_refusal_reasons() == ["actor_not_found", "actor_not_alive"]
    assert R.RAW_REASON_TO_GATE["actor_not_found"] == "character_not_found"
    assert R.RAW_REASON_TO_GATE["actor_not_alive"] == "character_dead"
    assert R.map_raw_reason("actor_not_found")["retry"] == "never"


def test_no_bridge_site_answers_the_word_that_said_both():
    """no_living_actor was answered for a null actor too (an out-of-range id, no
    controlled character), and read as "dead". Census of the bridge source
    (code, strings included) and of the MCP's map."""
    import pytest
    import re
    if not (BRIDGE / "src").is_dir():
        pytest.skip("no bridge sources in this layout")
    code = cpp_strip.strip(_bridge_text(), keep_strings=True)
    assert '"no_living_actor"' not in code
    assert "no_living_actor" not in R.RAW_REASON_TO_GATE


def test_every_actor_refusal_site_asks_the_one_helper():
    """Census: every `!actor || !g_char_is_alive(actor)` test in the bridge
    handlers is gone (each now asks actor_refusal)."""
    import pytest
    import re
    if not (BRIDGE / "src").is_dir():
        pytest.skip("no bridge sources in this layout")
    ah = (BRIDGE / "src" / "action_handlers.cpp").read_text()
    assert not re.search(r"!\s*\w*actor\s*\|\|\s*!\s*g_char_is_alive", ah)
    assert len(re.findall(r"\bactor_refusal\s*\(", ah)) >= 15


def test_every_actor_reason_the_map_holds_has_an_emitter():
    """Census (witness #10 minor 10): every `actor_*` raw reason in
    RAW_REASON_TO_GATE is a string the bridge answers somewhere (code, strings
    kept, comments stripped). A key with no emitter is dead vocabulary, and a
    map that keeps one reads as a promise the bridge never makes. Fail OPEN: the
    census must find the actor reasons it maps (two at least)."""
    import pytest
    import re
    if not (BRIDGE / "src").is_dir():
        pytest.skip("no bridge sources in this layout")
    code = cpp_strip.strip(_bridge_text(), keep_strings=True)
    keys = [k for k in R.RAW_REASON_TO_GATE if k.startswith("actor_")]
    assert len(keys) >= 2, keys
    assert [k for k in keys if f'"{k}"' not in code] == []
