"""The factions a VASSAL ruler is in, said by observe's news.

The game's AI may put a vassal ruler into a faction against his liege, and a faction he LEADS fights its war, when
its demand is refused, with him as its attacker (common/scripted_effects/00_faction_effects.txt
faction_start_war): a war his line declared. Before this, no read the seat is handed named his own memberships:
news said only the factions aimed AT him, and the vassal's options were a listing he had to ask
for (do action_class=factions_to_join). The rows here come from the bridge's query_factions on the liege (the
member ids the game keeps, the read factions.py's writes already confirm with). Fixture: a query_factions reply
recorded live (a test run, 0873.05.28, liege 12618), copied whole. Built, not proven live on a vassal seat's observe.
"""
import asyncio
import re
import json
from pathlib import Path

import pytest

import faction_letters as FL
import news
import redesign as R
from test_ireland_walk_replies import NewsGw

HERE = Path(__file__).resolve().parent
REC = json.loads((HERE / "fixtures" / "query_factions_867.json").read_text(encoding="utf-8"))["reply"]
LIEGE, LEADER, MEMBER, INDEP_LEADER, OUTSIDER = 12618, 12864, 14505, 10845, 99
CLAIMANT = 88
LEAVE_88 = {"tool": "do", "args": {"intent": "faction", "args": {"action": "leave", "faction_id": CLAIMANT}}}


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    news._FABRICATING.clear()
    yield
    news.reset()
    news._FABRICATING.clear()


def _without(reply: dict, fid: int, who: int) -> dict:
    """The recorded reply with `who` taken out of faction `fid`'s member ids (the state before he joined)."""
    out = json.loads(json.dumps(reply))
    for f in out["factions"]:
        if f["faction_id"] == fid:
            f["members"] = [m for m in f["members"] if m != who]
    return out


def test_1_the_recorded_reply():
    """The fixture is the bridge's own reply: four factions aimed at 12618, the claimant faction led by 12864 with
    14505 and 12521 in it, an independence faction led by 10845, a peasant faction with no character member."""
    rows = {f["faction_id"]: (f["type"], f.get("leader_character_id"), f.get("members")) for f in REC["factions"]}
    assert rows[CLAIMANT] == ("claimant_faction", LEADER, [LEADER, MEMBER, 12521])
    assert rows[165] == ("independence_faction", INDEP_LEADER, [INDEP_LEADER])
    assert rows[186][2] == [] and REC["character_id"] == LIEGE


def test_1_membership_is_read_from_the_member_ids():
    assert set(FL.member_part(REC, MEMBER)["factions_in"]) == {CLAIMANT}
    assert set(FL.member_part(REC, LEADER)["factions_in"]) == {CLAIMANT}
    assert set(FL.member_part(REC, INDEP_LEADER)["factions_in"]) == {165}
    assert FL.member_part(REC, OUTSIDER)["factions_in"] == {}


def test_1_a_read_that_did_not_answer_is_none_and_no_liege_is_empty():
    assert FL.member_part(None, MEMBER)["factions_in"] is None
    assert FL.member_part({"error": "character not found"}, MEMBER)["factions_in"] is None
    assert FL.member_part(FL.NO_LIEGE, MEMBER)["factions_in"] == {}


def test_1_a_faction_whose_members_did_not_read_is_unread_not_absent():
    """The bridge writes no `members` when the member list does not read; a faction he does not lead is then
    named as unread, never counted as one he is out of."""
    rep = json.loads(json.dumps(REC))
    del rep["factions"][1]["members"]
    p = FL.member_part(rep, MEMBER)
    assert p["factions_in"] == {} and p["factions_in_unread"] == [CLAIMANT]
    assert set(FL.member_part(rep, LEADER)["factions_in"]) == {CLAIMANT}       # the leader id still reads


def test_1_first_read_names_each_membership_and_a_led_one_says_the_war_is_his():
    snap = {"actor_id": LEADER, **FL.member_part(REC, LEADER)}
    (row,) = FL.member_news({}, snap)["factions_you_are_in"]
    assert row["faction_id"] == CLAIMANT and row["type"] == "claimant_faction" and row["you_lead"] is True
    assert row["next"] == LEAVE_88
    assert "attacker" in row["if_refused"] and "declared" in row["if_refused"]
    (row,) = FL.member_news({}, {"actor_id": MEMBER, **FL.member_part(REC, MEMBER)})["factions_you_are_in"]
    assert row["you_lead"] is False and row["leader_id"] == LEADER and row["next"] == LEAVE_88


