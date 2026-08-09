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
INI_WRITES = (ROOT / "src" / "config_gui" / "ini_write_set.h").read_text()


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

        arm = required(
            PROXY, r"bool armRedirect\(\) \{(.*?)\n\}", "armRedirect"
        ).group(1)
        if "kLauncherEntryExpected" not in arm or "std::memcmp(g_entryPoint" not in arm:
            raise ValueError("proxy does not verify the launcher entry byte window")
        if arm.index("std::memcmp(g_entryPoint") > arm.index("VirtualProtect(g_entryPoint"):
            raise ValueError("proxy makes the entry writable before verifying its bytes")
        if len(re.findall(r"0x[0-9a-f]{2}", required(
            PROXY,
            r"kLauncherEntryExpected\s*=\s*\{(.*?)\};",
            "launcher entry expected array",
        ).group(1))) != 17:
            raise ValueError("launcher entry verification window is not 17 bytes")
        for token in (
            "kLauncherEntryRelocationOffset = 13",
            "kLauncherPreferredImageBase = 0x00400000",
            "relocateEntryWindow(base, expectedEntry)",
        ):
            if token not in PROXY:
                raise ValueError(f"launcher entry verification is not ASLR-aware: {token}")

        original = required(
            PROXY,
            r"void runOriginalEntryPoint\(\) \{(.*?)\n\}",
            "runOriginalEntryPoint",
        ).group(1)
        if "if (!VirtualProtect" not in original or "ExitProcess(1)" not in original:
            raise ValueError("entry restore failure can recurse into the redirect")

        if "LastWrite" in GUI or "verifyWrite(" in GUI:
            raise ValueError("GUI reverted to last-key-only save verification")
        for token in ("g_iniWrites.verify", "g_settingsWrites.verify"):
            if token not in GUI:
                raise ValueError(f"GUI does not exhaustively verify {token}")
        for token in ("for (size_t i = 0; i < count_; ++i)",
                      "entries_[i].deleted", "GetPrivateProfileStringA"):
            if token not in INI_WRITES:
                raise ValueError(f"INI write-set helper is missing {token}")

        close = required(
            GUI, r"case WM_CLOSE:(.*?)case WM_DESTROY:", "WM_CLOSE handler"
        ).group(1)
        if not re.search(
            r"const SaveOutcome saved = saveToIni\(\);.*?"
            r"if \(!saved\.ok\(\)\) \{.*?return 0;.*?\}.*?DestroyWindow",
            close, re.DOTALL,
        ):
            raise ValueError("failed close-time save can still destroy the window")
    except (OSError, ValueError) as exc:
        return fail(str(exc))

    print(
        "launcher contract ok: game executables, shared files, redirect "
        "controls, language mapping, entry verification, and save persistence agree"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
