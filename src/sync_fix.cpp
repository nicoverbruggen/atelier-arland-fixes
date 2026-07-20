// Derived from Philip Rebohle's atelier-sync-fix and subsequent Map/Unmap
// work by TellowKrinkle; substantially altered for Arland. See LICENSE.
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "sync_fix.h"
#include "util.h"

namespace atfix {

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

struct ShadowDrawKey {
  uintptr_t depthResource = 0;
  uintptr_t vertexShader = 0;
  uintptr_t pixelShader = 0;
  uintptr_t inputLayout = 0;
  uintptr_t vertexBuffer = 0;
  uint32_t targetWidth = 0;
  uint32_t targetHeight = 0;
  uint32_t targetFormat = 0;
  uint32_t contextType = 0;
  uint32_t indexed = 0;

  bool operator<(const ShadowDrawKey& other) const {
    return std::tie(depthResource, vertexShader, pixelShader, inputLayout,
      vertexBuffer, targetWidth, targetHeight, targetFormat, contextType,
      indexed) <
      std::tie(other.depthResource, other.vertexShader, other.pixelShader,
        other.inputLayout, other.vertexBuffer, other.targetWidth,
        other.targetHeight, other.targetFormat, other.contextType,
        other.indexed);
  }
};

struct ShadowDrawStats {
  uint64_t calls = 0;
  uint64_t elements = 0;
};

mutex g_shadowTraceMutex;
std::map<ShadowDrawKey, ShadowDrawStats> g_shadowDraws;
uint64_t g_shadowDepthOnlyBinds = 0;
uint64_t g_shadowTraceFrames = 0;
uint64_t g_shadowTraceSequence = 0;
uint64_t g_shadowReceiveDraws = 0;
std::set<uintptr_t> g_shadowSrvs;      // PS SRVs backed by the 1024x1024 fmt-44 map
std::set<uintptr_t> g_nonShadowSrvs;   // classified as not the shadow map

// Distinct depth-only shadow targets bound per window, keyed by depth resource.
struct ShadowTargetStats {
  uint64_t binds = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
};
std::map<uintptr_t, ShadowTargetStats> g_shadowTargets;

// Per-color-render-target draw buckets. Isolates the cut-in pass: it composes to
// its own target, so a distinct bucket appears whose shadow-sampling count tells
// us whether the cut-in ground samples the shadow map at all.
struct ReceiverBucketKey {
  uintptr_t colorResource = 0;
  uintptr_t pixelShader = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  bool operator<(const ReceiverBucketKey& o) const {
    return std::tie(colorResource, pixelShader, width, height, format) <
      std::tie(o.colorResource, o.pixelShader, o.width, o.height, o.format);
  }
};
struct ReceiverBucketStats {
  uint64_t draws = 0;
  uint64_t shadowSampling = 0;
};
std::map<ReceiverBucketKey, ReceiverBucketStats> g_receiverBuckets;

bool receiverRtTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_SHADOW_RECEIVE_RT");
    return value && value[0] != '0';
  }();
  return enabled;
}

// Per-vertex-buffer coverage: did a given mesh draw to the shadow depth map
// (casting), the color target (visible), or both? A character-sized mesh that
// is color-only during the cut-in is visible-but-not-casting.
struct VbCoverage {
  bool shadow = false;
  bool color = false;
  uint32_t maxElements = 0;
};
std::map<uintptr_t, VbCoverage> g_vbCoverage;
// Meshes (by vertex buffer) that have EVER cast a shadow, with their size — used
// to spot a mesh that casts in the overview but goes visible-not-casting in the
// cut-in (the decoupled geometry).
std::map<uintptr_t, uint32_t> g_vbEverCast;
std::set<uintptr_t> g_decoupledReported;

bool casterCoverageEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_CASTER_COVERAGE");
    return value && value[0] != '0';
  }();
  return enabled;
}

void traceCasterCoverage(ID3D11DeviceContext* context, UINT elementCount) {
  if (!casterCoverageEnabled())
    return;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  context->OMGetRenderTargets(1, &rtv, &dsv);
  const bool shadowPass = dsv && !rtv;   // depth-only = shadow caster pass
  bool colorPass = false;
  if (rtv) {
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (res) {
      ID3D11Texture2D* tex = nullptr;
      if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d = {};
        tex->GetDesc(&d);
        colorPass = d.Width == 1920 && d.Height == 1080;  // main scene target
        tex->Release();
      }
      res->Release();
    }
  }
  if (shadowPass || colorPass) {
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    context->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (vb) {
      const uintptr_t key = reinterpret_cast<uintptr_t>(vb);
      std::lock_guard lock(g_shadowTraceMutex);
      auto& c = g_vbCoverage[key];
      if (shadowPass) c.shadow = true;
      if (colorPass) c.color = true;
      if (elementCount > c.maxElements) c.maxElements = elementCount;
      if (shadowPass && elementCount >= 300) {
        auto& mx = g_vbEverCast[key];
        if (elementCount > mx) mx = elementCount;
      }
      vb->Release();
    }
  }
  if (rtv) rtv->Release();
  if (dsv) dsv->Release();
}

