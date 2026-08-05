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
// `components` is 4 for a float4 field and 3 for a float3. The alpha test is
// what makes this predicate selective, so dropping it for a float3 is a real
// loss of confidence and is only done where the field census says the offset
// carries nothing else. See kDimFieldsClassic for that reasoning.
bool dimHoldValueMatches(const float* v, uint32_t components) {
  const auto ad = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; };
  const float s = v[0];
  if (!(ad(v[0], v[1]) < 0.01f && ad(v[0], v[2]) < 0.01f &&
        s > 0.5f && s < 0.98f))
    return false;
  // A float3 field ends at v[2]; v[3] is whatever the compiler packed after it,
  // usually structure padding, so testing it would reject every real match.
  return components < 4 || ad(v[3], 1.0f) < 0.02f;
}

bool dimHoldValueMatches(const float* v) { return dimHoldValueMatches(v, 4); }

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
struct DimHoldField { uint32_t size; uint32_t offset; uint32_t components; };
// Rorona and Meruru: the 16-byte $Params the common pack uses for the scene
// fade, plus the arena-authored layouts. Battle arenas ship their own shaders,
// and until these were added the single 16-byte entry covered the ground only,
// leaving water, cloud and sky materials dark through a cut-in.
//
// Every entry here was checked for offset ambiguity across the whole asset
// tree (btlField, cg, chara, obj, effmodel, item_obj) before being added: each
// carries `diffuse` at one offset and one offset only. Two candidates were
// deliberately LEFT OUT, and adding them by size would be unsafe:
//
//   320  `diffuse` sits at 272 in some shaders and at 288 in others, within a
//        single asset group in every game. The rival field at 272 is `Kd`, the
//        Phong diffuse reflectance -- semantically a diffuse colour, so a grey
//        Kd is indistinguishable from the fade under any value predicate. 5
//        shaders in Rorona, 11 in Totori, 6 in Meruru.
//   112@80  `lightColor` in ~1900 shaders across the trilogy. Never patch it.
//
// (112, 96) IS included, as a float3. The complete layout census across cg,
// btlField, chara, obj, effmodel and item_obj found only three distinct
// 112-byte layouts in the whole trilogy:
//
//   ModelViewProj@0 lightPositionOS@64 lightColor@80 diffuse@96(12)
//       Rorona 782, Totori 459, Meruru 673
//   WvpXf@0 ... refRate@88 diffuse@96(16)          Rorona 2, Meruru 6
//   interval@0 ... ModelViewProj@32 HdrRange@96(4) Rorona 1, Meruru 3
//
// So offset 96 is `diffuse` in every layout but one, and Totori has only the
// first. The residual is `HdrRange`, a lone float whose following 8 bytes are
// undeclared: firing there needs three equal floats in (0.5, 0.98) spanning a
// field and its padding, and the effect if it ever did would be to move one
// HDR range from about 0.7 to 1.0 on one shader during a cut-in.
//
// 432 and 240 exist only in Meruru, 128 in both; an entry whose size never
// appears in the running game simply never matches, so one table serves both.
constexpr DimHoldField kDimFieldsClassic[] = {
  {16, 0, 4}, {112, 96, 3}, {128, 96, 4}, {240, 208, 4},
  {304, 272, 4}, {432, 336, 4},
};
// Totori's battle arenas render with the FIELDMAP shader family (runtime
// CUTIN_CB trace 2026-07-23: the dim flowed through (304,16) fieldmap fog,
// (48,32), (80,0), (32,0) — not the btl_field layouts the static analysis
// predicted for battle). (304,16)/(144,0)/(160,16) also FEED THE RECEPTION
// GATE (the fog VS computes 2.7-2*min(diffuse...), closing below 0.85), so on
// Totori the dim-hold doubles as the gate-hold and is settle-gated (see
// dimHoldPatch).
//
// (304,272) was excluded on the reading that it is a row of
// PSSGLightModelViewProjTex. That is true of ONE 304-byte layout and false of
// the other, and the exclusion was suppressing the fix for the second:
//
//   common pack, fieldmap_shadow_fog_vert  $Params  304: diffuse@16,
//       PSSGLightModelViewProjTex@240..303, so 272 really is a matrix row
//   arena packs, the vp/fp pairs           $Globals 304: WorldITXf@0,
//       WvpXf@64, WorldXf@128, ViewIXf@192, gTimer@256, diffuse@272
//
// Battle arenas ship their own shaders, and their materials -- water, cloud
// and sky domes among them -- are the ones that stayed dark through a cut-in
// while everything else brightened. A CUTIN_CB trace on 2026-08-05 logged
// `size=304 offset=272` and `size=416 offset=336` carrying the fade, so the
// arena buffers do receive it.
//
// Both layouts are 304 bytes, so size cannot tell them apart. What can is the
// content: in the common-pack layout the fade sits at 16 and 272 holds matrix
// data, and in the arena layout 16 is inside WorldITXf and the fade sits at
// 272. dimHoldPatch therefore refuses 272 on any buffer whose offset 16
// already looks like the fade, which leaves the matrix row untouched in the
// only layout where it exists.
// 320 is left out here for the same reason as in kDimFieldsClassic: `diffuse`
// sits at 272 in btlField and 288 in chara, with both offsets present inside
// obj alone, and the rival at 272 is `Kd`. (112, 96) is included as a float3;
// in Totori that offset is `diffuse` in 459 of 459 shaders, the only game
// where the census finds no other layout at all.
constexpr DimHoldField kDimFieldsTotori[] = {
  {16, 0, 4}, {32, 0, 4}, {48, 32, 4}, {80, 0, 4}, {96, 80, 4},
  {112, 96, 3}, {144, 0, 4}, {160, 16, 4}, {160, 144, 4}, {224, 208, 4},
  {304, 16, 4}, {304, 272, 4}, {416, 336, 4},
  {12960, 12944, 4}, {13024, 13008, 4},
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
  // Two 304-byte layouts exist and only their contents separate them: the
  // common-pack receiver carries the fade at 16 and matrix data at 272, the
  // arena materials the reverse. If 16 already looks like the fade this is the
  // former, and 272 must be left alone -- it is a PSSGLightModelViewProjTex
  // row there, and writing it would corrupt the shadow projection.
  const bool commonPack304 = size == 304 &&
    dimHoldValueMatches(reinterpret_cast<const float*>(
      static_cast<const uint8_t*>(data) + 16));
  for (size_t i = 0; i < count; i++) {
    if (fields[i].size != size)
      continue;
    if (commonPack304 && fields[i].offset == 272)
      continue;
    float* v = reinterpret_cast<float*>(
      static_cast<uint8_t*>(data) + fields[i].offset);
    if (!dimHoldValueMatches(v, fields[i].components))
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

// ---- ARLAND_CUTIN_CB_TRACE -------------------------------------------------
// Discovery diagnostic: during cinematic battle states, scan every constant
// buffer written on any path for the cut-in fade and log each unique
// (path, size, offset) once with the value it carried.
//
// This is a restoration of the probe that produced kDimFieldsTotori on
// 2026-07-23 and was removed once it had answered that question. Three of its
// limits are lifted, because each is on its own enough to explain why the
// table it produced misses the arena-authored materials that leave water and
// clouds dark:
//
//   - it stopped at 16384 bytes, so the two ~13000-byte skinning layouts were
//     scanned only in part and anything past that not at all;
//   - it stopped after 64 unique records, which a scene with arena shaders
//     can exhaust before it reaches them;
//   - it logged only exact matches, so a float3 `diffuse` (the arena phong
//     materials put one at offset 96 of a 112-byte buffer) was invisible: the
//     four bytes after it are structure padding, so the w-component test fails
//     and the record was silently dropped.
//
// The near-miss logging is the important addition. A location that matches on
// xyz but not on w is exactly the shape of a float3 field, and telling those
// apart from noise is what the value predicate cannot do on its own.
//
// 16-byte alignment is kept and is not a limit: HLSL constant-buffer packing
// never lets a float4 straddle a 16-byte boundary, so a scan at that stride
// cannot miss one.
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
  const auto ad = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; };
  for (uint32_t off = 0; off + 16 <= size; off += 16) {
    float v[4];
    std::memcpy(v, bytes + off, sizeof(v));
    const float s = v[0];
    if (!(s > 0.5f && s < 0.98f) ||
        ad(v[0], v[1]) >= 0.01f || ad(v[0], v[2]) >= 0.01f)
      continue;
    // Exact when w is ~1 (a float4 field), near-miss otherwise (very likely a
    // float3 field whose w is the padding that follows it).
    const bool exact = ad(v[3], 1.0f) < 0.02f;
    const uint64_t key = (uint64_t(uintptr_t(path) & 0xffffu) << 48) |
      (uint64_t(size) << 20) | off;
    {
      std::lock_guard lock(traceMutex);
      if (seen.size() >= 4096 || !seen.insert(key).second)
        continue;
    }
    log("CUTIN_CB ", exact ? "match" : "near ", " path=", path,
        " size=", std::dec, size, " offset=", off,
        " v=(", v[0], ",", v[1], ",", v[2], ",", v[3], ")");
  }
}

