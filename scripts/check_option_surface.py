#!/usr/bin/env python3
"""Check the settings launcher's defaults against the defaults the code uses.

No ini ships any more. The values a user meets before the DLL has ever written a
file are the launcher's own, so those are what this checks, and it checks them by
running the launcher rather than by reading it. `--write-defaults <game> <path>`
writes exactly what a fresh Save would write, so a launcher that shows the wrong
value cannot pass by carrying a table that says the right one.

Run once per game, because the defaults differ per game.

Checked, per game:
  * every key the launcher writes is actually read by the code
  * every key the code reads is written, unless it is allowed below
  * where the code's default is a literal, the two values agree
  * where it comes from the feature table, the cell and the file agree,
    including a key being absent exactly when the cell is U

Needs Windows, since it runs the launcher. Set ARLAND_LAUNCHER_RUNNER to a
command prefix to check from elsewhere, for example `umu-run`.
"""

import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The mod's own ini. [Graphics], [Window] and [Lang] belong to the game's
# ArlandDX_Settings.ini, which this window also edits and which is not the
# option surface being checked here.
SECTIONS = ("Rendering", "Battle", "Startup", "Field", "Menus", "Launcher",
            "Diagnostics", "Debug")

# The launcher's argument for each game, in feature-table row order.
GAMES = ("rorona", "totori", "meruru")

# Options the code reads that a default run of the launcher does not write.
#
# Two shapes end up here and they are not the same thing, so every entry says
# which it is:
#
#   "exposed ..."      a control offers it, but its default state writes no key.
#                      An unset combo, or an inverted box whose unticked state
#                      is the correction being on. Nothing is hidden.
#   "not exposed ..."  no control offers it, so it can only be set by someone
#                      who already knows the name. The surface is the launcher,
#                      so this is a decision that the option is not for players,
#                      not a note that its control has not been written yet.
NOT_WRITTEN_AT_DEFAULT: dict[tuple[str, str], str] = {
    ("Debug", "View"):
        "exposed on the Debug tab, which appears when verbose logging is on."
        " A default run writes no key: index 0 means no view and writes"
        " nullptr, which deletes it",
    ("Debug", "FieldJitterFix"):
        "exposed on the same tab, and inverted: the box is ticked to turn the"
        " correction off, so a default run deletes the key rather than writing"
        " one",
}

# Keys where the launcher and the code differ on purpose, because they are not
# answering the same question. Each needs the reason, not just the exemption.
DIFFERENT_BY_DESIGN: dict[tuple[str, str], str] = {}

# Feature-table rows carrying no ini key of their own, because the option is a
# valued knob rather than a switch and has its own reader. The cell still says
# which games have the feature, and that is what decides whether the key belongs
# in a given game's file, so each is matched to its row by the row's name.
KEYLESS_ROWS = {
    ("Rendering", "ShadowMultiplier"): "ShadowMultiplier",
}

# config.cpp reads some keys with a \x01 sentinel, because an empty default
# cannot tell a missing key from one someone deliberately blanked. The sentinel
# is not a default; the value comes from the eager seed in configPath().
SENTINEL = "\\x01"

SECTION_ALT = "|".join(SECTIONS)
PATTERNS = (
    # WritePrivateProfileStringA("Section", "Key", "value", path) -- the eager
    # seeding in configPath(), which writes an option's real default. First, so
    # it wins over a sentinel read of the same key.
    re.compile(rf'WritePrivateProfileStringA\("({SECTION_ALT})",\s*"(\w+)",\s*"([^"]*)"'),
    # arlandConfigBool("Section", "Key", true)
    re.compile(rf'arlandConfigBool\("({SECTION_ALT})",\s*"(\w+)",\s*(true|false)\)'),
    # GetPrivateProfileStringA("Section", "Key", "default", ...)
    re.compile(rf'GetPrivateProfileStringA\("({SECTION_ALT})",\s*"(\w+)",\s*"([^"]*)"'),
    # GetPrivateProfileStringW(L"Section", L"Key", L"default", ...). The launcher
    # proxy reads wide: its ini sits beside the game, and a Steam library path
    # can hold characters the ANSI code page cannot represent.
    re.compile(rf'GetPrivateProfileStringW\(L"({SECTION_ALT})",\s*L"(\w+)",\s*L"([^"]*)"'),
)

