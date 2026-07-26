// SPDX-License-Identifier: MIT
//
// Battle cut-in dim/gate patches and shadow-SRV classification, split out of
// sync_fix.cpp.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>
#include <utility>

#include "sync_fix.h"
#include "sync_internal.h"
#include "game.h"
#include "config.h"
#include "d3d11_procs.h"

namespace atfix {

// ---- battle cut-in dim/gate patches + shadow-SRV classifier (from sync_fix.cpp) ----
mutex g_shadowTraceMutex;
std::set<uintptr_t> g_shadowSrvs;      // PS SRVs backed by the 1024x1024 fmt-44 map
std::set<uintptr_t> g_nonShadowSrvs;   // classified as not the shadow map

// Reopen the receiver's shadow-reception gate during cut-ins. Per-game and
// override handling (env ARLAND_CUTIN_SHADOWS, ini [Battle] BattleCutInShadows)
// live in the capability matrix (game.cpp); all three games are OptIn while
// the cut-in character-juggling glitch is investigated (Totori's battle-state
// tracking landed 2026-07-23, English build). Cached because it is read in hot
// draw paths.
bool cutinGateHoldEnabled() {
  static const bool enabled = featureEnabled(Feature::CutInShadows);
  return enabled;
}

// Hold the scene light up during cut-ins so the scene keeps full brightness.
// The user-facing key BattleCutInDimming (env ARLAND_CUTIN_DIMMING) is worded as
// "may the cut-in dim the scene?", the inverse of this dim-hold action; the
// matrix descriptor carries that inversion. Rorona/Meruru default to holding
// (bright); Totori is Unsupported for now.
bool cutinDimHoldEnabled() {
  static const bool enabled = featureEnabled(Feature::CutInDimHold);
  return enabled;
}

// Either cut-in mechanism active — arms the shared constant-buffer capture path.
bool cutinShadowsEnabled() {
  return cutinGateHoldEnabled() || cutinDimHoldEnabled();
}

// Shared dim-hold value predicate: a float4 (s,s,s,~1) with s in (0.5,0.98) —
// the faded cut-in value. Match and write are split so callers can interpose
// settle gating between them.
bool dimHoldValueMatches(const float* v) {
  const float s = v[0];
  const float near1 = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; }(v[3], 1.0f);
  const float uni01 = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; }(v[0], v[1]);
  const float uni02 = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; }(v[0], v[2]);
  return near1 < 0.02f && uni01 < 0.01f && uni02 < 0.01f &&
    s > 0.5f && s < 0.98f;
}

bool dimHoldValuePatch(float* v) {
  if (!dimHoldValueMatches(v))
    return false;
  v[0] = v[1] = v[2] = 1.0f;
  return true;
}

// The cut-in dim `diffuse` field per constant-buffer layout. Rorona and Meruru
// ship the PS3-style shader pack where the scene-light fade is the 16-byte
// $Params. Totori's shader set was rewritten for D3D11 (static RE 2026-07-23:
// commonShaderWin.PSSG) and carries the same (0.7,0.7,0.7,1.0) BtlField fade
// through different layouts: battle ground btl_field_shadow_frag (32, diffuse
// @0), the chara_*_frag PS family (16, @0), and the toon character VS families
// (224,@208) (13024,@13008) (12960,@12944) (160,@144) (96,@80).
struct DimHoldField { uint32_t size; uint32_t offset; };
constexpr DimHoldField kDimFieldsClassic[] = { {16, 0} };
// Totori's battle arenas render with the FIELDMAP shader family (runtime
// CUTIN_CB trace 2026-07-23: the dim flowed through (304,16) fieldmap fog,
// (48,32), (80,0), (32,0) — not the btl_field layouts the static analysis
// predicted for battle). (304,16)/(144,0)/(160,16) also FEED THE RECEPTION
// GATE (the fog VS computes 2.7-2*min(diffuse...), closing below 0.85), so on
// Totori the dim-hold doubles as the gate-hold and is settle-gated (see
// dimHoldPatch). The trace also matched (304,272) — inside
// PSSGLightModelViewProjTex; a light-matrix row, never patch it.
constexpr DimHoldField kDimFieldsTotori[] = {
  {16, 0}, {32, 0}, {48, 32}, {80, 0}, {96, 80},
  {144, 0}, {160, 16}, {160, 144}, {224, 208},
  {304, 16}, {12960, 12944}, {13024, 13008},
};

