"""The version and date agree everywhere a user or a citation reads them.

CITATION.cff said 2026-10-17 while the CHANGELOG heading said 2026-10-18 the
day before release (found in the pre-release docs review). release.yml gates on
the CHANGELOG date; nothing gated the citation.
"""
import re
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _changelog_head():
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    m = re.search(r"^## (\d{4}\.\d{1,2}\.\d+) — (\d{4}-\d{2}-\d{2})$", text, re.M)
    assert m, "CHANGELOG.md has no '## <version> — <date>' heading"
    return m.group(1), m.group(2)


def _citation():
    text = (ROOT / "CITATION.cff").read_text(encoding="utf-8")
    v = re.search(r'^version:\s*"?([^"\n]+)"?$', text, re.M)
    d = re.search(r'^date-released:\s*"?(\d{4}-\d{2}-\d{2})"?$', text, re.M)
    assert v and d, "CITATION.cff lacks version or date-released"
    return v.group(1), d.group(1)


def test_version_is_the_same_in_version_file_pyproject_changelog_and_citation():
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    pyproject = tomllib.loads((ROOT / "pyproject.toml").read_text(encoding="utf-8"))
    if "version" in pyproject["project"].get("dynamic", []):
        src = (ROOT / pyproject["tool"]["hatch"]["version"]["path"]).read_text(encoding="utf-8")
        m = re.search(r'__version__\s*=\s*"([^"]+)"', src)
        assert m and m.group(1) == version
    else:
        assert pyproject["project"]["version"] == version
    assert _changelog_head()[0] == version
    assert _citation()[0] == version


def test_citation_release_date_is_the_changelog_date():
    assert _citation()[1] == _changelog_head()[1]
