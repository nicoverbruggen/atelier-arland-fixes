#!/usr/bin/env python3
"""Embed a binary file as a C++ byte array.

Usage: embed_font.py <input> <output.cpp> <symbol>

Emits, in namespace atfix, `const unsigned char <symbol>[]` holding the file's
bytes and `const unsigned int <symbol>Size = sizeof(...)`. The matching extern
declarations live in src/embedded_font.h. The build runs this from meson.build so
the large generated sources are not checked in; only the vendored .ttf files are.
"""
import os
import sys


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: embed_font.py <input> <output.cpp> <symbol>\n")
        return 2
    src, dst, symbol = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(src, "rb") as f:
        data = f.read()

    out = [
        "// SPDX-License-Identifier: MIT",
        "// Auto-generated from %s by scripts/embed_font.py." % os.path.basename(src),
        "// Do not edit or check in; the build regenerates it from the vendored font.",
        '#include "embedded_font.h"',
        "",
        "namespace atfix {",
        "",
        "const unsigned char %s[] = {" % symbol,
    ]
    for i in range(0, len(data), 16):
        out.append("  " + "".join("0x%02x," % b for b in data[i:i + 16]))
    out.append("};")
    out.append("const unsigned int %sSize = sizeof(%s);" % (symbol, symbol))
    out.append("")
    out.append("}  // namespace atfix")

    with open(dst, "w") as f:
        f.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
