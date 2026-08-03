#!/usr/bin/env python3
"""Check that the shipped Arland fix surface has not structurally drifted.

This is a source-level regression check, not a replacement for in-game
validation. It protects the central wiring that is easy to break while moving
code around: the feature enum/matrix, the game-side installer fan-out, and the
D3D11 synchronization hooks on which the core fix depends.
"""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
GAME_H = (ROOT / "src" / "game.h").read_text()
GAME_CPP = (ROOT / "src" / "game.cpp").read_text()
MAIN_CPP = (ROOT / "src" / "main.cpp").read_text()
MENU_CPP = (ROOT / "src" / "menu_fix.cpp").read_text()
SYNC_CPP = (ROOT / "src" / "sync_fix.cpp").read_text()


FEATURES = (
    "SyncFix", "MenuHitchFix", "AtlasCache", "FrameAtlasCache",
    "ResolutionOverride", "Msaa", "ShadowMultiplier", "BattleShadows",
    "CutInShadows", "CutInDimHold", "SkipStartupLogos", "SkipIntroMovie", "SynthesisAnimationRate",
)


def fail(message):
    print(f"core contract check failed: {message}", file=sys.stderr)
    return 1


def block(text, pattern, label):
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f"could not find {label}")
    return match.group(1)


def main():
    try:
        enum = block(
            GAME_H,
            r"enum class Feature : uint8_t \{(.*?)\};",
            "Feature enum",
        )
        enum_features = tuple(
            name for name in re.findall(r"^\s*(\w+),", enum, re.MULTILINE)
            if name != "Count"
        )
        if enum_features != FEATURES:
            raise ValueError(
                f"Feature enum changed: expected {FEATURES!r}, got {enum_features!r}"
            )

        descriptor = block(
            GAME_CPP,
            r"static const Descriptor table\[static_cast<int>\(Feature::Count\)\] = \{(.*?)\n\s*\};",
            "feature descriptor table",
        )
        descriptor_features = tuple(re.findall(r"/\*\s*(\w+)\s*\*/", descriptor))
        if descriptor_features != FEATURES:
            raise ValueError(
                "feature descriptor table no longer matches Feature enum: "
                f"{descriptor_features!r}"
            )

        matrix = block(
            GAME_CPP,
            r"constexpr Support kMatrix\[3\]\[static_cast<int>\(Feature::Count\)\] = \{(.*?)\n\s*\};",
            "capability matrix",
        )
        rows = re.findall(
            r"/\*\s*(Rorona|Totori|Meruru)\s*\*/\s*\{([^}]*)\}", matrix
        )
        expected_matrix = (
            ("Rorona", ("X", "X", "X", "X", "X", "O", "O", "X", "X", "X", "O", "O", "X")),
            ("Totori", ("X", "X", "X", "X", "X", "O", "O", "U", "X", "X", "O", "O", "X")),
            ("Meruru", ("X", "X", "X", "O", "X", "O", "O", "U", "X", "X", "O", "O", "X")),
        )
        actual_matrix = tuple(
            (name, tuple(re.findall(r"\b([UXO])\b", values)))
            for name, values in rows
        )
        if actual_matrix != expected_matrix:
            raise ValueError(
                f"capability matrix changed: expected {expected_matrix!r}, "
                f"got {actual_matrix!r}"
            )

        for installer in (
            "installBattleShadowRestore",
            "installFieldPhysics",
            "installWorldMapFix",
            "installItemGuard",
            "installStreamLifetimeFix",
            "installShopFix",
            "installLogoSkip",
            "installMovieSkip",
            "installMixCardFix",
        ):
            if not re.search(rf"\b{installer}\s*\(", MENU_CPP):
                raise ValueError(f"menu installer no longer calls {installer}()")

        if "arland::initializeGameHooks();" not in MAIN_CPP:
            raise ValueError("D3D11 initialization no longer enters game hooks")

        # These are the minimum synchronization hooks. Removing one can leave
        # the mod apparently loaded while making the shadow-copy path incomplete.
        required_hooks = (
            "CopyResource", "CopySubresourceRegion", "Map", "Unmap",
            "Draw", "DrawIndexed", "ExecuteCommandList", "UpdateSubresource",
        )
        context_block = block(
            SYNC_CPP,
            r"void hookContext\(ID3D11DeviceContext\* pContext\) \{(.*?)\n\}",
            "context hook installer",
        )
        for hook in required_hooks:
            if not re.search(rf"\b{hook}\s*\)", context_block):
                raise ValueError(f"required context hook is missing: {hook}")

        for hook in ("CreateBuffer", "CreateTexture2D", "CreateTexture3D"):
            if not re.search(rf"\b{hook}\s*\)", SYNC_CPP):
                raise ValueError(f"required device hook is missing: {hook}")
    except (OSError, ValueError) as exc:
        return fail(str(exc))

    print(
        "core contract ok: feature matrix, installer fan-out, and "
        "synchronization hook surface agree"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
