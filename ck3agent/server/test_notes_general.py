"""What the notes may say, in every set as it is served.

  * the default set (CK3_NOTES_SET unset, served to any install on any start) is general advice: no note of it
    names a title key or a start, in its text or its scope;
  * what is SERVED is defined once, by served_texts(), and is what it reads: the string constants and f-strings
    (joined, `{}` for each field) of the server modules, the hooks, the command line with its helper modules and
    the proposal mod's generator, not their docstrings or comments; the string literals of the bridge's sources
    (src and include, not its tests), adjacent literals joined as the compiler joins them; the notes as a seat
    reads them (each set's note texts, kinds and scopes, and the topic lines; a note's provenance is not served);
    the prompt files the command line writes for a seat (ck3agent/protocol/*.md, paragraph by paragraph); the
    game-mod scripts of the two mods without their `#` comments; and the tool descriptions
    (test_no_tool_description_tells_a_run_story, with mcp installed). A text the toolkit builds at run time from
    the game's own data is not among them;
  * no served text matches RUN_STORY (a count of runs or games, "recorded games", "in the runs", "an earlier
    game") or a class of CASE_CLASSES (one observed case cited as evidence: an in-game date other than a start's,
    a live case's tag, "the recorded case", a character by its id, a design phase's tag, runs or tests counted
    with a word between, a repeat count, one of thirteen past-tense verbs with a figure after it, a live test or
    check or "the live campaign", a probe as the evidence, an observed figure served as a bound ("up to about
    600 gold") or a measurement ("measured: ...", "a measured average rate"), a release candidate's label, a
    save's character ids as an example), and no note narrates in the past tense (NARRATED); no served text names
    a script (`*.sh`, `*.py`) the tree does not hold. These are spellings: a story told in other words is not caught, which is why each class has a
    planted sample and a mutant, and why a person reads the texts before a release;
  * a note (its text or its provenance) that states how many kinds the spending preset sets states
    seat_events.AI_VETO_PRESETS["spending"]'s count (the id carries no count);
each with a CONTROL that plants the defect in a copy of the file (or a module of its own) and sees the rule red on it.
"""
import ast
import copy
import importlib.util
import json
import re
import sys
from pathlib import Path

import pytest

import notes as N
from seat_events import AI_VETO_PRESETS

DOC = N.load()
SETS = (None, *DOC["sets"])
#: a title key (k_ireland, d_meath, c_dublin, e_france, b_x, h_x) or a named start: a start's year (867, 1066,
#: 1070-1079), a start's place or ruler
TITLE_KEY = re.compile(r"\b[hekdcb]_[a-z]")
START = re.compile(r"\b\d{3,4} start\b|\b0?(?:867|1066|107\d)\b|\b(?:Ireland|Irish|Dublin|Munster|Murchad|Athlone|Leinster|Meath|Francia|France|"
                   r"Aquitaine|Charles|Sweden|Svealand|Bj[oö]rn|Lorraine|Gottfried|Carinthia|Berthold|Norse)\b")
#: a count of runs or games, a recorded/seated game, "in the runs", "earlier runs", "another run": the story of one
#: recorded game (a unit after it is a duration, "one game day"; "Crusader Kings 3 game" is the game's name)
RUN_STORY = re.compile(
    r"(?i)(?<!Kings )\b(?:one|two|three|four|five|six|seven|eight|nine|ten|\d+) (?:seated |recorded |scripted )?"
    r"(?:runs?|games?)\b(?![-\w]| (?:days?|months?|years?|ticks?|threads?|calls?|dates?|turns?|states?|objects?|"
    r"engines?|rules?|files?|speeds?|data|clocks?|worlds?|versions?|builds?|binar(?:y|ies)|process(?:es)?|"
    r"windows?|saves?|menus?|logs?)\b)"
    r"|\b(?:recorded|seated|scripted) (?:runs?|games?)\b(?!-)|\bin the runs\b|\bearlier (?:runs?|games?)\b"
    r"|\b(?:another|that) (?:run|game)\b(?![-'\w]| (?:process(?:es)?|windows?|binar(?:y|ies)|builds?|versions?)\b)")
#: a note narrating what happened in a game, in the simple past (a participle in a rule, "if it is lost", is not)
NARRATED = re.compile(r"\b(?:was|were|went|took|gave|fell|rose|hired|converted|followed|bought|ran|saw|offered|"
                      r"became|happened|turned out)\b", re.I)
