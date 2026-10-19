"""Who owns the clock after launch, and what the hook says when nothing is bound.

Persona test 2026-10-18: the bridge forces the first tick by unpausing and nothing
paused the game afterwards (a player save lost ten game months before Claude's
first turn); the live-state hook opened every unbound turn with a succession
emergency; and the server's wait replaced a Bash `sleep` Claude Code refuses.
"""
from __future__ import annotations

import time

from ck3agent import cli, _bridge as br
from test_hook import load_hook, report


class Proc:
    def poll(self):
        return None


def test_launch_pauses_the_game_once_active(bridge, capsys):
    fb = bridge(replies={"get_tick_info": {"session_state": "active", "game_date": "867.1.1"}})
    assert br.wait_for_active(br.socket_path(), time.time() + 5, Proc()) == 0
    assert fb.paused and ("pause_game", {}) in fb.seen
    assert "game paused" in capsys.readouterr().out


def test_launch_no_pause_leaves_the_clock_running(bridge, capsys):
    fb = bridge(replies={"get_tick_info": {"session_state": "active", "game_date": "867.1.1"}})
    assert br.wait_for_active(br.socket_path(), time.time() + 5, Proc(), pause=False) == 0
    assert not fb.paused and ("pause_game", {}) not in fb.seen
    assert "game paused" not in capsys.readouterr().out


def test_launch_parses_no_pause():
    a = cli.build_parser().parse_args(["launch", "--continue", "--no-pause"])
    assert a.cont and a.no_pause
    assert not cli.build_parser().parse_args(["launch", "--continue"]).no_pause


def test_setup_env_turns_off_mcp_auto_background():
    d = cli.merge_settings({"env": {"FOO": "1"}}, None, None, cli.SETTINGS_ENV)
    assert d["env"] == {"FOO": "1", "CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS": "0"}
    assert cli.SETTINGS_ENV == {"CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS": "0"}


def test_hook_unbound_seat_is_not_a_succession_emergency():
    hook = load_hook()
    # Paused, nothing bound: the bridge answers query_character(0) with a declined
    # but non-empty reply, which used to read as "not landed".
    out = report(hook, {"get_tick_info": {"paused": True, "game_date": "900.1.1",
                                          "controlled_character_id": 0},
                        "query_character": {"success": False, "action": "query_character"}})
    assert "SUCCESSION" not in out and hook.NO_RULER in out


def test_hook_dead_bound_ruler_still_alerts():
    hook = load_hook()
    out = report(hook, {"get_tick_info": {"paused": True, "game_date": "900.1.1",
                                          "controlled_character_id": 123},
                        "query_character": {"id": 123, "alive": False, "name": "X"}})
    assert "SUCCESSION: controlled ruler 123 is DEAD" in out


def test_doctor_says_ready_only_when_the_game_is_active():
    from pathlib import Path
    play = Path("/p")
    assert cli.doctor_last_line(None, True, "active", play).startswith("Ready. Next: cd /p")
    dormant = cli.doctor_last_line(None, True, "dormant", play)
    assert dormant.startswith("Game not active yet (session_state=dormant)") and "Ready" not in dormant
    assert cli.doctor_last_line(None, False, "", play) == "Ready. Next: ck3agent launch --continue"
    assert cli.doctor_last_line("ck3agent setup", True, "active", play) == "Next: ck3agent setup"


def test_handshake_check_returns_the_state(bridge, capsys):
    bridge(ack={"session_state": "dormant"})
    assert br.handshake_check(br.socket_path()) == (True, "dormant")
    assert "session_state=dormant" in capsys.readouterr().out
