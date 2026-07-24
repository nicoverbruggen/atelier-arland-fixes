#!/usr/bin/env python3
# Thicken a TTF by geometrically expanding its outlines outward ("grow the
# border") to synthesize a heavier weight from a lighter master. A uniform
# stroke-expand keeps straight edges straight, unlike a stem-aware weight change.
#
# NOT part of the meson build: a one-off asset-prep step run inside the fntbld-oci
# image, which bundles FontForge. The .ttf it produces is vendored under
# vendor/font/ and embedded like any other font; the source .ttf is vendored
# alongside it so this is reproducible.
#
#   podman run --rm --security-opt label=disable -v <repo>:/work \
#     ghcr.io/nicoverbruggen/fntbld-oci:latest \
#     python3 /work/scripts/embolden_font.py <in.ttf> <out.ttf> <amount> \
#       [family] [full]
#
# `amount` is the pen diameter in font units (~16 at 1000 upem gives a medium
# between a Regular and a Bold). When the source license requires a modified font
# to be renamed (MgOpen fonts must drop the word "MgOpen"), pass a new family and
# full name: the source word is scrubbed from the name records while the copyright
# records are left intact.
import sys

import fontforge

_FORM = None


def stroke(glyph, amount):
    # The stroke() signature differs across FontForge versions; probe the known
    # forms on the first glyph and reuse whichever one is accepted.
    global _FORM
    forms = [
        lambda: glyph.stroke("circular", amount, cap="round", join="round",
                             removeinternal=True),
        lambda: glyph.stroke("circular", amount, "round", "round",
                             ("removeinternal",)),
        lambda: glyph.stroke("circular", amount, "round", "round",
                             "removeinternal"),
        lambda: glyph.stroke("circular", amount, removeinternal=True),
    ]
    if _FORM is not None:
        forms[_FORM]()
        return
    for i, form in enumerate(forms):
        try:
            form()
            _FORM = i
            return
        except (TypeError, ValueError):
            continue
    raise RuntimeError("no working stroke() signature in this FontForge")


def main() -> int:
    if len(sys.argv) < 4:
        sys.stderr.write(
            "usage: embolden_font.py <in.ttf> <out.ttf> <amount> [family] [full]\n")
        return 2
    inp, outp, amount = sys.argv[1], sys.argv[2], float(sys.argv[3])
    family = sys.argv[4] if len(sys.argv) > 4 else None
    full = sys.argv[5] if len(sys.argv) > 5 else None

    font = fontforge.open(inp)
    for glyph in font.glyphs():
        if glyph.isWorthOutputting():
            stroke(glyph, amount)
    font.selection.all()
    font.removeOverlap()
    font.addExtrema()
    font.round()

    if family:
        source = "MgOpen"   # the word the MgOpen license forbids in a derivative
        font.familyname = family
        font.fullname = full or family
        font.fontname = (full or family).replace(" ", "-")
        font.weight = "Medium"
        cleaned = []
        for lang, key, val in font.sfnt_names:
            if isinstance(val, str) and source in val:
                val = val.replace("MgOpen Cosmetica", family).replace(source, family)
            cleaned.append((lang, key, val))
        font.sfnt_names = tuple(cleaned)

    font.generate(outp)
    print("generated", outp, "(stroke +%g)" % amount)
    return 0


if __name__ == "__main__":
    sys.exit(main())
