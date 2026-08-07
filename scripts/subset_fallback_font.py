#!/usr/bin/env python3
# Build vendor/font/ArlandFallback.ttf: the symbols the bundled per-game faces do
# not carry, which font_hires falls back to per glyph instead of demoting the whole
# string to baked art.
#
# NOT part of the meson build: a one-off asset-prep step. The .ttf it produces is
# vendored under vendor/font/ and embedded like any other font. The sources are not
# vendored, because they are megabytes to keep a 7 KB result reproducible:
#
#   Source Sans 3 3.052   https://github.com/adobe-fonts/source-sans/releases/download/3.052R/TTF-source-sans-3.052R.zip
#                         sha256 1b0dd1ec44b39f1dd98bbd153a1a3815f083639874ddee02c842bd601bad3d21
#   Inter 4.1             https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
#                         sha256 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
#
#   python3 scripts/subset_fallback_font.py \
#     <unzipped>/extras/ttf/Inter-Regular.ttf \
#     <unzipped>/TTF/SourceSans3-Semibold.ttf \
#     vendor/font/ArlandFallback.ttf
#
# Needs fontTools. Two sources because no single face draws all of these well. Inter
# is the default: its triangles and circles match the cap height of the text around
# them, its arrows carry the weight, and it is the only one of the two with the
# travel-route arrow and the reference mark. Source Sans 3 supplies the musical
# notes, where Inter's head is too small for its stem and drops below the line, and
# the Greek letters, which it draws closer to Cosmetica's weight. The RANGES table
# below says which source each family comes from. The music fonts that were also
# compared (Noto Music, Bravura) draw their note for a notation stave, which hangs
# below a line of running text.
#
# Both sources are under the SIL Open Font License. Source Sans 3 reserves the name
# "Source" and "Inter" is rsms's trademark, so the merged face is renamed either
# way; both copyright records are kept, and licenses/ArlandFallback.txt credits each
# source for the glyphs it supplied.
#
# Eight characters in the three games' English text are missing from the face that
# game uses, and every one of them is a symbol: ⇔ in Totori's travel routes, and
# ♪ ※ △ ○ → α β γ in Meruru's tips and quest names. The ranges below are those
# eight plus the rest of each family they come from, which is what stops the next
# string reaching for a sibling the subset does not have. A family the games never
# touch does not belong here, and neither do letters: the fallback face does not
# match the game's in design, which reads as a symbol next to a word and as a
# mistake inside one.
#
# Everything the rasterizer does not read is dropped as well. stb_truetype ignores
# hinting, glyph names and OpenType layout, and a name table carries the license
# text in every language the font ships, which the release ships as a file anyway.
# That is most of the file: the same coverage is tens of KB with those tables and
# under 10 KB without.
import sys

from fontTools import subset
from fontTools.merge import Merger
from fontTools.ttLib import TTFont
from fontTools.ttLib.scaleUpem import scale_upem

# Which source draws each family, as an index into the sources given on the command
# line. A family falls through to the next source when its preferred one has no
# glyph for it, so a wrong guess costs coverage rather than the build.
DEFAULT, NOTES = 0, 1
RANGES = [
    (0x03B1, 0x03C9, NOTES),    # Greek small letters: Meruru's quest tiers (Chim Delivery α)
    (0x2022, 0x2022, DEFAULT),  # bullet, which the ・ fold maps to
    (0x2026, 0x2026, DEFAULT),  # ellipsis
    (0x203B, 0x203B, DEFAULT),  # reference mark
    (0x2190, 0x2195, DEFAULT),  # single arrows
    (0x21D0, 0x21D5, DEFAULT),  # double arrows, including the travel-route arrow
    (0x25A0, 0x25A1, DEFAULT),  # squares
    (0x25B2, 0x25BD, DEFAULT),  # triangles, including the play and pause pointers
    (0x25C6, 0x25C7, DEFAULT),  # diamonds
    (0x25CB, 0x25CF, DEFAULT),  # circles
    (0x2605, 0x2606, DEFAULT),  # stars
    (0x2660, 0x2667, DEFAULT),  # card suits
    (0x266A, 0x266F, NOTES),    # musical notes
]
NAME = "Arland Fallback"
# The em size the merged font is expressed in. Sources are scaled to it first,
# because the merge puts every outline in one coordinate space and Source Sans 3
# is drawn on 1000 units where Inter is on 2048. Scaling up rather than down so
# no source loses precision.
UPEM = 2048
# Tables stb_truetype never reads. `post` and the hinting programs go through the
# subsetter's own options below; these are the ones it would otherwise keep.
DROP = ["DSIG", "MATH", "GSUB", "GPOS", "GDEF", "gasp", "prep", "cvt ", "fpgm"]


