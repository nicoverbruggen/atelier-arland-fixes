#!/usr/bin/env python3
"""Check default.ini against the defaults the code actually uses.

default.ini is what users get in the release archive, and it repeats values that
really live in src/. It is now the only description of the
option surface, and a stale shipped file misrepresents the mod to exactly the people least able to notice. This
compares them and fails if they disagree.

Checked in both directions:
  * every option the code reads appears in default.ini
  * every option in default.ini is actually read by the code
  * where the default is a literal in the source, the values match
  * the settings launcher's own fallbacks agree with default.ini, since it has
    to show a value before the DLL has ever written one

Run from the repository root, or via `python3 scripts/check_default_ini.py`.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SECTIONS = ("Rendering", "Battle", "Debug", "Field", "Menus", "Startup",
            "Launcher", "Diagnostics")

# Options the code reads that are deliberately kept out of default.ini. [Debug]
# View is a developer view selector with no meaning to a player; the launcher
# only offers it when verbose logging is on. It is listed here rather than left
# out of SECTIONS, so that the exclusion is a decision on the record instead of
# a section nobody thought to check.
UNDOCUMENTED = {("Debug", "View")}

# Features seeded through featureEnabled() have no literal default at a call
# site: the value comes from the per-game capability matrix, inverted for the
# keys whose descriptor says so. Those used to be skipped, which meant three
# shipped defaults were checked by nothing at all. parse_matrix_defaults()
# derives them instead.

SECTION_ALT = "|".join(SECTIONS)
PATTERNS = (
    # arlandConfigBool("Section", "Key", true)
    re.compile(rf'arlandConfigBool\("({SECTION_ALT})",\s*"(\w+)",\s*(true|false)\)'),
    # GetPrivateProfileIntA("Section", "Key", 100, path)
    re.compile(rf'GetPrivateProfileIntA\("({SECTION_ALT})",\s*"(\w+)",\s*(\d+)'),
    # WritePrivateProfileStringA("Section", "Key", "value", path) -- the seeding
    # that makes an option discoverable, which writes its default.
    re.compile(rf'WritePrivateProfileStringA\("({SECTION_ALT})",\s*"(\w+)",\s*"([^"]*)"'),
    # GetPrivateProfileStringW(L"Section", L"Key", L"default", ...). The 32-bit
    # launcher proxy reads wide: its ini sits beside the game, and a Steam
    # library path can hold characters the ANSI code page cannot represent.
    re.compile(rf'GetPrivateProfileStringW\(L"({SECTION_ALT})",\s*L"(\w+)",\s*L"([^"]*)"'),
)
# Reads whose default is elsewhere, recorded for presence only. Two shapes: the
# "\x01 means absent" idiom, and game.cpp's feature descriptor table, whose rows
# are { env, section, key, invert } and whose defaults come from a per-game
# matrix rather than the call site.
KEY_ONLY = (
    re.compile(rf'GetPrivateProfile\w+A\("({SECTION_ALT})",\s*"(\w+)"'),
    re.compile(rf'"ARLAND_\w+",\s*"({SECTION_ALT})",\s*"(\w+)"'),
)

# The launcher keeps its own copy of every default, because it has to show a
# value before the DLL has ever run. That copy drifts silently: it is a separate
# file with a separate idiom, and a launcher that disagrees does not just
# display the wrong thing, it writes the wrong thing back on the next Save.
LAUNCHER_PATTERNS = (
    # iniBool("Section", "Key", true)
    re.compile(rf'iniBool\("({SECTION_ALT})",\s*"(\w+)",\s*(true|false)\)'),
    # iniString("Section", "Key", buf, sizeof(buf));
    #   comboSelectByValue(ctrl, items, n, buf[0] ? buf : "8", 0);
    # The default sits in the fallback of the ternary, one or more statements
    # later, and is tied to the read by the buffer name.
    re.compile(
        rf'iniString\("({SECTION_ALT})",\s*"(\w+)",\s*(\w+),[^;]*;'
        rf'[^;]*\?\s*\3\s*:\s*"([^"]*)"'
    ),
)


def parse_ini(path):
    """(section, key) -> value, ignoring comments and blank lines."""
    values = {}
    section = None
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif "=" in line and section:
            key, _, value = line.partition("=")
            values[(section, key.strip())] = value.strip()
    return values


def parse_source():
    """(section, key) -> default value from the source, or None if not a literal."""
    defaults = {}
    for source in sorted((ROOT / "src").glob("*.cpp")):
        text = source.read_text()
        for pattern in PATTERNS:
            for section, key, value in pattern.findall(text):
                defaults.setdefault((section, key), value)
        for pattern in KEY_ONLY:
            for section, key in pattern.findall(text):
                defaults.setdefault((section, key), None)
    return defaults


def parse_matrix_defaults():
    """Defaults that come from the capability matrix rather than a literal.

    game.cpp holds a descriptor row per feature (env name, ini section, ini key,
    invert flag) and a matrix with one column per feature and one row per game,
    where X is on by default, O is opt-in and U is unsupported. featureEnabled()
    turns a cell into the key's default: on-by-default means the key defaults
    true, and an inverted descriptor flips that. A feature the matrix supports
    differently across games has no single correct value, so it is skipped.
    """
    text = (ROOT / "src" / "game.cpp").read_text(encoding="utf-8")
    rows = re.findall(
        r'\{\s*(?:"[A-Z_0-9]+"|nullptr),\s*(?:"(\w+)"|nullptr),\s*'
        r'(?:"(\w+)"|nullptr),\s*(true|false)\s*\}',
        text,
    )
    matrix = re.findall(r"/\*\s*\w+\s*\*/\s*\{([^}]*)\}", text)
    grids = [[c.strip() for c in m.split(",") if c.strip()] for m in matrix]
    grids = [g for g in grids if g and all(c in ("X", "O", "U") for c in g)]
    if len(grids) != 3 or len({len(g) for g in grids}) != 1:
        return {}

    defaults = {}
    for index, (section, key, invert) in enumerate(rows):
        if not section or not key or index >= len(grids[0]):
            continue
        cells = {g[index] for g in grids if g[index] != "U"}
        if len(cells) != 1:
            continue          # genuinely per-game; no single shipped default
        on_by_default = cells.pop() == "X"
        if invert == "true":
            on_by_default = not on_by_default
        defaults[(section, key)] = "true" if on_by_default else "false"
    return defaults


def parse_launcher():
    """(section, key) -> default value the settings launcher falls back to."""
    defaults = {}
    text = (ROOT / "src" / "config_gui" / "main.cpp").read_text()
    for pattern in LAUNCHER_PATTERNS:
        for match in pattern.finditer(text):
            defaults.setdefault(
                (match.group(1), match.group(2)), match.groups()[-1]
            )
    return defaults


def main():
    ini_path = ROOT / "default.ini"
    ini = parse_ini(ini_path)
    source = parse_source()
    launcher = parse_launcher()
    problems = []

    for entry in sorted(set(source) - set(ini) - UNDOCUMENTED):
        problems.append(
            f"{entry[0]}/{entry[1]}: read by the code but missing from default.ini"
        )
    for entry in sorted(set(ini) - set(source)):
        problems.append(
            f"{entry[0]}/{entry[1]}: in default.ini but never read by the code"
        )

    matrix_defaults = parse_matrix_defaults()
    for entry in sorted(set(ini) & set(source)):
        expected = source[entry]
        if expected is None:
            expected = matrix_defaults.get(entry)
        if expected is None:
            continue
        actual = ini[entry]
        if actual.lower() != expected.lower():
            problems.append(
                f"{entry[0]}/{entry[1]}: default.ini says {actual!r}, "
                f"the code defaults to {expected!r}"
            )

    for entry in sorted(set(ini) & set(launcher)):
        actual = ini[entry]
        expected = launcher[entry]
        if actual.lower() != expected.lower():
            problems.append(
                f"{entry[0]}/{entry[1]}: default.ini says {actual!r}, "
                f"the settings launcher falls back to {expected!r}"
            )

    if problems:
        print("default.ini disagrees with the code:\n", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\nUpdate default.ini to match, or adjust the "
            "allowlists in this script if an option is deliberately undocumented.",
            file=sys.stderr,
        )
        return 1

    print(f"default.ini agrees with the code ({len(ini)} options checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