// The dim-field table for the running game, cached.
std::pair<const DimHoldField*, size_t> dimHoldFields() {
  static const std::pair<const DimHoldField*, size_t> fields =
    currentTitle() == Title::Totori
      ? std::make_pair(kDimFieldsTotori, std::size(kDimFieldsTotori))
      : std::make_pair(kDimFieldsClassic, std::size(kDimFieldsClassic));
  return fields;
}

bool dimHoldEligibleSize(uint32_t size) {
  const auto [fields, count] = dimHoldFields();
  for (size_t i = 0; i < count; i++)
    if (fields[i].size == size)
      return true;
  return false;
}

float gateHoldSettleRamp(float s);   // defined below with gateHoldPatch

// Rewrite a matching light-intensity `diffuse` to full brightness in place.
// Returns true if it patched. On Totori the dim field doubles as the fieldmap
// reception gate, so the hold is settle-gated there (same stray-shadow cover
// logic as gateHoldPatch); Rorona/Meruru keep the instant hold — their gate
// lives in the separate 880 receiver and carries its own settle gating.
bool dimHoldPatch(void* data, uint32_t size) {
  const auto [fields, count] = dimHoldFields();
  static const bool settleGated = currentTitle() == Title::Totori;
  bool patched = false;
  for (size_t i = 0; i < count; i++) {
    if (fields[i].size != size)
      continue;
    float* v = reinterpret_cast<float*>(
      static_cast<uint8_t*>(data) + fields[i].offset);
    if (!dimHoldValueMatches(v))
      continue;
    float target = 1.0f;
    if (settleGated) {
      const float t = gateHoldSettleRamp(v[0]);
      if (t <= 0.0f)
        continue;
      target = v[0] + (1.0f - v[0]) * t;
    }
    v[0] = v[1] = v[2] = target;
    patched = true;
  }
  return patched;
}

// ARLAND_CUTIN_CB_TRACE: during cinematic battle states, scan constant-buffer
// payloads on every write path for the dim value pattern ((s,s,s,~1),
// s in (0.5,0.98)) at any 16-aligned offset, and log each unique
// (path, size, offset) once. Discovery diagnostic for games whose dim-carrying
// layouts are not yet in the dim-hold table (used to pin Totori's real
// layouts when the statically derived table missed).
bool cutinCbTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_CUTIN_CB_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

void cutinCbTraceScan(const char* path, const void* data, uint32_t size) {
  if (!cutinCbTraceEnabled() || !data || size < 16 ||
      !arlandInCinematicBattle())
    return;
  static mutex traceMutex;
  static std::set<uint64_t> seen;
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  const uint32_t limit = size < 16384u ? size : 16384u;
  for (uint32_t off = 0; off + 16 <= limit; off += 16) {
    float v[4];
    std::memcpy(v, bytes + off, sizeof(v));
    const float s = v[0];
    auto ad = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; };
    if (ad(v[3], 1.0f) < 0.02f && ad(v[0], v[1]) < 0.01f &&
        ad(v[0], v[2]) < 0.01f && s > 0.5f && s < 0.98f) {
      const uint64_t key = (uint64_t(uintptr_t(path) & 0xffffu) << 48) |
        (uint64_t(size) << 20) | off;
      std::lock_guard lock(traceMutex);
      if (seen.size() < 64 && seen.insert(key).second)
        log("CUTIN_CB path=", path, " size=", size, " offset=", off,
          " v=(", v[0], ",", v[1], ",", v[2], ",", v[3], ")");
    }
  }
}

