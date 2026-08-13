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
SYNC_UPLOAD_POLICY = (ROOT / "src" / "sync_upload_policy.h").read_text()
SHARPEN_CPP = (ROOT / "src" / "sharpen.cpp").read_text()
SMAA_CPP = (ROOT / "src" / "smaa.cpp").read_text()
SSAA_CPP = (ROOT / "src" / "supersample.cpp").read_text()


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

        # Factory interception and Present interception are separate decisions.
        # On the D3D11CreateDevice route the factory hook is the only place the
        # display-resolution policy can be applied, and that policy belongs to
        # no Present consumer, so gating the factory on presentHookNeeded()
        # loses the override -- including the desktop-mode default a blank ini
        # gets -- while the device hooks still resize the engine's targets.
        factory_installer = block(
            MAIN_CPP,
            r"void hookFactoryForSwapChain\(ID3D11Device\* device\) \{(.*?)\n\}",
            "factory hook installer",
        )
        if "factoryHookNeeded()" not in factory_installer:
            raise ValueError(
                "the factory hook no longer has its own installation predicate"
            )
        factory_predicate = block(
            MAIN_CPP,
            r"bool factoryHookNeeded\(\) \{(.*?)\n\}",
            "factory hook predicate",
        )
        if "resolutionOverrideNeeded()" not in factory_predicate:
            raise ValueError(
                "factoryHookNeeded no longer covers the resolution policy"
            )
        swap_chain_installer = block(
            MAIN_CPP,
            r"void hookSwapChain\(IDXGISwapChain\* swapChain\) \{(.*?)\n\}",
            "Present hook installer",
        )
        if "presentHookNeeded()" not in swap_chain_installer:
            raise ValueError(
                "the Present hook no longer gates on its consumers"
            )
        # Sharpening is placed at the pre-UI boundary and is a separate setting
        # from edge smoothing, so the boundary must be detected for either one.
        # It also needs the Present hook, not for a pass but for the shader-
        # compiler preload that cannot run from a draw detour. Gate either on
        # SMAA alone and the Sharpen slider silently does nothing.
        present_predicate = block(
            MAIN_CPP,
            r"bool presentHookNeeded\(\) \{(.*?)\n\}",
            "Present hook predicate",
        )
        if "sharpenEnabled()" not in present_predicate:
            raise ValueError(
                "presentHookNeeded no longer covers the sharpening preload"
            )
        pre_ui_predicate = block(
            SYNC_CPP,
            r"bool preUiBoundaryNeeded\(\) \{(.*?)\n\}",
            "pre-UI boundary predicate",
        )
        for accessor in ("smaaEnabled()", "sharpenEnabled()"):
            if accessor not in pre_ui_predicate:
                raise ValueError(
                    "preUiBoundaryNeeded no longer covers both of its passes: "
                    + accessor
                )
        # Every gate on that boundary has to ask the shared question. A stray
        # smaaEnabled() here is the defect coming back one site at a time.
        for gate in (
            r"bool smaaSceneRtBoundaryEnabled\(\) \{(.*?)\n\}",
            r"void trackSmaaRenderTargets\(\n?.*?\) \{(.*?)\n\}",
            r"void smaaDrawBoundary\(ID3D11DeviceContext\* context\) \{(.*?)\n\}",
            r"void smaaSceneBoundary\(.*?\) \{(.*?)\n\}",
        ):
            body = block(SYNC_CPP, gate, "pre-UI boundary gate")
            if "preUiBoundaryNeeded()" not in body:
                raise ValueError(
                    "a pre-UI boundary gate no longer asks preUiBoundaryNeeded()"
                )
        # The OMSetDepthStencilState hook is the only writer of depthDisabled,
        # which both boundaries read. Installing it on a narrower condition than
        # the boundary itself leaves the flag false for the whole session: the
        # depth-state boundary never fires at all, and the scene-target one loses
        # its first-UI-draw trigger and falls back to bind-away, which can land
        # after the UI has drawn.
        context_hooks = block(
            SYNC_CPP,
            r"void hookContext\(ID3D11DeviceContext\* pContext\) \{(.*?)\n\}",
            "context hook installer",
        )
        depth_gate = re.search(
            r"if \(([^\n]*)\)\n\s*HOOK_PROC\([^;]*OMSetDepthStencilState\);",
            context_hooks,
        )
        if not depth_gate:
            raise ValueError(
                "OMSetDepthStencilState is no longer installed behind a gate"
            )
        if "preUiBoundaryNeeded()" not in depth_gate.group(1):
            raise ValueError(
                "the depth-state writer is gated more narrowly than the pre-UI "
                "boundary that reads it: " + depth_gate.group(1).strip()
            )

        # The predicate has to answer for both halves of applyResolutionOverride:
        # the recorded original swap-chain size and the rewritten chain.
        resolution_predicate = block(
            SYNC_CPP,
            r"bool resolutionOverrideNeeded\(\) \{(.*?)\n\}",
            "resolution override predicate",
        )
        for accessor in ("displayResolution(", "renderResolution("):
            if accessor not in resolution_predicate:
                raise ValueError(
                    "resolutionOverrideNeeded no longer mirrors "
                    "applyResolutionOverride: " + accessor
                )

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

        # Dirty-shadow failures must remain retryable, and patched constant-
        # buffer bytes must be identical in the game resource and its staging
        # mirror. The policy has a failure-injection harness; these checks pin
        # the production path to that tested policy.
        required_upload_wiring = (
            '#include "sync_upload_policy.h"',
            "updateMirroredSubresource<ID3D11Resource>(\n"
            "    pResource, effectiveData,",
            "const ShadowUploadResult result = uploadDirtyShadow(upload);",
            "markShadowDirty(resource, subresource);",
        )
        for fragment in required_upload_wiring:
            if fragment not in SYNC_CPP:
                raise ValueError(
                    "sync upload path no longer uses its tested policy: "
                    + fragment
                )
        source_at = SYNC_UPLOAD_POLICY.find("operations.mapSource()")
        destination_at = SYNC_UPLOAD_POLICY.find("operations.mapDestination()")
        if source_at < 0 or destination_at < 0 or source_at > destination_at:
            raise ValueError(
                "dirty-shadow upload no longer maps the readable source first"
            )
        if "submit(shadow, effectiveData);" not in SYNC_UPLOAD_POLICY:
            raise ValueError(
                "staging mirror no longer receives the effective base payload"
            )

        # Fullscreen passes run inside the game's frame. Preserve every render
        # target sharpening can displace, and never draw any pass after a
        # failed WRITE_DISCARD map (which would reuse stale constants).
        sharpen_state = (
            "OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs",
            "D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv",
            "for (auto*& rtv : rtvs) release(rtv)",
        )
        for fragment in sharpen_state:
            if fragment not in SHARPEN_CPP:
                raise ValueError(
                    "sharpening no longer preserves every render target: "
                    + fragment
                )
        for label, source in (
            ("sharpening", SHARPEN_CPP),
            ("SMAA", SMAA_CPP),
            ("supersampling", SSAA_CPP),
        ):
            if "if (FAILED(mapResult))" not in source:
                raise ValueError(
                    f"{label} no longer skips the pass after a failed map"
                )

        # Swap-chain setup is allowed to see an unsuitable or transiently
        # failing chain first. Completion must be published only on success.
        if "noted.exchange(true)" in SSAA_CPP:
            raise ValueError("supersampling setup is one-shot before success")
        active_at = SSAA_CPP.find("g_active.store(true")
        initialized_at = SSAA_CPP.find("initialized = true;", active_at)
        if active_at < 0 or initialized_at < active_at:
            raise ValueError(
                "supersampling setup completion is not published after success"
            )

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
        "text-buffer lifetimes, synchronization hooks/uploads, render-pass "
        "safety, retryable SSAA setup, and compiler portability agree"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