SERVER = Path(__file__).resolve().parent
ROOT = SERVER.parents[1]
#: one observed case told as a served text's evidence, each a class of its own (the publishing step's string rules
#: carry the same spellings under the same names, and its own test holds them equal)
CASE_CLASSES = {
    # an in-game date (a year of the game's starts and after, a month, maybe a day); a start's date is allowed
    "ingame_date": r"(?<![\w.])(?:8[6-9]\d|9\d\d|1[0-4]\d\d)\.(?:0?[1-9]|1[0-2])(?:\.(?:0?[1-9]|[12]\d|3[01]))?(?![\w.]*\d)",
    "live_case": r"\(live: (?!held\b)",
    "recorded_case": r"(?i)\b(?:the|a|in the) recorded cases?\b",
    "character_id": r"(?i)\b(?:ruler|character|courtier|count|duke|king|heir|actor) (?:id )?\d{4,7}\b",
    "design_phase": r"\b(?:the )?P\d+ (?:artifact|phase|stage)\b|\[P\d+\]",
    # runs, games or tests counted with a word between ("two live runs", "both runs", "the second run")
    "counted_case": (r"(?i)\b(?:both|the (?:first|second|third|other)) (?:[\w-]+ )?(?:runs?|games?|tests?)\b(?! (?:days?|"
                     r"months?|years?|ticks?|threads?|process(?:es)?)\b)|\b(?:one|two|three|four|five|six|seven|eight|"
                     r"nine|ten|\d+) (?:live|short|long|later|other|more|such) (?:runs?|games?|tests?)\b"),
    # a live case told by its tag ("(live)", "Live: -350", "live, 10 of 10", "in a live run", "measured live")
    "live_tag": (r"(?i)\(live\)|(?:^|[;.] )live: |\blive, \d+ of \d+\b|\bin (?:a|one|that|the) live "
                 r"(?:run|game|test|walk)\b|\bmeasured (?:live|twice|once)\b"),
    # a case counted by its repeats or its one ruler ("(twice)", "(3 times)", "one ruler's chaplain")
    "repeat_case": r"\((?:twice|\d+ times)\)|\bone ruler's\b",
    # what happened, in the past tense with a figure ("prestige fell 335-472", "the AI made this ask ... +136")
    "narrated_figure": (r"\b(?:fell|rose|came|ran|took|made|went|dropped|gave|refused|arrived|created|hired)\b"
                        r"[^.;){}]{0,40}\d"),
    # a live test, check or campaign as the evidence ("a live gift test", "live checks", "the live campaign")
    "live_test": r"(?i)\blive (?:[\w/-]+ ){0,2}(?:tests?|checks?)\b|\btick \d+ both sides\b|\bthe live campaign\b",
    # a probe as the evidence ("a no-op in the probe", "the probe's call")
    "probe_case": r"(?i)\b(?:in|by) the probe\b|\bthe probe's\b",
    # an observed figure served as a bound or a measurement ("up to about 600 gold", "measured: about 22 prestige",
    # "a measured average rate"): a price the game scales is stated by its rule, an estimate as an estimate
    "price_bound": r"(?i)\bup to (?:about )?[\d,]+ (?:gold|prestige|piety)\b",
    "measured_head": r"(?i)\bmeasured: ",
    "measured_average": r"(?i)\bmeasured (?:average|rate|mean)\b",
    # a release candidate's label ("older than rc2"): the public versions are calendar versions
    "release_label": r"(?i)\brc\d+\b",
    # a save's character ids as an example of the ruler list ("CK3_AGENT_CHARACTER_ID=13902,12145")
    "example_id": r"CHARACTER_ID=\d{4,}",
}
_CASE_RX = {k: re.compile(v) for k, v in CASE_CLASSES.items()}
#: the dates a served text may name: the starts' (a mock state's, a task's), and the declared episode horizon's end
START_DATES = {(867, 1, 1), (1066, 9, 15), (1178, 10, 1)}
ALLOWED_DATES = {("episode_policy.h", (1086, 9, 15))}
#: run-story spellings a file may hold, each reviewed: the launch record's warning in the command line names the game
#: process the record may describe ("it may describe another game"), not a recorded game
ALLOWED_STRINGS = {("cli.py", "another game")}
#: a count of kinds the spending preset is said to set: "(sets ten)", "sets 10 kinds"
WORDS = {"one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10,
         "eleven": 11, "twelve": 12}
