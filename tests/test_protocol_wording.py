"""The shipped protocol must not tell Claude to do what the bridge refuses.

Measured live on 2026-10-18: `release_character` for a control session's only
ruler is refused with `last_claim_of_control_session`; the ruler is freed when
the session ends. Three protocol files used to instruct that release as the
normal stop step.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / "ck3agent" / "protocol"
PROTOCOL_FILES = ("CLAUDE.md", "CLAUDE-desktop.md", "run-prompt.md")


def _stopping_sections():
    for name in PROTOCOL_FILES:
        text = (PROTOCOL / name).read_text(encoding="utf-8")
        yield name, text


def _section(text, heading):
    """The body of `## heading`, up to the next `## `; "" when absent."""
    start = text.find("## " + heading)
    if start < 0:
        return ""
    end = text.find("\n## ", start + 3)
    return text[start:] if end < 0 else text[start:end]


def test_no_protocol_file_instructs_releasing_the_ruler_to_stop():
    # The stop steps: the "Stopping" sections and the whole run prompt. A
    # release of ONE OF SEVERAL rulers is allowed and documented under
    # "Several rulers"; releasing to stop is not.
    for name, text in _stopping_sections():
        stop = text if name == "run-prompt.md" else _section(text, "Stopping")
        assert stop, f"{name}: no stop section"
        assert 'session(action="release"' not in stop, name
    for name in ("CLAUDE.md", "CLAUDE-desktop.md"):
        text = (PROTOCOL / name).read_text(encoding="utf-8")
        outside = text.replace(_section(text, "Several rulers"), "")
        assert 'session(action="release"' not in outside, name


def test_the_several_rulers_section_says_the_five_rules():
    """lane-multi: add, pin character_id, one posture each, the process-wide veto,
    one clock, and that only a non-last ruler can be released."""
    for name in ("CLAUDE.md", "CLAUDE-desktop.md"):
        sec = " ".join(_section((PROTOCOL / name).read_text(encoding="utf-8"),
                                "Several rulers").split())
        assert sec, name
        assert 'session(action="add", character_id=<id>)' in sec, name
        assert "`control`" in sec and "switches" in sec, name
        assert "my_claims" in sec and "character_id" in sec, name
        assert 'set_phase(phase=..., character_id=<id>)' in sec, name
        assert "process-wide" in sec and "expansion" in sec, name
        assert "one clock" in sec.lower(), name
        assert 'session(action="release", character_id=<id>)' in sec, name
        assert "one of several" in sec and "freed when the session ends" in sec, name
        assert "heir of the primary only" in sec, name


def test_the_run_prompt_binds_every_ruler_the_goal_names():
    text = (PROTOCOL / "run-prompt.md").read_text(encoding="utf-8")
    assert "my_claims" in text
    assert 'session(action="add", character_id=...)' in text
    assert 'session(action="control", character_id=...)' in text
    assert '"Several rulers" in CLAUDE.md' in text


def test_protocol_files_name_the_refusal_or_the_run_frees_it():
    for name, text in _stopping_sections():
        assert "last_claim_of_control_session" in text or "frees it on exit" in text, name


def test_protocol_waits_through_the_server_not_bash_sleep():
    # Claude Code refuses a foreground `sleep` of 30 s or more (2026-10-18); the
    # wait between cycles is session(action="wait").
    for name, text in _stopping_sections():
        assert "sleep 60" not in text, name
        if name != "CLAUDE-desktop.md":
            assert 'session(action="wait"' in text, name


def test_bridge_contract_lists_the_last_claim_refusal():
    text = (ROOT / "docs" / "reference" / "bridge-contract.md").read_text(encoding="utf-8")
    assert "last_claim_of_control_session" in text
    assert "frees every ruler it held" in text