bool shadowTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_SHADOW_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool shadowMatrixEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_SHADOW_MATRIX");
    return value && value[0] != '0';
  }();
  return enabled;
}

// Read the first 16 floats (one 4x4 matrix) of a VS constant buffer via a
// staging copy. Immediate context only. Returns false if unavailable.
ID3D11Buffer* g_cbStaging = nullptr;
bool readVsCbufferMatrix(ID3D11DeviceContext* context, UINT slot,
                         float out[16]) {
  if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    return false;
  ID3D11Buffer* cb = nullptr;
  context->VSGetConstantBuffers(slot, 1, &cb);
  if (!cb)
    return false;
  bool ok = false;
  if (!g_cbStaging) {
    ID3D11Device* dev = nullptr;
    context->GetDevice(&dev);
    if (dev) {
      D3D11_BUFFER_DESC d = {};
      d.ByteWidth = 64;
      d.Usage = D3D11_USAGE_STAGING;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      dev->CreateBuffer(&d, nullptr, &g_cbStaging);
      dev->Release();
    }
  }
  if (g_cbStaging) {
    D3D11_BUFFER_DESC sd = {};
    cb->GetDesc(&sd);
    if (sd.ByteWidth >= 64) {
      D3D11_BOX box = {0, 0, 0, 64, 1, 1};
      context->CopySubresourceRegion(g_cbStaging, 0, 0, 0, 0, cb, 0, &box);
      D3D11_MAPPED_SUBRESOURCE m = {};
      if (SUCCEEDED(context->Map(g_cbStaging, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, 64);
        context->Unmap(g_cbStaging, 0);
        ok = true;
      }
    }
  }
  cb->Release();
  return ok;
}

void traceShadowMatrix(ID3D11DeviceContext*, UINT) {}  // superseded by cb capture

// Deferred contexts block reading a cbuffer at draw time, but the game still
// WRITES cbuffers via Map(WRITE_DISCARD)/Unmap, which we hook — the CPU data is
// valid at Unmap. We capture constant-buffer writes that happen while the shadow
// depth target is bound: those matrices are the shadow light-view-projection and
// the caster world matrices. Correlate ms against BATTLE_STATE to compare states.
std::atomic<bool> g_inShadowPass{false};
struct CbMtxCap { const void* data; UINT size; };
std::map<std::pair<ID3D11Resource*, UINT>, CbMtxCap> g_cbMtxMaps;
std::atomic<uint64_t> g_cbMtxLogged{0};

void updateShadowPassFlag(UINT rtvCount,
                          ID3D11RenderTargetView* const* rtvs,
                          ID3D11DepthStencilView* dsv) {
  if (!shadowMatrixEnabled())
    return;
  bool haveColor = false;
  for (UINT i = 0; i < rtvCount && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    haveColor |= rtvs && rtvs[i];
  bool shadow = false;
  if (!haveColor && dsv) {
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (res) {
      ID3D11Texture2D* tex = nullptr;
      if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d = {};
        tex->GetDesc(&d);
        shadow = d.Width == 1024 && d.Height == 1024;
        tex->Release();
      }
      res->Release();
    }
  }
  g_inShadowPass.store(shadow, std::memory_order_release);
}

void captureCbMap(ID3D11Resource* resource, UINT sub,
                  const D3D11_MAPPED_SUBRESOURCE* mapped) {
  if (!shadowMatrixEnabled() || !resource || !mapped || !mapped->pData ||
      !g_inShadowPass.load(std::memory_order_acquire))
    return;
  ID3D11Buffer* b = nullptr;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&b))))
    return;
  D3D11_BUFFER_DESC d = {};
  b->GetDesc(&d);
  b->Release();
  if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || d.ByteWidth < 64 ||
      d.ByteWidth > 1024)
    return;
  std::lock_guard lock(g_shadowTraceMutex);
  g_cbMtxMaps[{resource, sub}] = {mapped->pData, d.ByteWidth};
}

void captureCbUnmap(ID3D11Resource* resource, UINT sub) {
  if (!shadowMatrixEnabled())
    return;
  CbMtxCap cap = {};
  {
    std::lock_guard lock(g_shadowTraceMutex);
    auto it = g_cbMtxMaps.find({resource, sub});
    if (it == g_cbMtxMaps.end())
      return;
    cap = it->second;
    g_cbMtxMaps.erase(it);
  }
  // Per-window cap so sampling spreads across the whole battle (incl. the
  // cut-in) instead of exhausting on the first frame.
  if (!cap.data || g_cbMtxLogged.fetch_add(1, std::memory_order_relaxed) > 24)
    return;
  const float* f = reinterpret_cast<const float*>(cap.data);
  // Log first matrix rows; a projection matrix has structured last column,
  // a world matrix has translation in row 3.
  log("SHADOW_MTX ms=", GetTickCount64(), " size=", cap.size,
      " cb=", reinterpret_cast<void*>(resource),
      " r0=", f[0], ",", f[1], ",", f[2], ",", f[3],
      " r1=", f[4], ",", f[5], ",", f[6], ",", f[7],
      " r2=", f[8], ",", f[9], ",", f[10], ",", f[11],
      " r3=", f[12], ",", f[13], ",", f[14], ",", f[15]);
}

