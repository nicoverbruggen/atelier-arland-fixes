#!/usr/bin/env python3
"""Check default.ini against the defaults the code actually uses.

default.ini is what users get in the release archive, and it repeats values that
really live in src/. Nothing stops the two drifting apart, and a stale shipped
file misrepresents the mod to exactly the people least able to notice. This
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
SECTIONS = ("Rendering", "Battle", "Launcher", "Diagnostics")

# Options the code reads that are deliberately kept out of default.ini. Empty:
# everything the DLL reads is a documented, shipped option.
UNDOCUMENTED = set()

# Seeded per game by featureEnabled() from a per-game matrix rather than from a
# single literal, so the value cannot be extracted from one call site.
VALUE_NOT_EXTRACTABLE = {
    ("Battle", "BattleCutInShadows"),
    ("Battle", "BattleCutInDimming"),
}

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

    for entry in sorted(set(ini) & set(source)):
        if entry in VALUE_NOT_EXTRACTABLE:
            continue
        expected = source[entry]
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
            "\nUpdate default.ini (and ADVANCED.md) to match, or adjust the "
            "allowlists in this script if an option is deliberately undocumented.",
            file=sys.stderr,
        )
        return 1

    print(f"default.ini agrees with the code ({len(ini)} options checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
