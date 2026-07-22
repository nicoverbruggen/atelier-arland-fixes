// Derived from Philip Rebohle's atelier-sync-fix and subsequent Map/Unmap
// work by TellowKrinkle; substantially altered for Arland. See LICENSE.
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "sync_fix.h"
#include "util.h"
#include "game.h"
#include "config.h"

namespace atfix {

// ============================================================================
// sync_fix.cpp — the D3D11 proxy layer. One translation unit because the pieces
// below share per-frame state (the constant-buffer snapshot cache, the
// shadow-SRV classifier, the immediate-context pointer and the DeviceProcs/
// ContextProcs vtable dispatch), so they are not separable into their own files
// without leaking those internals across headers. Sibling modules that ARE
// self-contained live elsewhere: config.cpp (arland-fix.ini), game.cpp (per-game
// capability matrix), framerate.cpp (cutscene present clamp). Sections, in order:
//
//   1. Vtable proc typedefs + DeviceProcs/ContextProcs dispatch tables.
//   2. Shadow-map "twin" plumbing — separate mod-owned high-res shadow maps for
//      ShadowMultiplier, redirected onto without touching the engine's own maps.
//   3. Cut-in shadow fix — dim-hold / gate-hold patches (dimHoldPatch,
//      gateHoldPatch), the tapScale rescale, and the draw-time gate-hold.
//   4. Constant-buffer snapshot cache (CbSnap / captureCbMap / captureCbUnmap /
//      readCb0Snap) feeding the cut-in patches and the contact-blob overlay.
//   5. Contact-blob cut-in overlay (§35) — self-contained shaders/state.
//   6. Shadow-SRV classifier (isShadowSrvLocked) shared by 2/3/5.
//   7. Resolution override + MSAA resolve + viewport/scissor correction.
//   8. The D3D11 device/context hook implementations that call into 1–7.
//   9. hookDevice / hookContext installation.
// ============================================================================

/** Hooking-related stuff */
using PFN_ID3D11Device_CreateBuffer = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using PFN_ID3D11Device_CreateDeferredContext = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  UINT, ID3D11DeviceContext**);
using PFN_ID3D11Device_CreateTexture1D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**);
using PFN_ID3D11Device_CreateTexture2D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_ID3D11Device_CreateTexture3D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**);
using PFN_ID3D11Device_CreateVertexShader = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using PFN_ID3D11Device_CreatePixelShader = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

using PFN_ID3D11DeviceContext_ClearRenderTargetView = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11RenderTargetView*, const FLOAT[4]);
using PFN_ID3D11DeviceContext_ClearDepthStencilView = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using PFN_ID3D11DeviceContext_ClearUnorderedAccessViewFloat = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11UnorderedAccessView*, const FLOAT[4]);
using PFN_ID3D11DeviceContext_ClearUnorderedAccessViewUint = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11UnorderedAccessView*, const UINT[4]);
using PFN_ID3D11DeviceContext_CopyResource = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, ID3D11Resource*);
using PFN_ID3D11DeviceContext_CopySubresourceRegion = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using PFN_ID3D11DeviceContext_CopyStructureCount = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Buffer*, UINT, ID3D11UnorderedAccessView*);
using PFN_ID3D11DeviceContext_Dispatch = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, UINT, UINT);
using PFN_ID3D11DeviceContext_DispatchIndirect = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Buffer*, UINT);
using PFN_ID3D11DeviceContext_OMSetRenderTargets = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PFN_ID3D11DeviceContext_UpdateSubresource = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using PFN_ID3D11DeviceContext_Map = HRESULT (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_ID3D11DeviceContext_Unmap = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT);
using PFN_ID3D11DeviceContext_RSSetViewports = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, const D3D11_VIEWPORT*);
using PFN_ID3D11DeviceContext_RSSetScissorRects = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, const D3D11_RECT*);
using PFN_ID3D11DeviceContext_Draw = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT);
using PFN_ID3D11DeviceContext_DrawIndexed = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_ID3D11DeviceContext_DrawInstanced = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_ID3D11DeviceContext_DrawIndexedInstanced = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_ID3D11DeviceContext_DrawAuto = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*);
using PFN_ID3D11DeviceContext_DrawInstancedIndirect = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_ID3D11DeviceContext_DrawIndexedInstancedIndirect = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_ID3D11DeviceContext_ExecuteCommandList = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN_ID3D11DeviceContext_FinishCommandList = HRESULT (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using PFN_ID3D11DeviceContext_PSSetShaderResources = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);

struct DeviceProcs {
  PFN_ID3D11Device_CreateBuffer                         CreateBuffer                  = nullptr;
  PFN_ID3D11Device_CreateDeferredContext                CreateDeferredContext         = nullptr;
  PFN_ID3D11Device_CreateTexture1D                      CreateTexture1D               = nullptr;
  PFN_ID3D11Device_CreateTexture2D                      CreateTexture2D               = nullptr;
  PFN_ID3D11Device_CreateTexture3D                      CreateTexture3D               = nullptr;
  PFN_ID3D11Device_CreateVertexShader                   CreateVertexShader            = nullptr;
  PFN_ID3D11Device_CreatePixelShader                    CreatePixelShader             = nullptr;
};

struct ContextProcs {
  PFN_ID3D11DeviceContext_ClearRenderTargetView         ClearRenderTargetView         = nullptr;
  PFN_ID3D11DeviceContext_ClearDepthStencilView         ClearDepthStencilView         = nullptr;
  PFN_ID3D11DeviceContext_ClearUnorderedAccessViewFloat ClearUnorderedAccessViewFloat = nullptr;
  PFN_ID3D11DeviceContext_ClearUnorderedAccessViewUint  ClearUnorderedAccessViewUint  = nullptr;
  PFN_ID3D11DeviceContext_CopyResource                  CopyResource                  = nullptr;
  PFN_ID3D11DeviceContext_CopySubresourceRegion         CopySubresourceRegion         = nullptr;
  PFN_ID3D11DeviceContext_CopyStructureCount            CopyStructureCount            = nullptr;
  PFN_ID3D11DeviceContext_Dispatch                      Dispatch                      = nullptr;
  PFN_ID3D11DeviceContext_DispatchIndirect              DispatchIndirect              = nullptr;
  PFN_ID3D11DeviceContext_OMSetRenderTargets            OMSetRenderTargets            = nullptr;
  PFN_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews OMSetRenderTargetsAndUnorderedAccessViews = nullptr;
  PFN_ID3D11DeviceContext_UpdateSubresource             UpdateSubresource             = nullptr;
  PFN_ID3D11DeviceContext_Map                           Map                           = nullptr;
  PFN_ID3D11DeviceContext_Unmap                         Unmap                         = nullptr;
  PFN_ID3D11DeviceContext_RSSetViewports                 RSSetViewports                = nullptr;
  PFN_ID3D11DeviceContext_RSSetScissorRects              RSSetScissorRects             = nullptr;
  PFN_ID3D11DeviceContext_Draw                          Draw                          = nullptr;
  PFN_ID3D11DeviceContext_DrawIndexed                   DrawIndexed                   = nullptr;
  PFN_ID3D11DeviceContext_DrawInstanced                 DrawInstanced                 = nullptr;
  PFN_ID3D11DeviceContext_DrawIndexedInstanced          DrawIndexedInstanced          = nullptr;
  PFN_ID3D11DeviceContext_DrawAuto                      DrawAuto                      = nullptr;
  PFN_ID3D11DeviceContext_DrawInstancedIndirect         DrawInstancedIndirect         = nullptr;
  PFN_ID3D11DeviceContext_DrawIndexedInstancedIndirect  DrawIndexedInstancedIndirect  = nullptr;
  PFN_ID3D11DeviceContext_ExecuteCommandList            ExecuteCommandList            = nullptr;
  PFN_ID3D11DeviceContext_FinishCommandList             FinishCommandList             = nullptr;
  PFN_ID3D11DeviceContext_PSSetShaderResources          PSSetShaderResources          = nullptr;
};

static mutex  g_hookMutex;
static mutex  g_globalMutex;

DeviceProcs   g_deviceProcs;
ContextProcs  g_immContextProcs;
ContextProcs  g_defContextProcs;
const DeviceProcs* getDeviceProcs(ID3D11Device* pDevice);
const ContextProcs* getContextProcs(ID3D11DeviceContext* pContext);

constexpr uint32_t HOOK_DEVICE  = (1u << 0);
constexpr uint32_t HOOK_IMM_CTX = (1u << 1);
constexpr uint32_t HOOK_DEF_CTX = (1u << 2);

uint32_t      g_installedHooks = 0u;

struct TransitionCounter {
  std::atomic<uint64_t> calls = 0;
  std::atomic<uint64_t> nanos = 0;
};

TransitionCounter g_transitionCreate;
TransitionCounter g_transitionMap;
TransitionCounter g_transitionCopy;
TransitionCounter g_transitionUpdate;
TransitionCounter g_transitionCommands;
std::array<std::array<TransitionCounter, 6>, 3> g_transitionMapKinds;

struct ReadMapKey {
  uintptr_t caller;
  uint32_t dimension;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t usage;
  uint32_t bindFlags;
  uint32_t cpuFlags;

  bool operator<(const ReadMapKey& other) const {
    return std::tie(caller, dimension, format, width, height, usage,
      bindFlags, cpuFlags) <
      std::tie(other.caller, other.dimension, other.format, other.width,
        other.height, other.usage, other.bindFlags, other.cpuFlags);
  }
};

struct ReadMapStats {
  uint64_t calls = 0;
  uint64_t nanos = 0;
  uint64_t estimatedBytes = 0;
  std::set<uintptr_t> resources;
};

mutex g_transitionReadMapMutex;
std::map<ReadMapKey, ReadMapStats> g_transitionReadMaps;
std::map<ReadMapKey, ReadMapStats> g_transitionWriteMaps;
TransitionCounter g_transitionShadowFlush;
std::atomic<uint64_t> g_transitionShadowFlushBytes = 0;

mutex g_shadowTraceMutex;
std::set<uintptr_t> g_shadowSrvs;      // PS SRVs backed by the 1024x1024 fmt-44 map
std::set<uintptr_t> g_nonShadowSrvs;   // classified as not the shadow map

// ---- Restored cut-in shadows (ARLAND_CUTIN_SHADOWS=1) ----------------------
// Opt-in fix for the battle cut-in/cinematic states, which ship with the
// ground's shadow reception gated shut and the scene light faded. Two
// mechanisms, both active under this single flag:
//   dim-hold : the cut-in darkening is a 16-byte $Params (s,s,s,1) whose
//              scene-light intensity s fades 1.0 (overview) -> 0.7 (cut-in).
//              Hold it at 1.0 during cinematic states so the ground keeps its
//              overview brightness. Identified by shape (uniform RGB, w=1,
//              s dropped below 1) rather than the per-launch pointer.
//   gate-hold: the 880-byte receiver material gates shadow RECEPTION on the
//              VS's `diffuse` at byte 832 (a name collision — the PS RDEF
//              calls byte 832 `shadowLPos`, but the VS reads cb0[52] as
//              diffuse). The receiver VS computes gate = 2.5 -
//              2*min(diffuse.w, diffuse.x); the PS samples the shadow map
//              ONLY if gate < 1, i.e. min-diffuse > 0.75. During the cut-in
//              diffuse.xyz is pinned to ~0.7 -> gate closed -> the ground
//              never samples the shadow map at all. Holding diffuse at 1.0
//              during cinematic states reopens the gate. Patched on every cb
//              write path AND (load-bearing) via a 16-byte boxed
//              UpdateSubresource right before each cinematic shadow-receiving
//              880 draw (gateHoldAtDraw).
// Shared arland-fix.ini boolean reader; writes the default back when the key is
// absent so it appears in the file for the user. Defined below with configPath.
bool arlandConfigBool(const char* section, const char* key, bool def);

// ShadowMultiplier (arland-fix.ini [Rendering], default 1 = unchanged): scales
// the engine's two 1024x1024 R24G8 shadow maps. Values 2, 4 and 8 enlarge the
// maps to 2048/4096/8192 (plus the caster viewport, the A->B copy box and the
// receiver's PCF tap scale); anything else keeps vanilla behaviour.
// ARLAND_SHADOW_MULTIPLIER overrides the ini. Defined below with configPath.
UINT shadowMapResolution();

// ---- shadow-res twin plumbing ------------------------------------------
// The enlarged shadow maps are SEPARATE mod-owned textures ("twins"), not
// in-place resizes: the engine's own 1024x1024 maps stay untouched so every
// engine-side size/memory assumption remains valid. The twin texture hangs
// off the engine texture via SetPrivateDataInterface (released with it), the
// caster DSV bind / receiver SRV bind / A->B copy are redirected or mirrored
// onto the twins, and the tag below marks twin textures for the viewport
// rewrite and the shadow-SRV classifier.
static const GUID IID_ShadowResResized  = {0xe2728d9e,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_ShadowResTwin     = {0xe2728d9f,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_ShadowResTwinView = {0xe2728da0,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};

bool isShadowResResized(ID3D11Resource* resource) {
  UINT marker = 0;
  UINT size = sizeof(marker);
  return resource && SUCCEEDED(resource->GetPrivateData(
    IID_ShadowResResized, &size, &marker)) && marker != 0;
}

// Host SRV pointers known to have no twin (fast negative path for the hot
// PSSetShaderResources hook). Cleared whenever a new twin is created so a
// scene rebuild that recycles pointers cannot permanently suppress the
// redirect; a stale negative costs at most a low-res shadow, never a crash.
mutex g_twinSrvNegMutex;
std::set<uintptr_t> g_twinSrvNegative;

// AddRef'd twin texture of an engine shadow map, or null.
ID3D11Resource* getShadowResTwinResource(ID3D11Resource* host) {
  ID3D11Resource* twin = nullptr;
  UINT size = sizeof(twin);
  if (host && SUCCEEDED(host->GetPrivateData(IID_ShadowResTwin, &size, &twin))
      && twin)
    return twin;
  return nullptr;
}

// AddRef'd DSV over the twin of the host DSV's texture (cached on the host
// DSV), or null when the host texture has no twin or creation fails.
ID3D11DepthStencilView* getShadowResTwinDsv(ID3D11DepthStencilView* hostDsv) {
  ID3D11DepthStencilView* twinDsv = nullptr;
  UINT size = sizeof(twinDsv);
  if (SUCCEEDED(hostDsv->GetPrivateData(IID_ShadowResTwinView, &size, &twinDsv))
      && twinDsv)
    return twinDsv;
  ID3D11Resource* hostRes = nullptr;
  hostDsv->GetResource(&hostRes);
  ID3D11Resource* twinRes = hostRes ? getShadowResTwinResource(hostRes) : nullptr;
  if (hostRes)
    hostRes->Release();
  if (!twinRes)
    return nullptr;
  D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc = { };
  hostDsv->GetDesc(&viewDesc);
  ID3D11Device* device = nullptr;
  hostDsv->GetDevice(&device);
  HRESULT hr = E_FAIL;
  if (device) {
    hr = device->CreateDepthStencilView(twinRes, &viewDesc, &twinDsv);
    device->Release();
  }
  twinRes->Release();
  if (FAILED(hr) || !twinDsv) {
    log("SHADOWRES twin DSV creation FAILED hr=0x", std::hex, hr);
    return nullptr;
  }
  hostDsv->SetPrivateDataInterface(IID_ShadowResTwinView, twinDsv);
  return twinDsv;   // creation ref belongs to the caller
}

// AddRef'd SRV over the twin of the host SRV's texture (cached on the host
// SRV), or null. Negative-cached by pointer for the hot path.
ID3D11ShaderResourceView* getShadowResTwinSrv(
    ID3D11ShaderResourceView* hostSrv) {
  {
    std::lock_guard lock(g_twinSrvNegMutex);
    if (g_twinSrvNegative.count(reinterpret_cast<uintptr_t>(hostSrv)))
      return nullptr;
  }
  ID3D11ShaderResourceView* twinSrv = nullptr;
  UINT size = sizeof(twinSrv);
  if (SUCCEEDED(hostSrv->GetPrivateData(IID_ShadowResTwinView, &size, &twinSrv))
      && twinSrv)
    return twinSrv;
  ID3D11Resource* hostRes = nullptr;
  hostSrv->GetResource(&hostRes);
  ID3D11Resource* twinRes = hostRes ? getShadowResTwinResource(hostRes) : nullptr;
  if (hostRes)
    hostRes->Release();
  if (!twinRes) {
    std::lock_guard lock(g_twinSrvNegMutex);
    g_twinSrvNegative.insert(reinterpret_cast<uintptr_t>(hostSrv));
    return nullptr;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = { };
  hostSrv->GetDesc(&viewDesc);
  ID3D11Device* device = nullptr;
  hostSrv->GetDevice(&device);
  HRESULT hr = E_FAIL;
  if (device) {
    hr = device->CreateShaderResourceView(twinRes, &viewDesc, &twinSrv);
    device->Release();
  }
  twinRes->Release();
  if (FAILED(hr) || !twinSrv) {
    log("SHADOWRES twin SRV creation FAILED hr=0x", std::hex, hr);
    std::lock_guard lock(g_twinSrvNegMutex);
    g_twinSrvNegative.insert(reinterpret_cast<uintptr_t>(hostSrv));
    return nullptr;
  }
  hostSrv->SetPrivateDataInterface(IID_ShadowResTwinView, twinSrv);
  return twinSrv;   // creation ref belongs to the caller
}

// Reopen the receiver's shadow-reception gate during cut-ins. Per-game and
// override handling (env ARLAND_CUTIN_SHADOWS, ini [Battle] BattleCutInShadows
// default true) now live in the capability matrix (game.cpp); Totori is
// Unsupported there until its cut-in detection is ported, so this stays off for
// Totori. Cached because it is read in hot draw paths.
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

bool cutinBlobEnabled();   // defined with the contact-blob overlay below

// Rewrite a matching 16-byte light-intensity $Params to full brightness in place.
// Returns true if it patched. Only touches (s,s,s,~1) with s in (0.5,0.98) — the
// faded cut-in value — so a genuine (1,1,1,1) or unrelated buffer is left alone.
bool dimHoldPatch(void* data, uint32_t size) {
  if (size != 16)
    return false;
  float* v = static_cast<float*>(data);
  const float s = v[0];
  const float near1 = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; }(v[3], 1.0f);
  const float uni01 = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; }(v[0], v[1]);
  const float uni02 = [](float a, float b) { float d = a - b; return d < 0 ? -d : d; }(v[0], v[2]);
  if (near1 < 0.02f && uni01 < 0.01f && uni02 < 0.01f &&
      s > 0.5f && s < 0.98f) {
    v[0] = v[1] = v[2] = 1.0f;
    return true;
  }
  return false;
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
    v[0] = v[1] = v[2] = 1.0f;
    if (v[3] < 0.76f) v[3] = 1.0f;   // ensure min(.w,.x) clears the 0.75 gate
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

// The write-path patches and the blob's cb0 reads consume CPU-side captures of
// constant-buffer writes (a Map payload is only visible between Map and Unmap).
bool cbCaptureEnabled() {
  return cutinShadowsEnabled() || cutinBlobEnabled() ||
    shadowMapResolution() > 1024;
}

// Snapshot payload width. Covers the full 880-byte field/receiver material,
// including PSSGLightModelViewProjTex (+752), tapScale (+816) and shadowLPos
// (+832). Every consumer buffer MUST use this constant — readCb0Snap copies
// the full payload.
constexpr uint32_t kCbSnapBytes = 896;

struct CbSnap {
  uint32_t size = 0;      // full buffer byte width
  uint8_t data[kCbSnapBytes] = {}; // first bytes of the latest write
};
mutex g_cbSnapMutex;
std::map<ID3D11Resource*, CbSnap> g_cbSnaps;
std::map<std::pair<ID3D11Resource*, UINT>, std::pair<const void*, uint32_t>>
    g_cbSnapPending;

void snapCbWrite(ID3D11Resource* resource, const void* data, uint32_t size) {
  CbSnap snap;
  snap.size = size;
  std::memcpy(snap.data, data, std::min<uint32_t>(size, sizeof(snap.data)));
  std::lock_guard lock(g_cbSnapMutex);
  g_cbSnaps[resource] = snap;
}

bool isConstantBuffer(ID3D11Resource* resource, D3D11_BUFFER_DESC* desc) {
  ID3D11Buffer* buffer = nullptr;
  if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))
    return false;
  buffer->GetDesc(desc);
  buffer->Release();
  return (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) && desc->ByteWidth >= 16;
}

