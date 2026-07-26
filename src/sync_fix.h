// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#include <d3d11.h>

#include "log.h"

namespace atfix {

void hookDevice(ID3D11Device* pDevice);
void hookContext(ID3D11DeviceContext* pContext);
bool applyResolutionOverride(DXGI_SWAP_CHAIN_DESC* pDesc);
bool arlandConfigBool(const char* section, const char* key, bool def);
void traceTransitionD3DFrame(uint64_t intervalMicros);
/* lives in sync_fix.cpp: reset the per-frame pre-UI SMAA latch (call at Present). */
void smaaResetFrame();
/* lives in sync_fix.cpp: ARLAND_PRESENT_TRACE diagnostic. notePresentBackbuffer
   captures the swap-chain backbuffer at Present; the probe then reports whether
   the game composites the frame directly into it (Scenario A) or into a separate
   texture copied in (Scenario B), which decides how supersampling downscales. */
bool presentTraceEnabled();

/* lives in sync_fix.cpp: record the identity of the surface the game composites
   the finished frame into -- the render-resolution texture under supersampling
   or borderless, the swap-chain backbuffer otherwise. Pre-UI SMAA locates the
   scene/UI boundary by this identity, because the mod resizes the engine's
   hard-coded auxiliary targets to the main render size and they are therefore
   indistinguishable from the scene target by dimensions. Call once per swap
   chain, after ssaaNoteSwapChain. */
void noteSceneAnchor(IDXGISwapChain* swapChain);

// Sample count of the MSAA twin attached to a resource, or 0 when it has none.
// Lets the supersampling trace report whether MSAA is stacked on the render
// target it downscales, which is otherwise invisible from that side.
unsigned int msaaTwinSamples(ID3D11Resource* host);

// The largest viewport any context has been given. Deferred contexts record the
// frame, so this is the only reliable way to ask what size the engine actually
// drew at; see the supersampling report.
void largestViewportSeen(unsigned int* width, unsigned int* height);
void notePresentBackbuffer(IDXGISwapChain* swapChain);

/* lives in main.cpp */
extern Log log;

/* lives in menu_fix.cpp: is a battle cinematic state (WaitAction/skill/result)
   currently active? Lets the D3D layer tag draws by cut-in vs overview. */
bool arlandInCinematicBattle();

/* lives in menu_fix.cpp: are the tactical-scene caster-clear hooks installed?
   When true, the mod front-runs the engine's late cut-in caster disable, so
   the D3D-layer dim/gate holds may engage immediately instead of waiting for
   the dim value to settle. */
bool arlandCutinCasterClearActive();

/* lives in menu_fix.cpp: current battle state name (null outside battle). */
const char* arlandBattleStateName();

/* lives in menu_fix.cpp: increments on every field/battle scene (re)build. */
uint32_t arlandSceneGeneration();

/* lives in menu_fix.cpp: §30m snode caster-flag restore, driven from the
   D3D-side 1024x1024 shadow-map clear (per-battle-frame render-thread hook). */
void arlandCutinShadowMapCleared();

}
