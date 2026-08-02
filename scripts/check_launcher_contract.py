#!/usr/bin/env python3
"""Check that the Arland launcher pieces agree on their shared contract.

The 64-bit settings window and the 32-bit msimg32 proxy cannot share code, but
they must agree on the files, executables, and environment variables that join
them.  Keep this check deliberately small and source-based so it runs without a
Windows toolchain or a built executable.
"""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
GUI = (ROOT / "src" / "config_gui" / "main.cpp").read_text()
PROXY = (ROOT / "src" / "launcher_proxy.cpp").read_text()


def fail(message):
    print(f"launcher contract check failed: {message}", file=sys.stderr)
    return 1


def required(text, pattern, label):
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f"could not find {label}")
    return match


def main():
    try:
        gui_games = tuple(
            re.findall(
                r'\{\s*"([^"]+)",\s*"([^"]+)",\s*L"([^"]+)"\s*\}',
                required(GUI, r"const Game kGames\[\] = \{(.*?)\};", "GUI game table").group(1),
            )
        )
        proxy_games = tuple(
            re.findall(
                r'\{\s*L"([^"]+)",\s*L"([^"]+)"\s*\}',
                required(
                    PROXY,
                    r"constexpr std::array<GameExecutables, 3> SupportedGames = \{\{(.*?)\}\};",
                    "proxy game table",
                ).group(1),
            )
        )

        # The proxy has no display name, so compare its launcher target against
        # the GUI's executable pair and ignore the GUI-only name field.
        expected_proxy_games = tuple(
            (english, multilingual)
            for english, multilingual, _ in gui_games
        )
        actual_proxy_games = tuple(
            (english, multilingual)
            for english, multilingual in proxy_games
        )
        if expected_proxy_games != actual_proxy_games:
            raise ValueError(
                f"GUI and proxy game executables differ: "
                f"GUI={expected_proxy_games!r}, proxy={actual_proxy_games!r}"
            )

        gui_launchers = tuple(
            re.findall(r'"([^"]+Launcher\.exe)"', GUI)
        )
        proxy_launchers = tuple(
            re.findall(r'L"([^"]+Launcher\.exe)"', PROXY)
        )
        # The GUI contains the common launcher name in comments and fallback
        # paths, while the proxy's supported host is intentionally one name.
        if "ArlandDXLauncher.exe" not in gui_launchers:
            raise ValueError("GUI does not reference ArlandDXLauncher.exe")
        if "ArlandDXLauncher.exe" not in proxy_launchers:
            raise ValueError("proxy does not recognize ArlandDXLauncher.exe")

        for source_name, text in (("GUI", GUI), ("proxy", PROXY)):
            for literal in ("arland-fix.ini", "ArlandDX_Settings.ini",
                            "SkipLauncher", "ARLAND_NO_REDIRECT",
                            "arland-fix-launcher.exe"):
                if literal not in text:
                    raise ValueError(f"{source_name} is missing {literal}")

        # The GUI and proxy independently encode the same language selection:
        # 1, 3, and 4 use the multilingual executable; everything else uses the
        # English executable. Check both source forms rather than allowing a
        # future edit to silently change one side.
        gui_language = required(
            GUI,
            r"const bool english = code != '1' && code != '3' && code != '4';",
            "GUI language mapping",
        )
        proxy_language = required(
            PROXY,
            r"const bool english = language\[0\] != L'1' && language\[0\] != L'3' &&\s*"
            r"language\[0\] != L'4';",
            "proxy language mapping",
        )
        if not gui_language or not proxy_language:
            raise ValueError("language mapping is incomplete")
    except (OSError, ValueError) as exc:
        return fail(str(exc))

    print(
        "launcher contract ok: game executables, shared files, redirect "
        "controls, and language mapping agree"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