// The game writes cbuffers via Map(WRITE_DISCARD)/Unmap; the CPU payload is
// only valid between Map and Unmap. Track the mapped pointer per subresource
// so captureCbUnmap can patch/snapshot it right before the real Unmap.
void captureCbMap(ID3D11Resource* resource, UINT sub,
                  const D3D11_MAPPED_SUBRESOURCE* mapped) {
  if (!cbCaptureEnabled() || !resource || !mapped || !mapped->pData)
    return;
  D3D11_BUFFER_DESC desc = {};
  if (!isConstantBuffer(resource, &desc))
    return;
  std::lock_guard lock(g_cbSnapMutex);
  g_cbSnapPending[{resource, sub}] = {mapped->pData, desc.ByteWidth};
}

void captureCbUnmap(ID3D11DeviceContext*, ID3D11Resource* resource,
                    UINT sub) {
  if (!cbCaptureEnabled())
    return;
  std::pair<const void*, uint32_t> pending{nullptr, 0};
  {
    std::lock_guard lock(g_cbSnapMutex);
    auto it = g_cbSnapPending.find({resource, sub});
    if (it != g_cbSnapPending.end()) {
      pending = it->second;
      g_cbSnapPending.erase(it);
    }
  }
  if (!pending.first)
    return;
  // Patch the CPU-visible contents in place BEFORE the real Unmap invalidates
  // them: dim-hold on the 16-byte $Params, gate-hold on the 880 receiver.
  if (arlandInCinematicBattle()) {
    if (cutinDimHoldEnabled())
      dimHoldPatch(const_cast<void*>(pending.first), pending.second);
    if (cutinGateHoldEnabled())
      gateHoldPatch(const_cast<void*>(pending.first), pending.second);
  }
  if (shadowMapResolution() > 1024)
    tapScalePatch(const_cast<void*>(pending.first), pending.second);
  if (cutinBlobEnabled())
    snapCbWrite(resource, pending.first, pending.second);
}

std::atomic<ID3D11DeviceContext*> g_immCtx{nullptr};

// 4x4 helpers on flat row-major float[16] (flat[r*4+c]).
void mtxMul(const float* a, const float* b, float* out) {
  float r[16];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                     a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
  std::memcpy(out, r, sizeof(r));
}

bool mtxInvert(const float* m, float* out) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r)
      if (std::fabs(a[r][col]) > std::fabs(a[pivot][col]))
        pivot = r;
    if (std::fabs(a[pivot][col]) < 1e-20)
      return false;
    if (pivot != col)
      for (int c = 0; c < 8; ++c)
        std::swap(a[pivot][c], a[col][c]);
    const double d = a[col][col];
    for (int c = 0; c < 8; ++c)
      a[col][c] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == col || a[r][col] == 0.0)
        continue;
      const double f = a[r][col];
      for (int c = 0; c < 8; ++c)
        a[r][c] -= f * a[col][c];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      out[r * 4 + c] = static_cast<float>(a[r][c + 4]);
  return true;
}

float mtxDet3(const float* m) {
  return m[0] * (m[5] * m[10] - m[6] * m[9]) -
         m[1] * (m[4] * m[10] - m[6] * m[8]) +
         m[2] * (m[4] * m[9] - m[5] * m[8]);
}

// Latest captured contents of the draw's VS cb0 (via the Map/Unmap snapshots).
bool readCb0Snap(ID3D11DeviceContext* ctx, uint32_t* size, uint8_t out[kCbSnapBytes]) {
  ID3D11Buffer* cb = nullptr;
  ctx->VSGetConstantBuffers(0, 1, &cb);
  if (!cb)
    return false;
  bool ok = false;
  {
    std::lock_guard lock(g_cbSnapMutex);
    auto it = g_cbSnaps.find(cb);
    if (it != g_cbSnaps.end()) {
      *size = it->second.size;
      std::memcpy(out, it->second.data, sizeof(it->second.data));
      ok = true;
    }
  }
  cb->Release();
  return ok;
}

// A shadow-map clear wipes the engine-side cut-in caster flags; the game-side
// restore walk lives in menu_fix (env-gated there).
void cutinShadowMapCleared(ID3D11DeviceContext*,
                           ID3D11DepthStencilView* dsv) {
  if (!dsv)
    return;
  ID3D11Resource* res = nullptr;
  dsv->GetResource(&res);
  if (!res)
    return;
  bool shadowMap = false;
  ID3D11Texture2D* tex = nullptr;
  if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    // The engine clears its own (always 1024) maps even when the twin
    // redirect is active, so the shipped size-only match stays correct in
    // every mode; the mirrored twin clear must NOT re-fire the callback.
    shadowMap = d.Width == 1024 && d.Height == 1024;
    tex->Release();
  }
  res->Release();
  if (shadowMap)
    arlandCutinShadowMapCleared();
}

// ---- §35 contact-blob cut-in shadow ----------------------------------------
// The cut-in has no shadow-receiving ground (REPORT §34), so instead of a
// projected caster we composite a soft dark ellipse under the character's
// screen-space feet, right before Present. Self-contained: our own shaders,
// vertex buffer and states; draws over the final frame into the swap-chain
// back buffer. Opt-in via ARLAND_CUTIN_BLOB=1, tunable via _SIZE/_Y/_ALPHA/
// _ASPECT (_Y is the world-space floor height the blob is projected onto).

using PFN_D3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
  const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
  ID3DBlob**, ID3DBlob**);

bool cutinBlobEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_BLOB");
    return v && v[0] != '0';
  }();
  return enabled;
}
float cutinEnvF(const char* name, float def) {
  const char* v = std::getenv(name);
  return v ? float(std::atof(v)) : def;
}

struct BlobVertex { float x, y, u, v, a; };
struct CutinChar { float mvp[16]; float W[16]; };
std::mutex g_blobMutex;
std::vector<CutinChar> g_cutinChars;      // captured this frame

ID3D11VertexShader* g_blobVS = nullptr;
ID3D11PixelShader* g_blobPS = nullptr;
ID3D11InputLayout* g_blobIL = nullptr;
ID3D11Buffer* g_blobVB = nullptr;
ID3D11BlendState* g_blobBlend = nullptr;
ID3D11RasterizerState* g_blobRS = nullptr;
ID3D11DepthStencilState* g_blobDS = nullptr;
ID3D11RenderTargetView* g_blobRTV = nullptr;
ID3D11Texture2D* g_blobBackBuf = nullptr;   // identity of the RTV's texture
bool g_blobInit = false, g_blobBroken = false;
static const UINT kMaxBlobs = 16;

void captureCutinMVP(const float* mvp, const float* W) {
  if (!cutinBlobEnabled())
    return;
  CutinChar c;
  std::memcpy(c.mvp, mvp, 64);
  std::memcpy(c.W, W, 64);
  std::lock_guard lock(g_blobMutex);
  if (g_cutinChars.size() < kMaxBlobs)
    g_cutinChars.push_back(c);
}

bool initBlobResources(ID3D11Device* dev) {
  if (g_blobInit)
    return !g_blobBroken;
  g_blobInit = true;
  g_blobBroken = true;   // assume failure until every step succeeds

  HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
  if (!comp)
    comp = LoadLibraryA("d3dcompiler.dll");
  if (!comp) { log("BLOB: no d3dcompiler"); return false; }
  auto D3DCompile = reinterpret_cast<PFN_D3DCompile>(
    GetProcAddress(comp, "D3DCompile"));
  if (!D3DCompile) { log("BLOB: no D3DCompile"); return false; }

  static const char* kVS =
    "struct VIn{float2 p:POSITION;float2 uv:TEXCOORD0;float a:TEXCOORD1;};"
    "struct VOut{float4 p:SV_POSITION;float2 uv:TEXCOORD0;float a:TEXCOORD1;};"
    "VOut main(VIn i){VOut o;o.p=float4(i.p,0,1);o.uv=i.uv;o.a=i.a;return o;}";
  // Soft contact shadow: radial falloff with a smoothstep-feathered rim and a
  // slightly denser core (t^2 * smoothstep), alpha-blended toward black.
  static const char* kPS =
    "float4 main(float4 p:SV_POSITION,float2 uv:TEXCOORD0,"
    "float a:TEXCOORD1):SV_TARGET{"
    "float2 d=uv*2-1;float t=saturate(1-dot(d,d));"
    "float f=t*t*(3-2*t);f*=0.6+0.4*t;"
    "return float4(0,0,0,f*a);}";

  ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
  if (FAILED(D3DCompile(kVS, std::strlen(kVS), "blobVS", nullptr, nullptr,
        "main", "vs_4_0", 0, 0, &vsb, &err))) {
    log("BLOB: VS compile failed");
    if (err) err->Release();
    return false;
  }
  if (FAILED(D3DCompile(kPS, std::strlen(kPS), "blobPS", nullptr, nullptr,
        "main", "ps_4_0", 0, 0, &psb, &err))) {
    log("BLOB: PS compile failed");
    if (err) err->Release();
    if (vsb) vsb->Release();
    return false;
  }
  bool ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(),
    vsb->GetBufferSize(), nullptr, &g_blobVS));
  ok = ok && SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(),
    psb->GetBufferSize(), nullptr, &g_blobPS));
  const D3D11_INPUT_ELEMENT_DESC il[3] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
     D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
     D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 16,
     D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  ok = ok && SUCCEEDED(dev->CreateInputLayout(il, 3, vsb->GetBufferPointer(),
    vsb->GetBufferSize(), &g_blobIL));
  vsb->Release();
  psb->Release();

  D3D11_BUFFER_DESC vb = {};
  vb.ByteWidth = kMaxBlobs * 6 * sizeof(BlobVertex);
  vb.Usage = D3D11_USAGE_DYNAMIC;
  vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ok = ok && SUCCEEDED(dev->CreateBuffer(&vb, nullptr, &g_blobVB));

  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &g_blobBlend));

  D3D11_RASTERIZER_DESC rs = {};
  rs.FillMode = D3D11_FILL_SOLID;
  rs.CullMode = D3D11_CULL_NONE;
  ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rs, &g_blobRS));

  D3D11_DEPTH_STENCIL_DESC ds = {};
  ds.DepthEnable = FALSE;
  ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &g_blobDS));

  if (!ok) { log("BLOB: resource creation failed"); return false; }
  g_blobBroken = false;
  log("BLOB: resources ready");
  return true;
}