// Which shader is bound when the receiver's constant buffer is in play.
// Totori ships two 304-byte fieldmap receivers whose reflection is identical,
// `fieldmap_shadow_fog_vert` and its HDR variant, and they differ only in the
// gate constant (2.7 against 2.5) and in whether `diffuse` also multiplies the
// vertex colour. Nothing static separates them, so this records the DXBC
// digest of the vertex shader bound at a draw that has a 304-byte constant
// buffer, and the digest is matched against the shader pack offline. Logging a
// digest rather than trying to name the shader in-process keeps this to a
// hash comparison and puts the identification where the assets are.
static const GUID IID_CutinShaderDigest =
  {0x9a3c17e6,0x5b62,0x4f0a,{0xb1,0x77,0x2e,0x64,0x9d,0x3f,0x8c,0x21}};

void cutinNoteShaderBytecode(ID3D11DeviceChild* shader,
                             const void* bytecode, SIZE_T length) {
  if (!cutinCbTraceEnabled() || !shader || !bytecode || length < 20)
    return;
  // DXBC lays its 16-byte digest at offset 4. The first eight bytes are plenty
  // to tell two shaders apart and keep the log line short.
  uint64_t digest = 0;
  std::memcpy(&digest, static_cast<const uint8_t*>(bytecode) + 4, sizeof(digest));
  shader->SetPrivateData(IID_CutinShaderDigest, sizeof(digest), &digest);
}

void cutinTraceBoundShader(ID3D11DeviceContext* context) {
  if (!cutinCbTraceEnabled() || !arlandInCinematicBattle())
    return;
  ID3D11Buffer* cb = nullptr;
  context->VSGetConstantBuffers(0, 1, &cb);
  if (!cb)
    return;
  D3D11_BUFFER_DESC bd = { };
  cb->GetDesc(&bd);
  cb->Release();
  if (bd.ByteWidth != 304)
    return;
  ID3D11VertexShader* vs = nullptr;
  context->VSGetShader(&vs, nullptr, nullptr);
  if (!vs)
    return;
  uint64_t digest = 0;
  UINT digestSize = sizeof(digest);
  if (SUCCEEDED(vs->GetPrivateData(IID_CutinShaderDigest, &digestSize, &digest)) &&
      digestSize == sizeof(digest)) {
    static mutex digestMutex;
    static std::set<uint64_t> seenDigests;
    bool fresh = false;
    {
      std::lock_guard lock(digestMutex);
      fresh = seenDigests.size() < 64 && seenDigests.insert(digest).second;
    }
    if (fresh)
      log("CUTIN_VS 304-byte cb bound, vs_digest=0x", std::hex, digest,
          std::dec);
  }
  vs->Release();
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
