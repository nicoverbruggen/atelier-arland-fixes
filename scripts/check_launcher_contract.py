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
GUI = (ROOT / "src" / "launcher" / "launcher_gui.cpp").read_text()
PROXY = (ROOT / "src" / "launcher" / "launcher_proxy.cpp").read_text()
INI_WRITES = (ROOT / "src" / "launcher" / "ini_write_set.h").read_text()


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
        #
        # Both compare the WHOLE value, as the stock launcher does. A
        # first-character test would read "10" as Japanese where the game reads
        # it as unrecognized and starts the English build, so the comparison
        # form is part of the contract rather than an implementation detail.
        gui_language = required(
            GUI,
            r'const bool english = lstrcmpA\(code, "1"\) != 0 && '
            r'lstrcmpA\(code, "3"\) != 0 &&\s*lstrcmpA\(code, "4"\) != 0;',
            "GUI language mapping",
        )
        proxy_language = required(
            PROXY,
            r'const bool english = lstrcmpW\(language\.data\(\), L"1"\) != 0 &&\s*'
            r'lstrcmpW\(language\.data\(\), L"3"\) != 0 &&\s*'
            r'lstrcmpW\(language\.data\(\), L"4"\) != 0;',
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

        # armRedirect runs from DllMain, so it holds the loader lock. Deciding
        # what to start means profile reads and file-attribute queries, and none
        # of that belongs there: it is the heaviest thing the proxy does and it
        # has to happen before the executable's entry point either way. Pinned
        # here because the pull to "just check the file exists first" is real and
        # the cost of giving in to it is invisible until a prefix deadlocks.
        arm = required(
            PROXY, r"bool armRedirect\(\) \{(.*?)\n\}", "armRedirect"
        ).group(1)
        for banned in (
            "PrivateProfile",
            "GetFileAttributes",
            "EnumDisplaySettings",
            "CreateFile",
            "skipLauncherRequested",
            "resolveGameExecutable",
        ):
            if banned in arm:
                raise ValueError(
                    "armRedirect holds the loader lock and must not touch the "
                    "file system: " + banned
                )

        # The five entry bytes go back before anything that can fail, so every
        # path after it reaches the stock launcher by a plain call. A restore
        # that cannot report its own failure is the dangerous one: the jump is
        # still live, and calling it comes straight back into the redirect.
        restore = required(
            PROXY, r"bool restoreEntryPoint\(\) \{(.*?)\n\}", "restoreEntryPoint"
        ).group(1)
        if "if (!VirtualProtect" not in restore or "return false" not in restore:
            raise ValueError("restoreEntryPoint does not report a failed restore")

        redirected = required(
            PROXY,
            r"void redirectedEntryPoint\(\) \{(.*?)\n\}",
            "redirectedEntryPoint",
        ).group(1)
        if ("if (!restoreEntryPoint())" not in redirected
                or "ExitProcess(1)" not in redirected):
            raise ValueError("entry restore failure can recurse into the redirect")
        if redirected.index("restoreEntryPoint()") > redirected.index(
                "resolveStartTarget()"):
            raise ValueError(
                "the entry point is restored after the file work rather than "
                "before it"
            )

        # runOriginalEntryPoint is call-only now: the restore belongs to its
        # caller and happens once. A protection change back in here would mean
        # two restores and the recursion hazard returning with them.
        original = required(
            PROXY,
            r"void runOriginalEntryPoint\(\) \{(.*?)\n\}",
            "runOriginalEntryPoint",
        ).group(1)
        if "VirtualProtect" in original:
            raise ValueError(
                "runOriginalEntryPoint restores again; the restore belongs to "
                "its caller"
            )
        if "reinterpret_cast<void (*)()>(g_entryPoint)()" not in original:
            raise ValueError(
                "runOriginalEntryPoint never calls the entry point it stands for"
            )

        # And every fallback has to actually be reached. This was unpinned until
        # the diagnostic removal in 04b929f deleted a log line that was the whole
        # body of an unbraced `if`, leaving the call as the new body: the path
        # then returned from the launcher's entry point instead of calling it.
        #
        # Indentation cannot tell the two apart, which is why the defect was
        # invisible -- the orphaned call keeps the indent it always had. The
        # statement BEFORE it does tell them apart: a real one ends in `;`, `}`
        # or the `{` that opens the block.
        lines = redirected.splitlines()
        calls = [i for i, line in enumerate(lines)
                 if "runOriginalEntryPoint();" in line]
        if not calls:
            raise ValueError(
                "redirectedEntryPoint never falls back to the stock launcher"
            )
        for call in calls:
            preceding = ""
            for line in reversed(lines[:call]):
                stripped = line.strip()
                if stripped and not stripped.startswith("//"):
                    preceding = stripped
                    break
            if not preceding.endswith((";", "}", "{")):
                raise ValueError(
                    "a fallback to the stock launcher is the body of an unbraced "
                    "condition, so it does not run when it should: " + preceding
                )


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