# Reads whose default is elsewhere, recorded for presence only.
KEY_ONLY = (
    re.compile(rf'arlandConfigBool\("({SECTION_ALT})",\s*"(\w+)"'),
    re.compile(rf'GetPrivateProfile\w+A\("({SECTION_ALT})",\s*"(\w+)"'),
    re.compile(rf'"ARLAND_\w+",\s*"({SECTION_ALT})",\s*"(\w+)"'),
)


def parse_retired():
    """(section, key) pairs config.cpp reports as retired.

    Guarded because the list is a claim about the code: a key listed there and
    still read by src/ would have the log telling a player it does nothing while
    it quietly went on working.
    """
    text = (ROOT / "src" / "core" / "config.cpp").read_text(encoding="utf-8")
    block = re.search(r"kRetiredKeys\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not block:
        return None
    return set(re.findall(r'\{\s*"(\w+)",\s*"(\w+)"\s*\}', block.group(1)))


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
    """(section, key) -> default from the source, or None if not a literal."""
    defaults = {}
    for source in sorted(ROOT.glob("src/**/*.cpp")):
        text = source.read_text(encoding="utf-8")
        for pattern in PATTERNS:
            for section, key, value in pattern.findall(text):
                if value == SENTINEL:
                    defaults.setdefault((section, key), None)
                else:
                    defaults.setdefault((section, key), value)
        for pattern in KEY_ONLY:
            for section, key in pattern.findall(text):
                defaults.setdefault((section, key), None)
    return defaults


def parse_matrix():
    """Per game: (section, key) -> ("X" | "O" | "U").

    game.cpp holds a descriptor row per feature and a kMatrix with one row per
    game, where X is on by default, O is opt-in and U is unsupported.
    featureEnabled() turns a cell into the key's default, so the cell is the
    default for that game.

    One descriptor field changes what a cell means: `invert` is set when the ini
    key is worded as the opposite of the feature, and featureEnabled then flips
    the default. BattleCutInDimming is the only such row, and reading its cell
    straight gives exactly the wrong answer.
    """
    text = (ROOT / "src" / "core" / "game.cpp").read_text(encoding="utf-8")
    rows = re.findall(
        r'/\*\s*(\w+)\s*\*/\s*\{\s*(?:"(\w+)"|nullptr),\s*(?:"(\w+)"|nullptr),'
        r'\s*(?:"(\w+)"|nullptr),\s*(true|false)\s*\}',
        text,
    )
    # Descriptor rows carry their Feature's name and support rows name their
    # Feature outright, so the two tables are paired BY NAME. They used to be
    # paired by position across two separate tables, which is the coupling the
    # support table was reshaped to remove.
    support = re.findall(
        r"\{\s*Feature::(\w+),\s*Rorona\(([XOU])\),\s*Totori\(([XOU])\),"
        r"\s*Meruru\(([XOU])\)\s*\}",
        text,
    )
    if not support or not rows or len(support) != len(rows):
        return None
    cells = {name: (r, t, m) for name, r, t, m in support}
    if len(cells) != len(support):
        return None

    per_game = [{} for _ in GAMES]
    columns = {}
    inverted = set()
    for feature, _env, section, key, invert in rows:
        if feature not in cells:
            return None
        columns[feature] = cells[feature]
        if not section or not key:
            continue
        if invert == "true":
            inverted.add((section, key))
        for game in range(len(GAMES)):
            per_game[game][(section, key)] = cells[feature][game]

    for entry, feature in KEYLESS_ROWS.items():
        cell = columns.get(feature)
        if cell is None:
            return None
        for game in range(len(GAMES)):
            per_game[game][entry] = cell[game]
    return per_game, inverted


def write_defaults(launcher, game, out):
    """Run the launcher headless. Returns an error string, or None on success."""
    runner = shlex.split(os.environ.get("ARLAND_LAUNCHER_RUNNER", ""))
    command = runner + [str(launcher), "--write-defaults", game, str(out)]
    try:
        result = subprocess.run(command, capture_output=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"could not run the launcher for {game}: {error}"
    if result.returncode != 0:
        return f"the launcher exited {result.returncode} for {game}"
    if not out.exists():
        return f"the launcher wrote no file for {game}"
    return None


def same(written, expected):
    """Compare an ini value with a source default across the two spellings."""
    if expected in ("true", "false") or written in ("true", "false"):
        truthy = {"true", "1", "yes", "on"}
        return (written.lower() in truthy) == (expected.lower() in truthy)
    if written.isdigit() and expected.isdigit():
        return int(written) == int(expected)
    return written == expected


def compare(game, ini, source, cells, inverted):
    problems = []

    for entry, value in sorted(ini.items()):
        if entry not in source:
            problems.append(
                f"{game}: the launcher writes [{entry[0]}] {entry[1]}, "
                "which nothing in src/ reads"
            )
            continue
        cell = cells.get(entry)
        if cell == "U":
            problems.append(
                f"{game}: the launcher writes [{entry[0]}] {entry[1]}, "
                "but the feature table says this game does not have it"
            )
            continue
        if entry in DIFFERENT_BY_DESIGN:
            continue
        # A keyless row's cell says whether the game has the option, not what it
        # is set to: those are valued knobs with their own reader in src/.
        from_table = cell is not None and entry not in KEYLESS_ROWS
        if from_table:
            actionable = cell == "X"
            expected = "true" if (actionable != (entry in inverted)) else "false"
        else:
            expected = source[entry]
        if expected is not None and not same(value, expected):
            where = "the feature table" if from_table else "src/"
            problems.append(
                f"{game}: [{entry[0]}] {entry[1]} is {value!r} from the "
                f"launcher and {expected!r} from {where}"
            )

    for entry in sorted(source):
        if entry in ini or entry in NOT_WRITTEN_AT_DEFAULT:
            continue
        if cells.get(entry) == "U":
            continue          # correctly absent: this game does not have it
        problems.append(
            f"{game}: [{entry[0]}] {entry[1]} is read by src/ but the launcher "
            "does not write it, so nobody can find it"
        )

    return problems


def main():
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <path to arland-fix-launcher.exe>",
              file=sys.stderr)
        return 2
    launcher = Path(sys.argv[1])
    if not launcher.exists():
        print(f"no launcher at {launcher}", file=sys.stderr)
        return 2

    source = parse_source()
    parsed = parse_matrix()
    if parsed is None:
        print("the feature table in src/core/game.cpp could not be read",
              file=sys.stderr)
        return 1
    per_game, inverted = parsed

    retired = parse_retired()
    if retired is None:
        print("the retired-key list in src/core/config.cpp could not be read",
              file=sys.stderr)
        return 1

    # A key cannot be both retired and read. The log would be telling a player
    # it does nothing while it went on working.
    problems = [
        f"[{section}] {key} is listed as retired but src/ still reads it"
        for section, key in sorted(retired & set(source))
    ]

    with tempfile.TemporaryDirectory() as tmp:
        for index, game in enumerate(GAMES):
            out = Path(tmp) / f"{game}.ini"
            failure = write_defaults(launcher, game, out)
            if failure:
                problems.append(failure)
                continue
            problems += compare(game, parse_ini(out), source, per_game[index],
                                inverted)

    for entry, reason in sorted(NOT_WRITTEN_AT_DEFAULT.items()):
        print(f"note: [{entry[0]}] {entry[1]}: {reason}")
    for entry, reason in sorted(DIFFERENT_BY_DESIGN.items()):
        print(f"note: [{entry[0]}] {entry[1]} differs on purpose: {reason}")

    if problems:
        print("\nThe launcher and src/ disagree about the mod's defaults:\n",
              file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        print("\nFix whichever is wrong, or add the option to NOT_WRITTEN_AT_DEFAULT "
              "in this script with the reason it cannot be offered.",
              file=sys.stderr)
        return 1

    print(f"\nok: {len(GAMES)} games checked against src/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
