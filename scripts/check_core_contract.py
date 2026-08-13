#!/usr/bin/env python3
"""Check that the shipped Arland fix surface has not structurally drifted.

This is a source-level regression check, not a replacement for in-game
validation. It protects the central wiring that is easy to break while moving
code around: the feature enum/matrix, the game-side installer fan-out, and the
D3D11 synchronization hooks on which the core fix depends.
"""

import pathlib
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
    "ResolutionOverride", "ShadowMultiplier", "BattleShadows",
    "CutInShadows", "CutInDimHold", "SkipStartupLogos", "SkipIntroMovie", "SynthesisAnimationRate",
    "FieldMonsterSnap", "FieldCharacterPull", "FastSaveMenu",
    "PadRescanBackoff",
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
            ("Rorona", ("X", "X", "X", "X", "X", "X", "X", "O", "O", "O", "O", "X", "X", "X", "X", "X")),
            ("Totori", ("X", "X", "X", "X", "X", "X", "U", "O", "O", "O", "O", "X", "X", "X", "X", "X")),
            ("Meruru", ("X", "X", "X", "O", "X", "X", "U", "O", "O", "O", "O", "X", "X", "X", "X", "X")),
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
            "installFieldCollisionFix",
            "installSaveMenuFix",
        ):
            if not re.search(rf"\b{installer}\s*\(", MENU_CPP):
                raise ValueError(f"menu installer no longer calls {installer}()")

        # High-resolution text leaves a larger allocation behind after the
        # output object's dimensions are restored. Its replay cache may use
        # that larger capacity only while the allocation generation is live:
        # the verified game free entry point invalidates it before reuse. This
        # source contract prevents a future simplification from restoring the
        # old pointer-equality-as-capacity bug.
        if "installedTextBuffers" in MENU_CPP:
            raise ValueError(
                "text replay again carries output/pointer capacity records"
            )
        allocator = block(
            MENU_CPP,
            r"bool installTextBitmapAllocator\(.*?\) \{(.*?)\n\}",
            "text bitmap allocator installer",
        )
        required_text_lifetime = (
            "transaction.create(allocate",
            "reinterpret_cast<void*>(&observedTextBufferAlloc)",
            "transaction.create(release",
            "reinterpret_cast<void*>(&trackedTextBufferFree)",
            "transaction.enableAll()",
            "transaction.commit()",
            "gameAlloc = &trackedTextBufferAlloc",
            "gameFree = &trackedTextBufferFree",
        )
        for fragment in required_text_lifetime:
            if fragment not in allocator:
                raise ValueError(
                    "text-buffer lifetime installer is missing: " + fragment
                )
        tracked_free = block(
            MENU_CPP,
            r"void trackedTextBufferFree\(void\* buffer\) \{(.*?)\n\}",
            "tracked text-buffer free",
        )
        forget_at = tracked_free.find("forgetTrackedTextBuffer(buffer)")
        free_at = tracked_free.find("originalTextBufferFree(buffer)")
        if forget_at < 0 or free_at < 0 or forget_at > free_at:
            raise ValueError(
                "text-buffer generation is not invalidated before game free"
            )
        observed_alloc = block(
            MENU_CPP,
            r"void observeTextBufferAllocation\(.*?\) \{(.*?)\n\}",
            "observed text-buffer allocation",
        )
        if "slot.pixels.store(0" not in observed_alloc:
            raise ValueError(
                "allocator-side address reuse no longer invalidates live capacity"
            )
        if "trackedTextBufferCapacity(pixelsAddress)" not in MENU_CPP:
            raise ValueError(
                "text replay no longer consults the live allocation capacity"
            )

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
        # MSVC has none of the GCC/Clang __builtin_* intrinsics, and the Linux
        # cross-build does, so a raw builtin compiles here and fails only on the
        # Windows CI job after a full build. util.h carries the portable
        # wrappers; everything else must go through them.
        src_dir = pathlib.Path(__file__).resolve().parent.parent / "src"
        offenders = []
        for path in sorted(src_dir.rglob("*.cpp")) + sorted(src_dir.rglob("*.h")):
            if path.name == "util.h":
                continue
            for number, line in enumerate(path.read_text().splitlines(), 1):
                if re.search(r"\b__builtin_\w+", line):
                    offenders.append(f"{path.name}:{number}: {line.strip()}")
        if offenders:
            raise ValueError(
                "GCC/Clang builtins outside util.h will not compile with MSVC; "
                "use the portable wrapper instead:\n  " + "\n  ".join(offenders)
            )

    except (OSError, ValueError) as exc:
        return fail(str(exc))

    print(
        "core contract ok: feature matrix, installer fan-out, "
        "text-buffer lifetimes, synchronization hook surface, and compiler "
        "portability agree"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
