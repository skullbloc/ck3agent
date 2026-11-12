"""The standing-policy mod that ships (policy_mod/ in the tarball) is the one the
bridge and the server speak to, and the install doc names it right.

The names live in three places: the bridge (bridge/include/script_templates.h
kPolicies, kPolicyModEnabledLine, kPolicyEvent), the server
(ck3agent/server/redesign.py POLICIES) and the mod's own files. A rename in one
that the others miss makes set_policy apply and nothing hold, or every
set_policy refused policy_mod_not_loaded.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT / "ck3agent" / "policy_mod" / "ck3agent_policies"
TEMPLATES = (ROOT / "bridge" / "include" / "script_templates.h").read_text(encoding="utf-8")


def _bridge_policies() -> dict[str, str]:
    block = TEMPLATES.split("kPolicies[] = {", 1)[1].split("};", 1)[0]
    return dict(re.findall(r'\{"([a-z0-9_]+)",\s*"([a-z0-9_]+)"\}', block))


def _const(name: str) -> str:
    m = re.search(name + r'\s*=\s*"([^"]+)"', TEMPLATES)
    assert m, name
    return m.group(1)


def test_the_mod_ships_its_descriptor_and_event_file():
    assert (MOD / "descriptor.mod").is_file()
    assert (MOD / "events" / "ck3agent_policy_events.txt").is_file()
    assert re.search(r'^name="ck3agent_policies"$', (MOD / "descriptor.mod").read_text(), re.M)


def test_bridge_server_and_mod_name_the_same_policies():
    pol = _bridge_policies()
    assert pol, "kPolicies not found in script_templates.h"
    sys.path.insert(0, str(ROOT / "ck3agent" / "server"))
    try:
        import redesign
    finally:
        sys.path.pop(0)
    assert tuple(pol) == tuple(redesign.POLICIES)
    events = (MOD / "events" / "ck3agent_policy_events.txt").read_text(encoding="utf-8-sig")
    for var in pol.values():
        assert f"has_variable = {var}" in events, var
    assert re.search(r"^" + re.escape(_const("kPolicyEvent")) + r" = \{", events, re.M)


def test_the_enabled_line_and_the_install_doc_name_the_descriptor():
    line = _const("kPolicyModEnabledLine")
    name, descriptor, _ = line.split("|")
    assert name == "ck3agent_policies" and descriptor == "mod/ck3agent_policies.mod"
    doc = (ROOT / "docs" / "standing-policies.md").read_text(encoding="utf-8")
    assert line in doc
    assert f'"{descriptor}"' in doc            # the dlc_load.json entry
    assert '"$G/mod/ck3agent_policies.mod"' in doc   # the descriptor file the step writes
