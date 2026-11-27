"""What a Munster seated run found.

- the mercenary veto never matched the AI's hire: the offsets header had CHireMercenaryCompanyCommand's two
vtables swapped (the veto compares cmd+0x00 with HIRE_MERC_CMD_VTABLE_PRIMARY). Checked here from the constructor's
own bytes (pinned from the binary, md5 09ba163bbff47f2eb8a69692536307e6, and re-read from the install when present),
with CAddLawCommand's constructor as the control. The hook's interaction leg is bridge ctest test_ai_veto rows 17a-c.
- the purse did not attribute the AI's d_meath creation that the same step's ai_acted row priced; the
k_ireland row carried the duchy text.
- claims_lost for a title the ruler now holds; vassals_joined named twice
across two baselines; succession_plan's shares listed twice; plan_war's de jure row read title null, dejure_gain 0.
"""
import asyncio
import os
import re
from pathlib import Path

import pytest

import news
import redesign as R
from test_ireland_walk_replies import Gw

MUNSTER, BRIAN = 31762, 36303
ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("CK3_GAME_DIR") or
              Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game").parent / "binaries" / "ck3"
BINARY_MD5 = "09ba163bbff47f2eb8a69692536307e6"


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    yield
    news.reset()


# ── The hire command's vtables, from its constructor's bytes ──────────────────────────────────

#: (header offset of the constructor, its first bytes) read from the 1.18.3.1 binary (objdump: the ctor of
#: CHireMercenaryCompanyCommand, its default/clone ctor, and CAddLawCommand's ctor FUN_067cd330)
CTORS = {
    "merc_ctor": (0x600a410, "554157415653504189ce4189d789f34889fd31f6e807125501488d05f832b50548894500488d059d33b5"
                             "0548894518895d2044897d24448975284883c4085b41"),
    "merc_clone": (0x600b720, "41565350bf30000000e8822161fb4889c34889c731f6e8f5fe5401488d05e61fb505488903488d058c20"
                              "b5054889431848c74320ffffffffc743280100000048"),
    "law_ctor": (0x66cd330, "554156534989d689f54889fb31f6e8ede2e800488d0596b95c05488903488d053cba5c0548894318896b20"
                            "4c8973285b"),
}
#: mov [reg+disp8], rax / mov [reg], rax after a lea rax,[rip+disp32]: the field it stores to
_STORE = {bytes.fromhex("48894500"): 0x00, bytes.fromhex("488903"): 0x00,
          bytes.fromhex("48894518"): 0x18, bytes.fromhex("48894318"): 0x18}


def vtable_stores(va: int, code: bytes) -> dict:
    """{object offset: the address stored there} for each `lea rax,[rip+d]; mov [this+off], rax` in `code`."""
    out = {}
    for i in range(len(code) - 7):
        if code[i:i + 3] != b"\x48\x8d\x05":
            continue
        tgt = va + i + 7 + int.from_bytes(code[i + 3:i + 7], "little", signed=True)
        for pat, off in _STORE.items():
            if code[i + 7:i + 7 + len(pat)] == pat:
                out[off] = tgt
    return out


def header_value(name: str) -> int:
    h = (ROOT / "re" / "offsets" / "ck3_offsets.h").read_text()
    m = re.search(rf"constexpr uintptr_t {name}\s*=\s*(0x[0-9a-fA-F]+);", h)
    assert m, name
    return int(m.group(1), 16)


def test_1a_the_primary_vtable_is_what_the_constructors_store_at_cmd_0():
    for k in ("merc_ctor", "merc_clone"):
        va, hx = CTORS[k]
        st = vtable_stores(va, bytes.fromhex(hx))
        assert st[0x00] == header_value("HIRE_MERC_CMD_VTABLE_PRIMARY"), (k, hex(st[0x00]))
        assert st[0x18] == header_value("HIRE_MERC_CMD_VTABLE_HELPER"), (k, hex(st[0x18]))