// ---- Cut-in shadow fix: re-issue decoupled meshes into the shadow map ----
// The attack cut-in shadow was never implemented in Rorona: the cinematic mesh
// is drawn to the screen but never submitted to the shadow depth map. We capture
// the shadow-pass pipeline (which runs first each frame) and, at each such mesh's
// color draw during the cut-in, re-issue it into the shadow map with its CURRENT
// (cinematic) transform, so a shadow appears under the on-screen character.
bool cutinShadowFixEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_BATTLE_CUTIN_SHADOW");
    return v && v[0] != '0';
  }();
  return enabled;
}

struct ShadowPipe {
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11Buffer* cb[14] = {};
  D3D11_VIEWPORT vp = {};
  bool valid = false;
};
mutex g_pipeMutex;
ShadowPipe g_shadowPipe;
// To tell per-pass cbuffers (light-VP: stable across all shadow draws) from
// per-object ones (world: changes per caster), track the first shadow draw's
// cbuffers this frame and which slots later change.
ID3D11Buffer* g_pipeFirstCb[14] = {};
bool g_pipeCbSeen = false;
bool g_pipeCbChanged[14] = {};
std::set<uintptr_t> g_frameShadowVBs;   // VBs already cast this frame
std::atomic<int> g_reissuesThisFrame{0};
std::atomic<uint64_t> g_reissueTotal{0};

void releaseShadowPipe(ShadowPipe& p) {
  if (p.dsv) p.dsv->Release();
  if (p.vs) p.vs->Release();
  if (p.il) p.il->Release();
  for (auto*& b : p.cb) { if (b) b->Release(); b = nullptr; }
  p = ShadowPipe{};
}

// Capture the shadow pipeline from a depth-only draw (called before color pass).
void captureShadowPipe(ID3D11DeviceContext* ctx) {
  ShadowPipe p;
  ID3D11RenderTargetView* rtv = nullptr;
  ctx->OMGetRenderTargets(1, &rtv, &p.dsv);
  if (rtv) { rtv->Release(); if (p.dsv) p.dsv->Release(); return; }  // not shadow
  if (!p.dsv)
    return;
  ctx->VSGetShader(&p.vs, nullptr, nullptr);
  ctx->IAGetInputLayout(&p.il);
  ctx->VSGetConstantBuffers(0, 14, p.cb);
  UINT n = 1;
  ctx->RSGetViewports(&n, &p.vp);
  p.valid = p.vs != nullptr;
  std::lock_guard lock(g_pipeMutex);
  releaseShadowPipe(g_shadowPipe);
  g_shadowPipe = p;
  // Track per-pass vs per-object cbuffer slots across the frame's shadow draws.
  if (!g_pipeCbSeen) {
    for (int i = 0; i < 14; ++i) {
      g_pipeFirstCb[i] = p.cb[i];
      if (g_pipeFirstCb[i]) g_pipeFirstCb[i]->AddRef();
    }
    g_pipeCbSeen = true;
  } else {
    for (int i = 0; i < 14; ++i)
      if (p.cb[i] != g_pipeFirstCb[i])
        g_pipeCbChanged[i] = true;   // this slot is per-object (world)
  }
}