def cut(path, codepoints):
    """Subset one source to `codepoints`, stripped of everything stb ignores."""
    font = TTFont(path)
    options = subset.Options()
    options.layout_features = []          # no kerning or ligatures: stb ignores both
    options.name_IDs = [0, 1, 2, 4, 6, 14]   # copyright, naming, license URL
    options.name_languages = [0x409]      # English only
    options.notdef_outline = True
    options.hinting = False
    options.glyph_names = False
    options.drop_tables += DROP
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)
    if font["head"].unitsPerEm != UPEM:
        scale_upem(font, UPEM)
    return font


def main():
    if len(sys.argv) < 4:
        sys.stderr.write(
            "usage: subset_fallback_font.py <preferred.ttf> [<next.ttf> ...] <out.ttf>\n")
        return 2
    sources, dst = sys.argv[1:-1], sys.argv[-1]
    cmaps = [set(TTFont(p).getBestCmap()) for p in sources]

    # Each character goes to its preferred source, then to whichever later one has
    # it. Report the split, because which face a glyph came from is the whole point
    # of using more than one.
    take = [set() for _ in sources]
    wanted = set()
    for lo, hi, prefer in RANGES:
        for cp in range(lo, hi + 1):
            wanted.add(cp)
            order = [prefer] + [i for i in range(len(sources)) if i != prefer]
            for i in order:
                if cp in cmaps[i]:
                    take[i].add(cp)
                    break

    parts, claimed = [], set()
    for path, mine in zip(sources, take):
        if not mine:
            sys.stderr.write("%s: nothing to take\n" % path)
            continue
        claimed |= mine
        parts.append(cut(path, mine))
        sys.stderr.write("%s: %s\n" % (path.split("/")[-1],
                                       " ".join(chr(c) for c in sorted(mine))))
    absent = sorted(wanted - claimed)
    if absent:
        sys.stderr.write("no source has: %s\n"
                         % " ".join("U+%04X %s" % (c, chr(c)) for c in absent))
    if not parts:
        sys.stderr.write("no glyphs to merge\n")
        return 1

    if len(parts) == 1:
        font = parts[0]
    else:
        # Merger works on files, so round-trip the subsets through the output path.
        paths = []
        for i, part in enumerate(parts):
            p = "%s.part%d" % (dst, i)
            part.save(p)
            paths.append(p)
        font = Merger().merge(paths)
        import os
        for p in paths:
            os.remove(p)

    # The merge keeps only the first source's name table, so the copyright of every
    # source that contributed a glyph has to be put back by hand.
    credits = " / ".join(dict.fromkeys(
        r.toStr() for f in parts for r in f["name"].names if r.nameID == 0))
    for record in font["name"].names:
        if record.nameID == 0:
            record.string = credits
        elif record.nameID in (1, 4):
            record.string = NAME
        elif record.nameID == 6:
            record.string = NAME.replace(" ", "")
    font.save(dst)
    sys.stderr.write("%d glyphs, %d codepoints\n"
                     % (font["maxp"].numGlyphs, len(claimed)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
