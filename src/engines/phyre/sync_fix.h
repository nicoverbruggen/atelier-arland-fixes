// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

// Shadow Map redirection activates only after all immediate-context hooks
// install. A failed Unmap hook must leave Map forwarding the original resource
// without creating shadow or constant-buffer capture state.
// Failure-injection tests cover create and enable failures, normal redirection,
// and shared hook targets. Per-target records retain both successful and failed
// installations so a second context cannot reinterpret an abandoned hook as
// success or invalidate another table's direct forwarding pointer.
//
// Shadow receiver views cache only confirmed absence of an enlarged twin.
// A failed view allocation remains retryable. A D3D11 regression injects one
// allocation failure and verifies that the next bind creates and reuses the
// twin view. While creation keeps failing, the receiver uses the original map;
// recovery does not make that temporary producer/consumer mismatch disappear.

#include <d3d11.h>

#include "../../core/log.h"

namespace atfix {

void hookDevice(ID3D11Device* pDevice);
void hookContext(ID3D11DeviceContext* pContext);
bool applyResolutionOverride(DXGI_SWAP_CHAIN_DESC* pDesc);
/* lives in sync_fix.cpp: true when applyResolutionOverride would record or
   rewrite anything, so the factory route knows it has a job of its own. */
bool resolutionOverrideNeeded();
void traceTransitionD3DFrame(uint64_t intervalMicros);
/* lives in sync_fix.cpp: reset the per-frame pre-UI SMAA latch (call at Present). */
void smaaResetFrame();
/* lives in sync_fix.cpp: ARLAND_PRESENT_TRACE diagnostic. notePresentBackbuffer
   captures the swap-chain backbuffer at Present; the probe then reports whether
   the game composites the frame directly into it (Scenario A) or into a separate
   texture copied in (Scenario B), which decides how supersampling downscales. */
bool presentTraceEnabled();

/* lives in sync_fix.cpp: record the identity of the surface the game composites
   the finished frame into -- the render-resolution texture under supersampling,
   the swap-chain backbuffer otherwise. Pre-UI SMAA locates the scene/UI
   boundary by this identity, because the mod resizes the engine's hard-coded
   auxiliary targets to the main render size and they are therefore
   indistinguishable from the scene target by dimensions. Call once per swap
   chain, after ssaaNoteSwapChain. */
void noteSceneAnchor(IDXGISwapChain* swapChain);

// The largest viewport any context has been given. Deferred contexts record the
// frame, so this is the only reliable way to ask what size the engine actually
// drew at; see the supersampling report.
void largestViewportSeen(unsigned int* width, unsigned int* height);
void notePresentBackbuffer(IDXGISwapChain* swapChain);

/* lives in main.cpp */
extern Log log;

/* lives in battle_shadow_restore.cpp: is a battle cinematic state (WaitAction/skill/result)
   currently active? Lets the D3D layer tag draws by cut-in vs overview. */
bool arlandInCinematicBattle();

/* lives in battle_shadow_restore.cpp: are the tactical-scene caster-clear hooks installed?
   When true, the mod front-runs the engine's late cut-in caster disable, so
   the D3D-layer dim/gate holds may engage immediately instead of waiting for
   the dim value to settle. */
bool arlandCutinCasterClearActive();

/* lives in battle_shadow_restore.cpp: increments on every field/battle scene (re)build. */
uint32_t arlandSceneGeneration();

/* lives in battle_shadow_restore.cpp: samples the battle state, driven from the
   D3D-side 1024x1024 shadow-map clear (per-battle-frame render-thread hook), so
   the cut-in holds decide on this frame's state and not the previous one. */
void arlandCutinShadowMapCleared();

/* ARLAND_ATLAS_RECONCILE: font-atlas writes as the D3D11 layer sees them,
   checked by the menu layer against the unlocks its invalidation observes. */
bool atlasReconcileEnabled();
uint64_t atlasWriteMapCount();

}