// Re-issue the currently-bound mesh into the shadow map. Saves and restores all
// pipeline state it touches. `indexed` picks Draw vs DrawIndexed.
void reissueMeshToShadow(ID3D11DeviceContext* ctx, bool indexed,
                         UINT count, UINT startIndex, INT baseVertex) {
  ShadowPipe pipe;
  {
    std::lock_guard lock(g_pipeMutex);
    if (!g_shadowPipe.valid)
      return;
    pipe = g_shadowPipe;  // shallow copy of raw pointers (valid within frame)
  }
  // Save state we will change.
  ID3D11RenderTargetView* savedRtv[8] = {};
  ID3D11DepthStencilView* savedDsv = nullptr;
  ctx->OMGetRenderTargets(8, savedRtv, &savedDsv);
  ID3D11VertexShader* savedVs = nullptr;
  ctx->VSGetShader(&savedVs, nullptr, nullptr);
  ID3D11InputLayout* savedIl = nullptr;
  ctx->IAGetInputLayout(&savedIl);
  ID3D11Buffer* savedCb[14] = {};
  ctx->VSGetConstantBuffers(0, 14, savedCb);
  UINT savedVpN = 8;
  D3D11_VIEWPORT savedVp[8] = {};
  ctx->RSGetViewports(&savedVpN, savedVp);

  // Bind shadow output + shadow VS + shadow input layout + shadow viewport.
  ID3D11RenderTargetView* noRtv[1] = {nullptr};
  ctx->OMSetRenderTargets(1, noRtv, pipe.dsv);
  ctx->VSSetShader(pipe.vs, nullptr, 0);
  if (pipe.il) ctx->IASetInputLayout(pipe.il);
  ctx->RSSetViewports(1, &pipe.vp);
  // Override only the per-PASS cbuffer slots (light-VP / light params — stable
  // across all shadow draws this frame) with the shadow buffers; keep the
  // currently-bound buffer for per-OBJECT slots (this mesh's world transform).
  ID3D11Buffer* useCb[14] = {};
  for (int i = 0; i < 14; ++i)
    useCb[i] = (!g_pipeCbChanged[i] && g_pipeFirstCb[i]) ? g_pipeFirstCb[i]
                                                         : savedCb[i];
  ctx->VSSetConstantBuffers(0, 14, useCb);

  if (indexed)
    getContextProcs(ctx)->DrawIndexed(ctx, count, startIndex, baseVertex);
  else
    getContextProcs(ctx)->Draw(ctx, count, startIndex);
  g_reissueTotal.fetch_add(1, std::memory_order_relaxed);

  // Restore.
  ctx->OMSetRenderTargets(8, savedRtv, savedDsv);
  ctx->VSSetShader(savedVs, nullptr, 0);
  ctx->IASetInputLayout(savedIl);
  ctx->RSSetViewports(savedVpN, savedVp);
  ctx->VSSetConstantBuffers(0, 14, savedCb);
  for (auto* r : savedRtv) if (r) r->Release();
  if (savedDsv) savedDsv->Release();
  if (savedVs) savedVs->Release();
  if (savedIl) savedIl->Release();
  for (auto* b : savedCb) if (b) b->Release();
}

// At a draw: if it's a shadow-pass draw, capture the pipe + remember the VB as
// having cast; if it's a decoupled cut-in mesh (visible, ever-cast, not cast
// this frame), re-issue it into the shadow map.
void cutinShadowDraw(ID3D11DeviceContext* ctx, bool indexed, UINT count,
                     UINT startIndex, INT baseVertex) {
  if (!cutinShadowFixEnabled())
    return;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ctx->OMGetRenderTargets(1, &rtv, &dsv);
  const bool shadowPass = dsv && !rtv;
  bool colorMain = false;
  if (rtv) {
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
  }
  if (rtv) rtv->Release();
  if (dsv) dsv->Release();

  ID3D11Buffer* vb = nullptr;
  UINT stride = 0, offset = 0;
  ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
  const uintptr_t vbKey = reinterpret_cast<uintptr_t>(vb);
  if (vb) vb->Release();
  if (!vbKey)
    return;

  if (shadowPass) {
    captureShadowPipe(ctx);
    std::lock_guard lock(g_shadowTraceMutex);
    g_frameShadowVBs.insert(vbKey);
    if (count >= 300) {                       // record character-scale casters
      auto& mx = g_vbEverCast[vbKey];
      if (count > mx) mx = count;
    }
    return;
  }
  // No battle-state gate: the character close-up that lacks a shadow happens
  // during action selection (SelectCommand/SelectTarget), not just WaitAction.
  // The "not cast this frame" check below already excludes meshes casting
  // normally, so any visible mesh that isn't in the shadow map is a candidate.
  if (!colorMain || count < 300)
    return;
  bool everCast, castThisFrame;
  {
    std::lock_guard lock(g_shadowTraceMutex);
    everCast = g_vbEverCast.count(vbKey) != 0;
    castThisFrame = g_frameShadowVBs.count(vbKey) != 0;
  }
  if (!everCast || castThisFrame)
    return;
  const int n = g_reissuesThisFrame.fetch_add(1, std::memory_order_relaxed);
  if (n >= 64)
    return;
  reissueMeshToShadow(ctx, indexed, count, startIndex, baseVertex);
  if (g_reissueTotal.load(std::memory_order_relaxed) % 200 == 1) {
    uint32_t stableMask = 0;
    for (int i = 0; i < 14; ++i)
      if (!g_pipeCbChanged[i] && g_pipeFirstCb[i]) stableMask |= (1u << i);
    log("CUTIN_REISSUE total=", g_reissueTotal.load(std::memory_order_relaxed),
        " vb=", reinterpret_cast<void*>(vbKey), " count=", count,
        " pipe_valid=", g_shadowPipe.valid,
        " overridden_slots=0x", std::hex, stableMask, std::dec);
  }
}