def test_1_joined_since_the_last_read():
    prev = {"actor_id": MEMBER, **FL.member_part(_without(REC, CLAIMANT, MEMBER), MEMBER)}
    snap = {"actor_id": MEMBER, **FL.member_part(REC, MEMBER)}
    assert prev["factions_in"] == {}
    (row,) = FL.member_news(prev, snap)["factions_you_are_in"]
    assert row["why"] == FL.JOINED


def test_1_a_membership_already_said_is_not_news_again_but_a_led_faction_is():
    snap = {"actor_id": MEMBER, **FL.member_part(REC, MEMBER)}
    assert "factions_you_are_in" not in FL.member_news(snap, snap)
    lead = {"actor_id": LEADER, **FL.member_part(REC, LEADER)}
    (row,) = FL.member_news(lead, lead)["factions_you_are_in"]
    assert row["you_lead"] is True and row["why"] == FL.LEADS


def test_1_left_and_unread_are_said():
    prev = {"actor_id": MEMBER, **FL.member_part(REC, MEMBER)}
    snap = {"actor_id": MEMBER, **FL.member_part(_without(REC, CLAIMANT, MEMBER), MEMBER)}
    assert FL.member_news(prev, snap)["factions_left"] == [CLAIMANT]
    gone = FL.member_news(prev, {"actor_id": MEMBER, **FL.member_part(None, MEMBER)})
    assert "factions_you_are_in_unread" in gone and "factions_you_are_in" not in gone


class _VassalGw(NewsGw):
    """NewsGw seated as `me`, a vassal of LIEGE (liege 0: independent), query_factions answering the recording."""

    def __init__(self, me: int, liege: int):
        super().__init__()
        self.replies["query_character"] = lambda p: {
            "id": p.get("character_id") or me, "name": "Ibrahim", "alive": True, "gold": 84.0, "age": 41,
            "liege_id": liege if (p.get("character_id") or me) == me else 0, "top_liege_id": liege or me}
        self.replies["query_factions"] = lambda p: REC if p.get("character_id") == LIEGE else {"factions": []}


def _faction_reads(gw):
    return [p for a, p in gw.reads if a == "query_factions"]


@pytest.mark.parametrize("depth", ["news", "brief"])
def test_1_observe_reads_the_liege_factions_and_says_the_ones_he_leads(depth):
    gw = _VassalGw(LEADER, LIEGE)
    env = run(R.observe_impl(gw, character_id=LEADER, depth=depth))
    assert {"character_id": LIEGE} in _faction_reads(gw)
    (row,) = env["result"]["news"]["factions_you_are_in"]
    assert row["faction_id"] == CLAIMANT and row["you_lead"] is True and row["next"] == LEAVE_88


@pytest.mark.parametrize("depth", ["news", "brief"])
def test_1_CONTROL_an_independent_ruler_reads_no_liege_factions(depth):
    """Non-discriminating on purpose: passes before the change too (no read, no row); it guards over-reach."""
    gw = _VassalGw(LEADER, 0)
    env = run(R.observe_impl(gw, character_id=LEADER, depth=depth))
    assert _faction_reads(gw) == [] and "factions_you_are_in" not in env["result"]["news"]


# ── court_type, the AI's 500-prestige court-type change (0867.03.23, prestige 986 -> -305 with
#    crown_authority_1 the same day, in three games; the class only in unclassified_commands) ─────────────────────
import seat_events as SE  # noqa: E402

COURT_KEYS = ("court_diplomatic", "court_warlike", "court_administrative", "court_intrigue", "court_scholarly",
              "court_tribal", "court_nomadic")


def test_2_court_type_is_a_veto_kind_and_in_the_spending_preset():
    assert "court_type" in SE.AI_VETO_WHATS and "court_type" in SE.AI_VETO_PRESETS["spending"]


def test_2_every_court_type_is_priced_from_the_game_files():
    if SE.game_dir() is None:
        pytest.skip("no game install")
    assert {k: SE.court_type_price(k) for k in COURT_KEYS} == {k: {"prestige": 500.0} for k in COURT_KEYS}
    assert SE.court_type_price("court_of_nothing") is None and SE.court_type_price(None) is None


def test_2_a_recorded_court_type_change_carries_its_price():
    if SE.game_dir() is None:
        pytest.skip("no game install")
    row = SE.attempt_row({"what": "court_type", "key": "court_warlike", "dropped": True, "priced": "no",
                          "unpriced_why": "court_type_cost_in_files", "date": "0867.03.23"})
    assert row["price"] == {"prestige": 500.0} and row["price_source"].endswith("00_court_types.txt cost")
    unk = SE.attempt_row({"what": "court_type", "priced": "no", "unpriced_why": "court_type_cost_in_files"})
    assert "price" not in unk and unk["price_unread"] == "court_type_cost_in_files"


