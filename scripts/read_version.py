#!/usr/bin/env python3
"""Read and validate the repository's canonical release version."""

from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: read_version.py VERSION")
    version = Path(sys.argv[1]).read_text(encoding="utf-8").strip()
    parts = version.split(".")
    if len(parts) != 3 or any(not part.isdecimal() for part in parts):
        raise SystemExit("VERSION must contain three numeric components: X.Y.Z")
    print(version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