// Per-frame reset (called from Present): the "cast this frame" set and re-issue
// budget start fresh each frame.
void cutinShadowPresent() {
  if (!cutinShadowFixEnabled())
    return;
  {
    std::lock_guard lock(g_shadowTraceMutex);
    g_frameShadowVBs.clear();
    g_reissuesThisFrame.store(0, std::memory_order_relaxed);
  }
  std::lock_guard lock(g_pipeMutex);
  for (auto*& b : g_pipeFirstCb) { if (b) b->Release(); b = nullptr; }
  g_pipeCbSeen = false;
  for (auto& c : g_pipeCbChanged) c = false;
}

void traceShadowTargetBind(UINT renderTargetCount,
                           ID3D11RenderTargetView* const* renderTargets,
                           ID3D11DepthStencilView* depthTarget) {
  if (!shadowTraceEnabled() || !depthTarget ||
      renderTargetCount > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
    return;
  bool haveColor = false;
  for (UINT index = 0; index < renderTargetCount; ++index)
    haveColor |= renderTargets && renderTargets[index];
  if (haveColor)
    return;

  uintptr_t depthResource = 0;
  uint32_t width = 0, height = 0, format = 0;
  ID3D11Resource* resource = nullptr;
  depthTarget->GetResource(&resource);
  if (resource) {
    depthResource = reinterpret_cast<uintptr_t>(resource);
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      width = desc.Width; height = desc.Height; format = uint32_t(desc.Format);
      texture->Release();
    }
    resource->Release();
  }

  std::lock_guard lock(g_shadowTraceMutex);
  ++g_shadowDepthOnlyBinds;
  auto& target = g_shadowTargets[depthResource];
  ++target.binds;
  target.width = width; target.height = height; target.format = format;
}

void traceShadowDraw(ID3D11DeviceContext* context, bool indexed,
                     UINT elementCount, UINT instanceCount) {
  if (!shadowTraceEnabled())
    return;

  ID3D11RenderTargetView* renderTarget = nullptr;
  ID3D11DepthStencilView* depthTarget = nullptr;
  context->OMGetRenderTargets(1, &renderTarget, &depthTarget);
  if (renderTarget || !depthTarget) {
    if (renderTarget) renderTarget->Release();
    if (depthTarget) depthTarget->Release();
    return;
  }

  ID3D11Resource* depthResource = nullptr;
  ID3D11Texture2D* depthTexture = nullptr;
  D3D11_TEXTURE2D_DESC depthDesc = { };
  depthTarget->GetResource(&depthResource);
  if (depthResource && SUCCEEDED(
        depthResource->QueryInterface(IID_PPV_ARGS(&depthTexture))))
    depthTexture->GetDesc(&depthDesc);

  ID3D11VertexShader* vertexShader = nullptr;
  ID3D11PixelShader* pixelShader = nullptr;
  ID3D11InputLayout* inputLayout = nullptr;
  ID3D11Buffer* vertexBuffer = nullptr;
  UINT stride = 0;
  UINT offset = 0;
  context->VSGetShader(&vertexShader, nullptr, nullptr);
  context->PSGetShader(&pixelShader, nullptr, nullptr);
  context->IAGetInputLayout(&inputLayout);
  context->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

  const ShadowDrawKey key = {
    reinterpret_cast<uintptr_t>(depthResource),
    reinterpret_cast<uintptr_t>(vertexShader),
    reinterpret_cast<uintptr_t>(pixelShader),
    reinterpret_cast<uintptr_t>(inputLayout),
    reinterpret_cast<uintptr_t>(vertexBuffer),
    depthDesc.Width,
    depthDesc.Height,
    uint32_t(depthDesc.Format),
    uint32_t(context->GetType()),
    indexed ? 1u : 0u,
  };
  {
    std::lock_guard lock(g_shadowTraceMutex);
    auto& stats = g_shadowDraws[key];
    ++stats.calls;
    stats.elements += uint64_t(elementCount) * instanceCount;
  }

  if (vertexBuffer) vertexBuffer->Release();
  if (inputLayout) inputLayout->Release();
  if (pixelShader) pixelShader->Release();
  if (vertexShader) vertexShader->Release();
  if (depthTexture) depthTexture->Release();
  if (depthResource) depthResource->Release();
  depthTarget->Release();
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
      shadow = desc.Width == 1024 && desc.Height == 1024 &&
        desc.Format == DXGI_FORMAT_R24G8_TYPELESS;
      texture->Release();
    }
    resource->Release();
  }
  (shadow ? g_shadowSrvs : g_nonShadowSrvs).insert(key);
  return shadow;
}