def _snap(tier, vetoes):
    return {"actor_id": 12015, "ai_vetoes": vetoes, "ruler_tier": tier, "gold": 453.0, "prestige": 986.67}


def test_2_an_unset_court_veto_is_said_to_a_king_with_its_price_and_call():
    if SE.game_dir() is None:
        pytest.skip("no game install")
    row = news.veto_unset(_snap(4, []))["court_type"]
    assert row["price"]["prestige"] == 500.0 and row["prestige"] == 986.67
    assert row["next"] == {"tool": "do", "args": {"intent": "ai_veto", "args": {"op": "set", "what": "court_type"}}}


def test_2_CONTROL_no_court_row_once_vetoed_or_below_a_king():
    assert "court_type" not in (news.veto_unset(_snap(4, ["court_type"])) or {})
    assert "court_type" not in (news.veto_unset(_snap(3, [])) or {})


# ── The 8 KB rule never cuts a list to zero rows (a recorded game, 0867.05.27: news.titles_lost
#    0 of 11, ai_acted 0 of 3, events_answered 0 of 2, council_changed 0 of 3) ────────────────────────────────────
import reply_fit as RF  # noqa: E402

OVER = json.loads((HERE / "fixtures" / "observe_francia_over_cap.json").read_text(encoding="utf-8"))["env"]


def _paths(o, p=""):
    """{dotted path: rows} of every non-empty list under o (rows of a list are not walked: the rule cuts the outer)."""
    out = {}
    if isinstance(o, dict):
        for k, v in o.items():
            q = f"{p}.{k}" if p else str(k)
            if isinstance(v, list) and v:
                out[q] = v
            elif isinstance(v, dict):
                out.update(_paths(v, q))
    return out


def test_3_the_fixture_is_over_the_cap_with_recorded_news_lists():
    assert RF.size(OVER) > RF.CAP
    n = OVER["result"]["news"]
    assert (len(n["titles_lost"]), len(n["ai_acted"]), len(n["council_changed"]), len(n["events_answered"])) == (3, 4, 3, 1)


def test_3_every_list_keeps_its_first_row_and_says_shown_of():
    out = RF.fit(json.loads(json.dumps(OVER)), "observe", {})
    assert RF.size(out) <= RF.CAP
    before, after = _paths(OVER["result"]), _paths(out["result"])
    tr = out["result"]["truncated"]
    named = {d["what"] for d in tr.get("dropped") or []}
    cut = {r["list"]: r for r in tr.get("lists") or []}
    for path, rows in before.items():
        if path in named or any(path.startswith(w + ".") for w in named):
            continue                                   # a whole block dropped by name, with the call that reads it
        got = after.get(path)
        assert got, f"{path}: {len(rows)} rows went to none"
        assert got[0] == rows[0] or isinstance(rows[0], dict) and set(got[0]) <= set(rows[0]), path
        if len(got) < len(rows):
            assert cut[path]["shown"] == len(got) and cut[path]["of"] == len(rows), path
    for path in ("news.titles_lost", "news.ai_acted", "news.events_answered"):
        assert after.get(path), path


def test_3_CONTROL_a_reply_under_the_cap_is_sent_as_it_is():
    small = json.loads(json.dumps(OVER))
    small["result"]["news"] = {"titles_lost": OVER["result"]["news"]["titles_lost"][:1]}
    for k in ("de_jure", "money", "since", "succession", "standing"):
        small["result"].pop(k, None)
    assert RF.size(small) <= RF.CAP and RF.fit(small, "observe", {}) is small


# ── Raid is proven live (a recorded game rows 9-10), so the switched listing names it plain ────────────────
import army_orders as AO  # noqa: E402


def test_6_the_switched_listing_names_raid_plain():
    assert AO.listed_actions({AO.ENV_ORDERS: "1"}) == ("disband", "move", "raid")
    assert not any("unproven" in a for a in AO.listed_actions({AO.ENV_ORDERS: "1", AO.ENV_HOLD: "1"}))


# ── Lorraine's three counts at the start (a recorded game: observe de_jure d_lower_lorraine 0 of 7, the
#    metric realm_counties 9, inspect realm realm_total_counties 2) ──────────────────────────────────────────────
#: The three reads, as the code makes them:
#:   * observe de_jure.primary_title: the DE JURE counties of the duchy title he holds that are in his realm. His two
#:     counties (Antwerpen, Bouillon) are de jure of d_brabant and d_luxembourg, so 0 of d_lower_lorraine's 7.
#:   * inspect realm realm_total_counties: query_realm, which credits every holder to his independent top liege, so
#:     for a vassal it is the counties he holds himself (2).
#:   * a harness's realm_counties (the goal's "counties at the start"): his row of the ruler listing
#:     (search_characters filter=ruler court_of=<liege>), his counties and his vassal tree's (9).
#: Here inspect realm reads the third for a vassal too and names what each of its numbers counts.
GOTT, HENRY = 28101, 99