def test_1a_CONTROL_the_decoder_reads_the_law_commands_vtable_the_veto_already_matched():
    va, hx = CTORS["law_ctor"]
    assert vtable_stores(va, bytes.fromhex(hx))[0x00] == header_value("ADD_LAW_CMD_VTABLE")


def test_1b_the_pinned_bytes_are_the_installs():
    if not BINARY.is_file():
        pytest.skip(f"no game binary at {BINARY}")
    import hashlib
    h = hashlib.md5()
    with BINARY.open("rb") as f:
        for c in iter(lambda: f.read(1 << 22), b""):
            h.update(c)
    if h.hexdigest() != BINARY_MD5:
        pytest.skip(f"another build: {h.hexdigest()}")
    with BINARY.open("rb") as f:
        for k, (va, hx) in CTORS.items():
            f.seek(va)                               # the text segment maps file offset = address
            assert f.read(len(hx) // 2).hex() == hx, k


def test_1c_the_veto_compares_with_the_primary():
    src = (ROOT / "bridge" / "src" / "ai_veto.cpp").read_text()
    assert "g_vt[static_cast<int>(av::What::kMercenary)] = g_base_addr + ck3::globals::HIRE_MERC_CMD_VTABLE_PRIMARY;" \
        in src


def test_1d_an_adventurers_hire_record_reads_as_a_mercenary_row():
    import seat_events as SE
    row = SE.attempt_row({"date": "1067.05.02", "what": "mercenary", "dropped": True, "laamp_id": 35996,
                          "key": "hire_laamp_mercenaries_interaction", "priced": "no", "gold": 277.6})
    assert row["laamp_id"] == 35996 and row["key"] == "hire_laamp_mercenaries_interaction"


# ── The AI's creation priced from the bridge's own record; the tier's text ────────────────────

#: A recorded game, step 15 (1070.11.28 -> 1071.07.26) and step 31 (1079.12.26 -> 1080.12.26): the news' own rows
MEATH = {"date": "1070.12.16", "gold_at_attempt": 162.91, "key": "d_meath", "prestige_at_attempt": 615.72,
         "price": {"gold": 162.5}, "sent_by": "the game's AI for your ruler (no veto set for it)", "title_id": 462,
         "what": "create_title"}
IRELAND = {"date": "1080.05.25", "gold_at_attempt": 325.21, "key": "k_ireland", "prestige_at_attempt": 729.89,
           "price": {"gold": 325.0}, "sent_by": "the game's AI for your ruler (no veto set for it)", "title_id": 461,
           "what": "create_title"}


def _gold(was, now, d0, d1, gained, acted, inc=4.46):
    prev = {"actor_id": MUNSTER, "gold": was, "date": d0, "monthly_income": inc}
    snap = {"actor_id": MUNSTER, "gold": now, "date": d1}
    return news._resource_change(prev, snap, "gold", {"titles_gained": [gained], "ai_acted": acted})


def test_3a_the_ais_duchy_creation_is_priced_from_its_record():
    g = _gold(159.56, 56.18, "1070.11.28", "1071.07.26", {"how": None, "key": "d_meath", "title_id": 462}, [MEATH])
    (row,) = [e for e in g["in_this_step"] if "d_meath" in e.get("what", "")]
    assert row["expected"] == -162.5 and "1070.12.16" in row["what"] and "no read says which" not in row["what"]
    assert "no create_title veto set" in row["decided_by"] and "covered its price" in row["decided_by"]
    assert row["counts"] == news.TIER_RULE_NOTE
    assert g.get("unexplained", 0) > -138.54


def test_3a_CONTROL_with_no_record_it_still_says_no_read_says_which():
    g = _gold(159.56, 56.18, "1070.11.28", "1071.07.26", {"how": None, "key": "d_meath", "title_id": 462}, [])
    (row,) = [e for e in g["in_this_step"] if "d_meath" in e.get("what", "")]
    assert "no read says which" in row["what"] and "expected" not in row


def test_3b_the_kingdoms_row_carries_no_duchy_text():
    g = _gold(299.5, 28.08, "1079.12.26", "1080.12.26", {"how": None, "key": "k_ireland", "title_id": 461}, [IRELAND])
    (row,) = [e for e in g["in_this_step"] if "k_ireland" in e.get("what", "")]
    assert row["expected"] == -325.0 and "counts" not in row
    assert "highest title is a county" not in row["decided_by"] and "duchy" not in row["decided_by"]
    assert "what" not in row["lever"] and row["lever"]["mint_yourself"]["args"]["args"]["title_key"] == "k_ireland"


def test_3b_CONTROL_a_count_short_of_the_price_keeps_the_gold_skip_rule():
    short = dict(MEATH, gold_at_attempt=29.6, price={"gold": 212.5})
    assert news.ai_created_by("d_meath", short) == "the game's AI for your ruler: " + news.AI_CREATE_RULE


def test_3c_the_prestige_row_names_the_ais_creation():
    prev = {"actor_id": MUNSTER, "prestige": 613.91, "date": "1070.11.28", "gold": 159.56}
    snap = {"actor_id": MUNSTER, "prestige": 930.2, "date": "1071.07.26", "gold": 56.18}
    p = news._resource_change(prev, snap, "prestige", {"titles_gained": [{"how": None, "key": "d_meath",
                                                                        "title_id": 462}], "ai_acted": [MEATH]})
    (row,) = [e for e in p["in_this_step"] if "d_meath" in e.get("what", "")]
    assert row["expected"] == 300 and "created for you by the game's AI on 1070.12.16" in row["what"]


# ── claims, vassals, shares and the de jure row ─────────────────────────────────────────────────────────────────────────────────────────────

def test_4a_a_claim_on_a_title_now_held_is_not_lost():
    """A recorded game, step 31: claims_lost {"title_id": 461, "title_name": "Ireland", "type": "unpressed"}."""
    claim = {"holder_id": None, "title_id": 461, "title_name": "Ireland", "type": "unpressed"}
    prev = {"actor_id": MUNSTER}
    snap = {"actor_id": MUNSTER, "titles": {461: "k_ireland", 518: "d_munster"}}
    out = news.claims_ended(prev, snap, {461: claim}, {})
    assert out["claims_lost"] == [] and out["claims_now_held"][0]["title_id"] == 461


def test_4a_CONTROL_a_claim_on_a_title_he_does_not_hold_is_lost():
    claim = {"holder_id": 31266, "title_id": 474, "title_name": "Ulster", "type": "pressed"}
    out = news.claims_ended({"actor_id": MUNSTER}, {"actor_id": MUNSTER, "titles": {518: "d_munster"}},
                            {474: claim}, {})
    assert out["claims_lost"] == [claim] and "claims_now_held" not in out


def test_4b_the_same_growth_named_again_says_when_it_was_named_first():
    """A recorded game: 8 -> 9 in the turn news of 1070.11.28 and again in the seat's observe of 1071.07.26."""
    turn = news.vassals_joined({"actor_id": MUNSTER, "realm_counties": 8, "date": "1070.10.20"},
                               {"actor_id": MUNSTER, "realm_counties": 9, "date": "1070.11.28"}, {})
    assert "named_before" not in turn
    obs = news.vassals_joined({"actor_id": MUNSTER, "realm_counties": 8, "date": "1070.05.23"},
                              {"actor_id": MUNSTER, "realm_counties": 9, "date": "1071.07.26"}, {})
    assert obs["named_before"]["date"] == "1070.11.28" and "1070.05.23" in obs["named_before"]["why"]


def test_4b_CONTROL_a_new_growth_is_not_marked():
    news.vassals_joined({"actor_id": MUNSTER, "realm_counties": 8}, {"actor_id": MUNSTER, "realm_counties": 9,
                                                                       "date": "1070.11.28"}, {})
    row = news.vassals_joined({"actor_id": MUNSTER, "realm_counties": 9}, {"actor_id": MUNSTER, "realm_counties": 10,
                                                                           "date": "1072.01.01"}, {})
    assert "named_before" not in row


def _plan_gw():
    from test_defeat_prices import _b_gw
    base = _b_gw()
    kids = {BRIAN: {"id": BRIAN, "alive": True, "is_female": False, "age": 41},
            71992: {"id": 71992, "alive": True, "is_female": False, "age": 15},
            64065: {"id": 64065, "alive": True, "is_female": False, "age": 12}}
    return Gw({**base.replies, "query_character": lambda p: kids.get(p.get("character_id"), {})})


def test_4c_a_share_is_listed_once():
    """The 1089 read: each duchy share in younger_shares and again in tier_shares (12.6 KB)."""
    from test_defeat_prices import SIM_1089
    fc, _ = run(R.partition_forecast_for(_plan_gw(), {"id": MUNSTER, "ruler_tier": 3,
                                                      "children": [BRIAN, 71992, 64065]},
                                         ["confederate_partition_succession_law", "male_preference_law"],
                                         {"primary": {"id": BRIAN}}, SIM_1089))
    taken = sorted(t for s in fc["younger_shares"] for t in s.get("takes") or [])
    assert taken == ["d_leinster", "d_ulster"] and "tier_shares" not in fc and "tier_shares_untaken" not in fc


def test_4c_CONTROL_a_share_no_son_takes_is_still_listed():
    from test_defeat_prices import SIM_1089
    fc, _ = run(R.partition_forecast_for(_plan_gw(), {"id": MUNSTER, "ruler_tier": 3, "children": [BRIAN, 71992]},
                                         ["confederate_partition_succession_law", "male_preference_law"],
                                         {"primary": {"id": BRIAN}}, SIM_1089))
    assert [t["title"] for t in fc["tier_shares_untaken"]] == ["d_leinster"]


def test_4d_the_de_jure_row_names_its_title_and_a_county_gained():
    """A recorded game, turn 1: {"cb": "individual_county_de_jure_cb", "cb_title_id": 531}, "title": null, "dejure_gain": 0."""
    import test_plan_war_claim_rows as T
    conquest = T._venaissin_conquest()
    rc = conquest["duchies"][0]["remaining_counties"][0]
    rc.update(conquerable_now=True, block_reason="",
              declare={"casus_belli": "individual_county_de_jure_cb", "target_id": T.LOTHAR,
                       "cb_title_id": T.C_VENAISSIN})
    dj = {"cb_key": "individual_county_de_jure_cb", "family": "de_jure", "valid": True, "scope": "county",
          "cb_title_id": T.C_VENAISSIN, "needs_cb_title_id": True, "target_mode": 1, "cost_prestige": 100,
          "cost_piety": 0, "affordable": True}
    gw = T._gw(query_conquest_targets=conquest, query_cb_validity_strict=T._strict("individual_county_de_jure_cb"),
               query_cbs_for_target=T._cbs([dj]), query_dejure_progress={"held": 1, "total": 4})
    env = run(R.plan_war_impl(gw, character_id=T.CHARLES, goal_scoped=False, ally_free_only=False))
    (row,) = env["result"]["targets"]
    assert row["declare_args"]["cb"] == "individual_county_de_jure_cb"
    assert row["title"] not in (None, 0) and row["dejure_gain"] >= 1 and row["is_de_jure"] is True


def test_1e_a_dropped_hire_names_its_company():
    """a live check: ai_vetoed {"what": "mercenary", "attempts": 1, ...} named no company."""
    import seat_events as SE
    snap = {"ai_veto_records": [{"seq": 1, "date": "1067.06.26", "character_id": MUNSTER, "what": "mercenary",
                                       "dropped": True, "company_id": 58, "origin": "game", "priced": "no",
                                       "gold": 277.56, "prestige": 895.56}], "actor_id": MUNSTER}
    rows = SE.veto_news({}, snap).get("ai_vetoed") or []
    assert rows and rows[0]["company_id"] == 58
