#!/usr/bin/env python3
"""Verify the proxy DLL exports required by the games and launcher.

The parser is dependency-free and reads the PE export directory directly, so
the same check works for MinGW builds on Linux and MSVC builds on Windows.

Usage:
    python3 scripts/check_exports.py build64/d3d11.dll build32/msimg32.dll
"""

import struct
import sys
from pathlib import Path


EXPECTED = {
    "d3d11.dll": {
        "D3D11CreateDevice": 22,
        "D3D11CreateDeviceAndSwapChain": 23,
    },
    "msimg32.dll": {
        "AlphaBlend": None,
        "TransparentBlt": None,
    },
}


class PeError(ValueError):
    pass


def unpack_from(fmt, data, offset, label):
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(data):
        raise PeError(f"truncated {label}")
    return struct.unpack_from(fmt, data, offset)


def read_c_string(data, offset):
    if offset < 0 or offset >= len(data):
        raise PeError("export name points outside the file")
    end = data.find(b"\0", offset)
    if end < 0:
        raise PeError("unterminated export name")
    try:
        return data[offset:end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise PeError("non-ASCII export name") from exc


def pe_exports(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise PeError("missing DOS header")

    (pe_offset,) = unpack_from("<I", data, 0x3C, "DOS header")
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise PeError("missing PE header")

    coff = pe_offset + 4
    _, section_count, _, _, _, optional_size, _ = unpack_from(
        "<HHIIIHH", data, coff, "COFF header"
    )
    optional = coff + 20
    if optional + optional_size > len(data):
        raise PeError("truncated optional header")

    (magic,) = unpack_from("<H", data, optional, "optional header")
    if magic == 0x10B:
        directory_offset = optional + 96
    elif magic == 0x20B:
        directory_offset = optional + 112
    else:
        raise PeError(f"unsupported optional-header magic 0x{magic:x}")
    if directory_offset + 8 > optional + optional_size:
        raise PeError("optional header has no export directory")

    export_rva, export_size = unpack_from(
        "<II", data, directory_offset, "export data-directory entry"
    )
    if not export_rva or not export_size:
        raise PeError("PE has no export directory")

    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        entry = section_table + index * 40
        if entry + 40 > len(data):
            raise PeError("truncated section table")
        virtual_size, virtual_address, raw_size, raw_offset = unpack_from(
            "<IIII", data, entry + 8, f"section {index}"
        )
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_offset(rva, size=1):
        for virtual_address, virtual_span, raw_offset, raw_size in sections:
            relative = rva - virtual_address
            if 0 <= relative and relative + size <= virtual_span:
                if relative + size > raw_size:
                    raise PeError(f"RVA 0x{rva:x} has no file-backed data")
                offset = raw_offset + relative
                if offset + size > len(data):
                    raise PeError(f"RVA 0x{rva:x} points outside the file")
                return offset
        raise PeError(f"RVA 0x{rva:x} is outside every section")

    export = rva_offset(export_rva, 40)
    ordinal_base, function_count, name_count, _, names_rva, ordinals_rva = unpack_from(
        "<IIIIII", data, export + 16, "export directory"
    )
    if name_count > function_count:
        raise PeError("export directory has more names than functions")

    names = rva_offset(names_rva, name_count * 4)
    ordinals = rva_offset(ordinals_rva, name_count * 2)
    result = {}
    for index in range(name_count):
        (name_rva,) = unpack_from("<I", data, names + index * 4, "export name table")
        (ordinal_index,) = unpack_from(
            "<H", data, ordinals + index * 2, "export ordinal table"
        )
        if ordinal_index >= function_count:
            raise PeError("export ordinal index exceeds the function table")
        name = read_c_string(data, rva_offset(name_rva))
        if name in result:
            raise PeError(f"duplicate export name {name!r}")
        result[name] = ordinal_base + ordinal_index
    return result


def check(path):
    expected = EXPECTED.get(path.name.lower())
    if expected is None:
        raise PeError(f"no export contract defined for {path.name!r}")

    exports = pe_exports(path)
    problems = []
    for name, ordinal in expected.items():
        actual = exports.get(name)
        if actual is None:
            problems.append(f"missing export {name}")
        elif ordinal is not None and actual != ordinal:
            problems.append(f"{name} has ordinal {actual}, expected {ordinal}")
    if problems:
        raise PeError("; ".join(problems))

    details = ", ".join(
        f"{name}@{exports[name]}" if ordinal is not None else name
        for name, ordinal in expected.items()
    )
    print(f"exports ok: {path} ({details})")


def main(argv):
    if len(argv) < 2:
        print(f"usage: {Path(argv[0]).name} DLL [DLL ...]", file=sys.stderr)
        return 2

    failed = False
    for raw_path in argv[1:]:
        path = Path(raw_path)
        try:
            check(path)
        except (OSError, PeError) as exc:
            print(f"export check failed: {path}: {exc}", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