// Count draws that sample the shadow map (the receiver side). If cut-in frames
// show zero of these while overview frames show many, the cut-in never binds the
// shadow SRV — the missing "receive" step.
void traceShadowReceive(ID3D11DeviceContext* context) {
  if (!shadowTraceEnabled())
    return;
  ID3D11ShaderResourceView* srvs[16] = {};
  context->PSGetShaderResources(0, 16, srvs);
  bool samplesShadow = false;
  for (ID3D11ShaderResourceView* srv : srvs)
    if (srv) {
      std::lock_guard lock(g_shadowTraceMutex);
      if (isShadowSrvLocked(srv)) {
        samplesShadow = true;
        break;
      }
    }

  ReceiverBucketKey bucket;
  bool haveBucket = false;
  if (receiverRtTraceEnabled()) {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    if (rtv) {
      ID3D11Resource* resource = nullptr;
      rtv->GetResource(&resource);
      if (resource) {
        ID3D11Texture2D* texture = nullptr;
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
          D3D11_TEXTURE2D_DESC desc = {};
          texture->GetDesc(&desc);
          ID3D11PixelShader* ps = nullptr;
          context->PSGetShader(&ps, nullptr, nullptr);
          bucket = {reinterpret_cast<uintptr_t>(resource),
            reinterpret_cast<uintptr_t>(ps), desc.Width,
            desc.Height, uint32_t(desc.Format)};
          haveBucket = true;
          if (ps) ps->Release();
          texture->Release();
        }
        resource->Release();
      }
      rtv->Release();
    }
    if (dsv)
      dsv->Release();
  }

  {
    std::lock_guard lock(g_shadowTraceMutex);
    if (samplesShadow)
      ++g_shadowReceiveDraws;
    if (haveBucket) {
      auto& stats = g_receiverBuckets[bucket];
      ++stats.draws;
      if (samplesShadow)
        ++stats.shadowSampling;
    }
  }

  for (ID3D11ShaderResourceView* srv : srvs)
    if (srv)
      srv->Release();
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

const char* configPath() {
  static const std::array<char, MAX_PATH + 1> path = [] {
    std::array<char, MAX_PATH + 1> result = { };
    const DWORD pathLength = GetModuleFileNameA(
      nullptr, result.data(), MAX_PATH);
    if (pathLength && pathLength < MAX_PATH) {
      char* back = std::strrchr(result.data(), '\\');
      char* forward = std::strrchr(result.data(), '/');
      char* slash = !back || (forward && forward > back) ? forward : back;
      if (slash)
        std::memcpy(slash + 1, "arland-fix.ini", sizeof("arland-fix.ini"));
    }

    if (result[0] &&
        GetFileAttributesA(result.data()) == INVALID_FILE_ATTRIBUTES) {
      WritePrivateProfileStringA("Rendering", "MSAA", "1", result.data());
      WritePrivateProfileStringA("Rendering", "Width", "", result.data());
      WritePrivateProfileStringA("Rendering", "Height", "", result.data());
    }
    return result;
  }();
  return path[0] ? path.data() : nullptr;
}

bool configuredResolution(UINT* width, UINT* height) {
  const char* path = configPath();
  if (!path)
    return false;
  char widthValue[16] = { };
  char heightValue[16] = { };
  GetPrivateProfileStringA("Rendering", "Width", "", widthValue,
    sizeof(widthValue), path);
  GetPrivateProfileStringA("Rendering", "Height", "", heightValue,
    sizeof(heightValue), path);
  const unsigned long parsedWidth = std::strtoul(widthValue, nullptr, 10);
  const unsigned long parsedHeight = std::strtoul(heightValue, nullptr, 10);
  if (parsedWidth < 640 || parsedWidth > 16384 ||
      parsedHeight < 360 || parsedHeight > 16384)
    return false;
  *width = static_cast<UINT>(parsedWidth);
  *height = static_cast<UINT>(parsedHeight);
  return true;
}

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

