"""The incoming-proposal mod that ships (proposal_mod/ in the tarball: the fixed
files and the generator of the interaction copies) is the one the bridge and the
server speak to, and the install doc names it right.

The names live in four places: the bridge (bridge/include/script_templates.h
kProposalClasses, kProposalModEnabledLine), the server
(ck3agent/server/proposals.py CLASSES, MOD_ENV), the generator
(ck3agent/proposal_mod/proposal_overrides.py CLASSES) and the mod's descriptor.
A rename in one that the others miss holds nothing, or refuses every call
proposal_mod_not_loaded.
"""
from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PKG = ROOT / "ck3agent" / "proposal_mod"
MOD = PKG / "ck3agent_proposals"
TEMPLATES = (ROOT / "bridge" / "include" / "script_templates.h").read_text(encoding="utf-8")
DOC = (ROOT / "docs" / "incoming-proposals.md").read_text(encoding="utf-8")


def _generator():
    spec = importlib.util.spec_from_file_location("proposal_overrides", PKG / "proposal_overrides.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["proposal_overrides"] = mod
    spec.loader.exec_module(mod)
    return mod


def _server():
    sys.path.insert(0, str(ROOT / "ck3agent" / "server"))
    try:
        import proposals
    finally:
        sys.path.pop(0)
    return proposals


def _const(name: str) -> str:
    m = re.search(name + r'\s*=\s*"([^"]+)"', TEMPLATES)
    assert m, name
    return m.group(1)


def test_the_mod_ships_its_descriptor_and_its_generator():
    assert re.search(r'^name="ck3agent_proposals"$', (MOD / "descriptor.mod").read_text(), re.M)
    assert (PKG / "proposal_overrides.py").is_file()
    assert sorted(p.name for p in (MOD / "common").glob("*/*.txt")), "the mod's fixed files are missing"


def test_bridge_server_and_generator_name_the_same_classes():
    block = TEMPLATES.split("kProposalClasses[] = {", 1)[1].split("};", 1)[0]
    bridge = tuple(re.findall(r'\{"([a-z_]+)",\s*"[a-z_]+"\}', block))
    assert bridge, "kProposalClasses not found in script_templates.h"
    assert bridge == tuple(_generator().CLASSES) == tuple(_server().CLASSES)
    po, srv = _generator(), _server()
    assert {k: v[0] for k, v in po.FIRST_SET.items()} == srv.KIND_CLASS
    assert {k: v[1] for k, v in po.FIRST_SET.items()} == srv.FAMILY


def test_the_enabled_line_and_the_install_doc_name_the_descriptor():
    line = _const("kProposalModEnabledLine")
    name, descriptor, _ = line.split("|")
    assert name == "ck3agent_proposals" and descriptor == "mod/ck3agent_proposals.mod"
    assert line in DOC
    assert f'"{descriptor}"' in DOC                       # the dlc_load.json entry
    assert '"$G/mod/ck3agent_proposals.mod"' in DOC       # the descriptor file the step writes
    assert "python3 proposal_mod/proposal_overrides.py generate" in DOC
    assert f'["{_server().MOD_ENV}"] = "{_server().MOD_DECLARED}"' in DOC


def test_a_copy_is_the_source_plus_marked_lines_only():
    po = _generator()
    src = ("arrange_marriage_interaction = {\n\tcategory = interaction_category_diplomacy\n"
           "\tis_shown = {\n\t\talways = yes\n\t}\n\ton_accept = {\n\t\tadd_gold = 1\n\t}\n}\n")
    copy, done = po.inject(src, {"arrange_marriage_interaction": ("marriage", "resend")})
    assert done == ["arrange_marriage_interaction"]
    assert copy != src and po.strip_marked(copy) == src
    added = po.added_lines(copy)
    assert added and all(po.ALLOWED_LINE.match(l) for l in added)


def test_the_generator_refuses_a_directory_that_is_not_a_game_install(tmp_path):
    po = _generator()
    with pytest.raises(po.Refused) as e:
        po.generate(tmp_path)
    assert e.value.code == "game_interactions_missing"
