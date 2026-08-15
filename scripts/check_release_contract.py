#!/usr/bin/env python3
"""Keep the source stamp and third-party release licensing intact.

Ported from the sibling Dusk project's script of the same name, and given the
assertions this repository actually needs. Dusk vendors one third-party asset
under its own licence; this project vendors four typefaces as well, embeds them
in the DLL, and ships their licences in the release archive.

The font licences are the reason this check exists. Both release layouts copy
the whole `licenses/` directory with one `cp -r`, so a licence that disappears
from it disappears from the archive with nothing to say so. MinHook and SMAA are
copied by name and would fail loudly; the fonts would not.

Run from the repository root, or via `python3 scripts/check_release_contract.py`.
"""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent

# Vendored trees that carry their own LICENSE.txt, and the name that licence is
# given inside the release archive. LICENSE points readers at both spellings, so
# both have to keep agreeing with what the packaging steps write.
VENDORED = (
    ("vendor/minhook/LICENSE.txt", "MinHook.txt"),
    ("vendor/smaa/LICENSE.txt", "SMAA.txt"),
)

# Vendored code that ships no LICENSE.txt of its own, so licenses/ is the only
# record of its terms. Keyed by the file that would be built without it.
LOOSE = (("vendor/stb/stb_truetype.h", "stb_truetype.txt"),)

# The two release layouts. Both build the same archive and both have to be
# checked: the local script is what a maintainer runs, the workflow is what
# actually publishes. They write the same copies with different spellings, one
# through shell variables and one with literal paths, so these are matched by
# shape rather than by an exact line.
LAYOUTS = ("scripts/build_linux.sh", ".github/workflows/build.yml")
COPY_LICENSES = re.compile(r"cp\s+-r\s+\S*licenses\S*\s+\S*/LICENSES")


def fail(message):
    print(f"release contract check failed: {message}", file=sys.stderr)
    return 1


def embedded_fonts(meson):
    """The .ttf files meson compiles into the DLL, by file stem.

    Read out of meson rather than listed here, so a font added to the build
    without a licence fails this check instead of shipping unlicensed.
    """
    fonts = re.findall(r"input\s*:\s*'vendor/font/([^']+)\.ttf'", meson)
    if not fonts:
        raise ValueError("no embedded fonts found in meson.build")
    return fonts


def main():
    try:
        meson = (ROOT / "meson.build").read_text(encoding="utf-8")
        main_cpp = (ROOT / "src" / "core" / "main.cpp").read_text(encoding="utf-8")
        stamp = (ROOT / "src" / "core" / "version_git.h.in").read_text(
            encoding="utf-8"
        )
        license_text = (ROOT / "LICENSE").read_text(encoding="utf-8")

        # ---- the source stamp
        for fragment in (
            "version_git_header = vcs_tag(",
            "'git', '-c', 'safe.directory=*', 'describe'",
            "'--dirty=-dirty'",
            "fallback : 'unknown'",
        ):
            if fragment not in meson:
                raise ValueError("Meson source stamp is missing: " + fragment)
        if (
            '#include "version_git.h"' not in main_cpp
            or "ARLAND_FIX_GIT" not in main_cpp
        ):
            raise ValueError("the game DLL no longer logs the generated source stamp")
        if '#define ARLAND_FIX_GIT "@VCS_TAG@"' not in stamp:
            raise ValueError("version_git.h.in no longer publishes @VCS_TAG@")

        # ---- the bundled fonts
        for font in embedded_fonts(meson):
            source = ROOT / "vendor" / "font" / f"{font}.ttf"
            licence = ROOT / "licenses" / f"{font}.txt"
            if not source.is_file():
                raise ValueError(f"meson embeds vendor/font/{font}.ttf, which is absent")
            if not licence.is_file():
                raise ValueError(
                    f"{font}.ttf is embedded in the DLL with no licenses/{font}.txt "
                    f"to ship beside it"
                )
            # A font licence that defers to the OFL keeps its full text in the
            # same directory, and the archive is where a user reads it.
            if "OFL-1.1.txt" in licence.read_text(encoding="utf-8") and not (
                ROOT / "licenses" / "OFL-1.1.txt"
            ).is_file():
                raise ValueError(
                    f"licenses/{font}.txt defers to OFL-1.1.txt, which is absent"
                )

        # ---- vendored code whose licence lives only in licenses/
        for source, licence in LOOSE:
            if (ROOT / source).is_file() and not (ROOT / "licenses" / licence).is_file():
                raise ValueError(f"{source} ships with no licenses/{licence}")

        # ---- what each release layout packages
        for path in LAYOUTS:
            layout = (ROOT / path).read_text(encoding="utf-8")
            if not COPY_LICENSES.search(layout):
                raise ValueError(
                    f"{path} no longer copies licenses/ into the release archive"
                )
            for vendored, name in VENDORED:
                if vendored not in layout or f"LICENSES/{name}" not in layout:
                    raise ValueError(
                        f"{path} no longer packages {vendored} as LICENSES/{name}"
                    )

        # ---- what LICENSE promises is in the archive
        for _, name in VENDORED:
            if f"LICENSES/{name}" not in license_text:
                raise ValueError(f"LICENSE no longer names the packaged {name}")
    except (OSError, ValueError) as exc:
        return fail(str(exc))

    print("release contract ok: source stamp, font licences and vendored licence "
          "packaging agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