// Transition-aware settle detector for the reception gate (stray-shadow fix,
// static RE 2026-07-23). The vanilla cut-in fades the scene light through the
// receiver's 0.75 reception threshold during exactly the windows in which the
// engine juggles the non-focus battlers: their per-node caster flags
// (PNode+0xC2, PNode::setCastShadow) are cleared only ~0.25 s AFTER the hide
// starts (deferred with the visual cross-fade) and restored INSTANTLY at
// cut-in exit, before positions finish restoring. The closed gate is the
// designed cover for those stale-caster frames; holding it open during the
// fade is what exposed stray floor shadows for hidden/"sky"-parked
// characters. Fix: only hold the gate once the observed dim value has been
// bit-identical for >= 100 ms — entry-side that is after the fade bottoms out
// (casters already cleared by then), and exit-side the value starts moving on
// the first frame, releasing the hold so the vanilla covered window returns.
// Returns the hold strength for the observed dim value s: 0 while the value
// is still animating or freshly settled (< 60 ms stable), then easing 0→1
// over the following 120 ms so the brightness/reception hold fades in instead
// of popping (visible as a hard brightness step in capture analysis).
float gateHoldSettleRamp(float s) {
  // When the tactical caster-clear hooks are active, the mod front-runs the
  // engine's late cut-in caster disable — there are no stale casters for a
  // held-open gate to expose, so the hold engages immediately and the visible
  // dim ride-down disappears entirely. Settle+ramp remains the fallback for
  // builds where those hooks did not install.
  if (arlandCutinCasterClearActive())
    return 1.0f;
  static std::atomic<uint32_t> observedBits{0};
  static std::atomic<uint64_t> stableSinceMs{0};
  constexpr uint64_t kSettleMs = 60;
  constexpr uint64_t kRampMs = 120;
  uint32_t bits = 0;
  std::memcpy(&bits, &s, sizeof(bits));
  const uint64_t now = GetTickCount64();
  if (observedBits.exchange(bits, std::memory_order_acq_rel) != bits) {
    stableSinceMs.store(now, std::memory_order_release);
    return 0.0f;
  }
  const uint64_t stable =
    now - stableSinceMs.load(std::memory_order_acquire);
  if (stable < kSettleMs)
    return 0.0f;
  const uint64_t ramp = stable - kSettleMs;
  return ramp >= kRampMs ? 1.0f : float(ramp) / float(kRampMs);
}

// Open the shadow-reception gate in an 880 receiver material: force the faded
// diffuse at byte 832 (float4 (s,s,s,~1), s in (0.5,0.98)) back to 1.0 so
// min(diffuse.w,diffuse.x) > 0.75. Returns true if patched.
bool gateHoldPatch(void* data, uint32_t size) {
  if (size != 880)
    return false;
  float* v = reinterpret_cast<float*>(static_cast<uint8_t*>(data) + 832);
  auto ad = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; };
  const float s = v[0];
  if (ad(v[0], v[1]) < 0.01f && ad(v[0], v[2]) < 0.01f &&
      s > 0.5f && s < 0.98f) {
    const float t = gateHoldSettleRamp(s);
    if (t <= 0.0f)
      return false;
    const float target = s + (1.0f - s) * t;
    v[0] = v[1] = v[2] = target;
    if (v[3] < 0.76f) v[3] = target;  // min(.w,.x) clears the 0.75 gate as t rises
    return true;
  }
  return false;
}

// With an enlarged shadow map, keep the receiver's soft-PCF edge one texel
// wide: rescale the 880 receiver material's tapScale (float4 @816, components
// ~±1/1024 = one 1024-map texel in UV) to ~±1/<new size>. Value-conditional
// like dimHoldPatch/gateHoldPatch: only components whose magnitude looks like
// the vanilla one-texel offset are touched, so unrelated 880-byte buffers (and
// already-rescaled payloads) are left alone. Returns true if it patched.
bool tapScalePatch(void* data, uint32_t size) {
  const UINT res = shadowMapResolution();
  if (size != 880 || res <= 1024)
    return false;
  float* v = reinterpret_cast<float*>(static_cast<uint8_t*>(data) + 816);
  const float ratio = 1024.0f / static_cast<float>(res);
  bool patched = false;
  for (int i = 0; i < 4; i++) {
    const float mag = v[i] < 0.0f ? -v[i] : v[i];
    if (mag > 0.8f / 1024.0f && mag < 1.25f / 1024.0f) {
      v[i] *= ratio;
      patched = true;
    }
  }
  return patched;
}