UINT msaaSamples() {
  static const UINT samples = [] {
    const char* path = configPath();

    char value[16] = { };
    const DWORD length = GetEnvironmentVariableA("ARLAND_MSAA", value, sizeof(value));
    unsigned long requested = 1;
    if (length) {
      requested = std::strtoul(value, nullptr, 10);
    } else if (path) {
      requested = GetPrivateProfileIntA(
        "Rendering", "MSAA", 1, path);
    }
    if (requested < 2)
      return 1u;
    if (requested >= 8)
      return 8u;
    if (requested >= 4)
      return 4u;
    return 2u;
  }();
  return samples;
}

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

    if (changed)
      pDesc = &desc;
  }

  const HRESULT hr = procs->CreateTexture2D(pDevice, pDesc, pData, ppTexture);
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
  if (!fullSizeViewport && !halfSizeViewport &&
      !fullSizeScissor && !halfSizeScissor)
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
      texture->Release();
      const UINT mainWidth = g_mainRtWidth.load(std::memory_order_relaxed);
      const UINT mainHeight = g_mainRtHeight.load(std::memory_order_relaxed);
      const bool fullSizeTarget = desc.Width == mainWidth &&
        desc.Height == mainHeight;
      const bool halfSizeTarget = desc.Width == mainWidth / 2 &&
        desc.Height == mainHeight / 2;
      resizeViewport = (fullSizeViewport && fullSizeTarget) ||
        (halfSizeViewport && halfSizeTarget);
      resizeScissor = (fullSizeScissor && fullSizeTarget) ||
        (halfSizeScissor && halfSizeTarget);
      if (resizeViewport) {
        viewport.Width = static_cast<FLOAT>(desc.Width);
        viewport.Height = static_cast<FLOAT>(desc.Height);
      }
      if (resizeScissor) {
        scissor.right = static_cast<LONG>(desc.Width);
        scissor.bottom = static_cast<LONG>(desc.Height);
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
  traceShadowTargetBind(RTVCount, ppRTVs, pDSV);
  updateShadowPassFlag(RTVCount, ppRTVs, pDSV);

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
  traceShadowTargetBind(RTVCount, ppRTVs, pDSV);
  updateShadowPassFlag(RTVCount, ppRTVs, pDSV);

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

  procs->UpdateSubresource(pContext, pResource,
    Subresource, pBox, pData, RowPitch, SlicePitch);

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
  captureCbUnmap(pResource, Subresource);
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
  traceShadowDraw(pContext, false, VertexCount, 1);
  traceShadowReceive(pContext);
  traceCasterCoverage(pContext, VertexCount);
  traceShadowMatrix(pContext, VertexCount);
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

  cutinShadowDraw(pContext, false, VertexCount, StartVertexLocation, 0);
  procs->Draw(pContext, VertexCount, StartVertexLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawIndexed(
        ID3D11DeviceContext* pContext, UINT IndexCount,
        UINT StartIndexLocation, INT BaseVertexLocation) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  traceShadowDraw(pContext, true, IndexCount, 1);
  traceShadowReceive(pContext);
  traceCasterCoverage(pContext, IndexCount);
  traceShadowMatrix(pContext, IndexCount);
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
  cutinShadowDraw(pContext, true, IndexCount, StartIndexLocation, BaseVertexLocation);
  procs->DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawInstanced(
        ID3D11DeviceContext* pContext, UINT VertexCountPerInstance,
        UINT InstanceCount, UINT StartVertexLocation,
        UINT StartInstanceLocation) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  traceShadowDraw(
    pContext, false, VertexCountPerInstance, InstanceCount);
  traceShadowReceive(pContext);
  traceCasterCoverage(pContext, VertexCountPerInstance);
  traceShadowMatrix(pContext, VertexCountPerInstance);
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
  traceShadowDraw(
    pContext, true, IndexCountPerInstance, InstanceCount);
  traceShadowReceive(pContext);
  traceCasterCoverage(pContext, IndexCountPerInstance);
  traceShadowMatrix(pContext, IndexCountPerInstance);
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
      " resolution_trace=", resolutionTraceEnabled(),
      " shadow_trace=", shadowTraceEnabled());

  DeviceProcs* procs = &g_deviceProcs;
  HOOK_PROC(ID3D11Device, pDevice, procs, 3,  CreateBuffer);
  HOOK_PROC(ID3D11Device, pDevice, procs, 27, CreateDeferredContext);
  HOOK_PROC(ID3D11Device, pDevice, procs, 4,  CreateTexture1D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 5,  CreateTexture2D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 6,  CreateTexture3D);

  g_installedHooks |= HOOK_DEVICE;
}