def _realm_gw(liege, row_n=9):
    gw = NewsGw()
    gw.replies["query_character"] = lambda p: {"id": p.get("character_id") or GOTT, "name": "Gottfried",
                                               "alive": True, "liege_id": liege, "top_liege_id": liege or GOTT}
    gw.replies["query_realm"] = {"character_id": GOTT, "primary_title_key": "d_lower_lorraine",
                                 "realm_total_counties": 2, "domain_size": 2}
    gw.replies["search_characters"] = lambda p: ({"characters": [{"id": GOTT, "realm_counties": row_n},
                                                                 {"id": 777, "realm_counties": 3}]}
                                                 if p.get("court_of") == liege else {"characters": []})
    return gw


def test_5_a_vassal_s_inspect_realm_carries_the_count_the_goal_reads_and_names_each():
    gw = _realm_gw(HENRY)
    r = run(R.inspect_impl(gw, subject="realm", character_id=GOTT))["result"]
    assert r["realm_total_counties"] == 2 and r["realm_counties_with_vassals"] == 9
    assert "himself" in r["realm_total_counties_counts"]
    assert "court_of" in r["realm_counties_with_vassals_source"]
    assert ("search_characters", {"filter": "ruler", "sort": "realm_counties", "max_results": R.COURT_ROWS_MAX,
                                  "verbose": True, "court_of": HENRY}) in gw.reads


def test_5_CONTROL_an_independent_ruler_reads_no_listing_and_his_total_is_his_realm():
    gw = _realm_gw(0)
    r = run(R.inspect_impl(gw, subject="realm", character_id=GOTT))["result"]
    assert "realm_counties_with_vassals" not in r and not [a for a, _ in gw.reads if a == "search_characters"]


def test_5_a_vassal_row_that_does_not_read_is_said_unread():
    gw = _realm_gw(HENRY)
    gw.replies["search_characters"] = {"characters": []}
    r = run(R.inspect_impl(gw, subject="realm", character_id=GOTT))["result"]
    assert "realm_counties_with_vassals" not in r and "realm_counties_with_vassals_unread" in r


# ── The shared notes scoped by task: a start's own notes (the mint call for k_ireland among them) reach only the
#    seats of that start.
#    The file's notes are a pool: the default set leaves out the notes of one start (`default_drop`), the
#    Ireland set (ireland1066) is the pool whole with the file's former turn one, and a named set (CK3_NOTES_SET)
#    drops the notes that are another start's and adds its own. ────────
import notes as N  # noqa: E402

DOC = N.load()
#: Re-pinned the default set after its notes change (guest activities, the disband rule, the head of faith's
#: terms; trims to fit turn one): 71400ef31e43 -> 7f94c539a281. The turn-one spending note names
#: legacies (renown), the preset's eighth kind: 7f94c539a281 -> 98383f5270a0. The vassalization note names no start (its "7 for most in 1066" went): 98383f5270a0 -> e555d881a9b3
#: One note's provenance said without its run label: 8ebe2dc4f37b -> 20a8fe61ca1e. Every provenance string
#: says the kind of game it was read in, not a run's label: 20a8fe61ca1e -> 35986fea2644.
#: The pool whole with the former default turn one is the Ireland set now; its version moved with the run stories
#: rewritten as advice and convert-faith-local scoped to Dublin: 35986fea2644 -> e7fbb44b476e.
#: served texts as present-tense advice (no one game's figures or story), the spending note names activities:
#: e7fbb44b476e -> 6c8b31a399b1. Figures as the game's rules where notes gave observed ones: 6c8b31a399b1 ->
#: bbd401008911. Each figure checked against the game's files and scoped to its source: bbd401008911 -> 064f1c00fb99.
#: The truce's other modifiers, the claim letter's struggle term before its floor, the crown-authority price by realm
#: size: 064f1c00fb99 -> e774957d1f8a. The truce note scoped to most casus belli: e774957d1f8a -> cc2876078df3.
IRELAND_VERSION = "cc2876078df3"
#: the default set: the pool without default_drop (general notes only)
BASE_VERSION = "440ecf864719"   # The truce note scoped to most casus belli: bce65ef7dcb5 -> 440ecf864719. The truce, claim letter and crown-authority wording: f631a8a72e85 -> bce65ef7dcb5. Figures checked against the game's files: b92201c11c8e -> f631a8a72e85. Figures as the game's rules: ae51ae54648b -> b92201c11c8e. Served texts as present-tense advice: e1b6533e7b30 -> ae51ae54648b. The spending note names activities and gold/income decisions, the guest note drops "not proven"; title prices out of four notes (formable_titles reads them), the spending note names the goal chain
#                                  (the base's e555d881a9b3; raised armies are a blocked_by condition now)
SETS = ("francia867", "sweden867", "lorraine1066")
#: the sets whose seats are of the Ireland region
IRISH_SETS = ("ireland1066", "munster1066once")
#: the title and ruler keys of each start's region; a set must carry none of another's
REGION = {
    "ireland": r"Ireland|Irish|k_ireland|Munster|Murchad|Dublin|Athlone|Leinster|Meath|Ulster|Insular",
    "francia867": r"e_france|Francia|k_france|Aquitaine|Charles|Brittany",
    "sweden867": r"k_sweden|Sweden|Svealand|Bj[oö]rn",
    "lorraine1066": r"Lorraine|Gottfried|Holy Roman",
}
OWN = {"francia867": ("e_france", "80%", "two kingdom titles", "court type"), "sweden867": ("k_sweden", "13", "two duchy titles"),
       "lorraine1066": ("ai_veto what=war", "at least once", "a faction he leads")}