PRESET_COUNT = re.compile(r"what=spending\W{0,3}(?:\(|, )?sets (\w+)", re.I)
#: the same count where a provenance names the preset's kinds: "the preset's ten kinds"
PROVENANCE_COUNT = re.compile(r"\bpreset'?s? (\w+) kinds\b", re.I)


def default_problems(doc) -> list:
    d = N.derive(doc, None)
    return [f"{n['id']}: {m.group(0)!r}" for n in d["notes"] for f in ("text", "scope")
            for m in [TITLE_KEY.search(n[f]) or START.search(n[f].replace("CK3 1.18", ""))] if m]


def story_problems(doc) -> list:
    return [f"{name}/{n['id']}: {m.group(0)!r}" for name in (None, *doc["sets"]) for n in N.derive(doc, name)["notes"]
            for m in [RUN_STORY.search(n["text"]) or NARRATED.search(n["text"])] if m]


def served_strings(paths) -> list[tuple[str, int, str]]:
    """Every string constant of the modules that is not a docstring (a module's, a class's, a function's): the
    texts a reply can carry. A comment is not a string and is not read."""
    out = []
    for p in paths:
        tree = ast.parse(Path(p).read_text(encoding="utf-8"))
        docs = {id(n.body[0].value) for n in ast.walk(tree)
                if isinstance(n, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and n.body
                and isinstance(n.body[0], ast.Expr) and isinstance(n.body[0].value, ast.Constant)}
        out += [(Path(p).name, n.lineno, n.value) for n in ast.walk(tree)
                if isinstance(n, ast.Constant) and isinstance(n.value, str) and id(n) not in docs]
        # an f-string whole, each field as `{}` (its pieces are read above too)
        out += [(Path(p).name, n.lineno, "".join(v.value if isinstance(v, ast.Constant) else "{}" for v in n.values))
                for n in ast.walk(tree) if isinstance(n, ast.JoinedStr)]
    return out


def c_strings(paths) -> list[tuple[str, int, str]]:
    """Every string literal of the C/C++ files (raw strings included; comments and character literals not), read by
    the one C++ tokenizer (bridge/scripts/cpp_strip.py): the texts a bridge reply, note or log line can carry."""
    sys.path.insert(0, str(ROOT / "bridge" / "scripts"))
    import cpp_strip
    out = []
    for p in paths:
        text = Path(p).read_text(encoding="utf-8", errors="replace")
        lits = [x for x in cpp_strip.literals(text) if x[0] in ("string", "raw_string")]
        out += [(Path(p).name, text.count("\n", 0, s) + 1, body) for _, s, _, body in lits]
        # adjacent literals joined, as the compiler joins them ("a " "b" is one text "a b")
        run: list = []
        for kind, s, e, body in lits + [(None, -1, -1, "")]:
            if run and kind is not None and not text[run[-1][2]:s].strip():
                run.append((kind, s, e, body))
                continue
            if len(run) > 1:
                out.append((Path(p).name, text.count("\n", 0, run[0][1]) + 1, "".join(x[3] for x in run)))
            run = [(kind, s, e, body)] if kind is not None else []
    return out


def note_texts(doc) -> list[tuple[str, int, str]]:
    """The notes as a seat reads them, in every set: each note's text, kind and scope (notes.render), and the topic
    lines; a note's provenance is a record for the file's readers and is not served."""
    out = []
    for name in (None, *doc["sets"]):
        d = N.derive(doc, name)
        out += [(f"notes/{name or 'default'}/{n['id']}", 0, n[f]) for n in d["notes"] for f in ("text", "kind", "scope")]
        out += [(f"notes/{name or 'default'}/topics", 0, t) for t in (d.get("topics") or {}).values()]
    return out


def md_texts(paths) -> list[tuple[str, int, str]]:
    """Each paragraph of a Markdown file as one text, its lines joined by a space (a phrase a line break splits is
    read whole): the prompt files a seat is given whole."""
    out = []
    for p in paths:
        para: list = []
        for i, line in enumerate(Path(p).read_text(encoding="utf-8").splitlines() + [""], 1):
            if line.strip():
                para.append((i, line.strip()))
            elif para:
                out.append((Path(p).name, para[0][0], " ".join(x for _, x in para)))
                para = []
    return out


def mod_texts(paths) -> list[tuple[str, int, str]]:
    """Each line of a game-mod script with its `#` comment cut (a `#` inside quotes is text): what the toolkit
    writes into the game and the game reads. A comment is not read."""
    out = []
    for p in paths:
        for i, line in enumerate(Path(p).read_text(encoding="utf-8-sig").splitlines(), 1):
            code, q = [], False
            for ch in line:
                if ch == '"':
                    q = not q
                elif ch == "#" and not q:
                    break
                code.append(ch)
            if "".join(code).strip():
                out.append((Path(p).name, i, "".join(code)))
    return out


def served_texts() -> list[tuple[str, int, str]]:
    """THE definition of what is served: the texts the module statement lists, and no others."""
    return (served_strings(served_python()) + c_strings(bridge_sources()) + note_texts(DOC)
            + md_texts(protocol_files()) + mod_texts(mod_files()))


def server_modules() -> list[Path]:
    return sorted(p for p in SERVER.glob("*.py") if not p.name.startswith(("test_", "conftest")))


def package_dir() -> Path:
    """The directory that holds the command line and its helpers (ck3agent/; in a tree that keeps them among the
    files laid over the package, that one copy); a tree with none, or two, fails."""
    got = [p.parent for p in (ROOT / "ck3agent" / "cli.py", *sorted(ROOT.glob("*/overlay/ck3agent/cli.py")))
           if p.is_file()]
    assert len(got) == 1, got
    return got[0]


def served_python() -> list[Path]:
    """The Python that serves text: the server modules, the hooks, the command line with its helper modules
    (ck3agent/*.py), and the generator of the proposal mod's overrides (ck3agent/proposal_mod/proposal_overrides.py,
    or the one copy a tree keeps elsewhere); a file that is not there fails."""
    hooks = sorted(p for p in (ROOT / "ck3agent" / "hooks").glob("*.py") if not p.name.startswith(("test_", "conftest")))
    cli = sorted(p for p in package_dir().glob("*.py") if not p.name.startswith(("test_", "conftest")))
    gen = [p for p in (ROOT / "ck3agent" / "proposal_mod" / "proposal_overrides.py",
                       ROOT / "scripts" / "proposal_overrides.py") if p.is_file()]
    assert hooks and package_dir() / "cli.py" in cli and len(gen) == 1, (hooks, cli, gen)
    return server_modules() + hooks + cli + gen


def protocol_files() -> list[Path]:
    """The prompt files the command line writes for a seat (ck3agent/protocol/*.md); a tree without them fails."""
    out = sorted((package_dir() / "protocol").glob("*.md"))
    assert out, package_dir()
    return out


def mod_files() -> list[Path]:
    """The game-mod scripts the toolkit's mods put into the game (ck3agent/policy_mod, ck3agent/proposal_mod: each
    .txt and descriptor.mod); a tree without them fails."""
    out = sorted(p for d in ("policy_mod", "proposal_mod") for p in (ROOT / "ck3agent" / d).rglob("*")
                 if p.suffix in (".txt", ".mod"))
    assert out, ROOT / "ck3agent"
    return out


def bridge_sources() -> list[Path]:
    """The bridge's sources (src and include, not its tests and not its third-party code); a tree without them
    fails."""
    out = sorted(p for d in ("src", "include") for p in (ROOT / "bridge" / d).rglob("*")
                 if p.suffix in (".cpp", ".cc", ".h", ".hpp") and "test" not in p.relative_to(ROOT / "bridge").parts[:-1]
                 and not p.name.startswith("test_"))
    assert out, ROOT / "bridge"
    return out


def case_hits(name: str, text: str, classes=None) -> list[tuple[str, str]]:
    """(class, token) of every observed-case class in one served text (a start's date, the declared horizon's end
    allowed)."""
    out = []
    for k, rx in (classes or _CASE_RX).items():
        for m in rx.finditer(text):
            if k == "ingame_date":
                d = tuple(int(x) for x in m.group(0).split("."))
                if len(d) == 3 and (d in START_DATES or (name, d) in ALLOWED_DATES):
                    continue
            out.append((k, m.group(0)))
    return out


def string_problems(paths, cpaths=(), classes=None, texts=None) -> list:
    texts = served_strings(paths) + c_strings(cpaths) if texts is None else texts
    return ([f"{f}:{ln}: {m.group(0)!r}" for f, ln, v in texts for m in RUN_STORY.finditer(v)
             if (f, m.group(0)) not in ALLOWED_STRINGS]
            + [f"{f}:{ln}: {k} {tok!r}" for f, ln, v in texts for k, tok in case_hits(f, v, classes)])


def count_problems(doc) -> list:
    want = len(AI_VETO_PRESETS["spending"])
    out = []
    for name in (None, *doc["sets"]):
        for n in N.derive(doc, name)["notes"]:
            for f, rx in (("text", PRESET_COUNT), ("provenance", PROVENANCE_COUNT)):
                for m in rx.finditer(n.get(f) or ""):
                    said = WORDS.get(m.group(1).lower(), int(m.group(1)) if m.group(1).isdigit() else None)
                    if said != want:
                        out.append(f"{name}/{n['id']} ({f}): says {m.group(1)}, the preset sets {want}")
            if re.search(r"-(?:one|two|three|four|five|six|seven|eight|nine|ten|\d+)$", n["id"]) and "spend" in n["id"]:
                out.append(f"{name}/{n['id']}: the id carries a count")
    return out


def test_the_default_set_names_no_title_key_and_no_start():
    assert default_problems(DOC) == []
    assert DOC["turn_one"] and all(x not in DOC["default_drop"] for x in DOC["turn_one"])


def test_no_served_note_tells_a_run_story():
    assert story_problems(DOC) == []


def test_no_served_string_tells_a_run_story_or_cites_a_case():
    assert string_problems((), texts=served_texts()) == []


#: a script named in a served text, resolved by name against the tree it runs in (a log line naming a script that is
#: not shipped points a reader at nothing)
SCRIPT_NAME = re.compile(r"(?<![\w/.-])[\w-]+\.(?:sh|py)\b")
_NOT_WALKED = ("build", "project", "third_party", "__pycache__", "node_modules")


def tree_script_names(root: Path = ROOT) -> set[str]:
    out = set()
    for d, ds, fs in __import__("os").walk(root):
        ds[:] = [x for x in ds if not x.startswith(".") and x not in _NOT_WALKED]
        out |= {f for f in fs if f.endswith((".sh", ".py"))}
    return out


def script_problems(texts, names) -> list:
    return [f"{f}:{ln}: {m.group(0)!r}" for f, ln, v in texts for m in SCRIPT_NAME.finditer(v)
            if m.group(0) not in names]


def test_no_served_string_names_a_script_the_tree_lacks():
    assert script_problems(served_texts(), tree_script_names()) == []


def test_CONTROL_a_served_script_name_resolves_by_name(tmp_path):
    """A log line naming a script the tree lacks reds; the same line naming a script it holds does not, nor does a
    path's tail. MUTANT: with every name counted as held, the plant passes."""
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools" / "run_me.sh").write_text("")
    names = tree_script_names(tmp_path)
    plant = [("x.cpp", 1, "terminated by gone_away.sh with reason=lost"), ("x.cpp", 2, "run run_me.sh; see a/b.py")]
    assert script_problems(plant, names) == ["x.cpp:1: 'gone_away.sh'"]
    assert script_problems(plant, names | {"gone_away.sh"}) == []


def test_no_tool_description_tells_a_run_story():
    if importlib.util.find_spec("mcp") is None:
        pytest.skip("the mcp library is not installed: the tool listing cannot be built")
    import mcp_server
    listing = json.dumps(mcp_server.registered_tool_definitions())
    assert [m.group(0) for m in RUN_STORY.finditer(listing)] == [] and case_hits("listing", listing) == []


def test_a_note_states_the_spending_presets_count_or_none():
    assert count_problems(DOC) == []


def _planted(fn):
    d = copy.deepcopy(DOC)
    fn(d)
    return d


def test_CONTROL_each_rule_reds_on_its_planted_defect():
    first = next(n for n in DOC["notes"] if n["id"] not in DOC["default_drop"])
    i = DOC["notes"].index(first)
    assert default_problems(_planted(lambda d: d["notes"][i].update(text=first["text"] + " Mint k_ireland.")))
    assert default_problems(_planted(lambda d: d["notes"][i].update(scope="CK3 1.18; 1066 start, Dublin")))
    assert default_problems(_planted(lambda d: d["default_drop"].remove("ai-may-create-title")))
    assert story_problems(_planted(lambda d: d["notes"][i].update(text=first["text"] + " Three runs lost.")))
    assert story_problems(_planted(lambda d: d["notes"][i].update(text="In the runs it waited.")))
    assert story_problems(_planted(lambda d: d["notes"][i].update(text="Unvetoed, the AI hired a company.")))
    assert story_problems(_planted(lambda d: d["notes"][i].update(text="Prestige fell to -305 (recorded games).")))
    assert not story_problems(_planted(lambda d: d["notes"][i].update(text="+10 once, never again if it is lost.")))
    spend = _planted(lambda d: d["notes"][0].update(text="Before step 1: do ai_veto what=spending (sets eight)."))
    assert count_problems(spend) and not count_problems(_planted(lambda d: d["notes"][0].update(
        text=f"do ai_veto what=spending (sets {len(AI_VETO_PRESETS['spending'])}).")))
    assert count_problems(_planted(lambda d: d["sets"]["francia867"]["add"][-1].update(id="ai-spends-eight")))
    prov = next(n for n in DOC["sets"]["sweden867"]["add"] if PROVENANCE_COUNT.search(n.get("provenance") or ""))
    k = DOC["sets"]["sweden867"]["add"].index(prov)
    assert count_problems(_planted(lambda d: d["sets"]["sweden867"]["add"][k].update(
        provenance="The preset's eight kinds (seat_events AI_VETO_PRESETS)")))


def test_CONTROL_the_reply_string_rule_reds_on_a_planted_module_and_reads_no_comment(tmp_path):
    """A module of its own: a reply constant telling a run's story reds, in each of its spellings; the same words in
    a comment and in a docstring do not (they are not served), nor do a duration and the game's name."""
    for story in ("in one run the AI hired a company", "a faction followed it in two runs",
                  "(recorded games: 15-350 prestige)", "one game's gold fell short", "as it did in that game"):
        f = tmp_path / "m.py"
        f.write_text(f'"""module doc: in two runs it went."""\n# in one run it went\nX = {story!r}\n')
        assert string_problems([f]) == [f"m.py:3: {RUN_STORY.search(story).group(0)!r}"], story
    f.write_text('"""in two runs it went."""\n# in one run\nX = ("within one game day", "a Crusader Kings 3 game", '
                 '"if / else_if chain (exactly one of them runs)")\n')
    assert string_problems([f]) == []


def test_CONTROL_the_prompt_and_mod_rules_read_a_split_phrase_and_no_comment(tmp_path):
    """A run story in a prompt file reds when a line break splits it, and in a mod script's code; the same words in
    the mod's `#` comment do not (a `#` inside quotes is text). The prompt files, the command line's helpers, the
    proposal mod's generator and the mod scripts are among what is read, so an empty result is a reading."""
    md = tmp_path / "CLAUDE.md"
    md.write_text("A rule.\n\n- In recorded\n  games each surrender cost a lot.\n")
    assert string_problems((), texts=md_texts([md])) == ["CLAUDE.md:3: 'recorded games'"]
    mod = tmp_path / "e.txt"
    mod.write_text('# in recorded games it went\nx = { desc = "a # in recorded games" }\n')
    assert string_problems((), texts=mod_texts([mod])) == ["e.txt:2: 'recorded games'"]
    names = {p.name for p in served_python()}
    assert {"_console.py", "_bridge.py", "proposal_overrides.py"} <= names
    assert {p.name for p in protocol_files()} >= {"CLAUDE.md", "CLAUDE-desktop.md"}
    assert any(p.name == "ck3agent_policy_events.txt" for p in mod_files())


def test_CONTROL_the_rules_read_what_is_served():
    """The start-specific notes are in the file's pool: the default-set rule reds on them when default_drop is
    emptied, so it reads the served set, not the file."""
    assert default_problems(_planted(lambda d: d.update(default_drop=[])))
    assert json.dumps(DOC["sets"]["ireland1066"]["drop"]) == "[]"


#: per observed-case class: a planted served string it reds on, and the ordinary text beside it that it reads as none
CASE_PLANTS = {
    "ingame_date": ("the seat filled again by 1073.11.27", "a start at 1066.9.15; CK3 1.18.2; 12.5 gold"),
    "live_case": ("it stops nothing (live: two counts created theirs)", "lock_state (live: held against a change)"),
    "recorded_case": ("send it then (in the recorded case it applied)", "a recorded save's case is read"),
    "character_id": ("it froze (a full scan, ruler 13902)", "the ruler's character_id=13902; count 12 counties"),
    "design_phase": ("not in the P1 artifact", "phase P1 of the port; [terrain] the map"),
    "counted_case": ("the AI paid it (two live runs: 29.6 and 4.81 gold)", "both armies, from the first game day"),
    "live_tag": ("the piety is taken. Live: -350 at the send", "the live game state (live: held against a change)"),
    "repeat_case": ("a claim after 14 months (twice)", "twice the price, from one ruler of yours"),
    "narrated_figure": ("in three steps prestige fell 335-472", "prestige falls by 335 a year; it ran."),
    "live_test": ("both purses move (a live gift test)", "the live game state; a test of 3 kinds"),
    "probe_case": ("it was a no-op in the probe", "a probe of the map reads 3 rows"),
    "price_bound": ("(about 15-350 prestige, up to about 600 gold)", "a hire up to the reserve; 600 gold at most"),
    "measured_head": ("measured: about 22 prestige", "measured has your score; the unmeasured rows"),
    "measured_average": ("a measured average rate", "a rate measured from the two reads"),
    "release_label": ("the bridge is older than rc2", "rc=2 at exit; the src2 field"),
    "example_id": ("CK3_AGENT_CHARACTER_ID=13902,12145", "CK3_AGENT_CHARACTER_ID=<id1>,<id2>; character_id=13902"),
}


def test_CONTROL_each_case_class_reds_on_its_plant_and_on_nothing_else(tmp_path):
    """Each class reds on its planted served string, in a Python constant and in a C++ literal (a raw string too),
    and not on the ordinary text beside it; a comment and a docstring holding the same plant are not read. MUTANT:
    with the class removed, its plant reds no other class, so each class is what catches it."""
    assert set(CASE_PLANTS) == set(CASE_CLASSES)
    for k, (plant, ordinary) in CASE_PLANTS.items():
        py = tmp_path / "m.py"
        py.write_text(f'"""{plant}"""\n# {plant}\nX = {plant!r}\nY = {ordinary!r}\n')
        cc = tmp_path / "m.cpp"
        cc.write_text(f'// {plant}\n/* {plant} */\nconst char* a = "{plant}";\nauto b = R"x({plant})x";\n'
                      f'const char* c = "{ordinary}"; char q = \'"\';\n')
        got = string_problems([py], [cc])
        assert [x.split(": ", 1)[0] for x in got] == ["m.py:3", "m.cpp:3", "m.cpp:4"], (k, got)
        assert all(f" {k} " in x for x in got), (k, got)
        mutant = {j: rx for j, rx in _CASE_RX.items() if j != k}
        assert string_problems([py], [cc], classes=mutant) == [], k
    # the allowed dates: a start's, the declared horizon's end in its own file only
    assert case_hits("x.h", "1066.09.15 867.1.1") == [] and case_hits("episode_policy.h", "1086.09.15") == []
    assert case_hits("other.h", "1086.09.15") == [("ingame_date", "1086.09.15")]


def test_CONTROL_every_allowed_string_excuses_a_hit():
    """Each allowance names a spelling its file holds (an allowance that excuses nothing is stale), and a module of
    another name holding it reds."""
    texts = served_strings(served_python())
    for f, tok in ALLOWED_STRINGS:
        assert any(n == f and tok in [m.group(0) for m in RUN_STORY.finditer(v)] for n, _, v in texts), (f, tok)


def test_CONTROL_the_served_code_is_read_whole():
    """The paths the rule reads include the hooks, the command line and the bridge's reply code (a reply string a
    tree is known to hold is among what it reads), so an empty result is a reading, not a skipped tree."""
    names = {p.name for p in served_python()}
    assert {"inject_state.py", "cli.py", "seat_events.py"} <= names
    texts = {v for _, _, v in c_strings([p for p in bridge_sources() if p.name == "map_graph.cpp"])}
    assert any(v.startswith("scope=region needs") for v in texts)
    assert any(t == "freezes the game thread in its trigger eval"
               for _, _, t in c_strings([p for p in bridge_sources() if p.name == "decision_eval_plan.h"]))
