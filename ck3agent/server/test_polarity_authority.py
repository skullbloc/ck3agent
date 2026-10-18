"""ck-6kvq.9 / §2.2 — the bridge is the single polarity authority.

OFFLINE acceptance for the bead: "a polarity disagreement between bridge and
client is reported as an error rather than silently mis-routed."

The gateway keeps its own copy of the taxonomy (mcp_server.py) because it needs
one to pick a pause flip and a timeout. That copy is a HINT. The bridge's table
is the authority — it is the thing that actually partitions a flush buffer, and
the scheduler cannot invent a polarity it is handed. These tests assert three
things:

  1. the bridge's classification WINS over the local one;
  2. a disagreement is returned AND logged at ERROR — never silently corrected;
  3. agreement is silent, and an unparseable/absent table leaves the local
     classification in place rather than mis-routing on garbage.
"""

import logging
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

mcp_server = pytest.importorskip("mcp_server")

BridgeGateway = mcp_server.BridgeGateway
Polarity = BridgeGateway.Polarity


class _FakeState:
    """The gateway only touches state for IPC, which these tests never do."""
    mock_mode = True
    connected = False


def _gw():
    return BridgeGateway(_FakeState())


def test_bridge_table_overrides_the_local_hint():
    gw = _gw()
    # Locally, an unrecognised `query_*` name defaults to PAUSED.
    assert gw._polarity("query_made_up_thing") is Polarity.PAUSED

    gw.sync_polarity_table({"query_made_up_thing": "tick_read"})
    # After sync the BRIDGE decides.
    assert gw._polarity("query_made_up_thing") is Polarity.UNPAUSED_TICK


def test_disagreement_is_returned_and_logged_as_an_error(caplog):
    gw = _gw()
    # query_realm is a plain PAUSED read in the gateway's table.
    assert gw._classify_local("query_realm") is Polarity.PAUSED

    with caplog.at_level(logging.ERROR):
        disagreements = gw.sync_polarity_table({"query_realm": "tick_write"})

    assert len(disagreements) == 1
    d = disagreements[0]
    assert d["action"] == "query_realm"
    assert d["client"] == "paused"
    assert d["bridge"] == "unpaused"

    errors = [r.getMessage() for r in caplog.records if r.levelno >= logging.ERROR]
    assert any("POLARITY DISAGREEMENT" in m for m in errors)
    assert any("query_realm" in m for m in errors)

    # And the correction is applied, not merely complained about.
    assert gw._polarity("query_realm") is Polarity.UNPAUSED_TICK


def test_the_known_live_disagreement_is_caught():
    """query_conquest_targets is an is_deferred_drain_action in the bridge and
    sits in the gateway's _UNPAUSED_READS. The two agree on the OUTCOME here but
    are reached by different rules — so the check must not be fooled into
    silence by a coincidence, and must flag the case where they truly differ."""
    gw = _gw()
    assert gw._classify_local("query_conquest_targets") is Polarity.UNPAUSED_TICK
    # Bridge agrees (tick_read collapses to UNPAUSED_TICK for the gateway).
    assert gw.sync_polarity_table({"query_conquest_targets": "tick_read"}) == []
    # Now the genuinely divergent case.
    assert gw.sync_polarity_table({"query_conquest_targets": "paused"}) == [
        {"action": "query_conquest_targets", "client": "unpaused", "bridge": "paused"}
    ]


def test_agreement_is_silent(caplog):
    gw = _gw()
    with caplog.at_level(logging.ERROR):
        disagreements = gw.sync_polarity_table({
            "get_tick_info": "any",
            "query_realm": "paused",
            "declare_war": "tick_write",
            "query_conquest_targets": "tick_read",
        })
    assert disagreements == []
    assert not [r for r in caplog.records if r.levelno >= logging.ERROR]


def test_unknown_wire_polarity_is_reported_and_not_adopted(caplog):
    gw = _gw()
    with caplog.at_level(logging.ERROR):
        disagreements = gw.sync_polarity_table({"query_realm": "sideways"})
    assert disagreements == []
    assert any("unknown polarity" in r.getMessage() for r in caplog.records
               if r.levelno >= logging.ERROR)
    # Never adopted: a garbage value must not mis-route the call.
    assert gw._polarity("query_realm") is Polarity.PAUSED


def test_empty_table_leaves_the_local_classification_alone():
    gw = _gw()
    assert gw.sync_polarity_table({}) == []
    assert gw._polarity("get_tick_info") is Polarity.ANY
    assert gw._polarity("query_realm") is Polarity.PAUSED
    assert gw._polarity("declare_war") is Polarity.UNPAUSED_TICK


def test_tick_split_collapses_correctly_for_the_gateway():
    """The bridge splits TICK into tick_read/tick_write (§2.2: reads must drain
    before the writes of the same flush). The gateway has one UNPAUSED_TICK for
    both, which is correct for its purpose — it only picks a flip and a timeout,
    and the ORDERING is the bridge's business."""
    gw = _gw()
    gw.sync_polarity_table({"a_read": "tick_read", "a_write": "tick_write",
                            "a_legacy": "tick"})
    assert gw._polarity("a_read") is Polarity.UNPAUSED_TICK
    assert gw._polarity("a_write") is Polarity.UNPAUSED_TICK
    assert gw._polarity("a_legacy") is Polarity.UNPAUSED_TICK