// Draw the contact blobs over the final frame. Called from the Present hook
// BEFORE the real Present, so the back buffer holds this frame's image.
void cutinDrawContactBlobs(IDXGISwapChain* swapChain) {
  std::vector<CutinChar> chars;
  {
    std::lock_guard lock(g_blobMutex);
    chars.swap(g_cutinChars);
  }
  if (!cutinBlobEnabled() || !swapChain || chars.empty() ||
      !arlandInCinematicBattle())
    return;
  ID3D11DeviceContext* ctx = g_immCtx.load(std::memory_order_relaxed);
  if (!ctx)
    return;
  ID3D11Device* dev = nullptr;
  ctx->GetDevice(&dev);
  if (!dev)
    return;
  if (!initBlobResources(dev)) { dev->Release(); return; }

  ID3D11Texture2D* bb = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) {
    dev->Release();
    return;
  }
  if (bb != g_blobBackBuf) {
    if (g_blobRTV) { g_blobRTV->Release(); g_blobRTV = nullptr; }
    if (FAILED(dev->CreateRenderTargetView(bb, nullptr, &g_blobRTV)))
      g_blobRTV = nullptr;
    g_blobBackBuf = bb;   // identity only (not owned); not AddRef'd
  }
  D3D11_TEXTURE2D_DESC bd = {};
  bb->GetDesc(&bd);
  bb->Release();
  dev->Release();
  if (!g_blobRTV)
    return;

  const float sizeMul = cutinEnvF("ARLAND_CUTIN_BLOB_SIZE", 1.0f);
  const float groundY = cutinEnvF("ARLAND_CUTIN_BLOB_Y", 0.0f);
  const float alpha = cutinEnvF("ARLAND_CUTIN_BLOB_ALPHA", 0.55f);
  const float aspect =                       // screen-space width/height
    std::max(1.0f, cutinEnvF("ARLAND_CUTIN_BLOB_ASPECT", 2.6f));
  const UINT maxDraw = 4;                    // frontmost characters only

  // Pass 1: project each captured character's GROUND-CONTACT point — the
  // world position under the model pivot at the arena-floor plane (y =
  // groundY, default 0) — through its view-proj (VP = MVP * W^-1), and keep
  // only plausibly-visible on-ground actors.
  struct BlobCand { float cx, cy, w, screenH; };
  BlobCand cands[kMaxBlobs];
  UINT nCand = 0;
  for (const CutinChar& c : chars) {
    if (std::fabs(c.W[7]) < 1e-6f)           // HUD portrait (§33r), belt+braces
      continue;
    if (std::fabs(mtxDet3(c.W)) < 1e-12f)    // hidden model (near-zero scale)
      continue;
    float Wi[16], VP[16];
    if (!mtxInvert(c.W, Wi))
      continue;
    mtxMul(c.mvp, Wi, VP);                   // MVP = VP*W  =>  VP = MVP*W^-1
    auto projWorld = [&](float wx, float wy, float wz, float out[4]) {
      for (int r = 0; r < 4; ++r)
        out[r] = VP[r * 4 + 0] * wx + VP[r * 4 + 1] * wy +
                 VP[r * 4 + 2] * wz + VP[r * 4 + 3];
    };
    float foot[4], head[4];
    projWorld(c.W[3], groundY, c.W[11], foot);          // feet on the floor
    projWorld(c.W[3], groundY + 1.6f, c.W[11], head);   // ~head height
    if (foot[3] <= 0.01f)                    // behind the camera
      continue;
    const float fx = foot[0] / foot[3], fy = foot[1] / foot[3];
    if (fx < -1.05f || fx > 1.05f || fy < -1.05f || fy > 1.05f)
      continue;                              // feet off-screen
    const float hy = head[3] > 0.01f ? head[1] / head[3] : fy;
    const float screenH = std::fabs(fy - hy);            // NDC character height
    if (screenH < 0.02f)                     // degenerate/vanishing actor
      continue;
    bool dup = false;                        // same char, multiple materials
    for (UINT i = 0; i < nCand && !dup; ++i)
      dup = std::fabs(cands[i].cx - fx) < 0.03f &&
            std::fabs(cands[i].cy - fy) < 0.03f;
    if (dup || nCand >= kMaxBlobs)
      continue;
    cands[nCand++] = {fx, fy, foot[3], screenH};
    static std::atomic<uint32_t> chLogs{0};
    if (chLogs.fetch_add(1, std::memory_order_relaxed) % 60 == 0)
      log("CUTIN_BLOB_CH W=", c.W[3], ",", c.W[7], ",", c.W[11],
          " ndc=", fx, ",", fy, " screenH=", screenH, " w=", foot[3]);
  }
  // Frontmost first (smallest clip w = closest to camera), cap the count.
  std::sort(cands, cands + nCand,
            [](const BlobCand& a, const BlobCand& b) { return a.w < b.w; });
  const UINT nDraw = std::min<UINT>(nCand, maxDraw);

  // Pass 2: one flattened quad per kept character. Width scales with the
  // character's projected size; height derives from the screen-space aspect
  // so the oval reads as a ground shadow at any resolution.
  const float pixAspect = bd.Height ? float(bd.Width) / float(bd.Height)
                                    : 16.0f / 9.0f;
  BlobVertex verts[kMaxBlobs * 6];
  UINT nv = 0;
  for (UINT i = 0; i < nDraw; ++i) {
    const BlobCand& q = cands[i];
    const float halfW =
      std::min(0.9f, std::max(0.04f, 0.60f * q.screenH * sizeMul));
    const float halfH = halfW * pixAspect / aspect;
    const float x0 = q.cx - halfW, x1 = q.cx + halfW;
    const float y0 = q.cy - halfH, y1 = q.cy + halfH;
    const float a = alpha;
    const BlobVertex quad[6] = {
      {x0, y0, 0, 1, a}, {x0, y1, 0, 0, a}, {x1, y1, 1, 0, a},
      {x0, y0, 0, 1, a}, {x1, y1, 1, 0, a}, {x1, y0, 1, 1, a},
    };
    std::memcpy(&verts[nv], quad, sizeof(quad));
    nv += 6;
  }
  if (!nv)
    return;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(ctx->Map(g_blobVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return;
  std::memcpy(mapped.pData, verts, nv * sizeof(BlobVertex));
  ctx->Unmap(g_blobVB, 0);

  D3D11_VIEWPORT vp = {0, 0, float(bd.Width), float(bd.Height), 0, 1};
  const UINT stride = sizeof(BlobVertex), offset = 0;

  // Bind our own pipeline. This runs right before Present; the game rebinds
  // everything next frame, so a full state save/restore is unnecessary.
  ctx->OMSetRenderTargets(1, &g_blobRTV, nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->IASetInputLayout(g_blobIL);
  ctx->IASetVertexBuffers(0, 1, &g_blobVB, &stride, &offset);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(g_blobVS, nullptr, 0);
  ctx->PSSetShader(g_blobPS, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->HSSetShader(nullptr, nullptr, 0);
  ctx->DSSetShader(nullptr, nullptr, 0);
  ctx->OMSetBlendState(g_blobBlend, nullptr, 0xffffffff);
  ctx->OMSetDepthStencilState(g_blobDS, 0);
  ctx->RSSetState(g_blobRS);
  ctx->Draw(nv, 0);
  static std::atomic<uint32_t> blobLogs{0};
  static std::atomic<uint32_t> lastDrew{~0u};
  const uint32_t drew = nv / 6;
  if (lastDrew.exchange(drew, std::memory_order_relaxed) != drew ||
      blobLogs.fetch_add(1, std::memory_order_relaxed) % 120 == 0)
    log("CUTIN_BLOB drew=", drew, " captured=", chars.size(),
        " kept=", nCand);
}

// Feed the contact-blob overlay: during a cinematic battle state, capture each
// character skin draw's ModelViewProj (26048-byte $Globals @160) and Model so
// the blob pass can project the character's feet to screen space.
void cutinBlobCaptureDraw(ID3D11DeviceContext* ctx, UINT count) {
  if (!cutinBlobEnabled() || count < 300 || !arlandInCinematicBattle())
    return;
  ID3D11RenderTargetView* rtv = nullptr;
  ctx->OMGetRenderTargets(1, &rtv, nullptr);
  if (!rtv)
    return;
  bool colorMain = false;
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  if (res) {
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
      D3D11_TEXTURE2D_DESC d = {};
      tex->GetDesc(&d);
      colorMain = d.Width == 1920 && d.Height == 1080;
      tex->Release();
    }
    res->Release();
  }
  rtv->Release();
  if (!colorMain)
    return;
  uint32_t snapSize = 0;
  uint8_t snap[kCbSnapBytes];
  if (!readCb0Snap(ctx, &snapSize, snap) || snapSize != 26048)
    return;
  float W[16];
  std::memcpy(W, snap + 32, 64);         // Model @32 of the skin $Globals
  if (std::fabs(mtxDet3(W)) < 1e-12f)    // hidden model (near-zero scale)
    return;
  // Battle-HUD portraits use the skin material too and sit at exactly y=0;
  // arena characters always have |y| > 0.
  if (std::fabs(W[7]) < 1e-6f)
    return;
  captureCutinMVP(reinterpret_cast<const float*>(snap + 160), W);
}

// Classify a PS shader-resource view as the shadow map or not, caching the
// verdict by pointer so the desc query only happens once per view. Caller holds
// g_shadowTraceMutex.
bool isShadowSrvLocked(ID3D11ShaderResourceView* srv) {
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

bool transitionTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_MENU_TRANSITION_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

class TransitionTimer {
public:
  explicit TransitionTimer(TransitionCounter& counter)
  : m_counter(transitionTraceEnabled() ? &counter : nullptr) {
    if (m_counter) {
      m_counter->calls.fetch_add(1, std::memory_order_relaxed);
      m_started = std::chrono::steady_clock::now();
    }
  }

  ~TransitionTimer() {
    if (m_counter) {
      const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - m_started).count();
      m_counter->nanos.fetch_add(uint64_t(nanos), std::memory_order_relaxed);
    }
  }

private:
  TransitionCounter* m_counter;
  std::chrono::steady_clock::time_point m_started;
};

class TransitionMapKindTimer {
public:
  explicit TransitionMapKindTimer(D3D11_MAP mapType)
  : m_mapType(mapType >= D3D11_MAP_READ && mapType <= D3D11_MAP_WRITE_NO_OVERWRITE
      ? unsigned(mapType) : 0),
    m_enabled(transitionTraceEnabled()) {
    if (m_enabled)
      m_started = std::chrono::steady_clock::now();
  }

  void setBranch(unsigned branch) { m_branch = branch; }

  ~TransitionMapKindTimer() {
    if (!m_enabled || m_branch >= g_transitionMapKinds.size())
      return;
    auto& counter = g_transitionMapKinds[m_branch][m_mapType];
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - m_started).count();
    counter.nanos.fetch_add(uint64_t(nanos), std::memory_order_relaxed);
  }

private:
  unsigned m_branch = 0;
  unsigned m_mapType = 0;
  bool m_enabled = false;
  std::chrono::steady_clock::time_point m_started;
};

// Resolution behavior ported from TellowKrinkle's atelier-sync-fix rendering
// fork and adapted to this project's vtable-hook architecture.
// The old Arland renderers create the main depth target at the requested
// resolution, then create several render/depth targets at a hard-coded 1080p.
// Remember the former so those later targets can follow it.
static std::atomic<UINT> g_mainRtWidth  = { 0 };
static std::atomic<UINT> g_mainRtHeight = { 0 };
static std::atomic<UINT> g_originalSwapWidth  = { 0 };
static std::atomic<UINT> g_originalSwapHeight = { 0 };

// The games also submit a hard-coded 1080p viewport and scissor. Keep separate
// state for the immediate and deferred context paths; atomics keep the hooks
// safe if the engine records or submits state from another thread.
struct RasterState {
  std::atomic<UINT> viewportWidth  = { 0 };
  std::atomic<UINT> viewportHeight = { 0 };
  std::atomic<UINT> scissorWidth   = { 0 };
  std::atomic<UINT> scissorHeight  = { 0 };
  std::atomic<bool> dirty          = { false };
};

static RasterState g_immRasterState;
static RasterState g_defRasterState;

// MSAA design adapted from TellowKrinkle's atelier-sync-fix rendering fork.
// The game continues to own single-sample resources; matching multisample
// render targets are attached as private data and resolved before every read.
static const GUID IID_MSAAResource = {0xe2728d94,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_MSAAState = {0xe2728d93,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_MSAABoundHost = {0xe2728d98,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_ResolutionTrace = {0xe2728d99,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_CompositeTraceRole = {0xe2728d9a,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_ResolutionBufferData = {0xe2728d9b,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_DialogSnapshotResource = {0xe2728d9c,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_DialogScaledVertexBuffer = {0xe2728d9d,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};

enum class MSAAState : UINT { Clean, Dirty };

struct ResolutionTraceState {
  UINT id = 0;
  UINT shaderBinds = 0;
  UINT copies = 0;
};

static std::atomic<UINT> g_nextResolutionTraceId = { 1 };

enum CompositeTraceRole : UINT {
  CompositeVb0   = 1u << 0,
  CompositeVb1   = 1u << 1,
  CompositeVsCb0 = 1u << 2,
  CompositeVsCb1 = 1u << 3,
  CompositePsCb0 = 1u << 4,
  CompositePsCb1 = 1u << 5,
};

struct CompositeMappedBuffer {
  const void* data = nullptr;
  UINT size = 0;
  UINT roles = 0;
};

static mutex g_compositeMapMutex;
static std::map<std::pair<ID3D11Resource*, UINT>, CompositeMappedBuffer>
  g_compositeMaps;
static std::atomic<UINT> g_compositeDumpCount = { 0 };

bool resolutionTraceEnabled();

void tagCompositeBuffer(ID3D11Buffer* buffer, UINT role) {
  if (!buffer)
    return;
  UINT roles = 0;
  UINT size = sizeof(roles);
  buffer->GetPrivateData(IID_CompositeTraceRole, &size, &roles);
  roles |= role;
  buffer->SetPrivateData(IID_CompositeTraceRole, sizeof(roles), &roles);
}

UINT resolutionBufferData(ID3D11Buffer* buffer, uint32_t (&words)[16]) {
  if (!buffer)
    return 0;
  UINT size = sizeof(words);
  return SUCCEEDED(buffer->GetPrivateData(
    IID_ResolutionBufferData, &size, words)) ? size : 0;
}

void trackCompositeMap(ID3D11Resource* resource, UINT subresource,
                       const D3D11_MAPPED_SUBRESOURCE* mapped) {
  if (!resolutionTraceEnabled() || !resource || !mapped || !mapped->pData)
    return;
  UINT roles = 0;
  UINT roleSize = sizeof(roles);
  if (FAILED(resource->GetPrivateData(
        IID_CompositeTraceRole, &roleSize, &roles)) || !roles)
    return;
  ID3D11Buffer* buffer = nullptr;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))
    return;
  D3D11_BUFFER_DESC desc = { };
  buffer->GetDesc(&desc);
  buffer->Release();
  std::lock_guard lock(g_compositeMapMutex);
  g_compositeMaps[std::pair<ID3D11Resource*, UINT> { resource, subresource }] =
    CompositeMappedBuffer { mapped->pData, desc.ByteWidth, roles };
}

void dumpCompositeMap(ID3D11Resource* resource, UINT subresource) {
  CompositeMappedBuffer mapped = { };
  {
    std::lock_guard lock(g_compositeMapMutex);
    const auto entry = g_compositeMaps.find(
      std::pair<ID3D11Resource*, UINT> { resource, subresource });
    if (entry == g_compositeMaps.end())
      return;
    mapped = entry->second;
    g_compositeMaps.erase(entry);
  }
  if (g_compositeDumpCount.fetch_add(1, std::memory_order_relaxed) >= 512)
    return;
  uint32_t words[16] = { };
  const UINT bytes = std::min<UINT>(mapped.size, sizeof(words));
  std::memcpy(words, mapped.data, bytes);
  log("RES composite-buffer roles=0x", std::hex, mapped.roles,
      " resource=", resource,
      " size=", std::dec, mapped.size,
      " data=", std::hex,
      words[0], ",", words[1], ",", words[2], ",", words[3], ",",
      words[4], ",", words[5], ",", words[6], ",", words[7], ",",
      words[8], ",", words[9], ",", words[10], ",", words[11], ",",
      words[12], ",", words[13], ",", words[14], ",", words[15]);
}

void dumpCompositeUpdate(ID3D11Resource* resource, const void* data) {
  if (!resolutionTraceEnabled() || !resource || !data)
    return;
  UINT roles = 0;
  UINT roleSize = sizeof(roles);
  if (FAILED(resource->GetPrivateData(
        IID_CompositeTraceRole, &roleSize, &roles)) || !roles)
    return;
  if (g_compositeDumpCount.fetch_add(1, std::memory_order_relaxed) >= 512)
    return;
  ID3D11Buffer* buffer = nullptr;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))
    return;
  D3D11_BUFFER_DESC desc = { };
  buffer->GetDesc(&desc);
  buffer->Release();
  uint32_t words[16] = { };
  std::memcpy(words, data, std::min<UINT>(desc.ByteWidth, sizeof(words)));
  log("RES composite-update roles=0x", std::hex, roles,
      " resource=", resource,
      " size=", std::dec, desc.ByteWidth,
      " data=", std::hex,
      words[0], ",", words[1], ",", words[2], ",", words[3], ",",
      words[4], ",", words[5], ",", words[6], ",", words[7], ",",
      words[8], ",", words[9], ",", words[10], ",", words[11], ",",
      words[12], ",", words[13], ",", words[14], ",", words[15]);
}

bool resolutionTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_RESOLUTION_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool texture2DDesc(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC* desc) {
  if (!resource || !desc)
    return false;
  ID3D11Texture2D* texture = nullptr;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
    return false;
  texture->GetDesc(desc);
  texture->Release();
  return true;
}

bool isUnscaled1080Resource(ID3D11Resource* resource,
                            D3D11_TEXTURE2D_DESC* desc = nullptr) {
  if (!resolutionTraceEnabled() ||
      g_mainRtWidth.load(std::memory_order_relaxed) <= 1920 ||
      g_mainRtHeight.load(std::memory_order_relaxed) <= 1080)
    return false;
  D3D11_TEXTURE2D_DESC local = { };
  if (!texture2DDesc(resource, &local) ||
      local.Width != 1920 || local.Height != 1080)
    return false;
  if (desc)
    *desc = local;
  return true;
}

bool resolutionTraceState(ID3D11Resource* resource,
                          ResolutionTraceState* state) {
  if (!resource || !state)
    return false;
  UINT size = sizeof(*state);
  if (SUCCEEDED(resource->GetPrivateData(
        IID_ResolutionTrace, &size, state)) && size == sizeof(*state))
    return true;
  if (!isUnscaled1080Resource(resource))
    return false;
  state->id = g_nextResolutionTraceId.fetch_add(1, std::memory_order_relaxed);
  resource->SetPrivateData(IID_ResolutionTrace, sizeof(*state), state);
  return true;
}

void traceResolutionCopy(const char* operation,
                         ID3D11DeviceContext* context,
                         ID3D11Resource* destination,
                         ID3D11Resource* source) {
  if (!resolutionTraceEnabled())
    return;
  ResolutionTraceState dstState = { };
  ResolutionTraceState srcState = { };
  const bool tracedDst = resolutionTraceState(destination, &dstState);
  const bool tracedSrc = resolutionTraceState(source, &srcState);
  if (!tracedDst && !tracedSrc)
    return;
  ResolutionTraceState* state = tracedDst ? &dstState : &srcState;
  ++state->copies;
  (tracedDst ? destination : source)->SetPrivateData(
    IID_ResolutionTrace, sizeof(*state), state);
  if (state->copies > 8 && (state->copies & (state->copies - 1)) != 0)
    return;
  D3D11_TEXTURE2D_DESC dstDesc = { };
  D3D11_TEXTURE2D_DESC srcDesc = { };
  const bool haveDst = texture2DDesc(destination, &dstDesc);
  const bool haveSrc = texture2DDesc(source, &srcDesc);
  log("RES copy op=", operation,
      " context=", context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "deferred",
      " candidate=", state->id,
      " count=", state->copies,
      " dst=", destination,
      " dst_size=", haveDst ? dstDesc.Width : 0, "x", haveDst ? dstDesc.Height : 0,
      " dst_format=", haveDst ? dstDesc.Format : DXGI_FORMAT_UNKNOWN,
      " src=", source,
      " src_size=", haveSrc ? srcDesc.Width : 0, "x", haveSrc ? srcDesc.Height : 0,
      " src_format=", haveSrc ? srcDesc.Format : DXGI_FORMAT_UNKNOWN);
}

// configPath / arlandConfigBool / shadowMapResolution / configuredResolution
// moved to config.cpp (arland-fix.ini access). applyResolutionOverride stays
// here because it mutates the resolution globals below.
bool applyResolutionOverride(DXGI_SWAP_CHAIN_DESC* pDesc) {
  if (!pDesc)
    return false;
  UINT width = 0;
  UINT height = 0;
  if (!configuredResolution(&width, &height))
    return false;
  g_originalSwapWidth.store(pDesc->BufferDesc.Width, std::memory_order_relaxed);
  g_originalSwapHeight.store(pDesc->BufferDesc.Height, std::memory_order_relaxed);
  pDesc->BufferDesc.Width = width;
  pDesc->BufferDesc.Height = height;
  pDesc->BufferDesc.RefreshRate.Numerator = 0;
  pDesc->BufferDesc.RefreshRate.Denominator = 0;
  log("Overriding swap-chain resolution to ", std::dec, width, "x", height);
  return true;
}

// msaaSamples moved to config.cpp.

const DeviceProcs* getDeviceProcs(ID3D11Device* pDevice) {
  return &g_deviceProcs;
}

const ContextProcs* getContextProcs(ID3D11DeviceContext* pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
    ? &g_immContextProcs
    : &g_defContextProcs;
}

RasterState* getRasterState(ID3D11DeviceContext* pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
    ? &g_immRasterState
    : &g_defRasterState;
}

template<typename T>
T* getMSAAObject(ID3D11DeviceChild* host) {
  T* object = nullptr;
  UINT size = sizeof(object);
  return host && SUCCEEDED(host->GetPrivateData(IID_MSAAResource, &size, &object))
    ? object : nullptr;
}

void resolveIfMSAA(ID3D11DeviceContext* context, ID3D11Resource* host) {
  ID3D11Resource* multisampled = getMSAAObject<ID3D11Resource>(host);
  if (!multisampled)
    return;

  MSAAState state = MSAAState::Clean;
  UINT size = sizeof(state);
  if (SUCCEEDED(host->GetPrivateData(IID_MSAAState, &size, &state)) &&
      state == MSAAState::Dirty) {
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(multisampled->QueryInterface(IID_PPV_ARGS(&texture)))) {
      D3D11_TEXTURE2D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();
      context->ResolveSubresource(host, 0, multisampled, 0, desc.Format);
      state = MSAAState::Clean;
      host->SetPrivateData(IID_MSAAState, sizeof(state), &state);
    }
  }
  multisampled->Release();
}

void resolveBoundMSAA(ID3D11DeviceContext* context) {
  ID3D11Resource* host = nullptr;
  UINT size = sizeof(host);
  if (FAILED(context->GetPrivateData(IID_MSAABoundHost, &size, &host)) || !host)
    return;
  context->SetPrivateData(IID_MSAABoundHost, 0, nullptr);
  resolveIfMSAA(context, host);
  host->Release();
}

bool getOrCreateMSAAViews(ID3D11DeviceContext* context,
                         ID3D11RenderTargetView* hostRtv,
                         ID3D11DepthStencilView* hostDsv,
                         ID3D11RenderTargetView** msaaRtv,
                         ID3D11DepthStencilView** msaaDsv) {
  *msaaRtv = getMSAAObject<ID3D11RenderTargetView>(hostRtv);
  *msaaDsv = getMSAAObject<ID3D11DepthStencilView>(hostDsv);
  if (*msaaRtv && *msaaDsv)
    return true;
  if (*msaaRtv) { (*msaaRtv)->Release(); *msaaRtv = nullptr; }
  if (*msaaDsv) { (*msaaDsv)->Release(); *msaaDsv = nullptr; }

  ID3D11Resource* colorResource = nullptr;
  ID3D11Resource* depthResource = nullptr;
  ID3D11Texture2D* colorHost = nullptr;
  ID3D11Texture2D* depthHost = nullptr;
  hostRtv->GetResource(&colorResource);
  hostDsv->GetResource(&depthResource);
  if (!colorResource || !depthResource ||
      FAILED(colorResource->QueryInterface(IID_PPV_ARGS(&colorHost))) ||
      FAILED(depthResource->QueryInterface(IID_PPV_ARGS(&depthHost)))) {
    if (colorHost) colorHost->Release();
    if (depthHost) depthHost->Release();
    if (colorResource) colorResource->Release();
    if (depthResource) depthResource->Release();
    return false;
  }

  D3D11_TEXTURE2D_DESC colorDesc = { };
  D3D11_TEXTURE2D_DESC depthDesc = { };
  D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = { };
  D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = { };
  colorHost->GetDesc(&colorDesc);
  depthHost->GetDesc(&depthDesc);
  hostRtv->GetDesc(&rtvDesc);
  hostDsv->GetDesc(&dsvDesc);
  colorHost->Release();
  depthHost->Release();

  const UINT mainWidth = g_mainRtWidth.load(std::memory_order_relaxed);
  const UINT mainHeight = g_mainRtHeight.load(std::memory_order_relaxed);
  const bool eligible = msaaSamples() > 1 && mainWidth && mainHeight &&
    colorDesc.Width == mainWidth && colorDesc.Height == mainHeight &&
    depthDesc.Width == mainWidth && depthDesc.Height == mainHeight &&
    colorDesc.SampleDesc.Count == 1 && depthDesc.SampleDesc.Count == 1 &&
    colorDesc.ArraySize == 1 && depthDesc.ArraySize == 1 &&
    colorDesc.MipLevels == 1 && depthDesc.MipLevels == 1 &&
    (rtvDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
     rtvDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
  if (!eligible) {
    colorResource->Release();
    depthResource->Release();
    return false;
  }

  ID3D11Device* device = nullptr;
  context->GetDevice(&device);
  UINT samples = msaaSamples();
  while (samples > 1) {
    UINT colorQuality = 0;
    UINT depthQuality = 0;
    if (SUCCEEDED(device->CheckMultisampleQualityLevels(rtvDesc.Format, samples, &colorQuality)) &&
        SUCCEEDED(device->CheckMultisampleQualityLevels(dsvDesc.Format, samples, &depthQuality)) &&
        colorQuality && depthQuality)
      break;
    samples /= 2;
  }
  if (samples < 2) {
    device->Release();
    colorResource->Release();
    depthResource->Release();
    return false;
  }

  colorDesc.Format = rtvDesc.Format;
  colorDesc.SampleDesc = { samples, 0 };
  colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
  colorDesc.CPUAccessFlags = 0;
  colorDesc.MiscFlags = 0;
  colorDesc.Usage = D3D11_USAGE_DEFAULT;
  depthDesc.SampleDesc = { samples, 0 };
  depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  depthDesc.CPUAccessFlags = 0;
  depthDesc.MiscFlags = 0;
  depthDesc.Usage = D3D11_USAGE_DEFAULT;

  ID3D11Texture2D* colorMsaa = nullptr;
  ID3D11Texture2D* depthMsaa = nullptr;
  HRESULT hr = device->CreateTexture2D(&colorDesc, nullptr, &colorMsaa);
  if (SUCCEEDED(hr))
    hr = device->CreateTexture2D(&depthDesc, nullptr, &depthMsaa);
  if (SUCCEEDED(hr)) {
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    hr = device->CreateRenderTargetView(colorMsaa, &rtvDesc, msaaRtv);
    if (SUCCEEDED(hr))
      hr = device->CreateDepthStencilView(depthMsaa, &dsvDesc, msaaDsv);
  }
  if (SUCCEEDED(hr)) {
    colorResource->SetPrivateDataInterface(IID_MSAAResource, colorMsaa);
    depthResource->SetPrivateDataInterface(IID_MSAAResource, depthMsaa);
    hostRtv->SetPrivateDataInterface(IID_MSAAResource, *msaaRtv);
    hostDsv->SetPrivateDataInterface(IID_MSAAResource, *msaaDsv);
    log("Created ", std::dec, samples, "x MSAA targets at ",
        mainWidth, "x", mainHeight);
  } else {
    if (*msaaRtv) { (*msaaRtv)->Release(); *msaaRtv = nullptr; }
    if (*msaaDsv) { (*msaaDsv)->Release(); *msaaDsv = nullptr; }
    log("Failed to create MSAA targets, hr 0x", std::hex, hr);
  }
  if (colorMsaa) colorMsaa->Release();
  if (depthMsaa) depthMsaa->Release();
  device->Release();
  colorResource->Release();
  depthResource->Release();
  return SUCCEEDED(hr);
}

void flushDirtyShadows(ID3D11DeviceContext* pContext);

/** Metadata */
static const GUID IID_StagingShadowResource = {0xe2728d91,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};

struct ATFIX_RESOURCE_INFO {
  D3D11_RESOURCE_DIMENSION Dim;
  DXGI_FORMAT Format;
  uint32_t Width;
  uint32_t Height;
  uint32_t Depth;
  uint32_t Layers;
  uint32_t Mips;
  D3D11_USAGE Usage;
  uint32_t BindFlags;
  uint32_t MiscFlags;
  uint32_t CPUFlags;
};

void* ptroffset(void* base, ptrdiff_t offset) {
  auto address = reinterpret_cast<uintptr_t>(base) + offset;
  return reinterpret_cast<void*>(address);
}

uint32_t getFormatPixelSize(
        DXGI_FORMAT               Format) {
  struct FormatRange {
    DXGI_FORMAT MinFormat;
    DXGI_FORMAT MaxFormat;
    uint32_t FormatSize;
  };

  static const std::array<FormatRange, 7> s_ranges = {{
    { DXGI_FORMAT_R32G32B32A32_TYPELESS,  DXGI_FORMAT_R32G32B32A32_SINT,    16u },
    { DXGI_FORMAT_R32G32B32_TYPELESS,     DXGI_FORMAT_R32G32B32_SINT,       12u },
    { DXGI_FORMAT_R16G16B16A16_TYPELESS,  DXGI_FORMAT_R32G32_SINT,          8u  },
    { DXGI_FORMAT_R10G10B10A2_TYPELESS,   DXGI_FORMAT_R32_SINT,             4u  },
    { DXGI_FORMAT_B8G8R8A8_UNORM,         DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,  4u  },
    { DXGI_FORMAT_R8G8_TYPELESS,          DXGI_FORMAT_R16_SINT,             2u  },
    { DXGI_FORMAT_R8_TYPELESS,            DXGI_FORMAT_A8_UNORM,             1u  },
  }};

  for (const auto& range : s_ranges) {
    if (Format >= range.MinFormat && Format <= range.MaxFormat)
      return range.FormatSize;
  }

  log("Unhandled format ", Format);
  return 1u;
}

bool getResourceInfo(
        ID3D11Resource*           pResource,
        ATFIX_RESOURCE_INFO*      pInfo) {
  pResource->GetType(&pInfo->Dim);

  switch (pInfo->Dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      ID3D11Buffer* buffer = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&buffer));

      D3D11_BUFFER_DESC desc = { };
      buffer->GetDesc(&desc);
      buffer->Release();

      pInfo->Format = DXGI_FORMAT_UNKNOWN;
      pInfo->Width = desc.ByteWidth;
      pInfo->Height = 1;
      pInfo->Depth = 1;
      pInfo->Layers = 1;
      pInfo->Mips = 1;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      ID3D11Texture1D* texture = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE1D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      pInfo->Format = desc.Format;
      pInfo->Width = desc.Width;
      pInfo->Height = 1;
      pInfo->Depth = 1;
      pInfo->Layers = desc.ArraySize;
      pInfo->Mips = desc.MipLevels;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      ID3D11Texture2D* texture = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE2D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      pInfo->Format = desc.Format;
      pInfo->Width = desc.Width;
      pInfo->Height = desc.Height;
      pInfo->Depth = 1;
      pInfo->Layers = desc.ArraySize;
      pInfo->Mips = desc.MipLevels;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      ID3D11Texture3D* texture = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE3D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      pInfo->Format = desc.Format;
      pInfo->Width = desc.Width;
      pInfo->Height = desc.Height;
      pInfo->Depth = desc.Depth;
      pInfo->Layers = 1;
      pInfo->Mips = desc.MipLevels;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    default:
      log("Unhandled resource dimension ", pInfo->Dim);
      return false;
  }
}

void recordTransitionMapDetail(ID3D11Resource* resource, UINT subresource,
                               D3D11_MAP mapType, uintptr_t caller,
                               uint64_t nanos) {
  if (!transitionTraceEnabled() || !resource)
    return;
  ATFIX_RESOURCE_INFO info = { };
  if (!getResourceInfo(resource, &info))
    return;
  ReadMapKey key = {
    caller, uint32_t(info.Dim), uint32_t(info.Format), info.Width, info.Height,
    uint32_t(info.Usage), info.BindFlags, info.CPUFlags,
  };
  const uint32_t mip = info.Mips ? subresource % info.Mips : 0;
  const uint64_t width = std::max(info.Width >> mip, 1u);
  const uint64_t height = std::max(info.Height >> mip, 1u);
  const uint64_t depth = std::max(info.Depth >> mip, 1u);
  const uint64_t bytes = width * height * depth * getFormatPixelSize(info.Format);
  std::lock_guard lock(g_transitionReadMapMutex);
  auto& maps = mapType == D3D11_MAP_READ
    ? g_transitionReadMaps : g_transitionWriteMaps;
  auto& stats = maps[key];
  stats.calls++;
  stats.nanos += nanos;
  stats.estimatedBytes += bytes;
  stats.resources.insert(reinterpret_cast<uintptr_t>(resource));
}

D3D11_BOX getResourceBox(
  const ATFIX_RESOURCE_INFO*      pInfo,
        UINT                      Subresource) {
  uint32_t mip = Subresource % pInfo->Mips;

  uint32_t w = std::max(pInfo->Width >> mip, 1u);
  uint32_t h = std::max(pInfo->Height >> mip, 1u);
  uint32_t d = std::max(pInfo->Depth >> mip, 1u);

  return D3D11_BOX { 0, 0, 0, w, h, d };
}

bool isImmediatecontext(
        ID3D11DeviceContext*      pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

bool isCpuWritableResource(
  const ATFIX_RESOURCE_INFO*      pInfo) {
  return (pInfo->Usage == D3D11_USAGE_STAGING || pInfo->Usage == D3D11_USAGE_DYNAMIC)
      && (pInfo->CPUFlags & D3D11_CPU_ACCESS_WRITE)
      && (pInfo->Layers == 1)
      && (pInfo->Mips == 1);
}

bool isCpuReadableResource(
  const ATFIX_RESOURCE_INFO*      pInfo) {
  return (pInfo->Usage == D3D11_USAGE_STAGING)
      && (pInfo->CPUFlags & D3D11_CPU_ACCESS_READ)
      && (pInfo->Layers == 1)
      && (pInfo->Mips == 1);
}

ID3D11Resource* createShadowResourceLocked(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pBaseResource) {
  auto procs = getContextProcs(pContext);

  ID3D11Device* device = nullptr;
  pContext->GetDevice(&device);

  ATFIX_RESOURCE_INFO resourceInfo = { };
  getResourceInfo(pBaseResource, &resourceInfo);

  ID3D11Resource* shadowResource = nullptr;
  HRESULT hr;

  switch (resourceInfo.Dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      ID3D11Buffer* buffer = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&buffer));

      D3D11_BUFFER_DESC desc = { };
      buffer->GetDesc(&desc);
      buffer->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
      desc.StructureByteStride = 0;

      ID3D11Buffer* shadowBuffer = nullptr;
      hr = device->CreateBuffer(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      ID3D11Texture1D* texture = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE1D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

      ID3D11Texture1D* shadowBuffer = nullptr;
      hr = device->CreateTexture1D(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      ID3D11Texture2D* texture = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE2D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

      ID3D11Texture2D* shadowBuffer = nullptr;
      hr = device->CreateTexture2D(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      ID3D11Texture3D* texture = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE3D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

      ID3D11Texture3D* shadowBuffer = nullptr;
      hr = device->CreateTexture3D(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    default:
      log("Unhandled resource dimension ", resourceInfo.Dim);
      hr = E_INVALIDARG;
  }

  if (SUCCEEDED(hr)) {
    procs->CopyResource(pContext, shadowResource, pBaseResource);
    pBaseResource->SetPrivateDataInterface(IID_StagingShadowResource, shadowResource);
  } else
    log("Failed to create shadow resource, hr ", std::hex, hr);

  device->Release();
  return shadowResource;
}

ID3D11Resource* getShadowResourceLocked(
        ID3D11Resource*           pBaseResource) {
  ID3D11Resource* shadowResource = nullptr;
  UINT resultSize = sizeof(shadowResource);
  
  if (SUCCEEDED(pBaseResource->GetPrivateData(IID_StagingShadowResource, &resultSize, &shadowResource)))
    return shadowResource;

  return nullptr;
}

ID3D11Resource* getShadowResource(
        ID3D11Resource*           pBaseResource) {
  std::lock_guard lock(g_globalMutex);
  return getShadowResourceLocked(pBaseResource);
}

bool isMutableFontAtlas(ID3D11Resource* resource) {
  ATFIX_RESOURCE_INFO info = { };
  return resource && getResourceInfo(resource, &info) &&
    info.Dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D &&
    info.Usage == D3D11_USAGE_DYNAMIC &&
    info.Width == 512 && info.Height == 512;
}

ID3D11Resource* getOrCreateShadowResource(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pBaseResource) {
  std::lock_guard lock(g_globalMutex);
  ID3D11Resource* shadowResource = getShadowResourceLocked(pBaseResource);

  if (!shadowResource)
    shadowResource = createShadowResourceLocked(pContext, pBaseResource);

  return shadowResource;
}

void updateViewShadowResource(
        ID3D11DeviceContext*      pContext,
        ID3D11View*               pView) {
  auto procs = getContextProcs(pContext);

  ID3D11Resource* baseResource;
  pView->GetResource(&baseResource);

  ID3D11Resource* shadowResource = getShadowResource(baseResource);

  if (shadowResource) {
    ATFIX_RESOURCE_INFO resourceInfo = { };
    getResourceInfo(baseResource, &resourceInfo);

    uint32_t mipLevel = 0;
    uint32_t layerIndex = 0;
    uint32_t layerCount = 1;

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;

    if (SUCCEEDED(pView->QueryInterface(IID_PPV_ARGS(&rtv)))) {
      D3D11_RENDER_TARGET_VIEW_DESC desc = { };
      rtv->GetDesc(&desc);
      rtv->Release();

      switch (desc.ViewDimension) {
        case D3D11_RTV_DIMENSION_TEXTURE1D:
          mipLevel = desc.Texture1D.MipSlice;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
          mipLevel = desc.Texture1DArray.MipSlice;
          layerIndex = desc.Texture1DArray.FirstArraySlice;
          layerCount = desc.Texture1DArray.ArraySize;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE2D:
          mipLevel = desc.Texture2D.MipSlice;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
          mipLevel = desc.Texture2DArray.MipSlice;
          layerIndex = desc.Texture2DArray.FirstArraySlice;
          layerCount = desc.Texture2DArray.ArraySize;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE3D:
          mipLevel = desc.Texture3D.MipSlice;
          break;

        default:
          log("Unhandled RTV dimension ", desc.ViewDimension);
      }
    } else if (SUCCEEDED(pView->QueryInterface(IID_PPV_ARGS(&uav)))) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC desc = { };
      uav->GetDesc(&desc);
      uav->Release();

      switch (desc.ViewDimension) {
        case D3D11_UAV_DIMENSION_BUFFER:
          break;

        case D3D11_UAV_DIMENSION_TEXTURE1D:
          mipLevel = desc.Texture1D.MipSlice;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
          mipLevel = desc.Texture1DArray.MipSlice;
          layerIndex = desc.Texture1DArray.FirstArraySlice;
          layerCount = desc.Texture1DArray.ArraySize;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE2D:
          mipLevel = desc.Texture2D.MipSlice;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
          mipLevel = desc.Texture2DArray.MipSlice;
          layerIndex = desc.Texture2DArray.FirstArraySlice;
          layerCount = desc.Texture2DArray.ArraySize;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE3D:
          mipLevel = desc.Texture3D.MipSlice;
          break;

        default:
          log("Unhandled UAV dimension ", desc.ViewDimension);
      }
    } else {
      log("Unhandled view type");
    }

    for (uint32_t i = 0; i < layerCount; i++) {
      uint32_t subresource = D3D11CalcSubresource(mipLevel, layerIndex + i, resourceInfo.Mips);

      procs->CopySubresourceRegion(pContext,
        shadowResource, subresource, 0, 0, 0,
        baseResource,   subresource, nullptr);
    }

    shadowResource->Release();
  }

  baseResource->Release();
}

void updateRtvShadowResources(
        ID3D11DeviceContext*      pContext) {
  std::array<ID3D11RenderTargetView*, 8> rtvs;
  pContext->OMGetRenderTargets(rtvs.size(), rtvs.data(), nullptr);

  for (ID3D11RenderTargetView* rtv : rtvs) {
    if (rtv) {
      updateViewShadowResource(pContext, rtv);
      rtv->Release();
    }
  }
}

void updateUavShadowResources(
        ID3D11DeviceContext*      pContext) {
  std::array<ID3D11UnorderedAccessView*, 8> uavs;
  pContext->CSGetUnorderedAccessViews(0, uavs.size(), uavs.data());

  for (ID3D11UnorderedAccessView* uav : uavs) {
    if (uav) {
      updateViewShadowResource(pContext, uav);
      uav->Release();
    }
  }
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateBuffer(
        ID3D11Device*             pDevice,
  const D3D11_BUFFER_DESC*        pDesc,
  const D3D11_SUBRESOURCE_DATA*   pData,
        ID3D11Buffer**            ppBuffer) {
  TransitionTimer transitionTimer(g_transitionCreate);
  auto procs = getDeviceProcs(pDevice);
  D3D11_BUFFER_DESC desc;
  std::array<float, 12> scaledFullscreenQuad = { };
  bool createScaledDialogQuad = false;

  if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
    desc = *pDesc;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    pDesc = &desc;
  }

  const UINT mainWidth = g_mainRtWidth.load(std::memory_order_relaxed);
  const UINT mainHeight = g_mainRtHeight.load(std::memory_order_relaxed);
  if (mainWidth > 1920 && mainHeight > 1080 && pDesc && pData &&
      pData->pSysMem && pDesc->ByteWidth == sizeof(scaledFullscreenQuad) &&
      (pDesc->BindFlags & D3D11_BIND_VERTEX_BUFFER)) {
    std::memcpy(scaledFullscreenQuad.data(), pData->pSysMem,
                sizeof(scaledFullscreenQuad));
    const std::array<float, 12> originalFullscreenQuad = {
      0.0f, 1080.0f, 0.0f,
      1920.0f, 1080.0f, 0.0f,
      0.0f, 0.0f, 0.0f,
      1920.0f, 0.0f, 0.0f,
    };
    if (scaledFullscreenQuad == originalFullscreenQuad) {
      scaledFullscreenQuad[1] = static_cast<float>(mainHeight);
      scaledFullscreenQuad[3] = static_cast<float>(mainWidth);
      scaledFullscreenQuad[4] = static_cast<float>(mainHeight);
      scaledFullscreenQuad[9] = static_cast<float>(mainWidth);
      createScaledDialogQuad = true;
    }
  }

  // Gate-hold write path: an 880 receiver material created WITH initial data
  // (transient per-frame cb pattern) bypasses both the Map and
  // UpdateSubresource hooks — hold the shadow gate open in the initial
  // payload too.
  D3D11_SUBRESOURCE_DATA gateInit;
  uint8_t gateInitCopy[880];
  if (cutinGateHoldEnabled() && arlandInCinematicBattle() &&
      pDesc && pData && pData->pSysMem &&
      (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
      pDesc->ByteWidth == 880) {
    std::memcpy(gateInitCopy, pData->pSysMem, 880);
    if (gateHoldPatch(gateInitCopy, 880)) {
      gateInit = *pData;
      gateInit.pSysMem = gateInitCopy;
      pData = &gateInit;
    }
  }

  const HRESULT hr = procs->CreateBuffer(pDevice, pDesc, pData, ppBuffer);
  if (createScaledDialogQuad && SUCCEEDED(hr) && ppBuffer && *ppBuffer) {
    D3D11_SUBRESOURCE_DATA scaledData = *pData;
    scaledData.pSysMem = scaledFullscreenQuad.data();
    ID3D11Buffer* scaledBuffer = nullptr;
    if (SUCCEEDED(procs->CreateBuffer(
          pDevice, pDesc, &scaledData, &scaledBuffer)) && scaledBuffer) {
      (*ppBuffer)->SetPrivateDataInterface(
        IID_DialogScaledVertexBuffer, scaledBuffer);
      scaledBuffer->Release();
      log("Created targeted dialogue quad companion at ",
          std::dec, mainWidth, "x", mainHeight);
    } else {
      log("Failed to create targeted dialogue quad companion at ",
          std::dec, mainWidth, "x", mainHeight);
    }
  }
  if (resolutionTraceEnabled() && SUCCEEDED(hr) && ppBuffer && *ppBuffer &&
      pDesc && pData && pData->pSysMem && pDesc->ByteWidth <= 64 &&
      (pDesc->BindFlags & D3D11_BIND_VERTEX_BUFFER)) {
    (*ppBuffer)->SetPrivateData(
      IID_ResolutionBufferData, pDesc->ByteWidth, pData->pSysMem);
  }
  if (cutinBlobEnabled() && SUCCEEDED(hr) && ppBuffer && *ppBuffer &&
      pDesc && pData && pData->pSysMem && pDesc->ByteWidth >= 16 &&
      (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER))
    snapCbWrite(*ppBuffer, pData->pSysMem, pDesc->ByteWidth);
  return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateVertexShader(
        ID3D11Device*             pDevice,
  const void*                     pShaderBytecode,
        SIZE_T                    BytecodeLength,
        ID3D11ClassLinkage*       pClassLinkage,
        ID3D11VertexShader**      ppVertexShader) {
  auto procs = getDeviceProcs(pDevice);
  const HRESULT hr = procs->CreateVertexShader(pDevice, pShaderBytecode,
    BytecodeLength, pClassLinkage, ppVertexShader);
  return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreatePixelShader(
        ID3D11Device*             pDevice,
  const void*                     pShaderBytecode,
        SIZE_T                    BytecodeLength,
        ID3D11ClassLinkage*       pClassLinkage,
        ID3D11PixelShader**       ppPixelShader) {
  auto procs = getDeviceProcs(pDevice);
  const HRESULT hr = procs->CreatePixelShader(pDevice, pShaderBytecode,
    BytecodeLength, pClassLinkage, ppPixelShader);
  return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateDeferredContext(
        ID3D11Device*             pDevice,
        UINT                      Flags,
        ID3D11DeviceContext**     ppDeferredContext) {
  auto procs = getDeviceProcs(pDevice);
  HRESULT hr = procs->CreateDeferredContext(pDevice, Flags, ppDeferredContext);

  if (SUCCEEDED(hr) && ppDeferredContext)
    hookContext(*ppDeferredContext);

  return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateTexture1D(
        ID3D11Device*             pDevice,
  const D3D11_TEXTURE1D_DESC*     pDesc,
  const D3D11_SUBRESOURCE_DATA*   pData,
        ID3D11Texture1D**         ppTexture) {
  TransitionTimer transitionTimer(g_transitionCreate);
  auto procs = getDeviceProcs(pDevice);
  D3D11_TEXTURE1D_DESC desc;

  if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
    desc = *pDesc;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    pDesc = &desc;
  }

  return procs->CreateTexture1D(pDevice, pDesc, pData, ppTexture);
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateTexture2D(
        ID3D11Device*             pDevice,
  const D3D11_TEXTURE2D_DESC*     pDesc,
  const D3D11_SUBRESOURCE_DATA*   pData,
        ID3D11Texture2D**         ppTexture) {
  TransitionTimer transitionTimer(g_transitionCreate);
  auto procs = getDeviceProcs(pDevice);
  D3D11_TEXTURE2D_DESC desc;
  D3D11_TEXTURE2D_DESC originalDesc = { };
  const bool haveOriginalDesc = pDesc != nullptr;
  bool createShadowTwin = false;

  if (pDesc) {
    originalDesc = *pDesc;
    desc = *pDesc;
    bool changed = false;

    if (desc.Usage == D3D11_USAGE_STAGING) {
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
      changed = true;
    }

    UINT mainWidth = g_mainRtWidth.load(std::memory_order_relaxed);
    UINT mainHeight = g_mainRtHeight.load(std::memory_order_relaxed);
    if (!mainWidth && (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
      // The trilogy creates its main depth target before the hard-coded
      // 1920x1080 auxiliary targets. Record 1080p too so MSAA can identify
      // the main scene, but only resize later targets for higher resolutions.
      const UINT originalWidth =
        g_originalSwapWidth.load(std::memory_order_relaxed);
      const UINT originalHeight =
        g_originalSwapHeight.load(std::memory_order_relaxed);
      const bool matchesOriginalSwap = originalWidth && originalHeight &&
        desc.Width == originalWidth && desc.Height == originalHeight;
      const bool knownMainShape = desc.Width >= 1920 && desc.Height >= 1080 &&
        static_cast<uint64_t>(desc.Width) * 9 ==
          static_cast<uint64_t>(desc.Height) * 16;
      if (matchesOriginalSwap || knownMainShape) {
        UINT overrideWidth = 0;
        UINT overrideHeight = 0;
        if (configuredResolution(&overrideWidth, &overrideHeight)) {
          desc.Width = overrideWidth;
          desc.Height = overrideHeight;
          changed = true;
        }
        g_mainRtWidth.store(desc.Width, std::memory_order_relaxed);
        g_mainRtHeight.store(desc.Height, std::memory_order_relaxed);
        mainWidth = desc.Width;
        mainHeight = desc.Height;
        log("Detected main render size ", std::dec, mainWidth, "x", mainHeight);
      }
    } else if (mainWidth > 1920 && mainHeight > 1080 && !pData) {
      const bool fullSizeTarget =
        (desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) &&
        desc.Width == 1920 && desc.Height == 1080;
      const bool halfSizeBlurTarget =
        (desc.BindFlags & D3D11_BIND_RENDER_TARGET) &&
        (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
        desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
        desc.Width == 960 && desc.Height == 540 &&
        desc.MipLevels == 1 && desc.ArraySize == 1 &&
        desc.SampleDesc.Count == 1;
      if (fullSizeTarget || halfSizeBlurTarget) {
        desc.Width = halfSizeBlurTarget ? mainWidth / 2 : mainWidth;
        desc.Height = halfSizeBlurTarget ? mainHeight / 2 : mainHeight;
        changed = true;
        log("Resizing hard-coded ",
            halfSizeBlurTarget ? "960x540 blur target" : "1920x1080 target",
            " to ", std::dec, desc.Width, "x", desc.Height);
      }
    }

    // Opt-in shadow-map upscale, twin-allocation flavour: the engine's two
    // 1024x1024 R24G8 shadow maps (caster A and receiver B, §8 two-map
    // architecture; §33a probe: exactly two such textures exist) are left
    // COMPLETELY untouched so every engine-side size/memory assumption stays
    // valid. Eligible hosts get a separate mod-owned enlarged twin created
    // below (after the host), and the caster DSV / receiver SRV / A->B copy
    // are redirected onto the twins. Anything ambiguous (initial data,
    // staging/CPU-accessible, mips, arrays, MSAA, misc flags) DECLINES the
    // twin and is logged; that host simply keeps the vanilla 1024 path.
    if (shadowMapResolution() > 1024 &&
        originalDesc.Width == 1024 && originalDesc.Height == 1024 &&
        originalDesc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
      createShadowTwin = !pData &&
        originalDesc.Usage == D3D11_USAGE_DEFAULT &&
        originalDesc.CPUAccessFlags == 0 &&
        originalDesc.MiscFlags == 0 &&
        originalDesc.MipLevels == 1 && originalDesc.ArraySize == 1 &&
        originalDesc.SampleDesc.Count == 1;
      if (!createShadowTwin)
        log("SHADOWRES DECLINE 1024x1024 R24G8 candidate:",
            " data=", pData != nullptr,
            " usage=", std::dec, originalDesc.Usage,
            " cpu=0x", std::hex, originalDesc.CPUAccessFlags,
            " misc=0x", originalDesc.MiscFlags,
            " bind=0x", originalDesc.BindFlags,
            " mips=", std::dec, originalDesc.MipLevels,
            " array=", originalDesc.ArraySize,
            " samples=", originalDesc.SampleDesc.Count);
    }

    if (changed)
      pDesc = &desc;
  }

  const HRESULT hr = procs->CreateTexture2D(pDevice, pDesc, pData, ppTexture);
  if (createShadowTwin && SUCCEEDED(hr) && ppTexture && *ppTexture) {
    const UINT shadowRes = shadowMapResolution();
    D3D11_TEXTURE2D_DESC twinDesc = originalDesc;
    twinDesc.Width = shadowRes;
    twinDesc.Height = shadowRes;
    ID3D11Texture2D* twin = nullptr;
    const HRESULT twinHr =
      procs->CreateTexture2D(pDevice, &twinDesc, nullptr, &twin);
    if (SUCCEEDED(twinHr) && twin) {
      const UINT marker = 1;
      twin->SetPrivateData(IID_ShadowResResized, sizeof(marker), &marker);
      (*ppTexture)->SetPrivateDataInterface(IID_ShadowResTwin, twin);
      twin->Release();   // host private data keeps the twin alive
      {
        std::lock_guard lock(g_twinSrvNegMutex);
        g_twinSrvNegative.clear();   // new generation: re-probe SRVs
      }
      log("SHADOWRES twin created ", std::dec, shadowRes, "x", shadowRes,
          " for host=", *ppTexture, " bind=0x", std::hex,
          originalDesc.BindFlags);
    } else {
      // Fail-safe: no twin means this host silently keeps the vanilla path.
      log("SHADOWRES twin creation FAILED hr=0x", std::hex, twinHr,
          " (falling back to 1024 for this map)");
    }
  }
  if (resolutionTraceEnabled() && SUCCEEDED(hr) && ppTexture && *ppTexture &&
      haveOriginalDesc) {
    D3D11_TEXTURE2D_DESC actual = { };
    (*ppTexture)->GetDesc(&actual);
    const bool resizedFull = originalDesc.Width == 1920 &&
      originalDesc.Height == 1080 &&
      (actual.Width != originalDesc.Width || actual.Height != originalDesc.Height);
    const bool resizedHalf = originalDesc.Width == 960 &&
      originalDesc.Height == 540 &&
      (actual.Width != originalDesc.Width || actual.Height != originalDesc.Height);
    if (resizedFull || resizedHalf) {
      ResolutionTraceState trace = { };
      trace.id = g_nextResolutionTraceId.fetch_add(1, std::memory_order_relaxed);
      (*ppTexture)->SetPrivateData(IID_ResolutionTrace, sizeof(trace), &trace);
      log("RES create resized candidate=", trace.id,
          " resource=", *ppTexture,
          " original=", originalDesc.Width, "x", originalDesc.Height,
          " actual=", actual.Width, "x", actual.Height,
          " format=", actual.Format,
          " bind=0x", std::hex, actual.BindFlags);
    }
  }
  if (SUCCEEDED(hr) && ppTexture && *ppTexture &&
      isUnscaled1080Resource(*ppTexture)) {
    ResolutionTraceState trace = { };
    resolutionTraceState(*ppTexture, &trace);
    D3D11_TEXTURE2D_DESC actual = { };
    (*ppTexture)->GetDesc(&actual);
    log("RES create candidate=", trace.id,
        " resource=", *ppTexture,
        " format=", actual.Format,
        " usage=", actual.Usage,
        " bind=0x", std::hex, actual.BindFlags,
        " cpu=0x", actual.CPUAccessFlags,
        " misc=0x", actual.MiscFlags,
        " mips=", std::dec, actual.MipLevels,
        " array=", actual.ArraySize,
        " samples=", actual.SampleDesc.Count,
        " initial_data=", pData != nullptr);
  }
  return hr;
}

void STDMETHODCALLTYPE ID3D11DeviceContext_RSSetViewports(
        ID3D11DeviceContext* pContext,
        UINT                 NumViewports,
  const D3D11_VIEWPORT*      pViewports) {
  auto procs = getContextProcs(pContext);
  RasterState* state = getRasterState(pContext);
  state->dirty.store(true, std::memory_order_release);
  if (NumViewports && pViewports) {
    state->viewportWidth.store(static_cast<UINT>(pViewports[0].Width), std::memory_order_relaxed);
    state->viewportHeight.store(static_cast<UINT>(pViewports[0].Height), std::memory_order_relaxed);
  }
  procs->RSSetViewports(pContext, NumViewports, pViewports);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_RSSetScissorRects(
        ID3D11DeviceContext* pContext,
        UINT                 NumRects,
  const D3D11_RECT*          pRects) {
  auto procs = getContextProcs(pContext);
  RasterState* state = getRasterState(pContext);
  state->dirty.store(true, std::memory_order_release);
  if (NumRects && pRects) {
    state->scissorWidth.store(static_cast<UINT>(pRects[0].right - pRects[0].left), std::memory_order_relaxed);
    state->scissorHeight.store(static_cast<UINT>(pRects[0].bottom - pRects[0].top), std::memory_order_relaxed);
  }
  procs->RSSetScissorRects(pContext, NumRects, pRects);
}

void updateViewportScissor(ID3D11DeviceContext* pContext) {
  RasterState* state = getRasterState(pContext);
  if (!state->dirty.exchange(false, std::memory_order_acq_rel))
    return;

  UINT viewportCount = 1;
  UINT scissorCount = 1;
  D3D11_VIEWPORT viewport = { };
  D3D11_RECT scissor = { };
  pContext->RSGetViewports(&viewportCount, &viewport);
  pContext->RSGetScissorRects(&scissorCount, &scissor);
  const bool fullSizeViewport = viewportCount == 1 &&
    viewport.TopLeftX == 0.0f && viewport.TopLeftY == 0.0f &&
    viewport.Width == 1920.0f && viewport.Height == 1080.0f;
  const bool halfSizeViewport = viewportCount == 1 &&
    viewport.TopLeftX == 0.0f && viewport.TopLeftY == 0.0f &&
    viewport.Width == 960.0f && viewport.Height == 540.0f;
  const bool fullSizeScissor = scissorCount == 1 &&
    scissor.left == 0 && scissor.top == 0 &&
    scissor.right == 1920 && scissor.bottom == 1080;
  const bool halfSizeScissor = scissorCount == 1 &&
    scissor.left == 0 && scissor.top == 0 &&
    scissor.right == 960 && scissor.bottom == 540;
  // The engine sizes the shadow caster pass viewport from its own texture
  // metadata (still 1024 when the map is enlarged D3D-side). MinDepth/MaxDepth
  // (the caster's 0.5..1.0 depth remap, §8) are preserved by only rewriting
  // Width/Height.
  const UINT shadowRes = shadowMapResolution();
  const bool shadowSizeViewport = shadowRes > 1024 && viewportCount == 1 &&
    viewport.TopLeftX == 0.0f && viewport.TopLeftY == 0.0f &&
    viewport.Width == 1024.0f && viewport.Height == 1024.0f;
  const bool shadowSizeScissor = shadowRes > 1024 && scissorCount == 1 &&
    scissor.left == 0 && scissor.top == 0 &&
    scissor.right == 1024 && scissor.bottom == 1024;
  if (!fullSizeViewport && !halfSizeViewport &&
      !fullSizeScissor && !halfSizeScissor &&
      !shadowSizeViewport && !shadowSizeScissor)
    return;

  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11Resource* resource = nullptr;
  pContext->OMGetRenderTargets(1, &rtv, &dsv);
  if (rtv) {
    rtv->GetResource(&resource);
    rtv->Release();
  }
  if (dsv) {
    if (!resource)
      dsv->GetResource(&resource);
    dsv->Release();
  }
  bool resizeViewport = false;
  bool resizeScissor = false;
  if (resource) {
    ID3D11Texture2D* texture = nullptr;
    const HRESULT hr = resource->QueryInterface(IID_PPV_ARGS(&texture));
    resource->Release();
    if (SUCCEEDED(hr)) {
      D3D11_TEXTURE2D_DESC desc = { };
      texture->GetDesc(&desc);
      // Only a texture we resized at creation counts as the shadow target;
      // a native target that merely matches the enlarged size is left alone.
      const bool shadowTarget = (shadowSizeViewport || shadowSizeScissor) &&
        desc.Width == shadowRes && desc.Height == shadowRes &&
        desc.Format == DXGI_FORMAT_R24G8_TYPELESS &&
        isShadowResResized(texture);
      texture->Release();
      const UINT mainWidth = g_mainRtWidth.load(std::memory_order_relaxed);
      const UINT mainHeight = g_mainRtHeight.load(std::memory_order_relaxed);
      const bool fullSizeTarget = desc.Width == mainWidth &&
        desc.Height == mainHeight;
      const bool halfSizeTarget = desc.Width == mainWidth / 2 &&
        desc.Height == mainHeight / 2;
      resizeViewport = (fullSizeViewport && fullSizeTarget) ||
        (halfSizeViewport && halfSizeTarget) ||
        (shadowSizeViewport && shadowTarget);
      resizeScissor = (fullSizeScissor && fullSizeTarget) ||
        (halfSizeScissor && halfSizeTarget) ||
        (shadowSizeScissor && shadowTarget);
      if (resizeViewport) {
        viewport.Width = static_cast<FLOAT>(desc.Width);
        viewport.Height = static_cast<FLOAT>(desc.Height);
      }
      if (resizeScissor) {
        scissor.right = static_cast<LONG>(desc.Width);
        scissor.bottom = static_cast<LONG>(desc.Height);
      }
      if (shadowTarget && (resizeViewport || resizeScissor)) {
        static std::atomic<uint32_t> vpLogs{0};
        const uint32_t n = vpLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || n % 1024 == 0)
          log("SHADOWRES caster viewport/scissor 1024 -> ", std::dec,
              desc.Width, " (vp=", resizeViewport,
              " sc=", resizeScissor, ")");
      }
    }
  }

  auto procs = getContextProcs(pContext);
  if (resizeViewport)
    procs->RSSetViewports(pContext, 1, &viewport);
  if (resizeScissor)
    procs->RSSetScissorRects(pContext, 1, &scissor);
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateTexture3D(
        ID3D11Device*             pDevice,
  const D3D11_TEXTURE3D_DESC*     pDesc,
  const D3D11_SUBRESOURCE_DATA*   pData,
        ID3D11Texture3D**         ppTexture) {
  TransitionTimer transitionTimer(g_transitionCreate);
  auto procs = getDeviceProcs(pDevice);
  D3D11_TEXTURE3D_DESC desc;

  if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
    desc = *pDesc;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    pDesc = &desc;
  }

  return procs->CreateTexture3D(pDevice, pDesc, pData, ppTexture);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_ClearRenderTargetView(
        ID3D11DeviceContext*      pContext,
        ID3D11RenderTargetView*   pRTV,
  const FLOAT                     pColor[4]) {
  auto procs = getContextProcs(pContext);
  ID3D11RenderTargetView* msaa = getMSAAObject<ID3D11RenderTargetView>(pRTV);
  procs->ClearRenderTargetView(pContext, msaa ? msaa : pRTV, pColor);
  if (msaa)
    msaa->Release();

  if (pRTV)
    updateViewShadowResource(pContext, pRTV);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_ClearDepthStencilView(
        ID3D11DeviceContext*      pContext,
        ID3D11DepthStencilView*   pDSV,
        UINT                      ClearFlags,
        FLOAT                     Depth,
        UINT8                     Stencil) {
  auto procs = getContextProcs(pContext);
  ID3D11DepthStencilView* msaa = getMSAAObject<ID3D11DepthStencilView>(pDSV);
  procs->ClearDepthStencilView(pContext, msaa ? msaa : pDSV,
    ClearFlags, Depth, Stencil);
  if (msaa)
    msaa->Release();
  // Shadow-res twin: keep the enlarged caster map in lockstep with the
  // engine's own (untouched) map — the engine clears its map at the start of
  // every shadow pass.
  if (shadowMapResolution() > 1024 && pDSV) {
    ID3D11DepthStencilView* twinDsv = getShadowResTwinDsv(pDSV);
    if (twinDsv) {
      procs->ClearDepthStencilView(pContext, twinDsv,
        ClearFlags, Depth, Stencil);
      twinDsv->Release();
    }
  }
  cutinShadowMapCleared(pContext, pDSV);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_ClearUnorderedAccessViewFloat(
        ID3D11DeviceContext*      pContext,
        ID3D11UnorderedAccessView* pUAV,
  const FLOAT                     pColor[4]) {
  auto procs = getContextProcs(pContext);
  procs->ClearUnorderedAccessViewFloat(pContext, pUAV, pColor);

  if (pUAV)
    updateViewShadowResource(pContext, pUAV);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_ClearUnorderedAccessViewUint(
        ID3D11DeviceContext*      pContext,
        ID3D11UnorderedAccessView* pUAV,
  const UINT                      pColor[4]) {
  auto procs = getContextProcs(pContext);
  procs->ClearUnorderedAccessViewUint(pContext, pUAV, pColor);

  if (pUAV)
    updateViewShadowResource(pContext, pUAV);
}

HRESULT tryCpuCopy(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pDstResource,
        UINT                      DstSubresource,
        UINT                      DstX,
        UINT                      DstY,
        UINT                      DstZ,
        ID3D11Resource*           pSrcResource,
        UINT                      SrcSubresource,
  const D3D11_BOX*                pSrcBox) {
  if (isMutableFontAtlas(pSrcResource))
    return E_NOTIMPL;

  auto procs = getContextProcs(pContext);
  ATFIX_RESOURCE_INFO dstInfo = { };
  getResourceInfo(pDstResource, &dstInfo);

  if (!isCpuWritableResource(&dstInfo))
    return E_INVALIDARG;

  /* Compute source region for the given copy */
  ATFIX_RESOURCE_INFO srcInfo = { };
  getResourceInfo(pSrcResource, &srcInfo);

  D3D11_BOX srcBox = getResourceBox(&srcInfo, SrcSubresource);
  D3D11_BOX dstBox = getResourceBox(&dstInfo, DstSubresource);

  if (pSrcBox)
    srcBox = *pSrcBox;

  uint32_t w = std::min(srcBox.right - srcBox.left, dstBox.right - DstX);
  uint32_t h = std::min(srcBox.bottom - srcBox.top, dstBox.bottom - DstY);
  uint32_t d = std::min(srcBox.back - srcBox.front, dstBox.back - DstZ);

  srcBox = { srcBox.left,     srcBox.top,     srcBox.front,
             srcBox.left + w, srcBox.top + h, srcBox.front + d };

  dstBox = { DstX,     DstY,     DstZ,
             DstX + w, DstY + h, DstZ + d };

  if (!w || !h || !d)
    return S_OK;

  /* Check if we can map the destination resource immediately. The
   * engine creates all buffers that cause the severe stalls right
   * before mapping them, so this should succeed. */
  D3D11_MAPPED_SUBRESOURCE dstSr;
  D3D11_MAPPED_SUBRESOURCE srcSr;
  HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;

  if (dstInfo.Usage == D3D11_USAGE_DYNAMIC) {
    /* Don't bother with dynamic images etc., haven't seen a situation where it's relevant */
    if (dstInfo.Dim == D3D11_RESOURCE_DIMENSION_BUFFER && w == dstInfo.Width)
      hr = procs->Map(pContext, pDstResource, DstSubresource, D3D11_MAP_WRITE_DISCARD, 0, &dstSr);
  } else {
    hr = procs->Map(pContext, pDstResource, DstSubresource, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &dstSr);
  }

  if (FAILED(hr)) {
    if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
      log("Failed to map destination resource, hr 0x", std::hex, hr);
      log("Resource dim ", dstInfo.Dim, ", size ", dstInfo.Width , "x", dstInfo.Height, ", usage ", dstInfo.Usage);
    }
    return hr;
  }

  ID3D11Resource* shadowResource = nullptr;

  if (!isCpuReadableResource(&srcInfo)) {
    shadowResource = getOrCreateShadowResource(pContext, pSrcResource);
    hr = procs->Map(pContext, shadowResource, SrcSubresource, D3D11_MAP_READ, 0, &srcSr);

    if (FAILED(hr)) {
      shadowResource->Release();

      log("Failed to map shadow resource, hr 0x", std::hex, hr);
      procs->Unmap(pContext, pDstResource, DstSubresource);
      return hr;
    }
  } else {
    hr = procs->Map(pContext, pSrcResource, SrcSubresource, D3D11_MAP_READ, 0, &srcSr);

    if (FAILED(hr)) {
      log("Failed to map source resource, hr 0x", std::hex, hr);
      log("Resource dim ", srcInfo.Dim, ", size ", srcInfo.Width , "x", srcInfo.Height, ", usage ", srcInfo.Usage);
      procs->Unmap(pContext, pDstResource, DstSubresource);
      return hr;
    }
  }

  /* Do the copy */
  if (dstInfo.Dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    std::memcpy(
      ptroffset(dstSr.pData, dstBox.left),
      ptroffset(srcSr.pData, srcBox.left), w);
  } else {
    uint32_t formatSize = getFormatPixelSize(dstInfo.Format);

    for (uint32_t z = 0; z < d; z++) {
      for (uint32_t y = 0; y < h; y++) {
        uint32_t dstOffset = (dstBox.left) * formatSize
                           + (dstBox.top + y) * dstSr.RowPitch
                           + (dstBox.front + z) * dstSr.DepthPitch;
        uint32_t srcOffset = (srcBox.left) * formatSize
                           + (srcBox.top + y) * srcSr.RowPitch
                           + (srcBox.front + z) * srcSr.DepthPitch;
        std::memcpy(
          ptroffset(dstSr.pData, dstOffset),
          ptroffset(srcSr.pData, srcOffset),
          w * formatSize);
      }
    }
  }

  procs->Unmap(pContext, pDstResource, DstSubresource);

  if (shadowResource) {
    procs->Unmap(pContext, shadowResource, SrcSubresource);
    shadowResource->Release();
  } else {
    procs->Unmap(pContext, pSrcResource, SrcSubresource);
  }

  return S_OK;
}

void STDMETHODCALLTYPE ID3D11DeviceContext_CopyResource(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pDstResource,
        ID3D11Resource*           pSrcResource) {
  TransitionTimer transitionTimer(g_transitionCopy);
  auto procs = getContextProcs(pContext);

  traceResolutionCopy("resource", pContext, pDstResource, pSrcResource);

  resolveIfMSAA(pContext, pSrcResource);

  ID3D11Resource* dstShadow = getShadowResource(pDstResource);

  // Shadow-res twin: mirror a whole-resource copy between shadow maps onto
  // the equal-sized twins (the engine's own 1024 copy stays untouched).
  if (shadowMapResolution() > 1024) {
    ID3D11Resource* dstTwin = getShadowResTwinResource(pDstResource);
    if (dstTwin) {
      ID3D11Resource* srcTwin = getShadowResTwinResource(pSrcResource);
      if (srcTwin) {
        procs->CopyResource(pContext, dstTwin, srcTwin);
        static std::atomic<uint32_t> mirrorLogs{0};
        const uint32_t n = mirrorLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || n % 4096 == 0)
          log("SHADOWRES mirrored CopyResource on twins");
        srcTwin->Release();
      }
      dstTwin->Release();
    }
  }

  bool needsBaseCopy = true;
  bool needsShadowCopy = true;

  if (isImmediatecontext(pContext)) {
    HRESULT hr = tryCpuCopy(pContext, pDstResource,
      0, 0, 0, 0, pSrcResource, 0, nullptr);
    needsBaseCopy = FAILED(hr);

    if (!needsBaseCopy && dstShadow) {
      hr = tryCpuCopy(pContext, dstShadow,
        0, 0, 0, 0, pSrcResource, 0, nullptr);
      needsShadowCopy = FAILED(hr);
    }
  }

  if (needsBaseCopy || (dstShadow && needsShadowCopy))
    flushDirtyShadows(pContext);

  if (needsBaseCopy)
    procs->CopyResource(pContext, pDstResource, pSrcResource);

  if (dstShadow) {
    if (needsShadowCopy)
      procs->CopyResource(pContext, dstShadow, pSrcResource);

    dstShadow->Release();
  }
}

void STDMETHODCALLTYPE ID3D11DeviceContext_CopySubresourceRegion(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pDstResource,
        UINT                      DstSubresource,
        UINT                      DstX,
        UINT                      DstY,
        UINT                      DstZ,
        ID3D11Resource*           pSrcResource,
        UINT                      SrcSubresource,
  const D3D11_BOX*                pSrcBox) {
  TransitionTimer transitionTimer(g_transitionCopy);
  auto procs = getContextProcs(pContext);

  D3D11_BOX scaledBox = { };
  const UINT mainWidth = g_mainRtWidth.load(std::memory_order_relaxed);
  const UINT mainHeight = g_mainRtHeight.load(std::memory_order_relaxed);
  if (mainWidth > 1920 && mainHeight > 1080 && pSrcBox &&
      DstSubresource == 0 && SrcSubresource == 0 &&
      DstX == 0 && DstY == 0 && DstZ == 0 &&
      pSrcBox->left == 0 && pSrcBox->top == 0 && pSrcBox->front == 0 &&
      pSrcBox->right == 1920 && pSrcBox->bottom == 1080 &&
      pSrcBox->back == 1) {
    D3D11_TEXTURE2D_DESC dstDesc = { };
    D3D11_TEXTURE2D_DESC srcDesc = { };
    if (texture2DDesc(pDstResource, &dstDesc) &&
        texture2DDesc(pSrcResource, &srcDesc) &&
        dstDesc.Width == mainWidth && dstDesc.Height == mainHeight &&
        srcDesc.Width == mainWidth && srcDesc.Height == mainHeight &&
        dstDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
        srcDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
      scaledBox = *pSrcBox;
      scaledBox.right = mainWidth;
      scaledBox.bottom = mainHeight;
      pSrcBox = &scaledBox;
      const UINT marker = 1;
      pDstResource->SetPrivateData(
        IID_DialogSnapshotResource, sizeof(marker), &marker);
      log("Expanded hard-coded dialogue snapshot copy from 1920x1080 to ",
          std::dec, mainWidth, "x", mainHeight);
    }
  }

  // Shadow-res twin: when the engine performs its (untouched, still valid)
  // 1024 A->B shadow-map transfer, mirror it as a full-subresource copy
  // between the enlarged twins so the high-res caster content reaches the
  // twin the receiver samples. Both twins are identical in size, so the
  // null-box copy is always legal; if either side has no twin, nothing
  // happens and the vanilla path stands.
  if (shadowMapResolution() > 1024) {
    ID3D11Resource* dstTwin = getShadowResTwinResource(pDstResource);
    if (dstTwin) {
      ID3D11Resource* srcTwin = getShadowResTwinResource(pSrcResource);
      if (srcTwin) {
        procs->CopySubresourceRegion(pContext,
          dstTwin, 0, 0, 0, 0, srcTwin, 0, nullptr);
        static std::atomic<uint32_t> mirrorLogs{0};
        const uint32_t n = mirrorLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || n % 4096 == 0)
          log("SHADOWRES mirrored A->B copy on twins");
        srcTwin->Release();
      }
      dstTwin->Release();
    }
  }

  traceResolutionCopy("subresource", pContext, pDstResource, pSrcResource);

  resolveIfMSAA(pContext, pSrcResource);

  ID3D11Resource* dstShadow = getShadowResource(pDstResource);

  bool needsBaseCopy = true;
  bool needsShadowCopy = true;

  if (isImmediatecontext(pContext)) {
    HRESULT hr = tryCpuCopy(pContext,
      pDstResource, DstSubresource, DstX, DstY, DstZ,
      pSrcResource, SrcSubresource, pSrcBox);
    needsBaseCopy = FAILED(hr);

    if (!needsBaseCopy && dstShadow) {
      hr = tryCpuCopy(pContext,
        dstShadow,    DstSubresource, DstX, DstY, DstZ,
        pSrcResource, SrcSubresource, pSrcBox);
      needsShadowCopy = FAILED(hr);
    }
  }

  if (needsBaseCopy || (dstShadow && needsShadowCopy))
    flushDirtyShadows(pContext);

  if (needsBaseCopy) {
    procs->CopySubresourceRegion(pContext,
      pDstResource, DstSubresource, DstX, DstY, DstZ,
      pSrcResource, SrcSubresource, pSrcBox);
  }

  if (dstShadow) {
    if (needsShadowCopy) {
      ATFIX_RESOURCE_INFO srcInfo = { };
      getResourceInfo(pSrcResource, &srcInfo);

      procs->CopySubresourceRegion(pContext,
        dstShadow,    DstSubresource, DstX, DstY, DstZ,
        pSrcResource, SrcSubresource, pSrcBox);
    }

    dstShadow->Release();
  }
}

void STDMETHODCALLTYPE ID3D11DeviceContext_CopyStructureCount(
        ID3D11DeviceContext*      pContext,
        ID3D11Buffer*             pDstBuffer,
        UINT                      DstOffset,
        ID3D11UnorderedAccessView* pSrcUav) {
  auto procs = getContextProcs(pContext);
  procs->CopyStructureCount(pContext, pDstBuffer, DstOffset, pSrcUav);

  ID3D11Resource* shadowResource = getShadowResource(pDstBuffer);
  ID3D11Buffer*   shadowBuffer   = nullptr;

  if (shadowResource) {
    shadowResource->QueryInterface(IID_PPV_ARGS(&shadowBuffer));
    shadowResource->Release();

    procs->CopyStructureCount(pContext, shadowBuffer, DstOffset, pSrcUav);
    shadowBuffer->Release();
  }
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Dispatch(
        ID3D11DeviceContext*      pContext,
        UINT                      X,
        UINT                      Y,
        UINT                      Z) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  procs->Dispatch(pContext, X, Y, Z);

  updateUavShadowResources(pContext);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DispatchIndirect(
        ID3D11DeviceContext*      pContext,
        ID3D11Buffer*             pParameterBuffer,
        UINT                      pParameterOffset) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  procs->DispatchIndirect(pContext, pParameterBuffer, pParameterOffset);

  updateUavShadowResources(pContext);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_OMSetRenderTargets(
        ID3D11DeviceContext*      pContext,
        UINT                      RTVCount,
        ID3D11RenderTargetView* const* ppRTVs,
        ID3D11DepthStencilView*   pDSV) {
  auto procs = getContextProcs(pContext);
  updateRtvShadowResources(pContext);
  resolveBoundMSAA(pContext);
  getRasterState(pContext)->dirty.store(true, std::memory_order_release);

  // Shadow-res twin: redirect the depth-only caster pass onto the enlarged
  // twin DSV so casters render at high resolution. Only depth-only binds are
  // redirected — if a color RTV is bound alongside (never observed, §33b E
  // events) the sizes could not match, so we fail safe to the engine's own
  // 1024 pass and log it.
  ID3D11DepthStencilView* shadowTwinDsv = nullptr;
  if (shadowMapResolution() > 1024 && pDSV) {
    shadowTwinDsv = getShadowResTwinDsv(pDSV);
    if (shadowTwinDsv) {
      static std::atomic<uint32_t> redirectLogs{0};
      const uint32_t n = redirectLogs.fetch_add(1, std::memory_order_relaxed);
      if (RTVCount == 0 || !ppRTVs || !ppRTVs[0]) {
        pDSV = shadowTwinDsv;
        if (n < 16 || n % 4096 == 0)
          log("SHADOWRES caster DSV redirected to twin");
      } else {
        if (n < 16 || n % 256 == 0)
          log("SHADOWRES DSV redirect SKIP: color RTV bound with shadow map");
        shadowTwinDsv->Release();
        shadowTwinDsv = nullptr;
      }
    }
  }

  ID3D11RenderTargetView* msaaRtv = nullptr;
  ID3D11DepthStencilView* msaaDsv = nullptr;
  if (RTVCount == 1 && ppRTVs && ppRTVs[0] && pDSV &&
      getOrCreateMSAAViews(pContext, ppRTVs[0], pDSV, &msaaRtv, &msaaDsv)) {
    ID3D11Resource* host = nullptr;
    ppRTVs[0]->GetResource(&host);
    if (host) {
      const MSAAState state = MSAAState::Dirty;
      host->SetPrivateData(IID_MSAAState, sizeof(state), &state);
      pContext->SetPrivateDataInterface(IID_MSAABoundHost, host);
      host->Release();
    }
    ppRTVs = &msaaRtv;
    pDSV = msaaDsv;
  }
  procs->OMSetRenderTargets(pContext, RTVCount, ppRTVs, pDSV);
  if (shadowTwinDsv) shadowTwinDsv->Release();
  if (msaaRtv) msaaRtv->Release();
  if (msaaDsv) msaaDsv->Release();
}

void STDMETHODCALLTYPE ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(
        ID3D11DeviceContext*      pContext,
        UINT                      RTVCount,
        ID3D11RenderTargetView* const* ppRTVs,
        ID3D11DepthStencilView*   pDSV,
        UINT                      UAVIndex,
        UINT                      UAVCount,
        ID3D11UnorderedAccessView* const* ppUAVs,
  const UINT*                     pUAVClearValues) {
  auto procs = getContextProcs(pContext);
  updateRtvShadowResources(pContext);
  resolveBoundMSAA(pContext);
  getRasterState(pContext)->dirty.store(true, std::memory_order_release);

  ID3D11RenderTargetView* msaaRtv = nullptr;
  ID3D11DepthStencilView* msaaDsv = nullptr;
  if (RTVCount == 1 && ppRTVs && ppRTVs[0] && pDSV && UAVCount == 0 &&
      getOrCreateMSAAViews(pContext, ppRTVs[0], pDSV, &msaaRtv, &msaaDsv)) {
    ID3D11Resource* host = nullptr;
    ppRTVs[0]->GetResource(&host);
    if (host) {
      const MSAAState state = MSAAState::Dirty;
      host->SetPrivateData(IID_MSAAState, sizeof(state), &state);
      pContext->SetPrivateDataInterface(IID_MSAABoundHost, host);
      host->Release();
    }
    ppRTVs = &msaaRtv;
    pDSV = msaaDsv;
  }
  procs->OMSetRenderTargetsAndUnorderedAccessViews(pContext,
    RTVCount, ppRTVs, pDSV, UAVIndex, UAVCount, ppUAVs, pUAVClearValues);
  if (msaaRtv) msaaRtv->Release();
  if (msaaDsv) msaaDsv->Release();
}

void STDMETHODCALLTYPE ID3D11DeviceContext_UpdateSubresource(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pResource,
        UINT                      Subresource,
  const D3D11_BOX*                pBox,
  const void*                     pData,
        UINT                      RowPitch,
        UINT                      SlicePitch) {
  TransitionTimer transitionTimer(g_transitionUpdate);
  auto procs = getContextProcs(pContext);

  if (!pBox && Subresource == 0)
    dumpCompositeUpdate(pResource, pData);

  const void* effectiveData = pData;
  uint8_t dimHoldCopy[16];
  uint8_t gateHoldCopy[880];
  if (cbCaptureEnabled() && pData && !pBox && Subresource == 0) {
    D3D11_BUFFER_DESC desc = {};
    if (isConstantBuffer(pResource, &desc)) {
      if (cutinBlobEnabled())
        snapCbWrite(pResource, pData, desc.ByteWidth);
      if (arlandInCinematicBattle()) {
        // Dim-hold: keep the faded light $Params at 1.0 during the cut-in.
        // pData is const (DEFAULT buffer), so patch a copy and pass that on.
        if (cutinDimHoldEnabled() && desc.ByteWidth == 16) {
          std::memcpy(dimHoldCopy, pData, 16);
          if (dimHoldPatch(dimHoldCopy, 16))
            effectiveData = dimHoldCopy;
        }
        // Gate-hold: open the 880 receiver material's shadow-reception gate.
        if (cutinGateHoldEnabled() && desc.ByteWidth == 880) {
          std::memcpy(gateHoldCopy, pData, 880);
          if (gateHoldPatch(gateHoldCopy, 880))
            effectiveData = gateHoldCopy;
        }
      }
      // Shadow-map upscale: rescale the receiver's PCF tap size to the
      // enlarged map's texel size. The 880 receiver material is written via
      // UpdateSubresource (not Map), so this is its load-bearing patch point.
      if (shadowMapResolution() > 1024 && desc.ByteWidth == 880) {
        if (effectiveData != gateHoldCopy)
          std::memcpy(gateHoldCopy, effectiveData, 880);
        if (tapScalePatch(gateHoldCopy, 880))
          effectiveData = gateHoldCopy;
      }
    }
  }

  procs->UpdateSubresource(pContext, pResource,
    Subresource, pBox, effectiveData, RowPitch, SlicePitch);

  ID3D11Resource* shadowResource = getShadowResource(pResource);
  
  if (shadowResource) {
    procs->UpdateSubresource(pContext, shadowResource,
      Subresource, pBox, pData, RowPitch, SlicePitch);
    shadowResource->Release();
  }
}

using SubresourceRef = std::pair<ID3D11Resource*, UINT>;

struct ShadowMapping {
  ID3D11Resource* shadow = nullptr;
};

static mutex g_activeMapMutex;
static std::map<SubresourceRef, ShadowMapping> g_activeMaps;
static mutex g_dirtyMutex;
static std::set<SubresourceRef> g_dirtyShadows;
static std::atomic<bool> g_haveDirtyShadows = { false };

void copyMappedSubresource(
  const ATFIX_RESOURCE_INFO*       pInfo,
        UINT                       Subresource,
  const D3D11_MAPPED_SUBRESOURCE*  pDst,
  const D3D11_MAPPED_SUBRESOURCE*  pSrc) {
  const D3D11_BOX box = getResourceBox(pInfo, Subresource);

  if (pInfo->Dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    std::memcpy(pDst->pData, pSrc->pData, box.right);
    return;
  }

  const uint32_t rowSize = box.right * getFormatPixelSize(pInfo->Format);
  for (uint32_t z = 0; z < box.back; z++) {
    for (uint32_t y = 0; y < box.bottom; y++) {
      std::memcpy(
        ptroffset(pDst->pData, y * pDst->RowPitch + z * pDst->DepthPitch),
        ptroffset(pSrc->pData, y * pSrc->RowPitch + z * pSrc->DepthPitch),
        rowSize);
    }
  }
}

void markShadowDirty(ID3D11Resource* pResource, UINT Subresource) {
  std::lock_guard lock(g_dirtyMutex);
  if (g_dirtyShadows.emplace(pResource, Subresource).second)
    pResource->AddRef();
  g_haveDirtyShadows.store(true, std::memory_order_release);
}

void flushDirtyShadows(ID3D11DeviceContext* pContext) {
  if (!isImmediatecontext(pContext) ||
      !g_haveDirtyShadows.load(std::memory_order_acquire))
    return;

  std::set<SubresourceRef> dirty;
  {
    std::lock_guard lock(g_dirtyMutex);
    dirty.swap(g_dirtyShadows);
    g_haveDirtyShadows.store(false, std::memory_order_release);
  }

  auto procs = getContextProcs(pContext);
  for (const auto& ref : dirty) {
    TransitionTimer flushTimer(g_transitionShadowFlush);
    ID3D11Resource* resource = ref.first;
    const UINT subresource = ref.second;
    bool stillMapped = false;
    {
      std::lock_guard lock(g_activeMapMutex);
      stillMapped = g_activeMaps.find(ref) != g_activeMaps.end();
    }

    if (stillMapped) {
      markShadowDirty(resource, subresource);
      resource->Release();
      continue;
    }

    ID3D11Resource* shadow = getShadowResource(resource);
    ATFIX_RESOURCE_INFO info = { };
    if (!shadow || !getResourceInfo(resource, &info)) {
      if (shadow)
        shadow->Release();
      resource->Release();
      continue;
    }

    const D3D11_MAP mapType = info.Usage == D3D11_USAGE_DYNAMIC
      ? D3D11_MAP_WRITE_DISCARD
      : D3D11_MAP_WRITE;
    D3D11_MAPPED_SUBRESOURCE dst = { };
    D3D11_MAPPED_SUBRESOURCE src = { };
    HRESULT hr = procs->Map(pContext, resource, subresource, mapType, 0, &dst);
    if (SUCCEEDED(hr)) {
      hr = procs->Map(pContext, shadow, subresource, D3D11_MAP_READ, 0, &src);
      if (SUCCEEDED(hr)) {
        copyMappedSubresource(&info, subresource, &dst, &src);
        if (transitionTraceEnabled()) {
          const uint32_t mip = info.Mips ? subresource % info.Mips : 0;
          const uint64_t width = std::max(info.Width >> mip, 1u);
          const uint64_t height = std::max(info.Height >> mip, 1u);
          const uint64_t depth = std::max(info.Depth >> mip, 1u);
          g_transitionShadowFlushBytes.fetch_add(
            width * height * depth * getFormatPixelSize(info.Format),
            std::memory_order_relaxed);
        }
        procs->Unmap(pContext, shadow, subresource);
      } else {
        log("Failed to map a shadow resource for upload, hr 0x", std::hex, hr);
      }
      procs->Unmap(pContext, resource, subresource);
    } else {
      log("Failed to map a destination resource for upload, hr 0x", std::hex, hr);
    }

    shadow->Release();
    resource->Release();
  }
}

HRESULT STDMETHODCALLTYPE ID3D11DeviceContext_Map(
        ID3D11DeviceContext*       pContext,
        ID3D11Resource*            pResource,
        UINT                       Subresource,
        D3D11_MAP                  MapType,
        UINT                       MapFlags,
        D3D11_MAPPED_SUBRESOURCE*  pMappedResource) {
  TransitionTimer transitionTimer(g_transitionMap);
  TransitionMapKindTimer mapKindTimer(MapType);
  const uintptr_t caller = transitionTraceEnabled()
    ? reinterpret_cast<uintptr_t>(__builtin_return_address(0)) : 0;
  auto procs = getContextProcs(pContext);
  if (!pResource || !isImmediatecontext(pContext)) {
    mapKindTimer.setBranch(0);
    const HRESULT hr = procs->Map(
      pContext, pResource, Subresource, MapType, MapFlags, pMappedResource);
    if (SUCCEEDED(hr)) {
      trackCompositeMap(pResource, Subresource, pMappedResource);
      captureCbMap(pResource, Subresource, pMappedResource);
    }
    return hr;
  }

  ID3D11Resource* shadow = getShadowResource(pResource);
  if (!shadow) {
    mapKindTimer.setBranch(1);
    const bool detailedMap = transitionTraceEnabled() &&
      (MapType == D3D11_MAP_READ || MapType == D3D11_MAP_WRITE_DISCARD);
    const auto directStarted = detailedMap
      ? std::chrono::steady_clock::now()
      : std::chrono::steady_clock::time_point{};
    const HRESULT hr = procs->Map(
      pContext, pResource, Subresource, MapType, MapFlags, pMappedResource);
    if (directStarted != std::chrono::steady_clock::time_point{}) {
      const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - directStarted).count();
      recordTransitionMapDetail(
        pResource, Subresource, MapType, caller, uint64_t(nanos));
    }
    if (SUCCEEDED(hr)) {
      trackCompositeMap(pResource, Subresource, pMappedResource);
      captureCbMap(pResource, Subresource, pMappedResource);
    }
    return hr;
  }

  mapKindTimer.setBranch(2);

  const HRESULT hr = procs->Map(pContext, shadow, Subresource,
    D3D11_MAP_READ_WRITE, MapFlags, pMappedResource);
  if (FAILED(hr)) {
    shadow->Release();
    return hr;
  }

  std::lock_guard lock(g_activeMapMutex);
  const auto [entry, inserted] = g_activeMaps.emplace(
    SubresourceRef { pResource, Subresource }, ShadowMapping { shadow });
  if (!inserted) {
    procs->Unmap(pContext, shadow, Subresource);
    shadow->Release();
    return E_FAIL;
  }
  trackCompositeMap(pResource, Subresource, pMappedResource);
      captureCbMap(pResource, Subresource, pMappedResource);
  return hr;
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Unmap(
        ID3D11DeviceContext*       pContext,
        ID3D11Resource*            pResource,
        UINT                       Subresource) {
  auto procs = getContextProcs(pContext);
  captureCbUnmap(pContext, pResource, Subresource);
  dumpCompositeMap(pResource, Subresource);
  ShadowMapping mapping;
  bool redirected = false;
  if (pResource) {
    std::lock_guard lock(g_activeMapMutex);
    const auto entry = g_activeMaps.find(SubresourceRef { pResource, Subresource });
    if (entry != g_activeMaps.end()) {
      mapping = entry->second;
      g_activeMaps.erase(entry);
      redirected = true;
    }
  }

  if (!redirected) {
    procs->Unmap(pContext, pResource, Subresource);
    return;
  }

  procs->Unmap(pContext, mapping.shadow, Subresource);
  mapping.shadow->Release();
  markShadowDirty(pResource, Subresource);
}

void traceResolutionDraw(ID3D11DeviceContext* context,
                         const char* operation,
                         UINT elementCount,
                         UINT instanceCount) {
  if (!resolutionTraceEnabled() || elementCount > 12)
    return;

  ID3D11RenderTargetView* rtv = nullptr;
  context->OMGetRenderTargets(1, &rtv, nullptr);
  ID3D11Resource* target = nullptr;
  D3D11_TEXTURE2D_DESC targetDesc = { };
  if (rtv) {
    rtv->GetResource(&target);
    rtv->Release();
  }
  const bool haveTarget = texture2DDesc(target, &targetDesc);
  const UINT mainWidth = g_mainRtWidth.load(std::memory_order_relaxed);
  const UINT mainHeight = g_mainRtHeight.load(std::memory_order_relaxed);
  const bool mainTarget = haveTarget && targetDesc.Width == mainWidth &&
    targetDesc.Height == mainHeight;
  const bool halfTarget = haveTarget && targetDesc.Width == mainWidth / 2 &&
    targetDesc.Height == mainHeight / 2;
  if (!mainTarget && !halfTarget) {
    if (target)
      target->Release();
    return;
  }

  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  context->VSGetShader(&vs, nullptr, nullptr);
  context->PSGetShader(&ps, nullptr, nullptr);
  ID3D11ShaderResourceView* views[8] = { };
  context->PSGetShaderResources(0, 8, views);
  ID3D11Resource* resources[8] = { };
  D3D11_TEXTURE2D_DESC resourceDescs[8] = { };
  for (UINT i = 0; i < 8; i++) {
    if (!views[i])
      continue;
    views[i]->GetResource(&resources[i]);
    texture2DDesc(resources[i], &resourceDescs[i]);
  }

  std::array<uintptr_t, 12> key = {
    reinterpret_cast<uintptr_t>(vs),
    reinterpret_cast<uintptr_t>(ps),
    reinterpret_cast<uintptr_t>(resources[0]),
    reinterpret_cast<uintptr_t>(resources[1]),
    reinterpret_cast<uintptr_t>(resources[2]),
    reinterpret_cast<uintptr_t>(resources[3]),
    reinterpret_cast<uintptr_t>(resources[4]),
    reinterpret_cast<uintptr_t>(resources[5]),
    reinterpret_cast<uintptr_t>(resources[6]),
    reinterpret_cast<uintptr_t>(resources[7]),
    elementCount,
    instanceCount,
  };
  static mutex traceMutex;
  static std::set<std::array<uintptr_t, 12>> seen;
  bool unique = false;
  {
    std::lock_guard lock(traceMutex);
    if (seen.size() < 4096)
      unique = seen.insert(key).second;
  }
  if (unique) {
  UINT viewportCount = 1;
  D3D11_VIEWPORT viewport = { };
  context->RSGetViewports(&viewportCount, &viewport);
  UINT scissorCount = 1;
  D3D11_RECT scissor = { };
  context->RSGetScissorRects(&scissorCount, &scissor);
  ID3D11InputLayout* inputLayout = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  context->IAGetInputLayout(&inputLayout);
  context->IAGetPrimitiveTopology(&topology);
  ID3D11Buffer* vertexBuffers[2] = { };
  UINT strides[2] = { };
  UINT offsets[2] = { };
  context->IAGetVertexBuffers(0, 2, vertexBuffers, strides, offsets);
  D3D11_BUFFER_DESC vbDescs[2] = { };
  for (UINT i = 0; i < 2; i++)
    if (vertexBuffers[i]) vertexBuffers[i]->GetDesc(&vbDescs[i]);
  ID3D11Buffer* vsBuffers[4] = { };
  ID3D11Buffer* psBuffers[4] = { };
  context->VSGetConstantBuffers(0, 4, vsBuffers);
  context->PSGetConstantBuffers(0, 4, psBuffers);

  const bool blurComposite = mainTarget && elementCount == 3 &&
    resourceDescs[0].Width == mainWidth / 2 &&
    resourceDescs[0].Height == mainHeight / 2 &&
    resourceDescs[0].Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
  if (blurComposite) {
    tagCompositeBuffer(vertexBuffers[0], CompositeVb0);
    tagCompositeBuffer(vertexBuffers[1], CompositeVb1);
    tagCompositeBuffer(vsBuffers[0], CompositeVsCb0);
    tagCompositeBuffer(vsBuffers[1], CompositeVsCb1);
    tagCompositeBuffer(psBuffers[0], CompositePsCb0);
    tagCompositeBuffer(psBuffers[1], CompositePsCb1);
    uint32_t vb0Data[16] = { };
    uint32_t vb1Data[16] = { };
    const UINT vb0DataSize = resolutionBufferData(vertexBuffers[0], vb0Data);
    const UINT vb1DataSize = resolutionBufferData(vertexBuffers[1], vb1Data);
    log("RES blur geometry vb0_size=", vb0DataSize,
        " vb0=", std::hex,
        vb0Data[0], ",", vb0Data[1], ",", vb0Data[2], ",",
        vb0Data[3], ",", vb0Data[4], ",", vb0Data[5], ",",
        vb0Data[6], ",", vb0Data[7], ",", vb0Data[8],
        " vb1_size=", std::dec, vb1DataSize,
        " vb1=", std::hex,
        vb1Data[0], ",", vb1Data[1], ",", vb1Data[2], ",",
        vb1Data[3], ",", vb1Data[4], ",", vb1Data[5]);
  }

  log("RES draw op=", operation,
      " elements=", elementCount,
      " instances=", instanceCount,
      " context=", context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "deferred",
      " target=", target, "/", targetDesc.Width, "x", targetDesc.Height,
      " viewport=", viewportCount ? viewport.TopLeftX : 0, ",", viewportCount ? viewport.TopLeftY : 0,
      "+", viewportCount ? viewport.Width : 0, "x", viewportCount ? viewport.Height : 0,
      " scissor=", scissorCount ? scissor.left : 0, ",", scissorCount ? scissor.top : 0,
      "-", scissorCount ? scissor.right : 0, ",", scissorCount ? scissor.bottom : 0,
      " topology=", topology,
      " layout=", inputLayout,
      " vs=", vs, " ps=", ps,
      " vb0=", vertexBuffers[0], "/", vbDescs[0].ByteWidth,
      "/", strides[0], "/", offsets[0],
      " vb1=", vertexBuffers[1], "/", vbDescs[1].ByteWidth,
      "/", strides[1], "/", offsets[1],
      " vs_cb=", vsBuffers[0], ",", vsBuffers[1], ",", vsBuffers[2], ",", vsBuffers[3],
      " ps_cb=", psBuffers[0], ",", psBuffers[1], ",", psBuffers[2], ",", psBuffers[3],
      " srv0=", resources[0], "/", resourceDescs[0].Width, "x", resourceDescs[0].Height, "/", resourceDescs[0].Format,
      " srv1=", resources[1], "/", resourceDescs[1].Width, "x", resourceDescs[1].Height, "/", resourceDescs[1].Format,
      " srv2=", resources[2], "/", resourceDescs[2].Width, "x", resourceDescs[2].Height, "/", resourceDescs[2].Format,
      " srv3=", resources[3], "/", resourceDescs[3].Width, "x", resourceDescs[3].Height, "/", resourceDescs[3].Format,
      " srv4=", resources[4], "/", resourceDescs[4].Width, "x", resourceDescs[4].Height, "/", resourceDescs[4].Format,
      " srv5=", resources[5], "/", resourceDescs[5].Width, "x", resourceDescs[5].Height, "/", resourceDescs[5].Format,
      " srv6=", resources[6], "/", resourceDescs[6].Width, "x", resourceDescs[6].Height, "/", resourceDescs[6].Format,
      " srv7=", resources[7], "/", resourceDescs[7].Width, "x", resourceDescs[7].Height, "/", resourceDescs[7].Format);

  if (inputLayout) inputLayout->Release();
  for (ID3D11Buffer* buffer : vertexBuffers)
    if (buffer) buffer->Release();
  for (ID3D11Buffer* buffer : vsBuffers)
    if (buffer) buffer->Release();
  for (ID3D11Buffer* buffer : psBuffers)
    if (buffer) buffer->Release();
  }

  if (vs) vs->Release();
  if (ps) ps->Release();
  for (UINT i = 0; i < 8; i++) {
    if (resources[i]) resources[i]->Release();
    if (views[i]) views[i]->Release();
  }
  if (target)
    target->Release();
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Draw(
        ID3D11DeviceContext* pContext, UINT VertexCount,
        UINT StartVertexLocation) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  gateHoldAtDraw(pContext);
  traceResolutionDraw(pContext, "draw", VertexCount, 1);

  // Carry dialogue-snapshot identity through the three-vertex blur passes so
  // the later four-vertex composite can be distinguished from the raw copy.
  UINT blurGeneration = 0;
  ID3D11ShaderResourceView* blurView = nullptr;
  ID3D11Resource* blurInput = nullptr;
  pContext->PSGetShaderResources(0, 1, &blurView);
  if (blurView)
    blurView->GetResource(&blurInput);
  if (blurInput) {
    UINT size = sizeof(blurGeneration);
    if (FAILED(blurInput->GetPrivateData(
          IID_DialogSnapshotResource, &size, &blurGeneration)))
      blurGeneration = 0;
  }
  if (blurGeneration && VertexCount == 3) {
    ID3D11RenderTargetView* targetView = nullptr;
    pContext->OMGetRenderTargets(1, &targetView, nullptr);
    ID3D11Resource* target = nullptr;
    if (targetView)
      targetView->GetResource(&target);
    if (target) {
      const UINT outputGeneration = blurGeneration + 1;
      target->SetPrivateData(IID_DialogSnapshotResource,
        sizeof(outputGeneration), &outputGeneration);
      target->Release();
    }
    if (targetView) targetView->Release();
  }
  if (blurInput) blurInput->Release();
  if (blurView) blurView->Release();

  cutinBlobCaptureDraw(pContext, VertexCount);
  procs->Draw(pContext, VertexCount, StartVertexLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawIndexed(
        ID3D11DeviceContext* pContext, UINT IndexCount,
        UINT StartIndexLocation, INT BaseVertexLocation) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  gateHoldAtDraw(pContext);
  traceResolutionDraw(pContext, "indexed", IndexCount, 1);

  // The 48-byte 1920x1080 quad is shared by other cutscene layers. Keep the
  // game's original buffer bound everywhere except the corrected dialogue
  // snapshot draw; globally scaling it makes portraits flash out of place.
  if (IndexCount == 4) {
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11Resource* resource = nullptr;
    pContext->PSGetShaderResources(0, 1, &view);
    if (view)
      view->GetResource(&resource);

    UINT blurGeneration = 0;
    UINT markerSize = sizeof(blurGeneration);
    const bool processedDialogueBlur = resource && SUCCEEDED(
      resource->GetPrivateData(
        IID_DialogSnapshotResource, &markerSize, &blurGeneration)) &&
        blurGeneration >= 2;

    if (processedDialogueBlur) {
      ID3D11Buffer* originalBuffer = nullptr;
      UINT stride = 0;
      UINT offset = 0;
      pContext->IAGetVertexBuffers(
        0, 1, &originalBuffer, &stride, &offset);

      ID3D11Buffer* scaledBuffer = nullptr;
      UINT scaledSize = sizeof(scaledBuffer);
      if (originalBuffer && SUCCEEDED(originalBuffer->GetPrivateData(
            IID_DialogScaledVertexBuffer, &scaledSize, &scaledBuffer)) &&
          scaledBuffer) {
        pContext->IASetVertexBuffers(
          0, 1, &scaledBuffer, &stride, &offset);
        procs->DrawIndexed(
          pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
        pContext->IASetVertexBuffers(
          0, 1, &originalBuffer, &stride, &offset);
        scaledBuffer->Release();
        originalBuffer->Release();
        if (resource) resource->Release();
        if (view) view->Release();
        return;
      }
      if (scaledBuffer) scaledBuffer->Release();
      if (originalBuffer) originalBuffer->Release();
    }

    if (resource) resource->Release();
    if (view) view->Release();
  }
  cutinBlobCaptureDraw(pContext, IndexCount);
  procs->DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawInstanced(
        ID3D11DeviceContext* pContext, UINT VertexCountPerInstance,
        UINT InstanceCount, UINT StartVertexLocation,
        UINT StartInstanceLocation) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  gateHoldAtDraw(pContext);
  traceResolutionDraw(
    pContext, "instanced", VertexCountPerInstance, InstanceCount);
  procs->DrawInstanced(pContext, VertexCountPerInstance, InstanceCount,
    StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawIndexedInstanced(
        ID3D11DeviceContext* pContext, UINT IndexCountPerInstance,
        UINT InstanceCount, UINT StartIndexLocation,
        INT BaseVertexLocation, UINT StartInstanceLocation) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  gateHoldAtDraw(pContext);
  traceResolutionDraw(
    pContext, "indexed-instanced", IndexCountPerInstance, InstanceCount);
  procs->DrawIndexedInstanced(pContext, IndexCountPerInstance, InstanceCount,
    StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawAuto(ID3D11DeviceContext* pContext) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  procs->DrawAuto(pContext);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawInstancedIndirect(
        ID3D11DeviceContext* pContext, ID3D11Buffer* pBufferForArgs,
        UINT AlignedByteOffsetForArgs) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  procs->DrawInstancedIndirect(pContext, pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawIndexedInstancedIndirect(
        ID3D11DeviceContext* pContext, ID3D11Buffer* pBufferForArgs,
        UINT AlignedByteOffsetForArgs) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  procs->DrawIndexedInstancedIndirect(pContext, pBufferForArgs,
    AlignedByteOffsetForArgs);
}

void traceResolutionShaderResource(ID3D11DeviceContext* context,
                                   UINT slot,
                                   ID3D11ShaderResourceView* view,
                                   ID3D11Resource* resource) {
  ResolutionTraceState state = { };
  if (!resolutionTraceState(resource, &state))
    return;
  ++state.shaderBinds;
  resource->SetPrivateData(IID_ResolutionTrace, sizeof(state), &state);
  if (state.shaderBinds > 8 &&
      (state.shaderBinds & (state.shaderBinds - 1)) != 0)
    return;

  D3D11_TEXTURE2D_DESC sourceDesc = { };
  texture2DDesc(resource, &sourceDesc);
  D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { };
  view->GetDesc(&srvDesc);

  ID3D11RenderTargetView* rtv = nullptr;
  context->OMGetRenderTargets(1, &rtv, nullptr);
  ID3D11Resource* target = nullptr;
  D3D11_TEXTURE2D_DESC targetDesc = { };
  if (rtv) {
    rtv->GetResource(&target);
    rtv->Release();
  }
  const bool haveTarget = texture2DDesc(target, &targetDesc);
  if (target)
    target->Release();

  UINT viewportCount = 1;
  D3D11_VIEWPORT viewport = { };
  context->RSGetViewports(&viewportCount, &viewport);
  UINT scissorCount = 1;
  D3D11_RECT scissor = { };
  context->RSGetScissorRects(&scissorCount, &scissor);

  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  context->VSGetShader(&vs, nullptr, nullptr);
  context->PSGetShader(&ps, nullptr, nullptr);
  ID3D11Buffer* vsBuffers[4] = { };
  ID3D11Buffer* psBuffers[4] = { };
  context->VSGetConstantBuffers(0, 4, vsBuffers);
  context->PSGetConstantBuffers(0, 4, psBuffers);

  log("RES bind candidate=", state.id,
      " count=", state.shaderBinds,
      " context=", context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "deferred",
      " slot=", slot,
      " resource=", resource,
      " format=", sourceDesc.Format,
      " srv_format=", srvDesc.Format,
      " target=", haveTarget ? targetDesc.Width : 0, "x", haveTarget ? targetDesc.Height : 0,
      " target_format=", haveTarget ? targetDesc.Format : DXGI_FORMAT_UNKNOWN,
      " viewport=", viewportCount ? viewport.TopLeftX : 0, ",", viewportCount ? viewport.TopLeftY : 0,
      "+", viewportCount ? viewport.Width : 0, "x", viewportCount ? viewport.Height : 0,
      " scissor=", scissorCount ? scissor.left : 0, ",", scissorCount ? scissor.top : 0,
      "-", scissorCount ? scissor.right : 0, ",", scissorCount ? scissor.bottom : 0,
      " vs=", vs, " ps=", ps,
      " vs_cb=", vsBuffers[0], ",", vsBuffers[1], ",", vsBuffers[2], ",", vsBuffers[3],
      " ps_cb=", psBuffers[0], ",", psBuffers[1], ",", psBuffers[2], ",", psBuffers[3]);

  if (vs)
    vs->Release();
  if (ps)
    ps->Release();
  for (ID3D11Buffer* buffer : vsBuffers)
    if (buffer) buffer->Release();
  for (ID3D11Buffer* buffer : psBuffers)
    if (buffer) buffer->Release();
}

void STDMETHODCALLTYPE ID3D11DeviceContext_PSSetShaderResources(
        ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumViews,
        ID3D11ShaderResourceView* const* ppShaderResourceViews) {
  auto procs = getContextProcs(pContext);
  if (ppShaderResourceViews) {
    for (UINT i = 0; i < NumViews; i++) {
      if (!ppShaderResourceViews[i])
        continue;
      ID3D11Resource* resource = nullptr;
      ppShaderResourceViews[i]->GetResource(&resource);
      if (resource) {
        resolveIfMSAA(pContext, resource);
        traceResolutionShaderResource(
          pContext, StartSlot + i, ppShaderResourceViews[i], resource);
        resource->Release();
      }
    }
  }
  // Shadow-res twin: substitute the receiver's shadow-map SRV with the
  // enlarged twin's SRV so the ground samples the high-res shadows. Views
  // over textures without twins pass through untouched (fast negative cache).
  if (shadowMapResolution() > 1024 && NumViews && ppShaderResourceViews &&
      NumViews <= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
    ID3D11ShaderResourceView*
      substituted[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    bool any = false;
    for (UINT i = 0; i < NumViews; i++) {
      substituted[i] = ppShaderResourceViews[i];
      if (!substituted[i])
        continue;
      if (ID3D11ShaderResourceView* twin =
            getShadowResTwinSrv(substituted[i])) {
        substituted[i] = twin;   // AddRef'd; released below after the call
        any = true;
      }
    }
    if (any) {
      procs->PSSetShaderResources(pContext, StartSlot, NumViews, substituted);
      for (UINT i = 0; i < NumViews; i++)
        if (substituted[i] && substituted[i] != ppShaderResourceViews[i])
          substituted[i]->Release();
      static std::atomic<uint32_t> srvLogs{0};
      const uint32_t n = srvLogs.fetch_add(1, std::memory_order_relaxed);
      if (n < 16 || n % 4096 == 0)
        log("SHADOWRES receiver SRV redirected to twin");
      return;
    }
  }
  procs->PSSetShaderResources(pContext, StartSlot, NumViews,
    ppShaderResourceViews);
}

HRESULT STDMETHODCALLTYPE ID3D11DeviceContext_FinishCommandList(
        ID3D11DeviceContext* pContext, BOOL RestoreDeferredContextState,
        ID3D11CommandList** ppCommandList) {
  TransitionTimer transitionTimer(g_transitionCommands);
  auto procs = getContextProcs(pContext);
  resolveBoundMSAA(pContext);
  return procs->FinishCommandList(pContext, RestoreDeferredContextState,
    ppCommandList);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_ExecuteCommandList(
        ID3D11DeviceContext* pContext, ID3D11CommandList* pCommandList,
        BOOL RestoreContextState) {
  TransitionTimer transitionTimer(g_transitionCommands);
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  procs->ExecuteCommandList(pContext, pCommandList, RestoreContextState);
}

#define HOOK_PROC(iface, object, table, index, proc) \
  hookProc(object, #iface "::" #proc, &table->proc, &iface ## _ ## proc, index)

template<typename T>
void hookProc(void* pObject, const char* pName, T** ppOrig, T* pHook, uint32_t index) {
  void** vtbl = *reinterpret_cast<void***>(pObject);

  MH_STATUS mh = MH_CreateHook(vtbl[index],
    reinterpret_cast<void*>(pHook),
    reinterpret_cast<void**>(ppOrig));

  if (mh) {
    if (mh != MH_ERROR_ALREADY_CREATED)
      log("Failed to create hook for ", pName, ": ", MH_StatusToString(mh));
    return;
  }

  mh = MH_EnableHook(vtbl[index]);

  if (mh) {
    log("Failed to enable hook for ", pName, ": ", MH_StatusToString(mh));
    return;
  }

  log("Created hook for ", pName, " @ ", reinterpret_cast<void*>(pHook));
}

void hookDevice(ID3D11Device* pDevice) {
  std::lock_guard lock(g_hookMutex);

  if (g_installedHooks & HOOK_DEVICE)
    return;

  log("Hooking device ", pDevice,
      " msaa=", msaaSamples(),
      " resolution_trace=", resolutionTraceEnabled());

  DeviceProcs* procs = &g_deviceProcs;
  HOOK_PROC(ID3D11Device, pDevice, procs, 3,  CreateBuffer);
  HOOK_PROC(ID3D11Device, pDevice, procs, 27, CreateDeferredContext);
  HOOK_PROC(ID3D11Device, pDevice, procs, 4,  CreateTexture1D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 5,  CreateTexture2D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 6,  CreateTexture3D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 12, CreateVertexShader);
  HOOK_PROC(ID3D11Device, pDevice, procs, 15, CreatePixelShader);

  g_installedHooks |= HOOK_DEVICE;
}

void hookContext(ID3D11DeviceContext* pContext) {
  std::lock_guard lock(g_hookMutex);

  uint32_t flag = HOOK_IMM_CTX;
  ContextProcs* procs = &g_immContextProcs;

  if (isImmediatecontext(pContext))
    g_immCtx.store(pContext, std::memory_order_relaxed);

  if (!isImmediatecontext(pContext)) {
    flag = HOOK_DEF_CTX;
    procs = &g_defContextProcs;
  }

  if (g_installedHooks & flag)
    return;

  log("Hooking context ", pContext);

  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 50, ClearRenderTargetView);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 53, ClearDepthStencilView);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 52, ClearUnorderedAccessViewFloat);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 51, ClearUnorderedAccessViewUint);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 47, CopyResource);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 46, CopySubresourceRegion);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 49, CopyStructureCount);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 41, Dispatch);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 42, DispatchIndirect);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 13, Draw);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 12, DrawIndexed);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 21, DrawInstanced);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 20, DrawIndexedInstanced);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 38, DrawAuto);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 40, DrawInstancedIndirect);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 39, DrawIndexedInstancedIndirect);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 58, ExecuteCommandList);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 114, FinishCommandList);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 8,  PSSetShaderResources);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 14, Map);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 15, Unmap);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 44, RSSetViewports);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 45, RSSetScissorRects);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 33, OMSetRenderTargets);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 34, OMSetRenderTargetsAndUnorderedAccessViews);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 48, UpdateSubresource);

  g_installedHooks |= flag;

  /* Immediate context and deferred context methods may share code */
  if (flag & HOOK_IMM_CTX)
    g_defContextProcs = g_immContextProcs;
}