def set_bytes(doc) -> int:
    """How a set's notes are counted against 7,416 B: per topic, the JSON list of its notes as served (render), the
    sizes summed (test_decision_veto's measure of the one file, now taken of each set as it is served)."""
    return sum(len(json.dumps([N.render(n) for n in doc["notes"] if n["topic"] == t], ensure_ascii=False).encode())
               for t in doc["topics"])


def _served(name):
    return N.derive(DOC, name)


def test_7_the_default_set_is_the_pool_without_one_starts_notes():
    d = _served(None)
    assert N.version(d) == BASE_VERSION and "sets" not in d and "default_drop" not in d
    assert N.boot({}).version == BASE_VERSION and "set" not in N.boot({}).status()
    assert [n["id"] for n in d["notes"]] == [n["id"] for n in DOC["notes"] if n["id"] not in DOC["default_drop"]]
    assert N.turn_one_texts(d) == N.boot({}).turn_one_reply()["notes"]


def test_7_the_ireland_set_is_the_pool_whole():
    d = _served("ireland1066")
    assert N.version(d) == IRELAND_VERSION and d["notes"] == DOC["notes"]
    assert d["turn_one"] == [x for x in DOC["sets"]["ireland1066"]["turn_one"]]
    assert set(DOC["turn_one"]) < set(d["turn_one"]) and "notes-pad-is-memory" in d["turn_one"]


@pytest.mark.parametrize("name", SETS)
def test_7_each_set_is_valid_within_its_byte_limits(name):
    d = _served(name)
    assert N.problems(d) == []
    assert len(json.dumps(N.turn_one_texts(d), ensure_ascii=False).encode()) <= N.TURN_ONE_MAX
    assert set_bytes(d) <= 7416, set_bytes(d)
    assert N.version(d) != BASE_VERSION


@pytest.mark.parametrize("name", (None,) + SETS + IRISH_SETS)
def test_7_no_set_carries_another_start_s_title_or_ruler(name):
    """The default set carries no start's titles or rulers at all; a named set only its own region's."""
    d = _served(name)
    mine = "ireland" if name in IRISH_SETS else name
    rendered = [N.render(n) for n in d["notes"]] + N.turn_one_texts(d)
    for other, pat in REGION.items():
        if other == mine:
            continue
        bad = [t for t in rendered if re.search(pat, t)]
        assert not bad, (name, other, bad)


@pytest.mark.parametrize("name", SETS)
def test_7_each_turn_one_set_states_its_own_goal(name):
    t1 = " ".join(N.turn_one_texts(_served(name)))
    for s in OWN[name]:
        assert s in t1, (name, s)


def test_7_the_set_is_chosen_at_boot_and_reported():
    n = N.boot({N.SET_ENV: "francia867"})
    assert n.status()["set"] == "francia867" and n.version == N.version(_served("francia867"))
    assert any("e_france" in t for t in n.turn_one_reply()["notes"])
    with pytest.raises(N.NotesBootRefusal) as e:
        N.boot({N.SET_ENV: "atlantis"})
    assert e.value.code == "notes_set_unknown"
    assert N.boot({N.ENV_VAR: "off", N.SET_ENV: "francia867"}).enabled is False