// Classify a PS shader-resource view as the shadow map or not, caching the
// verdict by pointer so the desc query only happens once per view. Caller holds
// g_shadowTraceMutex.
bool isShadowSrvLocked(ID3D11ShaderResourceView* srv) {
  // Invalidate the verdict caches on every scene rebuild. A rebuild can recycle a
  // destroyed SRV's address to a brand-new view, so a verdict keyed by the raw
  // pointer would misclassify the newcomer. This mirrors the twin-SRV negative-
  // cache clear on each new shadow-map generation; without it, a stale "shadow"
  // verdict on a recycled pointer could make gateHoldAtDraw write its 16 gate
  // bytes into an unrelated 880-byte VS cb0. Caller holds g_shadowTraceMutex, so
  // the static generation is accessed under the lock.
  static uint32_t cachedGeneration = 0;
  const uint32_t generation = arlandSceneGeneration();
  if (generation != cachedGeneration) {
    cachedGeneration = generation;
    g_shadowSrvs.clear();
    g_nonShadowSrvs.clear();
  }
  const uintptr_t key = reinterpret_cast<uintptr_t>(srv);
  if (g_shadowSrvs.count(key))
    return true;
  if (g_nonShadowSrvs.count(key))
    return false;
  bool shadow = false;
  ID3D11Resource* resource = nullptr;
  srv->GetResource(&resource);
  if (resource) {
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      // The shadow SRV bound at draw time is either the engine's own 1024
      // map (vanilla / redirect not engaged) or our tagged enlarged twin
      // (redirect engaged) — accept both; nothing else qualifies.
      shadow = desc.Format == DXGI_FORMAT_R24G8_TYPELESS &&
        ((desc.Width == 1024 && desc.Height == 1024) ||
         isShadowResResized(resource));
      texture->Release();
    }
    resource->Release();
  }
  (shadow ? g_shadowSrvs : g_nonShadowSrvs).insert(key);
  return shadow;
}

// Draw-time gate-hold — the load-bearing piece of the cut-in shadow fix,
// independent of the (engine-internal) write path. At every draw during a
// cinematic battle state whose VS cb0 is an 880-byte receiver material with
// the shadow SRV bound, record a 16-byte BOX UpdateSubresource over bytes
// [832,848) forcing diffuse=(1,1,1,1) right before the draw. This works even
// if the buffer was written pre-cinematic and re-bound stale, or written
// through a path none of our hooks cover. Partial constant-buffer updates are
// legal on the 11.1 runtime semantics DXVK implements, and we only touch the
// 16 gate bytes, so stale-snapshot matrix corruption is impossible.
void gateHoldAtDraw(ID3D11DeviceContext* context) {
  if (!cutinGateHoldEnabled() || !arlandInCinematicBattle())
    return;
  ID3D11Buffer* cb = nullptr;
  context->VSGetConstantBuffers(0, 1, &cb);
  if (!cb)
    return;
  D3D11_BUFFER_DESC bd = {};
  cb->GetDesc(&bd);
  if (bd.ByteWidth != 880) {
    cb->Release();
    return;
  }
  bool samplesShadow = false;
  ID3D11ShaderResourceView* srvs[16] = {};
  context->PSGetShaderResources(0, 16, srvs);
  for (ID3D11ShaderResourceView* srv : srvs)
    if (srv && !samplesShadow) {
      std::lock_guard lock(g_shadowTraceMutex);
      if (isShadowSrvLocked(srv))
        samplesShadow = true;
    }
  for (ID3D11ShaderResourceView* srv : srvs)
    if (srv)
      srv->Release();
  if (!samplesShadow) {
    cb->Release();
    return;
  }
  // Force the gate open for this draw: partial 16-byte update of the bound
  // DEFAULT buffer, recorded in-order before the draw on THIS context (legal
  // on deferred contexts too).
  static const float kOpen[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  D3D11_BOX box = {};
  box.left = 832;
  box.right = 848;
  box.top = 0;
  box.bottom = 1;
  box.front = 0;
  box.back = 1;
  getContextProcs(context)->UpdateSubresource(context, cb, 0, &box, kOpen,
                                              16, 16);
  cb->Release();
}

}  // namespace atfix