void traceTransitionD3DFrame(uint64_t intervalMicros) {
  const auto take = [](TransitionCounter& counter) {
    return std::array<uint64_t, 2> {
      counter.calls.exchange(0, std::memory_order_acq_rel),
      counter.nanos.exchange(0, std::memory_order_acq_rel) / 1000,
    };
  };
  const auto create = take(g_transitionCreate);
  const auto map = take(g_transitionMap);
  const auto copy = take(g_transitionCopy);
  const auto update = take(g_transitionUpdate);
  const auto commands = take(g_transitionCommands);
  const auto shadowFlush = take(g_transitionShadowFlush);
  const uint64_t shadowFlushBytes = g_transitionShadowFlushBytes.exchange(
    0, std::memory_order_acq_rel);
  std::array<std::array<std::array<uint64_t, 2>, 6>, 3> mapKinds = { };
  for (size_t branch = 0; branch < mapKinds.size(); branch++)
    for (size_t type = 0; type < mapKinds[branch].size(); type++)
      mapKinds[branch][type] = take(g_transitionMapKinds[branch][type]);
  std::map<ReadMapKey, ReadMapStats> readMaps;
  std::map<ReadMapKey, ReadMapStats> writeMaps;
  {
    std::lock_guard lock(g_transitionReadMapMutex);
    readMaps.swap(g_transitionReadMaps);
    writeMaps.swap(g_transitionWriteMaps);
  }
  if (!transitionTraceEnabled() || intervalMicros < 15000)
    return;
  log("TRANSITION d3d interval_us=", intervalMicros,
    " create_calls=", create[0], " create_us=", create[1],
    " map_calls=", map[0], " map_us=", map[1],
    " copy_calls=", copy[0], " copy_us=", copy[1],
    " update_calls=", update[0], " update_us=", update[1],
    " command_calls=", commands[0], " command_us=", commands[1],
    " shadow_flushes=", shadowFlush[0],
    " shadow_flush_us=", shadowFlush[1],
    " shadow_flush_bytes=", shadowFlushBytes);
  static const std::array<const char*, 3> branches = {
    "other-context", "direct", "shadow",
  };
  for (size_t branch = 0; branch < mapKinds.size(); branch++) {
    for (size_t type = 0; type < mapKinds[branch].size(); type++) {
      const auto& bucket = mapKinds[branch][type];
      if (bucket[0])
        log("TRANSITION map-kind interval_us=", intervalMicros,
          " branch=", branches[branch], " type=", type,
          " calls=", bucket[0], " us=", bucket[1]);
    }
  }
  const uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  for (const auto& [key, stats] : readMaps) {
    log("TRANSITION read-map interval_us=", intervalMicros,
      " caller_rva=0x", std::hex,
      module && key.caller >= module ? key.caller - module : key.caller,
      std::dec, " dim=", key.dimension, " format=", key.format,
      " size=", key.width, "x", key.height,
      " usage=", key.usage, " bind=0x", std::hex, key.bindFlags,
      " cpu=0x", key.cpuFlags, std::dec,
      " calls=", stats.calls, " resources=", stats.resources.size(),
      " bytes=", stats.estimatedBytes,
      " api_us=", stats.nanos / 1000);
  }
  for (const auto& [key, stats] : writeMaps) {
    log("TRANSITION write-map interval_us=", intervalMicros,
      " caller_rva=0x", std::hex,
      module && key.caller >= module ? key.caller - module : key.caller,
      std::dec, " dim=", key.dimension, " format=", key.format,
      " size=", key.width, "x", key.height,
      " usage=", key.usage, " bind=0x", std::hex, key.bindFlags,
      " cpu=0x", key.cpuFlags, std::dec,
      " calls=", stats.calls, " resources=", stats.resources.size(),
      " bytes=", stats.estimatedBytes,
      " api_us=", stats.nanos / 1000);
  }
}

}