void hookContext(ID3D11DeviceContext* pContext) {
  std::lock_guard lock(g_hookMutex);

  uint32_t flag = HOOK_IMM_CTX;
  ContextProcs* procs = &g_immContextProcs;

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

void traceShadowD3DFrame() {
  if (!shadowTraceEnabled())
    return;

  std::map<ShadowDrawKey, ShadowDrawStats> draws;
  std::map<ReceiverBucketKey, ReceiverBucketStats> receiverBuckets;
  std::map<uintptr_t, ShadowTargetStats> shadowTargets;
  std::map<uintptr_t, VbCoverage> vbCoverage;
  std::map<uintptr_t, uint64_t> drawsPerTarget;
  uint64_t depthOnlyBinds = 0;
  uint64_t receiveDraws = 0;
  uint64_t sequence = 0;
  {
    std::lock_guard lock(g_shadowTraceMutex);
    if (++g_shadowTraceFrames < 20)
      return;
    g_shadowTraceFrames = 0;
    sequence = ++g_shadowTraceSequence;
    depthOnlyBinds = g_shadowDepthOnlyBinds;
    g_shadowDepthOnlyBinds = 0;
    receiveDraws = g_shadowReceiveDraws;
    g_shadowReceiveDraws = 0;
    draws.swap(g_shadowDraws);
    receiverBuckets.swap(g_receiverBuckets);
    shadowTargets.swap(g_shadowTargets);
    vbCoverage.swap(g_vbCoverage);
  }
  g_cbMtxLogged.store(0, std::memory_order_relaxed);

  // Coverage summary: character-sized meshes (maxElements high) split by whether
  // they cast (shadow), are visible (color), or both. color-only = visible but
  // not casting — the transform/coherence break we're testing for.
  if (!vbCoverage.empty()) {
    uint64_t colorOnly = 0, both = 0, shadowOnly = 0;
    uint64_t colorOnlyBig = 0, bothBig = 0;
    for (const auto& [vb, c] : vbCoverage) {
      const bool big = c.maxElements >= 300;  // character-scale mesh
      if (c.color && !c.shadow) { ++colorOnly; if (big) ++colorOnlyBig; }
      else if (c.color && c.shadow) { ++both; if (big) ++bothBig; }
      else if (c.shadow && !c.color) ++shadowOnly;
    }
    log("SHADOW coverage window=", sequence,
        " color_only=", colorOnly, " (big=", colorOnlyBig, ")",
        " both=", both, " (big=", bothBig, ")",
        " shadow_only=", shadowOnly);
    // The decoupled mesh: casts in the overview (in g_vbEverCast) but is
    // visible-and-not-casting right now, during a battle cinematic state.
    const bool cinematic = arlandInCinematicBattle();
    for (const auto& [vb, c] : vbCoverage) {
      if (!(c.color && !c.shadow && c.maxElements >= 300))
        continue;
      auto ever = g_vbEverCast.find(vb);
      if (cinematic && ever != g_vbEverCast.end() &&
          g_decoupledReported.insert(vb).second)
        log("SHADOW DECOUPLED_MESH window=", sequence,
            " vb=", reinterpret_cast<void*>(vb),
            " elements_now=", c.maxElements,
            " elements_when_casting=", ever->second,
            " (visible now, cast in overview)");
    }
  }

  using GroupKey = std::tuple<uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
  struct GroupStats {
    uint64_t calls = 0;
    uint64_t elements = 0;
    std::set<uintptr_t> vertexBuffers;
  };
  std::map<GroupKey, GroupStats> groups;
  uint64_t totalCalls = 0;
  uint64_t totalElements = 0;
  for (const auto& [key, stats] : draws) {
    const GroupKey groupKey = {
      key.depthResource, key.vertexShader, key.pixelShader, key.inputLayout,
      key.targetWidth, key.targetHeight, key.targetFormat,
      key.contextType, key.indexed,
    };
    auto& group = groups[groupKey];
    group.calls += stats.calls;
    group.elements += stats.elements;
    drawsPerTarget[key.depthResource] += stats.calls;
    if (key.vertexBuffer)
      group.vertexBuffers.insert(key.vertexBuffer);
    totalCalls += stats.calls;
    totalElements += stats.elements;
  }

  log("SHADOW window=", sequence,
      " ms=", GetTickCount64(),
      " frames=20 depth_only_binds=", depthOnlyBinds,
      " recv_draws=", receiveDraws,
      " draws=", totalCalls,
      " elements=", totalElements,
      " groups=", groups.size());
  // Distinct shadow depth targets bound this window, with how many caster draws
  // each received. The battle binds two 1024x1024/44 targets; if one gets many
  // draws and the other zero, the empty one is the cut-in's unpopulated map.
  for (const auto& [resource, stats] : shadowTargets) {
    log("SHADOW target window=", sequence,
        " depth_resource=", reinterpret_cast<void*>(resource),
        " dims=", stats.width, "x", stats.height, "/", stats.format,
        " binds=", stats.binds,
        " caster_draws=", drawsPerTarget.count(resource)
          ? drawsPerTarget[resource] : 0);
  }

  size_t reported = 0;
  for (const auto& [key, stats] : groups) {
    if (reported++ >= 64)
      break;
    const auto& [depthResource, vs, ps, layout, width, height, format,
                 contextType, indexed] = key;
    log("SHADOW group window=", sequence,
        " calls=", stats.calls,
        " elements=", stats.elements,
        " buffers=", stats.vertexBuffers.size(),
        " depth_resource=", reinterpret_cast<void*>(depthResource),
        " target=", width, "x", height, "/", format,
        " context=", contextType,
        " indexed=", indexed,
        " vs=", reinterpret_cast<void*>(vs),
        " ps=", reinterpret_cast<void*>(ps),
        " layout=", reinterpret_cast<void*>(layout));
  }

  // Per-color-RT receiver buckets: a bucket with many draws but
  // shadow_sampling=0 that appears during a cut-in is the pass that fails to
  // sample the shadow map. Report the busiest buckets.
  if (!receiverBuckets.empty()) {
    std::vector<std::pair<ReceiverBucketKey, ReceiverBucketStats>> ordered(
      receiverBuckets.begin(), receiverBuckets.end());
    std::sort(ordered.begin(), ordered.end(),
      [](const auto& a, const auto& b) { return a.second.draws > b.second.draws; });
    size_t rt = 0;
    for (const auto& [key, stats] : ordered) {
      if (rt++ >= 24)
        break;
      log("SHADOW recv_rt window=", sequence,
          " draws=", stats.draws,
          " shadow_sampling=", stats.shadowSampling,
          " target=", key.width, "x", key.height, "/", key.format,
          " ps=", reinterpret_cast<void*>(key.pixelShader),
          " resource=", reinterpret_cast<void*>(key.colorResource));
    }
  }
}

}
