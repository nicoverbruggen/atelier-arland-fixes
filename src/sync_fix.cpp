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

// §20 experiment: a 1x1 comparison-sampleable depth SRV filled to a constant, so
// the receiver's sample_c returns the same value at every texel (no shadow).
ID3D11ShaderResourceView* g_litShadowSrv = nullptr;
std::atomic<bool> g_litShadowTried{false};
std::mutex g_litShadowMutex;

float cutinShadowFill();  // defined below with the other env gates

ID3D11ShaderResourceView* getLitShadowSrv(ID3D11DeviceContext* ctx) {
  std::lock_guard<std::mutex> lock(g_litShadowMutex);
  if (g_litShadowTried.load(std::memory_order_relaxed))
    return g_litShadowSrv;
  g_litShadowTried.store(true, std::memory_order_relaxed);
  ID3D11Device* dev = nullptr;
  ctx->GetDevice(&dev);
  if (!dev)
    return nullptr;
  float f = cutinShadowFill();
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  const uint32_t depth24 = uint32_t(f * float(0xFFFFFF)) & 0xFFFFFFu;  // R24, stencil 0
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R24G8_TYPELESS;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init = {};
  init.pSysMem = &depth24; init.SysMemPitch = 4;
  ID3D11Texture2D* tex = nullptr;
  if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &tex)) && tex) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(tex, &sd, &g_litShadowSrv);
    tex->Release();
  }
  dev->Release();
  log("CUTIN_SHADOW_OFF lit-srv created=", g_litShadowSrv != nullptr,
      " fill=", f);
  return g_litShadowSrv;
}

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

// Read a byte window of a VS constant buffer via a staging copy. Immediate
// context only. Returns false if unavailable. The staging buffer is sized for
// the whole 880-byte receiver material so both the legacy 64-byte matrix read
// and the V2 Model@0 + PSSGLightModelViewProjTex@752 window fit.
ID3D11Buffer* g_cbStaging = nullptr;
constexpr UINT kCbStagingBytes = 880;
bool readVsCbufferBytes(ID3D11DeviceContext* context, UINT slot, UINT offset,
                        UINT bytes, void* out) {
  if (bytes == 0 || offset + bytes > kCbStagingBytes)
    return false;
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
      d.ByteWidth = kCbStagingBytes;
      d.Usage = D3D11_USAGE_STAGING;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      dev->CreateBuffer(&d, nullptr, &g_cbStaging);
      dev->Release();
    }
  }
  if (g_cbStaging) {
    D3D11_BUFFER_DESC sd = {};
    cb->GetDesc(&sd);
    if (sd.ByteWidth >= offset + bytes) {
      D3D11_BOX box = {offset, 0, 0, offset + bytes, 1, 1};
      context->CopySubresourceRegion(g_cbStaging, 0, 0, 0, 0, cb, 0, &box);
      D3D11_MAPPED_SUBRESOURCE m = {};
      if (SUCCEEDED(context->Map(g_cbStaging, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, bytes);
        context->Unmap(g_cbStaging, 0);
        ok = true;
      }
    }
  }
  cb->Release();
  return ok;
}

// Read the first 16 floats (one 4x4 matrix) of a VS constant buffer.
bool readVsCbufferMatrix(ID3D11DeviceContext* context, UINT slot,
                         float out[16]) {
  return readVsCbufferBytes(context, slot, 0, 64, out);
}

void traceShadowMatrix(ID3D11DeviceContext*, UINT) {}  // superseded by cb capture

// ---- Cut-in recon (ARLAND_CUTIN_RECON=1) ----
// §30b concluded "no reusable light-VP cbuffer" from buffer POINTER churn
// across shadow draws (overridden_slots=0x0). But the engine allocates fresh
// buffers per draw, so a per-pass light-VP rewritten with IDENTICAL CONTENTS
// each draw would look exactly the same in that diagnostic. This recon captures
// (a) the CONTENTS of every constant-buffer write (Map/Unmap, UpdateSubresource,
// CreateBuffer initial data) and logs them per draw slot, and (b) the DXBC
// bytecode of the shadow-pass and character color vertex shaders, whose cbuffer
// declarations state the true constant layout. One run answers: is any shadow
// slot content-stable (light-VP), and does the shadow VS consume one baked
// world-x-lightVP matrix or separate world / light-VP constants?
bool cutinReconEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_RECON");
    return v && v[0] != '0';
  }();
  return enabled;
}

bool cutinShadowFixEnabled();
void healShadowCbWrite(ID3D11DeviceContext* ctx, ID3D11Resource* resource,
                       const void* data, uint32_t size);

// Battle lighting trace (§32): log the per-material light rig values at
// battle-state transitions, to catch the cut-in dimming the environment and
// (per the user report) failing to restore it afterwards.
bool cutinLightTraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_LIGHT_TRACE");
    return v && v[0] != '0';
  }();
  return enabled;
}

// Receiver-side probe (§32c): dump the shadow-sampling pixel shader and log
// its constant-buffer values per battle state, to find the phase-level switch
// that stops the ground from resolving shadows during actions.
bool cutinRecvTraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_RECV_TRACE");
    return v && v[0] != '0';
  }();
  return enabled;
}

// §36 diagnostic free-look: a clip-space zoom applied to the 3D scene's
// projection (ModelViewProj) at cbuffer write time, widening the effective
// field of view so the whole battlefield is visible. <1 zooms OUT (see more).
float cutinCamZoom() {
  static const float z = [] {
    const char* v = std::getenv("ARLAND_CUTIN_CAM_ZOOM");
    return v ? float(std::atof(v)) : 1.0f;
  }();
  return z;
}
bool cutinCamZoomEnabled() { return cutinCamZoom() != 1.0f; }

// §39 fix: the cut-in fades the ground's lit color COLOR0 (the receiver renders
// shadow as COLOR0 × (1+shadowFactor), so a faded COLOR0 = no visible shadow
// even though the shadow map is populated — §14/§38 of RORONA_COMBAT.md). The
// receiver VS builds COLOR0 ONLY from cb0 of the 768/880 field material, from
// the light constants at bytes 324..620 (point lights + dark + one-shot). We
// cache that block during a non-cinematic battle frame and restore it during
// cinematic states, so COLOR0 (and its shadow contrast) cannot fade.
bool cutinLightRestoreEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_LIGHT_RESTORE");
    return v && v[0] != '0';
  }();
  return enabled;
}

// §22 FIX: the cut-in darkening is a 16-byte $Params (s,s,s,1) whose scene-light
// intensity s fades 1.0 (overview) -> 0.7 (cut-in). Hold it at 1.0 during
// cinematic states so the ground keeps its overview brightness. Identified by
// shape (uniform RGB, w=1, s dropped below 1) rather than the per-launch pointer.
bool cutinDimHoldEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_DIM_HOLD");
    return v && v[0] != '0';
  }();
  return enabled;
}

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

// §32 FIX (adversarial-review finding): the 880 receiver material gates shadow
// RECEPTION on the VS's `diffuse` at byte 832 (a name collision — the PS RDEF
// calls byte 832 `shadowLPos`, but the VS reads cb0[52]=byte832 as diffuse). The
// receiver VS computes gate = 2.5 - 2*min(diffuse.w, diffuse.x); the PS samples
// the shadow map ONLY if gate < 1, i.e. min-diffuse > 0.75. During the cut-in
// diffuse.xyz is pinned to ~0.7 (< 0.75) -> gate closed -> the ground never
// samples the shadow map at all. Holding diffuse.x (and .yzw) at 1.0 during
// cinematic states OPENS the gate so the receiver samples shadows again.
bool cutinGateHoldEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_GATE_HOLD");
    return v && v[0] != '0';
  }();
  return enabled;
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

// §32b DIAGNOSIS: the UpdateSubresource-gated gate-hold never fired (zero
// GATE_DIFFUSE lines in a run where DIM_HOLD fired), so byte 832 does NOT get
// its cut-in 0.7 through a full-buffer 880 UpdateSubresource during cinematic
// frames — at least not in that run. Trace EVERY large-cb write on EVERY write
// path (map/update/update_box/create), with the @832 float4 + state + cine,
// UNGATED by cinematic state, so one run shows which path (if any) carries the
// receiver material and when. Gated on ARLAND_CUTIN_GATE_HOLD, throttled per
// (path,size) so it cannot flood.
void gateTraceCbWrite(const char* path, const void* data, uint32_t size,
                      const void* res) {
  if (!cutinGateHoldEnabled() || !data)
    return;
  if (size < 768 || size > 1024)
    return;
  static std::mutex traceMutex;
  static std::map<std::pair<std::string, uint32_t>, uint64_t> traceTicks;
  const uint64_t tick = GetTickCount64();
  {
    std::lock_guard<std::mutex> lock(traceMutex);
    auto& t = traceTicks[{path, size}];
    if (t && tick - t < 300)
      return;
    t = tick;
  }
  const char* st = arlandBattleStateName();
  float d[4] = {};
  if (size >= 848)
    std::memcpy(d, static_cast<const uint8_t*>(data) + 832, 16);
  log("GATE_TRACE path=", path, " res=", res, " size=", size,
      " state=", st ? st : "-", " cine=", arlandInCinematicBattle(),
      " @832=", d[0], ",", d[1], ",", d[2], ",", d[3]);
}

// §32b SAFETY NET: patch the gate at DRAW time, independent of the (unknown)
// write path. At every draw during a cinematic battle state whose VS cb0 is an
// 880-byte receiver material with the shadow SRV bound, record a 16-byte BOX
// UpdateSubresource over bytes [832,848) forcing diffuse=(1,1,1,1) right before
// the draw. This works even if the buffer was written pre-cinematic and
// re-bound stale, or written through a path none of our hooks cover. Partial
// constant-buffer updates are legal on the 11.1 runtime semantics DXVK
// implements (this deployment runs under DXVK), and we only touch the 16 gate
// bytes, so stale-snapshot matrix corruption is impossible. Kill switch:
// ARLAND_CUTIN_GATE_DRAW=0.
bool cutinGateDrawEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_GATE_DRAW");
    return !(v && v[0] == '0' && v[1] == '\0');
  }();
  return enabled;
}

// §20 EXPERIMENT: during cinematic states, swap the shadow map the RECEIVER
// samples for a uniform-depth 1x1 map, so sample_c returns a constant everywhere
// (no per-pixel shadow). If the ground snaps to full brightness during the
// cut-in, the shadow term IS the darkener (§19). Fill value is env-tunable to be
// robust to the comparison direction: 1.0 (far, default) vs 0.0 (near).
bool cutinShadowOffEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_SHADOW_OFF");
    return v && v[0] != '0';
  }();
  return enabled;
}
float cutinShadowFill() {
  static const float f = [] {
    const char* v = std::getenv("ARLAND_CUTIN_SHADOW_FILL");
    return v ? float(std::atof(v)) : 1.0f;
  }();
  return f;
}

// §41 V2 injection (ARLAND_CUTIN_INJECT_V2). Fixes the two established root
// causes of the dead injection path:
//   A) the injected caster was drawn through the piggybacked shadow-proxy
//      IL/VS, whose vertex format mismatches the recorded color VB -> garbage
//      positions, zero fragments. V2 records the character's OWN skin IL/VS
//      at the color draw and re-draws with them, feeding a cloned skin
//      cbuffer whose MVP slot (byte 160) holds our light-space matrix.
//   B) placement used @752 of "the last 880 write of the frame" fed a WORLD
//      position — but PSSGLightModelViewProjTex maps that mesh's LOCAL space.
//      V2 captures @752 AND Model@0 from the actual ground receive draw and
//      stores TexWorld = Tex x Model^-1 (object-independent world->UV).
bool cutinInjectV2Enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_INJECT_V2");
    return v && v[0] != '0';
  }();
  return enabled;
}

// §42: the receiver's ground VS/PS samples the shadow map at v = 1 - (row.vtx)
// — a code-side v-flip the @752 matrix itself does not contain. Fold that flip
// into g_texWorld row 1 (row1' = e3 - row1) so both the recvUV diagnostic and
// the injected placement (Binv2 x g_texWorld x W) land at the true sampled
// texel 1 - v. Default ON; disable with ARLAND_CUTIN_INJECT_MIRRORY=0 for A/B.
bool cutinInjectMirrorYEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_INJECT_MIRRORY");
    return !(v && v[0] == '0' && v[1] == '\0');
  }();
  return enabled;
}

// The cut-in fix consumes the same content snapshots the recon collects.
bool cbSnapEnabled() {
  return cutinReconEnabled() || cutinShadowFixEnabled() ||
    cutinLightTraceEnabled() || cutinRecvTraceEnabled() ||
    cutinCamZoomEnabled() || cutinLightRestoreEnabled() ||
    cutinDimHoldEnabled() || cutinInjectV2Enabled() ||
    cutinGateHoldEnabled();
}

// §24 diagnostic: the RECEIVER samples the shadow map with its own
// PSSGLightModelViewProjTex (880 material @752). Stash the latest one so the
// injector can compare where the receiver looks (recvUV) against where the
// caster lands (injUV) — if they diverge during the cut-in, that is the bug.
float g_recvTex[16] = {};
std::atomic<bool> g_recvTexValid{false};
std::mutex g_recvTexMutex;
void captureRecvTex(const void* data, uint32_t size) {
  if (size != 880 || !cutinShadowFixEnabled() || !data)
    return;
  std::lock_guard<std::mutex> lock(g_recvTexMutex);
  std::memcpy(g_recvTex, static_cast<const uint8_t*>(data) + 752, 64);
  g_recvTexValid.store(true, std::memory_order_relaxed);
}

// Scale clip x/y toward screen center (rows 0-1 of ModelViewProj) for the 3D
// scene materials. Character skin: MVP at byte 160; field ground: byte 128.
void cameraZoomCbWrite(void* data, uint32_t size) {
  if (!cutinCamZoomEnabled())
    return;
  size_t mvpOff;
  if (size == 26048) mvpOff = 160;
  else if (size == 768 || size == 880) mvpOff = 128;
  else return;
  float* m = reinterpret_cast<float*>(static_cast<uint8_t*>(data) + mvpOff);
  const float z = cutinCamZoom();
  for (int i = 0; i < 8; ++i)   // rows 0 and 1 (clip x, clip y)
    m[i] *= z;
}

// §39 light-fade fix. The 768/880 field/receiver material lays out per-frame
// transforms in bytes 0..319 (Model, ModelView, MVP@128, ViewProjection, ViewI),
// then the scene light constants in [320,624): g_af3PtLit{Pos,Col,Atten}0..3,
// g_f3DarkColor, g_nLightCount and the one-shot light — exactly the inputs the
// receiver VS combines into COLOR0. Because the receiver PS emits
// o0 = COLOR0 × (1+shadowFactor) with no color texture, a fading COLOR0 makes
// the (still-populated) shadow vanish. The shadow projection lives past 624
// (PSSGLightModelViewProjTex@752, tapScale@816) and MUST stay live, so the block
// stops short of it. We snapshot the block on a non-cinematic battle frame and
// stamp it back over cinematic frames so the light — and thus the shadow
// contrast — cannot fade during the cut-in.
constexpr size_t kLightBlockOff = 320;
constexpr size_t kLightBlockEnd = 624;   // exclusive; before PSSGLightMVPTex@752
std::mutex g_lightBaselineMutex;
std::array<uint8_t, kLightBlockEnd - kLightBlockOff> g_lightBaseline{};
std::atomic<bool> g_lightBaselineValid{false};

// Read-only diagnostic: dump a field/receiver material's light AND shadow
// constants so overview vs cut-in can be compared directly. Called from BOTH
// the Map path (768, DYNAMIC) and the UpdateSubresource path (880 receiver,
// DEFAULT) — the 880 is the material that actually samples the shadow map
// (tapScale/shadowLPos live only in it), and we were previously blind to it.
// Throttled per size so the 768 and 880 streams don't clobber each other.
void lightDiagLog(const void* data, uint32_t size) {
  if (!cutinLightRestoreEnabled() || !data || (size != 768 && size != 880))
    return;
  // Capture the state pointer ONCE — re-calling in the log() would race to null
  // on the render thread and stream a null const char* (crash).
  const char* st = arlandBattleStateName();
  if (!st)
    return;
  auto rf = [](const uint8_t* b, size_t off) {
    float v; std::memcpy(&v, b + off, 4); return v;
  };
  const uint8_t* d8 = static_cast<const uint8_t*>(data);
  int lightCount; std::memcpy(&lightCount, d8 + 524, 4);
  static std::atomic<uint64_t> g_lightLogTick768{0};
  static std::atomic<uint64_t> g_lightLogTick880{0};
  auto& gate = (size == 880) ? g_lightLogTick880 : g_lightLogTick768;
  const uint64_t tick = GetTickCount64() / 500;
  if (gate.exchange(tick) == tick)
    return;
  // 880-only shadow constants: tapScale@816 (PCF strength), the shadow
  // projection matrix PSSGLightModelViewProjTex@752 (16 floats; log its
  // diagonal + translation so a per-phase change is visible) and shadowLPos@832.
  // On the 768 material those bytes are overlay params, so skip them there.
  const float tap = (size == 880) ? rf(d8, 816) : 0.0f;
  log("LIGHT_DIAG ms=", GetTickCount64(), " size=", size,
      " state=", st, " cine=", arlandInCinematicBattle(),
      " nLight=", lightCount,
      " ptCol0=", rf(d8,384), ",", rf(d8,388), ",", rf(d8,392),
      " dark=", rf(d8,512), ",", rf(d8,516), ",", rf(d8,520),
      " oneCol=", rf(d8,592), ",", rf(d8,596), ",", rf(d8,600),
      " oneAtt=", rf(d8,608), ",", rf(d8,612), ",", rf(d8,616),
      " tapScale=", tap);
  if (size == 880) {
    // PSSGLightModelViewProjTex@752: flat row-major. Diagonal at +0,+20,+40,+60;
    // translation at +48,+52,+56. shadowLPos float3 @832.
    log("SHADOW_PROJ ms=", GetTickCount64(),
        " state=", st, " cine=", arlandInCinematicBattle(),
        " diag=", rf(d8,752), ",", rf(d8,772), ",", rf(d8,792), ",", rf(d8,812),
        " trans=", rf(d8,800), ",", rf(d8,804), ",", rf(d8,808),
        " shadowLPos=", rf(d8,832), ",", rf(d8,836), ",", rf(d8,840));
    // The receiver PS folds these output multipliers AFTER shadow: HdrRangeInv
    // (row 53 @848) and the overlay tail (852-864). HdrRangeInv measured flat
    // (0.5) so far; kept for confirmation.
    log("RECV_TAIL ms=", GetTickCount64(),
        " state=", st, " cine=", arlandInCinematicBattle(),
        " HdrRangeInv=", rf(d8,848),
        " overlayH=", rf(d8,852), " overlayMod=", rf(d8,856),
        " overlayFalloff=", rf(d8,860), ",", rf(d8,864));
    // FOG block (624-719). gHeightFogFarColor@688 feeds the lit term COLOR1 in
    // the receiver VS ([1320] mad o3, cb0[43], ...), so a fog darken here dims
    // the ground AND scales the shadow with it — top suspect for the 54% dim.
    log("RECV_FOG ms=", GetTickCount64(),
        " state=", st, " cine=", arlandInCinematicBattle(),
        " depthFogCol=", rf(d8,640), ",", rf(d8,644), ",", rf(d8,648),
        " heightMidCol=", rf(d8,672), ",", rf(d8,676), ",", rf(d8,680),
        " heightFarCol=", rf(d8,688), ",", rf(d8,692), ",", rf(d8,696),
        " fogDensity=", rf(d8,704), ",", rf(d8,708), ",", rf(d8,712), ",", rf(d8,716));
  }
}

// §21 broad net: log every SMALL constant-buffer write (<=256B, i.e. post/effect
// $Params, not the big material/skin buffers) during battle, change-detected and
// cine-tagged. Whatever value STEPS at the cut-in boundary is the darkener,
// regardless of which post-effect/setter drives it — catches the blind spots the
// float-uniform hook and material-cbuffer logging miss.
void logSmallCbChange(const void* data, uint32_t size, const void* resource) {
  if (!cutinLightRestoreEnabled() || !data)
    return;
  if (size < 16 || size > 256)
    return;
  const char* st = arlandBattleStateName();
  if (!st)
    return;
  float v[4] = {0, 0, 0, 0};
  std::memcpy(v, data, size < 16 ? size : 16);
  const uint64_t key =
    reinterpret_cast<uintptr_t>(resource) ^ (uint64_t(size) << 48);
  static std::mutex m;
  static std::unordered_map<uint64_t, std::pair<std::array<float, 4>, uint64_t>>
    last;
  const uint64_t tick = GetTickCount64();
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(m);
    auto it = last.find(key);
    if (it == last.end()) {
      last[key] = {{v[0], v[1], v[2], v[3]}, tick};
      changed = true;
    } else {
      float d = 0;
      for (int i = 0; i < 4; ++i) {
        float e = v[i] - it->second.first[i];
        d += e < 0 ? -e : e;
      }
      if (d > 1e-4f && tick - it->second.second > 200) {
        it->second = {{v[0], v[1], v[2], v[3]}, tick};
        changed = true;
      }
    }
  }
  if (changed)
    log("SMALLCB res=", resource, " size=", size,
        " cine=", arlandInCinematicBattle(), " state=", st,
        " v=", v[0], ",", v[1], ",", v[2], ",", v[3]);
}

void lightRestoreCbWrite(void* data, uint32_t size) {
  if (!cutinLightRestoreEnabled())
    return;
  lightDiagLog(data, size);
  if (size != 768 && size != 880)
    return;
  // Only act inside a battle; the field overworld uses the same material width
  // and must keep its own (correct) lighting.
  if (!arlandBattleStateName())
    return;
  uint8_t* p = static_cast<uint8_t*>(data) + kLightBlockOff;
  const size_t n = kLightBlockEnd - kLightBlockOff;
  const bool cine = arlandInCinematicBattle();
  std::lock_guard lock(g_lightBaselineMutex);
  if (cine) {
    if (g_lightBaselineValid)
      std::memcpy(p, g_lightBaseline.data(), n);   // hold the pre-cut-in light
  } else {
    std::memcpy(g_lightBaseline.data(), p, n);      // refresh baseline each frame
    g_lightBaselineValid = true;
  }
}

// Snapshot payload width. Covers the full 880-byte field/receiver material,
// including PSSGLightModelViewProjTex (+752), tapScale (+816) and shadowLPos
// (+832). Every consumer buffer MUST use this constant — readCb0Snap copies
// the full payload.
constexpr uint32_t kCbSnapBytes = 896;

struct CbSnap {
  uint32_t size = 0;      // full buffer byte width
  uint64_t seq = 0;       // global write sequence, for staleness checks
  uint8_t data[kCbSnapBytes] = {}; // first bytes of the latest write
};
mutex g_cbSnapMutex;
std::map<ID3D11Resource*, CbSnap> g_cbSnaps;
std::map<std::pair<ID3D11Resource*, UINT>, std::pair<const void*, uint32_t>>
    g_cbSnapPending;
std::atomic<uint64_t> g_cbSnapSeq{1};

void snapCbWrite(ID3D11Resource* resource, const void* data, uint32_t size) {
  CbSnap snap;
  snap.size = size;
  snap.seq = g_cbSnapSeq.fetch_add(1, std::memory_order_relaxed);
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

// DXBC bytecode of every vertex shader created while recon is on, keyed by the
// live shader object, so a draw's VS can be dumped to disk for offline layout
// analysis (RDEF / dcl_constantbuffer).
std::map<void*, std::vector<uint8_t>> g_vsBytecode;
std::set<void*> g_vsDumped;

void recordVsBytecode(ID3D11VertexShader* vs, const void* code, SIZE_T length) {
  std::lock_guard lock(g_cbSnapMutex);
  auto& bytes = g_vsBytecode[vs];
  bytes.assign(static_cast<const uint8_t*>(code),
               static_cast<const uint8_t*>(code) + length);
}

std::map<void*, std::vector<uint8_t>> g_psBytecode;
std::set<void*> g_psDumped;

void recordPsBytecode(ID3D11PixelShader* ps, const void* code, SIZE_T length) {
  std::lock_guard lock(g_cbSnapMutex);
  auto& bytes = g_psBytecode[ps];
  bytes.assign(static_cast<const uint8_t*>(code),
               static_cast<const uint8_t*>(code) + length);
}

void dumpPsOnce(void* ps, const char* role) {
  std::vector<uint8_t> bytes;
  {
    std::lock_guard lock(g_cbSnapMutex);
    if (!g_psDumped.insert(ps).second)
      return;
    auto it = g_psBytecode.find(ps);
    if (it == g_psBytecode.end()) {
      log("CUTIN_RECV_PSDUMP role=", role, " ps=", ps, " bytecode=missing");
      return;
    }
    bytes = it->second;
  }
  char name[96];
  std::snprintf(name, sizeof(name), "arland-ps-%s-%p.dxbc", role, ps);
  std::ofstream file(name, std::ios::out | std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  log("CUTIN_RECV_PSDUMP role=", role, " ps=", ps,
      " bytes=", bytes.size(), " file=", name);
}

void dumpVsOnce(void* vs, const char* role) {
  std::vector<uint8_t> bytes;
  {
    std::lock_guard lock(g_cbSnapMutex);
    if (!g_vsDumped.insert(vs).second)
      return;
    auto it = g_vsBytecode.find(vs);
    if (it == g_vsBytecode.end()) {
      log("CUTIN_RECON_VSDUMP role=", role, " vs=", vs, " bytecode=missing");
      return;
    }
    bytes = it->second;
  }
  char name[96];
  std::snprintf(name, sizeof(name), "arland-vs-%s-%p.dxbc", role, vs);
  std::ofstream file(name, std::ios::out | std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  log("CUTIN_RECON_VSDUMP role=", role, " vs=", vs,
      " bytes=", bytes.size(), " file=", name);
}

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
  if (cbSnapEnabled() && resource && mapped && mapped->pData) {
    D3D11_BUFFER_DESC desc = {};
    if (isConstantBuffer(resource, &desc)) {
      std::lock_guard lock(g_cbSnapMutex);
      g_cbSnapPending[{resource, sub}] = {mapped->pData, desc.ByteWidth};
    }
  }
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

// §32 lighting trace. Character skin (26048-byte $Globals): g_f3LitColor0 at
// +0, g_fLitAtten0 +12, g_f3DarkColor +16. Field/arena (768/880): four point
// lights g_af3PtLitCol0..3 at +384/400/416/432, g_f3DarkColor +512,
// g_nLightCount +524 (RDEF-recovered layouts). Logs a few writes per material
// class per battle-state change.
// §38c light-diff: cache each receiver/character material's full float block
// during a NON-cinematic (overview) frame, then when the SAME buffer is written
// during a cinematic state, log exactly which floats changed (byte offset,
// overview value -> cut-in value). Pinpoints the direct-light drop and proves
// whether it lives in this cbuffer (restorable) or not.
void traceLightDiff(ID3D11Resource* resource, const void* data, uint32_t size) {
  if (!cutinLightTraceEnabled())
    return;
  if (size != 880 && size != 768 && size != 26048)
    return;
  const char* state = arlandBattleStateName();
  if (!state)
    return;
  const bool cine = arlandInCinematicBattle();
  const float* f = reinterpret_cast<const float*>(data);
  // Diff SCALAR constants (skip per-frame matrices, which always differ). For
  // the 880 field material the scalar light/shadow params live at 320..751 and
  // 816..880; the tex matrix at 752..815 is reported by MAGNITUDE (a zeroed
  // matrix == shadow lookup disabled). 26048 skin: 0..32.
  auto isScalar = [&](size_t off) -> bool {
    if (size == 26048) return off < 32;
    return (off >= 320 && off < 752) || (off >= 816 && off < size);
  };
  static mutex m;
  struct Base { std::vector<float> vals; bool set = false; };
  static std::map<ID3D11Resource*, Base> baselines;
  std::lock_guard lock(m);
  Base& b = baselines[resource];
  const size_t n = size / 4;
  if (!cine) {
    b.vals.assign(f, f + n);
    b.set = true;
    return;
  }
  if (!b.set)
    return;
  static std::atomic<uint32_t> logs{0};
  if (logs.load(std::memory_order_relaxed) > 40)
    return;
  int changed = 0;
  char buf[512];
  int p = 0;
  for (size_t i = 0; i < n && i < b.vals.size(); ++i) {
    const size_t off = i * 4;
    if (!isScalar(off))
      continue;
    const float ov = b.vals[i], cv = f[i];
    if (std::fabs(ov - cv) > 1e-4f * (1.0f + std::fabs(ov))) {
      ++changed;
      if (p < int(sizeof(buf)) - 48)
        p += std::snprintf(buf + p, sizeof(buf) - p, "+%zu:%.3f->%.3f ",
                           off, ov, cv);
    }
  }
  // Tex-matrix magnitude (bytes 752..815) — overview vs cut-in.
  float texMagOv = 0, texMagCv = 0;
  if (size >= 816) {
    for (size_t i = 752 / 4; i < 816 / 4 && i < b.vals.size(); ++i) {
      texMagOv += std::fabs(b.vals[i]);
      texMagCv += std::fabs(f[i]);
    }
  }
  if ((changed || size >= 816) &&
      logs.fetch_add(1, std::memory_order_relaxed) <= 40)
    log("CUTIN_LIGHTDIFF size=", size, " state=", state,
        " changed=", changed, " texMag_ov=", texMagOv,
        " texMag_cut=", texMagCv, " ", buf);
}

void traceLightCbWrite(ID3D11Resource* resource, const void* data,
                       uint32_t size) {
  if (!cutinLightTraceEnabled())
    return;
  traceLightDiff(resource, data, size);
  if (size != 26048 && size != 768 && size != 880)
    return;
  const char* state = arlandBattleStateName();
  if (!state)
    return;
  // Log DISTINCT value tuples per material class per state (the first writes
  // each frame are always the same neutral placeholder materials — sampling
  // only those hides the live rigs).
  static mutex traceMutex;
  struct SizeTrace { const char* state = nullptr; std::set<std::string> seen; };
  static std::map<uint32_t, SizeTrace> perSize;
  const float* f = reinterpret_cast<const float*>(data);
  const uint8_t* base = static_cast<const uint8_t*>(data);
  char key[256];
  if (size == 26048) {
    std::snprintf(key, sizeof(key), "%.3f,%.3f,%.3f|%.3f|%.3f,%.3f,%.3f",
      f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
  } else {
    const float* col = reinterpret_cast<const float*>(base + 384);
    const float* dark = reinterpret_cast<const float*>(base + 512);
    const int32_t count = *reinterpret_cast<const int32_t*>(base + 524);
    std::snprintf(key, sizeof(key),
      "%d|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f",
      count, col[0], col[1], col[2], col[4], col[5], col[6],
      col[8], col[9], col[10], col[12], col[13], col[14],
      dark[0], dark[1], dark[2]);
  }
  std::lock_guard lock(traceMutex);
  auto& entry = perSize[size];
  if (entry.state != state) {
    entry.state = state;
    entry.seen.clear();
  }
  if (entry.seen.size() >= 10 || !entry.seen.insert(key).second)
    return;
  if (size == 26048) {
    log("CUTIN_LIGHT state=", state, " cb=", reinterpret_cast<void*>(resource),
      " size=", size,
      " lit0=", f[0], ",", f[1], ",", f[2], " atten=", f[3],
      " dark=", f[4], ",", f[5], ",", f[6]);
  } else {
    const float* col = reinterpret_cast<const float*>(base + 384);
    const float* dark = reinterpret_cast<const float*>(base + 512);
    const int32_t count = *reinterpret_cast<const int32_t*>(base + 524);
    log("CUTIN_LIGHT state=", state, " cb=", reinterpret_cast<void*>(resource),
      " size=", size, " nlights=", count,
      " col0=", col[0], ",", col[1], ",", col[2],
      " col1=", col[4], ",", col[5], ",", col[6],
      " col2=", col[8], ",", col[9], ",", col[10],
      " col3=", col[12], ",", col[13], ",", col[14],
      " dark=", dark[0], ",", dark[1], ",", dark[2]);
  }
}

void captureCbUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* resource,
                    UINT sub) {
  if (cbSnapEnabled()) {
    std::pair<const void*, uint32_t> pending{nullptr, 0};
    {
      std::lock_guard lock(g_cbSnapMutex);
      auto it = g_cbSnapPending.find({resource, sub});
      if (it != g_cbSnapPending.end()) {
        pending = it->second;
        g_cbSnapPending.erase(it);
      }
    }
    // Heal a hidden caster's matrix in place, then copy the CPU-visible
    // contents BEFORE the real Unmap invalidates them.
    if (pending.first) {
      healShadowCbWrite(ctx, resource, pending.first, pending.second);
      cameraZoomCbWrite(const_cast<void*>(pending.first), pending.second);
      lightRestoreCbWrite(const_cast<void*>(pending.first), pending.second);
      captureRecvTex(pending.first, pending.second);
      logSmallCbChange(pending.first, pending.second, resource);
      if (cutinDimHoldEnabled() && arlandInCinematicBattle() &&
          dimHoldPatch(const_cast<void*>(pending.first), pending.second)) {
        static std::atomic<uint64_t> t{0};
        const uint64_t k = GetTickCount64() / 500;
        if (t.exchange(k) != k)
          log("DIM_HOLD map res=", resource, " -> 1.0");
      }
      // §32b: trace large-cb writes on the Map path and hold the shadow gate
      // open here too — §16 concluded the 880 receiver is "never Map/Unmapped"
      // but that claim was made under a different build; if it IS mapped, this
      // is where the 0.7 lands, and Map'd memory can be patched in place.
      gateTraceCbWrite("map", pending.first, pending.second, resource);
      if (cutinGateHoldEnabled() && arlandInCinematicBattle() &&
          gateHoldPatch(const_cast<void*>(pending.first), pending.second)) {
        static std::atomic<uint64_t> gt{0};
        const uint64_t gk = GetTickCount64() / 500;
        if (gt.exchange(gk) != gk)
          log("GATE_HOLD map res=", resource, " diffuse@832 -> 1.0 (gate opened)");
      }
      snapCbWrite(resource, pending.first, pending.second);
      traceLightCbWrite(resource, pending.first, pending.second);
    }
  }
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
// The close-up/cut-in shadow was never implemented in Rorona: during action
// selection the engine hides the tactical model (scales it to ~0, so its caster
// draws a degenerate nothing) and draws the close-up mesh to color only. We
// re-issue that mesh into the shadow map at its CURRENT transform. The shadow
// VS (RDEF-confirmed) consumes ONE cbuffer at slot 0, 80 bytes: float4 diffuse
// + float4x4 PSSGLightModelViewProj — a per-caster baked model x light-VP. So
// the fix derives the light-VP each frame from a reference caster A drawn in
// both passes (L = S_A x W_A^-1; matrices flat row-major, S = L x W validated
// numerically against the recon log), then computes S_char = L x W_char from
// the close-up mesh's own Model matrix (captured from its color cbuffer write)
// and injects it via our own 80-byte constant buffer.
// Retarget draws (redraw recorded caster geometry at decoupled-mesh
// transforms) — §30m showed in-battle decoupled meshes are SCENERY, so this
// stays off unless explicitly probing the SelectCommand mini-stage.
bool cutinRetargetEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_RETARGET");
    return v && v[0] != '0';
  }();
  return enabled;
}

// §33 map-identity trace: assign short ids to every 1024x1024 shadow-map
// texture and record an ORDERED per-frame event stream — clears (C), caster
// draws (D), our injections (I), receiver samples (R), command-list
// executions (X) — flushed as one CUTIN_MAPFLOW line per frame. Answers which
// physical map each side touches and in what order.
bool cutinMapTraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_MAP_TRACE");
    return v && v[0] != '0';
  }();
  return enabled;
}

mutex g_mapTraceMutex;
std::map<void*, int> g_mapTexIds;      // texture/list/ctx ptr -> short id
std::set<void*> g_shadowMapTexSet;     // known 1024x1024 fmt44 textures
struct MapEvent { char kind; int id; int ctx; int id2; uint32_t count; };
std::vector<MapEvent> g_mapEvents;     // guarded by g_mapTraceMutex

int mapTraceId(void* p, const char* what) {
  auto it = g_mapTexIds.find(p);
  if (it != g_mapTexIds.end())
    return it->second;
  const int id = int(g_mapTexIds.size()) + 1;
  g_mapTexIds[p] = id;
  log("CUTIN_MAP id=", id, " ptr=", p, " kind=", what);
  return id;
}

void recordMapEvent(char kind, void* obj, const char* what,
                    ID3D11DeviceContext* ctx, void* obj2 = nullptr) {
  if (!cutinMapTraceEnabled())
    return;
  std::lock_guard lock(g_mapTraceMutex);
  const int id = mapTraceId(obj, what);
  if (kind == 'C' || kind == 'D' || kind == 'R' || kind == 'E')
    g_shadowMapTexSet.insert(obj);
  const int cid = ctx ? mapTraceId(ctx, "ctx") : 0;
  const int id2 = obj2 ? mapTraceId(obj2, what) : 0;
  if (!g_mapEvents.empty()) {
    MapEvent& last = g_mapEvents.back();
    if (last.kind == kind && last.id == id && last.ctx == cid &&
        last.id2 == id2) {
      ++last.count;
      return;
    }
  }
  if (g_mapEvents.size() < 192)
    g_mapEvents.push_back({kind, id, cid, id2, 1});
}

// Is this resource a known shadow-map texture? (registered at creation or
// first seen at a clear/draw/sample event)
bool isTracedShadowMapTex(void* p) {
  std::lock_guard lock(g_mapTraceMutex);
  return g_shadowMapTexSet.count(p) != 0;
}

// §33c depth readback: right after the engine's caster→receiver map copy,
// snapshot the RECEIVER map into a staging texture; two frames later read it
// on the immediate context and log a depth histogram. Ends all speculation
// about what actually reaches the sampled map (blocker present? casters
// present during cut-in? depth conventions?).
extern std::atomic<uint64_t> g_reconFrame;

bool cutinMapReadbackEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_MAP_READBACK");
    return v && v[0] != '0';
  }();
  return enabled;
}

std::atomic<ID3D11DeviceContext*> g_immCtx{nullptr};
ID3D11Texture2D* g_readbackTex = nullptr;          // guarded by g_mapTraceMutex
ID3D11Resource* g_recvMapTex = nullptr;            // guarded by g_mapTraceMutex
// §43 routing diagnostic: lock-free identity mirror of g_recvMapTex, so draw
// hooks and injectPendingShadows (which holds g_pipeMutex) can log the pointer
// without touching g_mapTraceMutex. NEVER dereferenced — identity only.
std::atomic<void*> g_recvMapTexPtr{nullptr};
std::atomic<uint64_t> g_readbackIssued{0};
std::atomic<uint64_t> g_readbackFrame{0};
std::atomic<uint32_t> g_readbackTries{0};

// Remember the receiver map (Q-copy destination); AddRef'd, replaced on change.
void rememberRecvMap(ID3D11Resource* dst) {
  g_recvMapTexPtr.store(dst, std::memory_order_relaxed);
  std::lock_guard lock(g_mapTraceMutex);
  if (g_recvMapTex == dst)
    return;
  if (g_recvMapTex)
    g_recvMapTex->Release();
  g_recvMapTex = dst;
  g_recvMapTex->AddRef();
}

// Present-side: snapshot the receiver map on the IMMEDIATE context (all
// command lists for this frame have executed by now), non-blocking read two
// frames later. No deferred-context staging traffic, no blocking Map.
void issueMapReadback() {
  const uint64_t frame = g_reconFrame.load(std::memory_order_relaxed);
  if (g_readbackIssued.load(std::memory_order_relaxed) || frame % 30 != 0)
    return;
  ID3D11DeviceContext* imm = g_immCtx.load(std::memory_order_relaxed);
  if (!imm)
    return;
  ID3D11Resource* src = nullptr;
  ID3D11Texture2D* staging = nullptr;
  bool needStaging = false;
  {
    std::lock_guard lock(g_mapTraceMutex);
    if (!g_recvMapTex)
      return;
    needStaging = g_readbackTex == nullptr;
  }
  if (needStaging) {
    // Created OUTSIDE g_mapTraceMutex: the CreateTexture2D hook takes the
    // same mutex for shadow-map registration (the §33c/§33d freezes were this
    // exact same-thread re-lock).
    ID3D11Device* dev = nullptr;
    imm->GetDevice(&dev);
    ID3D11Texture2D* nt = nullptr;
    if (dev) {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = 1024;
      td.Height = 1024;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R24G8_TYPELESS;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_STAGING;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      dev->CreateTexture2D(&td, nullptr, &nt);
      dev->Release();
    }
    if (!nt)
      return;
    std::lock_guard lock(g_mapTraceMutex);
    if (!g_readbackTex)
      g_readbackTex = nt;
    else
      nt->Release();
  }
  {
    std::lock_guard lock(g_mapTraceMutex);
    if (!g_recvMapTex || !g_readbackTex)
      return;
    src = g_recvMapTex;
    src->AddRef();
    staging = g_readbackTex;
  }
  getContextProcs(imm)->CopyResource(imm, staging, src);
  src->Release();
  g_readbackFrame.store(frame, std::memory_order_relaxed);
  g_readbackTries.store(0, std::memory_order_relaxed);
  g_readbackIssued.store(1, std::memory_order_relaxed);
}

void consumeMapReadback() {
  if (!cutinMapReadbackEnabled())
    return;
  issueMapReadback();
  if (!g_readbackIssued.load(std::memory_order_relaxed))
    return;
  const uint64_t frame = g_reconFrame.load(std::memory_order_relaxed);
  if (frame - g_readbackFrame.load(std::memory_order_relaxed) < 2)
    return;
  ID3D11DeviceContext* imm = g_immCtx.load(std::memory_order_relaxed);
  ID3D11Texture2D* tex;
  {
    std::lock_guard lock(g_mapTraceMutex);
    tex = g_readbackTex;
  }
  if (!imm || !tex) {
    g_readbackIssued.store(0, std::memory_order_relaxed);
    return;
  }
  D3D11_MAPPED_SUBRESOURCE m = {};
  const HRESULT hr = getContextProcs(imm)->Map(
    imm, tex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
  if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
    if (g_readbackTries.fetch_add(1, std::memory_order_relaxed) > 20)
      g_readbackIssued.store(0, std::memory_order_relaxed);
    return;
  }
  if (FAILED(hr)) {
    g_readbackIssued.store(0, std::memory_order_relaxed);
    return;
  }
  uint32_t clear1 = 0, clear0 = 0, low = 0, mid = 0, high = 0;
  float dmin = 2.0f, dmax = -1.0f;
  float center = -1.0f;
  // Spatial extent of caster (non-clear) texels, so we can SEE where the shadow
  // content sits in the map (UV 0..1) and compare to the injected St and to the
  // receiver's expected footprint. Accumulate in texel coords, report in UV.
  uint64_t sumX = 0, sumY = 0, casterN = 0;
  int minX = 1024, minY = 1024, maxX = -1, maxY = -1;
  for (int y = 0; y < 1024; y += 8) {
    const uint32_t* row = reinterpret_cast<const uint32_t*>(
      static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
    for (int x = 0; x < 1024; x += 8) {
      const float d = float(row[x] & 0x00FFFFFF) / 16777215.0f;
      if (x == 512 && y == 512)
        center = d;
      if (d >= 0.99999f) { ++clear1; continue; }
      if (d <= 0.00001f) { ++clear0; continue; }
      // Caster viewport remaps NDC z via [0.5,1.0]: blob NDC 0.3 -> 0.65.
      if (d < 0.70f) ++low;
      else if (d < 0.90f) ++mid;
      else ++high;
      dmin = std::min(dmin, d);
      dmax = std::max(dmax, d);
      sumX += uint32_t(x); sumY += uint32_t(y); ++casterN;
      minX = std::min(minX, x); maxX = std::max(maxX, x);
      minY = std::min(minY, y); maxY = std::max(maxY, y);
    }
  }
  getContextProcs(imm)->Unmap(imm, tex, 0);
  g_readbackIssued.store(0, std::memory_order_relaxed);
  const float cx = casterN ? float(sumX) / float(casterN) / 1024.0f : -1.0f;
  const float cy = casterN ? float(sumY) / float(casterN) / 1024.0f : -1.0f;
  log("CUTIN_MAP_DEPTH st=",
      arlandBattleStateName() ? arlandBattleStateName() : "-",
      " ones=", clear1, " zeros=", clear0,
      " low(<0.70)=", low, " mid=", mid, " high=", high,
      " min=", dmin, " max=", dmax, " center=", center,
      " samples=16384");
  log("CUTIN_MAP_SPAN st=",
      arlandBattleStateName() ? arlandBattleStateName() : "-",
      " casterTexels=", casterN, " centroidUV=", cx, ",", cy,
      " bboxUV=", (minX < 0 ? -1.0f : float(minX) / 1024.0f), ",",
      (minY < 0 ? -1.0f : float(minY) / 1024.0f), " -> ",
      (maxX < 0 ? -1.0f : float(maxX) / 1024.0f), ",",
      (maxY < 0 ? -1.0f : float(maxY) / 1024.0f));
}

void flushMapEvents() {
  if (!cutinMapTraceEnabled())
    return;
  std::vector<MapEvent> ev;
  {
    std::lock_guard lock(g_mapTraceMutex);
    ev.swap(g_mapEvents);
  }
  if (ev.empty())
    return;
  char line[1400];
  int n = 0;
  for (const MapEvent& e : ev) {
    if (n > int(sizeof(line)) - 32)
      break;
    n += std::snprintf(line + n, sizeof(line) - n, "%c%d", e.kind, e.id);
    if (e.id2)
      n += std::snprintf(line + n, sizeof(line) - n, "<%d", e.id2);
    if (e.ctx)
      n += std::snprintf(line + n, sizeof(line) - n, "@%d", e.ctx);
    if (e.count > 1)
      n += std::snprintf(line + n, sizeof(line) - n, "x%u", e.count);
    n += std::snprintf(line + n, sizeof(line) - n, " ");
  }
  const char* st = arlandBattleStateName();
  log("CUTIN_MAPFLOW st=", st ? st : "-", " ", line);
}

// §32h receiver-saturation test: every frame, draw the largest recorded
// caster into the shadow map as a HUGE blocker (map-centered, near depth) in
// EVERY battle state. Overview darkening validates the tool; if the cut-in
// then shows no change, the receiver's shadow contribution is disabled or
// saturated during actions and no caster-side work can ever be visible.
// Mode 1: draw the blocker through our own injection path (drawGeometry).
// Mode 2: hijack ONE healthy caster cb write per frame and turn the ENGINE's
// own draw into the blocker — bisects "injection path broken" (mode 2 lands
// depth, mode 1 does not) from "matrix convention wrong" (neither lands).
// Healed-shadow 3x3 amplification for visibility testing (default 1.0).
float cutinHealAmp() {
  static const float amp = [] {
    const char* v = std::getenv("ARLAND_CUTIN_HEAL_AMP");
    return v ? float(std::atof(v)) : 1.0f;
  }();
  return amp;
}

// §33n injection bisect INSIDE a live caster draw: 1 = double-draw the
// engine's own draw verbatim (baseline; must be harmless), 2 = double-draw
// with OUR cb0 bound (blob matrix; tests the cb write+bind in isolation),
// 3 = double-draw with RECORDED geometry+IL+VS under the engine's cb (tests
// geometry binding in isolation).
int cutinInjTestMode() {
  static const int mode = [] {
    const char* v = std::getenv("ARLAND_CUTIN_INJTEST");
    return v ? std::atoi(v) : 0;
  }();
  return mode;
}
std::atomic<uint32_t> g_injTestThisFrame{0};
std::atomic<uint32_t> g_injTestLogs{0};

int cutinBlockerMode() {
  static const int mode = [] {
    const char* v = std::getenv("ARLAND_CUTIN_BLOCKER");
    return v ? std::atoi(v) : 0;
  }();
  return mode;
}

bool cutinBlockerEnabled() {
  return cutinBlockerMode() == 1;
}

std::atomic<uint32_t> g_blockerHijacks{0};   // per-frame budget, reset at Present

inline int kRot9(int k) {
  static const int idx[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
  return idx[k];
}

// Character-scale healthy 3x3, cached at CHARACTER caster draws (count>=300
// guarantees character geometry — the ring cb's write-side cache could hold a
// tiny prop/effect scale, §33h: healed shadows were real but microscopic).
struct CharRot { float rot[9] = {}; uint64_t frame = 0; bool valid = false; };
CharRot g_charRot;                            // guarded by g_pipeMutex

// §33k: live color-pass Model matrix per vertex buffer, captured at character
// color draws whose VB also cast this frame — INCLUDING the cut-in attacker
// (its color mesh shares the caster VB, which is why the old decoupled-mesh
// filter always excluded it). Healing pairs a degenerate caster write with
// the geometry bound at that moment and rebuilds S = L x W_color: live
// attacker transform, correct scale and position, no caches.
struct ColorW { float W[16]; uint64_t frame; };
std::map<uintptr_t, ColorW> g_colorW;         // guarded by g_pipeMutex
std::atomic<uint32_t> g_blocker2Logs{0};

bool cutinShadowFixEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_BATTLE_CUTIN_SHADOW");
    return v && v[0] != '0';
  }();
  return enabled;
}

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

float mtxMaxDiff(const float* a, const float* b) {
  float err = 0.0f;
  for (int i = 0; i < 16; ++i)
    err = std::max(err, std::fabs(a[i] - b[i]));
  return err;
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

// Model matrix location inside the known color materials (RDEF-confirmed):
// the character skin material's 26048-byte $Globals holds Model at byte 32;
// the edge/outline material's 256-byte $Globals at byte 64; the two field
// materials (768- and 880-byte $Globals) at byte 0.
bool extractModel(uint32_t cbSize, const uint8_t* data, float outW[16]) {
  size_t offset;
  if (cbSize == 26048)
    offset = 32;
  else if (cbSize == 256)
    offset = 64;
  else if (cbSize == 768 || cbSize == 880)
    offset = 0;
  else
    return false;
  std::memcpy(outW, data + offset, 64);
  return true;
}

struct ShadowPipe {
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  UINT stencilRef = 0;
  D3D11_VIEWPORT vp = {};
  bool valid = false;
};
mutex g_pipeMutex;
ShadowPipe g_shadowPipe;
std::set<uintptr_t> g_frameShadowVBs;   // VBs already cast this frame
std::atomic<uint64_t> g_reissueTotal{0};
std::atomic<uint64_t> g_reconFrame{0};  // frame id (incremented at Present)

// Per-VB shadow matrix captured this frame, and the light-VP derived from a
// reference caster drawn in both passes. The light-VP drifts over time (the
// shadow bounds appear to re-center), so it is re-derived every frame a valid
// pair exists and its age is logged at re-issue.
struct CasterShadow {
  float S[16];
  float diffuse[4];
  uint64_t frame = 0;
};
std::map<uintptr_t, CasterShadow> g_casterShadow;  // guarded by g_shadowTraceMutex
struct LightState {
  float L[16];
  float diffuse[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  uint64_t frame = 0;
  bool valid = false;
};
LightState g_light;                                // guarded by g_pipeMutex
ID3D11Buffer* g_injectCb = nullptr;                // guarded by g_pipeMutex
ID3D11Buffer* g_injectCbV2 = nullptr;              // §41: 26048-byte skin cb clone
std::atomic<uint64_t> g_lvpDerives{0};
std::atomic<uint64_t> g_lvpChecks{0};
std::atomic<uint64_t> g_injectLogs{0};
std::atomic<uint64_t> g_queueLogs{0};
std::atomic<uint64_t> g_rearmLogs{0};

// A decoupled mesh recorded at its color draw, to be drawn into the NEXT
// frame's shadow pass. Injecting during the color pass is useless: the ground
// receiver draws that sample the shadow map have already executed by the time
// the character is drawn, so a same-frame late depth write is never read.
// One frame of shadow latency instead. Holds refs on vb/ib.
struct PendingInject {
  ID3D11Buffer* vb[4] = {};            // all leading streams, not just 0 —
  UINT stride[4] = {}, offset[4] = {}; // positions/UVs may be split
  ID3D11Buffer* ib = nullptr;
  DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
  UINT ibOffset = 0;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  bool indexed = false;
  UINT count = 0, startIndex = 0;
  INT baseVertex = 0;
  uintptr_t vbKey = 0;
  uint32_t cb0Size = 0;   // which color material the geometry came from
  float W[16];
  // §41 V2: the color draw's own input layout + vertex shader (AddRef'd) —
  // the only pipeline that interprets this VB's vertex format correctly.
  ID3D11InputLayout* il = nullptr;
  ID3D11VertexShader* vs = nullptr;
};
std::vector<PendingInject> g_injectReady;  // consumed in this frame's shadow pass
std::vector<PendingInject> g_injectNext;   // recorded during this frame's color pass
// §43: the frame renders MULTIPLE distinct 1024x1024 shadow maps (SelectCommand
// readback showed two live depth populations), and the cut-in ground samples
// one that a single-shot per-frame injection never reached (FLATZ proved the
// caster present in the readback map while the ground stayed shadowless). So
// instead of a single "injected this frame" bool, track WHICH shadow-map DSV
// textures have already received this frame's injection and piggyback every
// distinct shadow-map pass. Pointers are identity keys only (never
// dereferenced); cleared at Present, entry erased when that map is cleared
// mid-frame (re-arm into the new generation).
std::set<void*> g_injectedDsvTextures;     // all three guarded by g_pipeMutex

void releasePendingRefs(PendingInject& e) {
  for (auto* b : e.vb) if (b) b->Release();
  if (e.ib) e.ib->Release();
  if (e.il) e.il->Release();
  if (e.vs) e.vs->Release();
  e.il = nullptr;
  e.vs = nullptr;
}

void releasePendingList(std::vector<PendingInject>& list) {
  for (auto& e : list)
    releasePendingRefs(e);
  list.clear();
}

// Verbatim-replay probe (ARLAND_CUTIN_REPLAY=1): remember real character-caster
// shadow draws (geometry, input layout, exact 80-byte params) while their
// matrices are non-degenerate, and re-draw them every frame. While the engine
// draws them too this is a harmless identical overdraw; when the engine hides
// the model (close-up scales it to ~0) the replay keeps the last good caster
// alive. A byte-identical replay of draws that previously produced shadows —
// if even this shows nothing during the close-up, the receiver side is the
// problem, not our casters.
bool cutinReplayEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_REPLAY");
    return v && v[0] != '0';
  }();
  return enabled;
}

// §25 fix: render the injected caster with the RECEIVER's own projection
// (PSSGLightModelViewProjTex @752, unbiased to clip via M_b^-1) instead of the
// caster-derived light-VP, so the silhouette lands where the receiver samples.
// injUV != recvUV proved the two diverge during the cut-in.
bool cutinInjectRecvEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_INJECT_RECV");
    return v && v[0] != '0';
  }();
  return enabled;
}

// World->shadow-UV matrix from the real ground receiver, this/last frame.
// Best (= largest element count) qualifying draw wins each frame so a small
// prop's receiver matrix cannot displace the arena ground's.
float g_texWorld[16] = {};
std::atomic<bool> g_texWorldValid{false};
std::mutex g_texWorldMutex;
uint64_t g_texWorldFrame = 0;    // frame of current best capture (mutex)
UINT g_texWorldBestCount = 0;    // element count of that capture (mutex)

struct ReplayDraw {
  PendingInject geo;
  ID3D11InputLayout* il = nullptr;
  ID3D11VertexShader* vs = nullptr;   // §33m: IL must be bound WITH its VS
  uint8_t params[80] = {};
  uint64_t frame = 0;
};
std::map<std::tuple<uintptr_t, UINT, INT>, ReplayDraw> g_replayDraws;  // g_pipeMutex
// §33s: color-mesh VB -> shadow-proxy recording key. Characters cast with
// SEPARATE low-poly proxy meshes (different VB + input layout), so vbKey
// matching never links the cut-in color mesh to its proxy. Pair them by
// world position while both are healthy in the overview, then reuse the
// pairing during the action when the color mesh moves to the mini-stage.
std::map<uintptr_t, std::tuple<uintptr_t, UINT, INT>> g_colorToProxy;  // g_pipeMutex

void releaseReplayDraw(ReplayDraw& r) {
  releasePendingRefs(r.geo);
  if (r.il) r.il->Release();
  r.il = nullptr;
  if (r.vs) r.vs->Release();
  r.vs = nullptr;
}

// Geometry-injection draws (the §30e–§30i approach) are opt-in now that
// matrix healing exists; they only ever carried outline geometry anyway.
bool cutinInjectDrawsEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("ARLAND_CUTIN_INJECT_DRAWS");
    return v && v[0] != '0';
  }();
  return enabled;
}

// Matrix healing: the engine hides the close-up character's tactical model by
// ZERO-SCALING it (§30d), so its caster still draws each frame and writes an
// 80-byte shadow params buffer whose matrix is S = L x W0 — collapsed 3x3,
// INTACT translation. Rewrite such writes in place, before the real Unmap,
// with S' = L x W_closeup (the close-up character's Model recorded at its
// color draws last frame), pairing hidden caster to close-up character by
// nearest light-space translation. The engine then renders its own caster
// geometry/pose/pipeline with a healthy matrix — nothing is injected at all.
std::atomic<uint64_t> g_healLogs{0};
std::atomic<uint64_t> g_healTotal{0};

// Rotation/scale block of the most recent HEALTHY write PER CONSTANT BUFFER.
// Each caster's params live in its own cb object, so healing a degenerate
// write with the SAME resource's last healthy 3x3 pairs the hidden caster with
// itself — correct scale and facing by construction, no distance heuristics
// (§30k's nearest-pairing grabbed scenery transforms and smeared the map).
struct HealthyRot { float rot[9]; uint64_t frame; };
std::map<ID3D11Resource*, HealthyRot> g_healthyByCb;  // guarded by g_pipeMutex
// §32e heal-flow accounting: which cbs were healed this frame, and whether a
// character-caster shadow DRAW consumed a healed cb (if draws never consume
// them, the engine skips draw emission for hidden casters and constant healing
// can never work — injection with the healed matrix would be required).
std::map<ID3D11Resource*, uint64_t> g_healedCbs;      // guarded by g_pipeMutex
std::atomic<uint32_t> g_healsThisFrame{0};
std::atomic<uint32_t> g_healedDrawsThisFrame{0};

// §32f freeze-clone: the engine hides a caster by zero-scaling its PRE-POSED
// vertex stream as well as its matrix, so a healed matrix still rasterizes
// nothing. We keep our own copy of each character-caster's stream-0 vertex
// buffer, refreshed while the caster is healthy, and at a healed (hidden)
// caster draw we emit ONE extra draw with the frozen clone bound — the last
// healthy pose, translated by the healed (animating) matrix.
struct VbClone { ID3D11Buffer* clone[4] = {}; uint64_t refreshed = 0; };
std::map<uintptr_t, VbClone> g_vbClones;              // guarded by g_pipeMutex
struct HealedSlice { std::array<uint8_t, 80> bytes; uintptr_t vbKey; };
std::vector<HealedSlice> g_healedSlices;      // guarded by g_pipeMutex
std::atomic<uint32_t> g_pairOk{0}, g_pairMismatch{0};
std::atomic<uint32_t> g_cloneDraws{0};
std::atomic<uint32_t> g_cloneDrawLogs{0};

void healShadowCbWrite(ID3D11DeviceContext* ctx, ID3D11Resource* resource,
                       const void* data, uint32_t size) {
  if (size != 80 || !cutinShadowFixEnabled())
    return;
  float* f = reinterpret_cast<float*>(const_cast<void*>(data));
  float* m = f + 4;
  // Shadow params hold an affine matrix (bottom flat row 0,0,0,1).
  if (std::fabs(m[12]) > 1e-6f || std::fabs(m[13]) > 1e-6f ||
      std::fabs(m[14]) > 1e-6f || std::fabs(m[15] - 1.0f) > 1e-4f)
    return;
  static const int kRot[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
  float scale = 0.0f;
  for (int i : kRot)
    scale = std::max(scale, std::fabs(m[i]));
  const uint64_t frame = g_reconFrame.load(std::memory_order_relaxed);
  if (scale > 1e-5f) {            // healthy caster: remember its 3x3, leave it
    // §33f/§33g blocker mode 2: rewrite the first 8 healthy writes per frame
    // into huge map-centered near-depth blobs — the ENGINE's own draws become
    // the blocker (validated: the §33g "rectangle"; viewport remaps NDC z 0.3
    // to map depth 0.65).
    if (cutinBlockerMode() == 2 &&
        g_blockerHijacks.fetch_add(1, std::memory_order_relaxed) < 8) {
      for (int c = 0; c < 3; ++c) {
        m[0 + c] *= 60.0f;         // row 0 (x), amplified
        m[4 + c] *= 60.0f;         // row 1 (y), amplified
      }
      m[3] = 0.0f;                 // centered in the map
      m[7] = 0.0f;
      m[8] = 0; m[9] = 0; m[10] = 0; m[11] = 0.3f;   // constant NDC near depth
      if (g_blocker2Logs.fetch_add(1, std::memory_order_relaxed) % 60 == 0)
        log("CUTIN_BLOCKER2 hijacked healthy caster write");
      return;
    }
    std::lock_guard lock(g_pipeMutex);
    HealthyRot& h = g_healthyByCb[resource];
    for (int k = 0; k < 9; ++k)
      h.rot[k] = m[kRot[k]];
    h.frame = frame;
    return;
  }
  // Degenerate = hidden model. The engine keeps ANIMATING the hidden caster
  // (its translation tracks the attack cinematic), so KEEP the written
  // translation and rebuild only the collapsed 3x3. Only during cinematic
  // battle states — a despawned object elsewhere must not grow a ghost shadow.
  if (!arlandInCinematicBattle())
    return;
  const float t[3] = {m[3], m[7], m[11]};
  (void)t;
  if (cutinBlockerMode() == 3 &&
      g_blockerHijacks.fetch_add(1, std::memory_order_relaxed) < 8) {
    // §33g mode 3: give the engine's own HIDDEN-caster draw a huge valid
    // matrix. A rectangle during WaitAction proves the hidden caster's
    // vertices are alive and its draw emits — i.e. plain healing suffices
    // and only the healed shadow's size/placement was below notice.
    std::lock_guard lock(g_pipeMutex);
    auto hb = g_healthyByCb.find(resource);
    if (hb != g_healthyByCb.end()) {
      static const int kR[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
      for (int k = 0; k < 6; ++k)
        m[kR[k]] = hb->second.rot[k] * 60.0f;
      m[3] = 0.0f;
      m[7] = 0.0f;
      m[8] = 0; m[9] = 0; m[10] = 0; m[11] = 0.3f;
      if (g_blocker2Logs.fetch_add(1, std::memory_order_relaxed) % 60 == 0)
        log("CUTIN_BLOCKER3 hijacked DEGENERATE caster write");
      return;
    }
  }
  // §37 corrected heal. The degenerate write ALREADY carries a current-frame,
  // receiver-aligned translation: the engine keeps recomputing S = L_cur x W
  // and only zeroes the 3x3, so m[3],m[7],m[11] animate with the lunge and sit
  // exactly where the receiver samples this frame. Rebuild ONLY the 3x3 from
  // THIS frame's light matrix at the uniform Arland character scale (0.01),
  // keeping the engine's translation. Position + scale + light-matrix are then
  // all current-frame -> aligned with the receiver by construction. (Earlier
  // heals used a CACHED 3x3 = a STALE light matrix, which misaligned; §37.)
  // Optional exact rotation from a same-frame color Model of the same vb (only
  // when the proxy shares the color VB); otherwise 0.01 x L_3x3 is a correct-
  // SIZED ground blob — rotation is invisible at 0.01 scale.
  ID3D11Buffer* vb0 = nullptr;
  UINT st0 = 0, so0 = 0;
  ctx->IAGetVertexBuffers(0, 1, &vb0, &st0, &so0);
  const uintptr_t vbKey = reinterpret_cast<uintptr_t>(vb0);
  if (vb0)
    vb0->Release();

  static const int kR[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
  const float amp = cutinHealAmp();       // 1 = real size; >1 for visibility A/B
  float rot3[9];
  bool haveExact = false;
  {
    std::lock_guard lock(g_pipeMutex);
    if (!g_light.valid || frame - g_light.frame > 2) {
      static std::atomic<uint32_t> nolight{0};
      if (nolight.fetch_add(1, std::memory_order_relaxed) % 600 == 0)
        log("CUTIN_HEALSKIP reason=no_current_light frame=", frame,
            " light_frame=", g_light.frame);
      return;                              // no aligned light this frame
    }
    // Exact: if a same-frame color Model exists for this vb, S = L x W_color.
    auto it = vbKey ? g_colorW.find(vbKey) : g_colorW.end();
    if (it != g_colorW.end() && frame - it->second.frame <= 2) {
      float S[16];
      mtxMul(g_light.L, it->second.W, S);
      for (int k = 0; k < 9; ++k) rot3[k] = S[kR[k]];
      haveExact = true;
    } else {
      // Reliable: 3x3 = 0.01 x L_3x3 (character-scale blob, current-frame L).
      for (int k = 0; k < 9; ++k) rot3[k] = g_light.L[kR[k]] * 0.01f;
    }
  }
  for (int k = 0; k < 9; ++k)
    m[kR[k]] = rot3[k] * amp;
  m[11] -= 0.003f;                         // §33i tiny depth bias toward light
  {
    std::lock_guard lock(g_pipeMutex);
    g_healedCbs[resource] = frame;
  }
  g_healsThisFrame.fetch_add(1, std::memory_order_relaxed);
  g_healTotal.fetch_add(1, std::memory_order_relaxed);
  if (g_healLogs.fetch_add(1, std::memory_order_relaxed) % 120 == 0)
    log("CUTIN_HEAL total=", g_healTotal.load(std::memory_order_relaxed),
        " mode=", haveExact ? "exact" : "scale",
        " t=", m[3], ",", m[7], ",", m[11]);
}

void releaseShadowPipe(ShadowPipe& p) {
  if (p.dsv) p.dsv->Release();
  if (p.vs) p.vs->Release();
  if (p.ps) p.ps->Release();
  if (p.il) p.il->Release();
  if (p.rs) p.rs->Release();
  if (p.dss) p.dss->Release();
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
  ctx->PSGetShader(&p.ps, nullptr, nullptr);
  ctx->IAGetInputLayout(&p.il);
  ctx->RSGetState(&p.rs);
  ctx->OMGetDepthStencilState(&p.dss, &p.stencilRef);
  UINT n = 1;
  ctx->RSGetViewports(&n, &p.vp);
  p.valid = p.vs != nullptr;
  std::lock_guard lock(g_pipeMutex);
  releaseShadowPipe(g_shadowPipe);
  g_shadowPipe = p;
}

// Draw last frame's decoupled meshes into the CURRENT shadow pass. Called from
// the draw hook of a REAL character-caster shadow draw, before that draw is
// forwarded — so the depth target, viewport, VS, IL, PS, rasterizer and
// depth-stencil state are all a live character caster's own (§30f showed that
// inheriting the pass's FIRST draw instead risks piggybacking a prologue draw
// whose state silently no-ops our depth writes). Only the mesh geometry and
// our params cbuffer are swapped in and restored. Runs once per frame (or once
// per shadow-map clear generation).
void injectPendingShadows(ID3D11DeviceContext* ctx) {
  // Holds g_pipeMutex throughout: the pending list stays owned by
  // g_injectReady (released at Present, so a mid-frame shadow-map clear can
  // re-arm and the batch is re-drawn into the last cleared generation).
  std::lock_guard lock(g_pipeMutex);
  const bool doInject = cutinInjectDrawsEnabled() && !g_injectReady.empty() &&
                        g_light.valid;
  const bool doReplay = cutinReplayEnabled() && !g_replayDraws.empty();
  // Retarget: for each visible-not-casting mesh, redraw ITS OWN recorded
  // caster geometry (matched exactly by vertex-buffer key — the decoup VB is
  // that character's caster VB, that is how it passed the everCast filter) at
  // the close-up/cinematic transform. Covers the SelectCommand mini-stage,
  // where the real caster keeps casting at the arena spot the camera no
  // longer looks at.
  const bool doRetarget = cutinRetargetEnabled() &&
      !g_injectReady.empty() && g_light.valid &&
                          !g_replayDraws.empty();
  const bool doBlocker = cutinBlockerEnabled() && !g_replayDraws.empty();
  // §41 V2: re-draw the queued color meshes with their OWN skin pipeline and
  // the receiver-derived world->clip projection. Independent of g_light.
  const bool v2Active = cutinInjectV2Enabled() && !g_injectReady.empty();
  const bool doInjectV2 =
      v2Active && g_texWorldValid.load(std::memory_order_relaxed);
  if (!doInject && !doReplay && !doRetarget && !doBlocker && !v2Active)
    return;
  // §43: one injection per DISTINCT shadow-map DSV texture per frame. The
  // caller guarantees the bound DSV is a 1024x1024 shadow map; resolve its
  // texture and skip only if THIS map already got the batch, so every live
  // shadow map — including whichever one the cut-in ground actually samples —
  // receives the injected caster.
  void* dsvTexKey = nullptr;
  {
    ID3D11RenderTargetView* rtv0 = nullptr;
    ID3D11DepthStencilView* dsv0 = nullptr;
    ctx->OMGetRenderTargets(1, &rtv0, &dsv0);
    if (rtv0)
      rtv0->Release();
    if (dsv0) {
      ID3D11Resource* res0 = nullptr;
      dsv0->GetResource(&res0);
      if (res0) {
        dsvTexKey = res0;          // identity only, never dereferenced
        res0->Release();
      }
      dsv0->Release();
    }
  }
  if (!g_injectedDsvTextures.insert(dsvTexKey).second)
    return;                        // this map already injected this frame
  // §43 routing diagnostic: which map are we injecting INTO, vs the map the
  // readback watches (recvMapTex) — compare against GROUND_MAP groundSrvTex.
  if (v2Active) {
    static std::mutex injMapMutex;
    static std::map<void*, uint64_t> injMapTick;
    const uint64_t tick = GetTickCount64();
    bool doLog = false;
    {
      std::lock_guard<std::mutex> l(injMapMutex);
      auto& t = injMapTick[dsvTexKey];
      if (tick - t > 500 || t == 0) { t = tick; doLog = true; }
    }
    if (doLog)
      log("INJECT_MAP dsvTex=", dsvTexKey,
          " recvMapTex=", g_recvMapTexPtr.load(std::memory_order_relaxed));
  }
  const LightState& light = g_light;
  // §25: choose the projection the caster is rendered with. Default is the
  // caster-derived light-VP (light.L). With ARLAND_CUTIN_INJECT_RECV, use the
  // RECEIVER's own matrix instead (M_b^-1 x PSSGLightModelViewProjTex) so the
  // silhouette lands exactly where the receiver samples (injUV==recvUV).
  float Lrecv[16];
  const bool useRecv =
    cutinInjectRecvEnabled() && g_recvTexValid.load(std::memory_order_relaxed);
  if (useRecv) {
    // Unbias UV->NDC on xy (rows 0,1) AND pre-compensate the shadow viewport's
    // [0.5,1.0] depth remap on z (row2 = [0,0,2,-1]): Tex.z is already in
    // [0.5,1.0], so ndc_z = 2*Tex.z-1 makes viewport(ndc_z)=Tex.z, matching the
    // receiver's compare space exactly. Without this the depth is remapped twice.
    static const float Binv[16] = {2, 0, 0, -1,  0, -2, 0, 1,
                                   0, 0, 2, -1,  0,  0, 0, 1};
    float T[16];
    { std::lock_guard<std::mutex> lock(g_recvTexMutex); std::memcpy(T, g_recvTex, 64); }
    mtxMul(Binv, T, Lrecv);
  }
  const float* const Linj = useRecv ? Lrecv : light.L;
  const uint64_t frame = g_reconFrame.load(std::memory_order_relaxed);
  if (!g_injectCb) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) {
      D3D11_BUFFER_DESC desc = {};
      // 96 (not 80) bytes: the extra 16 keep healShadowCbWrite from matching
      // our own buffer, which lets the write go through the HOOKED Map/Unmap
      // like every engine write (§33l: the unhooked deferred-context Map is
      // the prime suspect for the zero-fragment injection bug).
      desc.ByteWidth = 96;
      desc.Usage = D3D11_USAGE_DYNAMIC;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      dev->CreateBuffer(&desc, nullptr, &g_injectCb);
      dev->Release();
    }
  }
  ID3D11Buffer* inject = g_injectCb;
  if (!inject)
    return;

  // Save only what we swap: geometry bindings and cb0. Shaders and all
  // output/raster state stay the piggybacked character draw's.
  ID3D11Buffer* savedVb[4] = {};
  UINT savedStride[4] = {}, savedOffset[4] = {};
  ctx->IAGetVertexBuffers(0, 4, savedVb, savedStride, savedOffset);
  ID3D11Buffer* savedIb = nullptr;
  DXGI_FORMAT savedIbFmt = DXGI_FORMAT_UNKNOWN;
  UINT savedIbOff = 0;
  ctx->IAGetIndexBuffer(&savedIb, &savedIbFmt, &savedIbOff);
  D3D11_PRIMITIVE_TOPOLOGY savedTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ctx->IAGetPrimitiveTopology(&savedTopo);
  ID3D11Buffer* savedCb0 = nullptr;
  ctx->VSGetConstantBuffers(0, 1, &savedCb0);
  if (cutinMapTraceEnabled()) {
    ID3D11RenderTargetView* mrtv = nullptr;
    ID3D11DepthStencilView* mdsv = nullptr;
    ctx->OMGetRenderTargets(1, &mrtv, &mdsv);
    if (mdsv) {
      ID3D11Resource* mres = nullptr;
      mdsv->GetResource(&mres);
      if (mres) {
        recordMapEvent('I', mres, "shadow_map", ctx);
        mres->Release();
      }
      mdsv->Release();
    }
    if (mrtv)
      mrtv->Release();
  }

  const auto drawGeometry = [&](const PendingInject& geo,
                                const float params[20]) -> bool {
    // §33l: write through the HOOKED Map/Unmap so our buffer takes exactly
    // the same path as engine writes (safe now: 96-byte buffer never matches
    // the healer's 80-byte filter, so no §30k re-lock).
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(inject, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return false;
    std::memcpy(mapped.pData, params, 80);
    std::memset(static_cast<uint8_t*>(mapped.pData) + 80, 0, 16);
    ctx->Unmap(inject, 0);
    ctx->IASetVertexBuffers(0, 4, const_cast<ID3D11Buffer* const*>(geo.vb),
                            geo.stride, geo.offset);
    if (geo.indexed)
      ctx->IASetIndexBuffer(geo.ib, geo.ibFormat, geo.ibOffset);
    ctx->IASetPrimitiveTopology(geo.topology);
    ctx->VSSetConstantBuffers(0, 1, &inject);
    if (geo.indexed)
      getContextProcs(ctx)->DrawIndexed(ctx, geo.count, geo.startIndex,
                                        geo.baseVertex);
    else
      getContextProcs(ctx)->Draw(ctx, geo.count, geo.startIndex);
    return true;
  };

  int drawn = 0;
  float firstS[4] = {};
  float firstWt[3] = {};
  uint32_t firstCb0 = 0;
  // §27 debug: force the injected caster to the NEAREST shadow depth (clip.z=0 ->
  // stored 0.5, below any ground depth 0.5-1.0) so it MUST occlude if the ground
  // reads our injection. A character-shaped dark patch => pipeline works, only
  // depth/placement precision was off. Nothing => the ground isn't reading our
  // injected content (clear/generation issue). Gate: ARLAND_CUTIN_INJECT_FLATZ.
  static const bool flatZ = [] {
    const char* v = std::getenv("ARLAND_CUTIN_INJECT_FLATZ");
    return v && v[0] != '0';
  }();
  if (doInject) {
    for (auto& e : g_injectReady) {
      float params[20];
      std::memcpy(params, light.diffuse, 16);
      mtxMul(Linj, e.W, params + 4);      // S = Linj x W (recv or caster proj)
      if (flatZ) {                         // zero the z-row -> clip.z = 0 everywhere
        params[4 + 8] = params[4 + 9] = params[4 + 10] = params[4 + 11] = 0.0f;
      }
      if (!drawn) {
        firstS[0] = params[4 + 3];       // injected origin clip x,y,z,w = column 3
        firstS[1] = params[4 + 7];
        firstS[2] = params[4 + 11];
        firstS[3] = params[4 + 15];
        firstWt[0] = e.W[3]; firstWt[1] = e.W[7]; firstWt[2] = e.W[11];
        firstCb0 = e.cb0Size;
      }
      if (!drawGeometry(e, params))
        break;
      ++drawn;
    }
  }

  int replayed = 0;
  int retargeted = 0;
  int blocked = 0;
  ID3D11InputLayout* savedIl = nullptr;
  ID3D11VertexShader* savedVs = nullptr;
  if (doReplay || doRetarget || doBlocker || doInjectV2) {
    ctx->IAGetInputLayout(&savedIl);
    ctx->VSGetShader(&savedVs, nullptr, nullptr);
  }

  // §41 V2 injection: for each queued skin mesh, draw its recorded geometry
  // through its OWN recorded IL/VS (correct vertex format -> real fragments)
  // with a cloned 26048-byte skin cbuffer whose MVP slot (bytes 160..223,
  // cb0[10..13] = the skin VS's SV_Position matrix) holds
  // S = Binv2 x TexWorld x W: world->shadow-clip through the matrix the
  // ground receiver actually samples with. OM/rasterizer/PS stay the
  // piggybacked live caster draw's (depth-only shadow pass).
  int drawnV2 = 0;
  float v2Wt[3] = {};
  float v2ClipZ = 0.0f, v2ClipW = 0.0f;
  float v2TW[16] = {};
  const bool v2TexValid = g_texWorldValid.load(std::memory_order_relaxed);
  if (v2TexValid) {
    std::lock_guard<std::mutex> l(g_texWorldMutex);
    std::memcpy(v2TW, g_texWorld, 64);
  }
  if (doInjectV2) {
    if (!g_injectCbV2) {
      ID3D11Device* dev = nullptr;
      ctx->GetDevice(&dev);
      if (dev) {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = 26048;   // full skin-material $Globals
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&desc, nullptr, &g_injectCbV2);
        dev->Release();
      }
    }
    if (g_injectCbV2) {
      // Same unbias constant as the INJECT_RECV path: UV->NDC on xy plus the
      // [0.5,1] shadow-viewport depth pre-compensation on z.
      static const float kBinv2[16] = {2, 0, 0, -1,  0, -2, 0, 1,
                                       0, 0, 2, -1,  0,  0, 0, 1};
      float Lworld[16];
      mtxMul(kBinv2, v2TW, Lworld);
      for (auto& e : g_injectReady) {
        if (!e.il || !e.vs)
          continue;               // recorded before V2 armed, or capture failed
        if (e.cb0Size != 26048)
          continue;               // skin material only: MVP@160 is its layout
        float S[16];
        mtxMul(Lworld, e.W, S);
        if (flatZ) {   // zero clip-z row -> stored depth 0.5 (low readback bin)
          S[8] = S[9] = S[10] = S[11] = 0.0f;
        }
        D3D11_MAPPED_SUBRESOURCE mp = {};
        if (FAILED(ctx->Map(g_injectCbV2, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
          break;
        std::memset(mp.pData, 0, 26048);
        std::memcpy(static_cast<uint8_t*>(mp.pData) + 160, S, 64);
        ctx->Unmap(g_injectCbV2, 0);
        ctx->IASetInputLayout(e.il);
        ctx->VSSetShader(e.vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &g_injectCbV2);
        ctx->IASetVertexBuffers(0, 4, const_cast<ID3D11Buffer* const*>(e.vb),
                                e.stride, e.offset);
        if (e.indexed)
          ctx->IASetIndexBuffer(e.ib, e.ibFormat, e.ibOffset);
        ctx->IASetPrimitiveTopology(e.topology);
        if (e.indexed)
          getContextProcs(ctx)->DrawIndexed(ctx, e.count, e.startIndex,
                                            e.baseVertex);
        else
          getContextProcs(ctx)->Draw(ctx, e.count, e.startIndex);
        if (!drawnV2) {
          v2Wt[0] = e.W[3]; v2Wt[1] = e.W[7]; v2Wt[2] = e.W[11];
          v2ClipZ = S[11]; v2ClipW = S[15];
        }
        ++drawnV2;
      }
    }
  }
  if (v2Active) {
    // Throttled ~2/s validation: is the character's world spot inside the
    // receiver's sampled UV window (in-map ~0.3..0.8), and at what depth?
    static std::atomic<uint64_t> v2LogTick{0};
    const uint64_t nowTick = GetTickCount64();
    uint64_t prevTick = v2LogTick.load(std::memory_order_relaxed);
    if (nowTick - prevTick >= 500 &&
        v2LogTick.compare_exchange_strong(prevTick, nowTick)) {
      if (!drawnV2 && !g_injectReady.empty()) {
        const PendingInject& e = g_injectReady.front();
        v2Wt[0] = e.W[3]; v2Wt[1] = e.W[7]; v2Wt[2] = e.W[11];
      }
      float ru = -1.0f, rv = -1.0f;
      if (v2TexValid) {
        const float p[4] = {v2Wt[0], v2Wt[1], v2Wt[2], 1.0f};
        float c[4];
        for (int i = 0; i < 4; ++i)
          c[i] = v2TW[i * 4 + 0] * p[0] + v2TW[i * 4 + 1] * p[1] +
                 v2TW[i * 4 + 2] * p[2] + v2TW[i * 4 + 3] * p[3];
        if (c[3] != 0.0f) { ru = c[0] / c[3]; rv = c[1] / c[3]; }
      }
      log("CUTIN_V2 texValid=", v2TexValid ? 1 : 0,
          " Wt=", v2Wt[0], ",", v2Wt[1], ",", v2Wt[2],
          " recvUV=", ru, ",", rv,
          " injClipZ=", v2ClipW != 0.0f ? v2ClipZ / v2ClipW : 0.0f,
          " drew=", drawnV2);
    }
  }
  // Verbatim replays: byte-identical re-draws of recorded caster draws. While
  // the engine still draws them this is invisible overdraw; when it hides the
  // model, the replay keeps the last good shadow alive.
  if (doReplay) {
    for (auto& [key, r] : g_replayDraws) {
      float params[20];
      std::memcpy(params, r.params, 80);
      if (r.il) ctx->IASetInputLayout(r.il);
      if (r.vs) ctx->VSSetShader(r.vs, nullptr, 0);
      if (!drawGeometry(r.geo, params))
        break;
      ++replayed;
    }
  }
  if (doRetarget) {
    for (auto& e : g_injectReady) {
      // Light-space position of this color entry (arena spot in overview,
      // mini-stage during the action).
      float Se[16];
      mtxMul(Linj, e.W, Se);
      const float TLe[3] = {Se[3], Se[7], Se[11]};

      const ReplayDraw* proxy = nullptr;
      // 1. exact vbKey (color and proxy share a buffer for some meshes).
      for (auto& [key, r] : g_replayDraws)
        if (r.geo.vbKey == e.vbKey) { proxy = &r; break; }
      // 2. pairing established earlier this battle.
      if (!proxy) {
        auto pit = g_colorToProxy.find(e.vbKey);
        if (pit != g_colorToProxy.end()) {
          auto rit = g_replayDraws.find(pit->second);
          if (rit != g_replayDraws.end())
            proxy = &rit->second;
        }
      }
      // 3. live position pairing: a proxy recording sitting at this entry's
      //    world spot IS this character's proxy (matches only in the overview
      //    where color and proxy coincide; cached for the action frames).
      if (!proxy) {
        float best = 0.15f;
        for (auto& [key, r] : g_replayDraws) {
          const float* Sp = reinterpret_cast<const float*>(r.params) + 4;
          const float d = std::max(std::max(std::fabs(Sp[3] - TLe[0]),
                                            std::fabs(Sp[7] - TLe[1])),
                                   std::fabs(Sp[11] - TLe[2]));
          if (d < best) {
            best = d;
            proxy = &r;
            g_colorToProxy[e.vbKey] = key;
          }
        }
      }
      if (!proxy)
        continue;
      float params[20];
      std::memcpy(params, proxy->params, 16);  // recorded diffuse
      mtxMul(Linj, e.W, params + 4);            // draw proxy at live color W
      // §33t: optional amplification (ARLAND_CUTIN_HEAL_AMP) grows the shadow
      // about its own world position so we can SEE where the retarget lands
      // without changing placement — a big blob under the attacker confirms
      // position; elsewhere means the pairing/transform is off.
      const float ramp = cutinHealAmp();
      if (ramp != 1.0f) {
        const float tx = params[4 + 3], ty = params[4 + 7];
        for (int r2 = 0; r2 < 2; ++r2)   // rows 0-1 only: grow the XY footprint
          for (int c = 0; c < 3; ++c)
            params[4 + r2 * 4 + c] *= ramp;
        params[4 + 3] = tx; params[4 + 7] = ty;
      }
      params[4 + 11] -= 0.003f;                // §33i depth bias
      // §33v: log where each cut-in retarget actually lands.
      if (arlandInCinematicBattle()) {
        static std::atomic<uint32_t> rtLogs{0};
        if (rtLogs.fetch_add(1, std::memory_order_relaxed) % 30 == 0)
          log("CUTIN_RETARGET_POS state=",
              arlandBattleStateName() ? arlandBattleStateName() : "-",
              " Wt=", e.W[3], ",", e.W[7], ",", e.W[11],
              " St=", params[4 + 3], ",", params[4 + 7], ",", params[4 + 11],
              " cb0=", e.cb0Size);
      }
      if (proxy->il) ctx->IASetInputLayout(proxy->il);
      if (proxy->vs) ctx->VSSetShader(proxy->vs, nullptr, 0);
      if (drawGeometry(proxy->geo, params))
        ++retargeted;
    }
  }
  if (doBlocker) {
    const ReplayDraw* best = nullptr;
    for (auto& [key, r] : g_replayDraws)
      if (!best || r.geo.count > best->geo.count)
        best = &r;
    if (best) {
      // Amplify the recording's own healthy S: rows 0/1 scaled, centered in
      // the map (translation zeroed); row 2 forced to constant near depth so
      // the blocker occludes every arena texel behind it. Three sizes so at
      // least one covers the map regardless of the mesh's model-space extent.
      for (float f : {20.0f, 60.0f, 180.0f}) {
        float params[20];
        std::memcpy(params, best->params, 16);
        float S2[16];
        std::memcpy(S2, reinterpret_cast<const float*>(best->params) + 4, 64);
        for (int row = 0; row < 2; ++row) {
          for (int c = 0; c < 3; ++c)
            S2[row * 4 + c] *= f;
          S2[row * 4 + 3] = 0.0f;
        }
        S2[8] = 0; S2[9] = 0; S2[10] = 0; S2[11] = 0.3f;
        S2[12] = 0; S2[13] = 0; S2[14] = 0; S2[15] = 1;
        std::memcpy(params + 4, S2, 64);
        if (best->il)
          ctx->IASetInputLayout(best->il);
        if (best->vs)
          ctx->VSSetShader(best->vs, nullptr, 0);
        if (!drawGeometry(best->geo, params))
          break;
        ++blocked;
      }
    }
    static std::atomic<uint32_t> blockLogs{0};
    if (blocked && blockLogs.fetch_add(1, std::memory_order_relaxed) % 120 == 0)
      log("CUTIN_BLOCKER drawn=", blocked,
          " count=", best ? best->geo.count : 0);
  }
  if (savedIl) {
    ctx->IASetInputLayout(savedIl);
    savedIl->Release();
  }
  if (savedVs) {
    ctx->VSSetShader(savedVs, nullptr, 0);
    savedVs->Release();
  }
  g_reissueTotal.fetch_add(drawn + replayed + retargeted + blocked + drawnV2,
                           std::memory_order_relaxed);

  // Restore.
  ctx->IASetVertexBuffers(0, 4, savedVb, savedStride, savedOffset);
  ctx->IASetIndexBuffer(savedIb, savedIbFmt, savedIbOff);
  ctx->IASetPrimitiveTopology(savedTopo);
  ctx->VSSetConstantBuffers(0, 1, &savedCb0);
  for (auto* b : savedVb) if (b) b->Release();
  if (savedIb) savedIb->Release();
  if (savedCb0) savedCb0->Release();
  if (g_injectLogs.fetch_add(1, std::memory_order_relaxed) % 120 == 0) {
    // Injected caster origin -> shadow-map UV (perspective divide + [0,1] remap),
    // to compare directly against CUTIN_MAP_SPAN centroidUV: where we aimed vs
    // where map content actually is.
    const float w = firstS[3] != 0.0f ? firstS[3] : 1.0f;
    const float uvx = 0.5f + 0.5f * firstS[0] / w;
    const float uvy = 0.5f - 0.5f * firstS[1] / w;
    // Where the RECEIVER samples for the same world point: recvUV = project(
    // Tex x Wtrans). If recvUV != injUV, caster L and receiver Tex diverge — the
    // caster is in the map but the receiver looks elsewhere (root cause).
    float rx = -1.0f, ry = -1.0f, rvalid = 0.0f;
    if (g_recvTexValid.load(std::memory_order_relaxed)) {
      float T[16];
      { std::lock_guard<std::mutex> lock(g_recvTexMutex); std::memcpy(T, g_recvTex, 64); }
      const float p[4] = {firstWt[0], firstWt[1], firstWt[2], 1.0f};
      float c[4];
      for (int i = 0; i < 4; ++i)
        c[i] = T[i*4+0]*p[0] + T[i*4+1]*p[1] + T[i*4+2]*p[2] + T[i*4+3]*p[3];
      // PSSGLightModelViewProjTex already maps to [0,1] UV (the "Tex" bias), so
      // recvUV = c.xy / c.w directly (no 0.5 remap).
      if (c[3] != 0.0f) { rx = c[0]/c[3]; ry = c[1]/c[3]; rvalid = 1.0f; }
    }
    log("CUTIN_INJECT n=", drawn, " replay=", replayed,
        " retarget=", retargeted, " total=",
        g_reissueTotal.load(std::memory_order_relaxed),
        " light_age=", frame - light.frame, " cb0=", firstCb0,
        " St=", firstS[0], ",", firstS[1], ",", firstS[2], ",", firstS[3],
        " injUV=", uvx, ",", uvy,
        " Wt=", firstWt[0], ",", firstWt[1], ",", firstWt[2],
        " recvUV=", rx, ",", ry, " recvValid=", rvalid);
  }
}

// A shadow-map clear after injection wipes the injected depth. Re-arm so the
// batch is re-drawn into the newly cleared generation — the receivers sample
// whichever generation is last.
void cutinShadowMapCleared(ID3D11DeviceContext* ctx,
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
    shadowMap = d.Width == 1024 && d.Height == 1024;
    tex->Release();
  }
  if (shadowMap)
    recordMapEvent('C', res, "shadow_map", ctx);
  res->Release();
  if (!shadowMap)
    return;
  // §30m: restore engine-cleared caster flags before this frame's caster
  // draws (game-side walk lives in menu_fix; env-gated there).
  arlandCutinShadowMapCleared();
  if (!cutinShadowFixEnabled())
    return;
  std::lock_guard lock(g_pipeMutex);
  // §43: re-arm only the CLEARED map (identity key), so the re-injection goes
  // into its new generation while other maps' injections stand.
  if (g_injectedDsvTextures.erase(static_cast<void*>(res)) &&
      g_rearmLogs.fetch_add(1, std::memory_order_relaxed) % 240 == 0)
    log("CUTIN_REARM shadow map cleared after injection; re-arming dsvTex=",
        static_cast<void*>(res));
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

// At a draw: shadow-pass draws capture the pipe + per-caster shadow matrix;
// color draws of a caster that also cast THIS frame derive/verify the frame's
// light-VP; color draws of a decoupled mesh (visible, ever-cast, not cast this
// frame) get re-issued into the shadow map with an injected matrix.
void cutinShadowDraw(ID3D11DeviceContext* ctx, bool indexed, UINT count,
                     UINT startIndex, INT baseVertex) {
  if (!cutinShadowFixEnabled())
    return;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ctx->OMGetRenderTargets(1, &rtv, &dsv);
  bool shadowPass = false;
  if (dsv && rtv && cutinMapTraceEnabled()) {
    // A pass writing a 1024x1024 shadow map WITH a color target bound would
    // be invisible to the depth-only classifier — log it as 'E'.
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (res) {
      if (isTracedShadowMapTex(res))
        recordMapEvent('E', res, "shadow_map", ctx);
      res->Release();
    }
  }
  if (dsv && !rtv) {
    // Depth-only is not enough: require the 1024x1024 shadow map itself, so
    // injection can never land in some other depth-only pass.
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (res) {
      ID3D11Texture2D* tex = nullptr;
      if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d = {};
        tex->GetDesc(&d);
        shadowPass = d.Width == 1024 && d.Height == 1024;
        if (shadowPass)
          recordMapEvent('D', res, "shadow_map", ctx);
        tex->Release();
      }
      res->Release();
    }
  }
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

  const uint64_t frame = g_reconFrame.load(std::memory_order_relaxed);

  if (shadowPass) {
    if (count < 300)
      return;
    uint32_t snapSize = 0;
    uint8_t snap[kCbSnapBytes];
    const bool haveParams =
      readCb0Snap(ctx, &snapSize, snap) && snapSize == 80;
    if (haveParams && cutinShadowFixEnabled()) {
      // Was this draw's cb0 healed this frame? (§32e heal-flow accounting.)
      ID3D11Buffer* cb0 = nullptr;
      ctx->VSGetConstantBuffers(0, 1, &cb0);
      if (cb0) {
        std::lock_guard lock(g_pipeMutex);
        auto it = g_healedCbs.find(cb0);
        if (it != g_healedCbs.end() && frame - it->second <= 1)
          g_healedDrawsThisFrame.fetch_add(1, std::memory_order_relaxed);
        cb0->Release();
      }
    }
    if (haveParams) {
      captureShadowPipe(ctx);
      // Piggyback this REAL character-caster shadow draw: its full pipeline
      // state (PS, raster, depth-stencil, viewport, VS, IL) is live and
      // correct, so the injected meshes render exactly like a real caster.
      injectPendingShadows(ctx);
      // Caster recording: remember this draw verbatim while its matrix is
      // non-degenerate (a hidden model casts with ~0 scale — don't let that
      // overwrite the last good recording). Used by the vbKey-matched
      // retarget draws and the opt-in verbatim replay.
      // §32f: at a HEALED caster draw (this draw's cb0 slice matches one we
      // healed), emit an extra draw with the frozen stream-0 clone — the
      // engine's own draw rasterizes collapsed vertices and shows nothing.
      {
        bool healedDraw = false;
        {
          std::lock_guard lock(g_pipeMutex);
          for (auto it = g_healedSlices.begin();
               it != g_healedSlices.end(); ++it)
            if (!std::memcmp(it->bytes.data(), snap, 80)) {
              // §33k pairing check: did the write's bound VB match the VB
              // this draw actually uses?
              (it->vbKey == vbKey ? g_pairOk : g_pairMismatch)
                .fetch_add(1, std::memory_order_relaxed);
              static std::atomic<uint32_t> pairLogs{0};
              if (pairLogs.fetch_add(1, std::memory_order_relaxed) % 120 == 0)
                log("CUTIN_PAIR ok=",
                    g_pairOk.load(std::memory_order_relaxed),
                    " mismatch=",
                    g_pairMismatch.load(std::memory_order_relaxed));
              g_healedSlices.erase(it);
              healedDraw = true;
              break;
            }
        }
        if (healedDraw) {
          ID3D11Buffer* clones[4] = {};
          bool haveClone = false;
          {
            std::lock_guard lock(g_pipeMutex);
            auto it = g_vbClones.find(vbKey);
            if (it != g_vbClones.end() && frame - it->second.refreshed < 300) {
              for (int i = 0; i < 4; ++i)
                if (it->second.clone[i]) {
                  clones[i] = it->second.clone[i];
                  clones[i]->AddRef();
                  haveClone = true;
                }
            }
          }
          if (haveClone) {
            ID3D11Buffer* orig[4] = {};
            UINT os[4] = {}, oo[4] = {};
            ctx->IAGetVertexBuffers(0, 4, orig, os, oo);
            ID3D11Buffer* bind[4];
            for (int i = 0; i < 4; ++i)
              bind[i] = clones[i] ? clones[i] : orig[i];
            ctx->IASetVertexBuffers(0, 4, bind, os, oo);
            auto procs = getContextProcs(ctx);
            if (indexed)
              procs->DrawIndexed(ctx, count, startIndex, baseVertex);
            else
              procs->Draw(ctx, count, startIndex);
            ctx->IASetVertexBuffers(0, 4, orig, os, oo);
            for (int i = 0; i < 4; ++i) {
              if (orig[i])
                orig[i]->Release();
              if (clones[i])
                clones[i]->Release();
            }
            g_cloneDraws.fetch_add(1, std::memory_order_relaxed);
            if (g_cloneDrawLogs.fetch_add(1, std::memory_order_relaxed)
                % 60 == 0) {
              const float* hm = reinterpret_cast<const float*>(snap + 16);
              log("CUTIN_CLONE_DRAW total=",
                  g_cloneDraws.load(std::memory_order_relaxed),
                  " vb=", reinterpret_cast<void*>(vbKey),
                  " count=", count,
                  " t=", hm[3], ",", hm[7], ",", hm[11]);
            }
          } else if (g_cloneDrawLogs.fetch_add(1, std::memory_order_relaxed)
                     % 240 == 0) {
            log("CUTIN_CLONE_DRAW no_clone vb=",
                reinterpret_cast<void*>(vbKey));
          }
        }
      }
      {
        const float* m = reinterpret_cast<const float*>(snap + 16);
        float scale = 0.0f;
        for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10})
          scale = std::max(scale, std::fabs(m[i]));
        if (scale > 1e-5f) {
          {
            // §33h: draw-side character-scale rot cache for the healer.
            std::lock_guard lock(g_pipeMutex);
            for (int k = 0; k < 9; ++k)
              g_charRot.rot[k] = m[kRot9(k)];
            g_charRot.frame = frame;
            g_charRot.valid = true;
          }
          // §32f: refresh (or create) this caster's frozen clones for ALL
          // bound streams (positions may live in any of streams 0-3) while it
          // is healthy — copied through the UNHOOKED CopyResource.
          ID3D11Buffer* srcs[4] = {};
          UINT ss[4] = {}, so[4] = {};
          ctx->IAGetVertexBuffers(0, 4, srcs, ss, so);
          {
            ID3D11Buffer* copies[4] = {};
            bool refresh = false;
            {
              std::lock_guard lock(g_pipeMutex);
              auto it = g_vbClones.find(vbKey);
              if (it != g_vbClones.end() && it->second.refreshed >= frame) {
                // already refreshed this frame
              } else {
                refresh = true;
                if (it != g_vbClones.end()) {
                  it->second.refreshed = frame;
                  for (int i = 0; i < 4; ++i)
                    if (it->second.clone[i]) {
                      copies[i] = it->second.clone[i];
                      copies[i]->AddRef();
                    }
                }
              }
            }
            if (refresh) {
              // create any missing clones outside the lock
              ID3D11Buffer* created[4] = {};
              ID3D11Device* dev = nullptr;
              for (int i = 0; i < 4; ++i) {
                if (!srcs[i] || copies[i])
                  continue;
                D3D11_BUFFER_DESC bd = {};
                srcs[i]->GetDesc(&bd);
                if (bd.ByteWidth > (8u << 20))
                  continue;
                bd.Usage = D3D11_USAGE_DEFAULT;
                bd.CPUAccessFlags = 0;
                bd.MiscFlags = 0;
                if (!dev)
                  ctx->GetDevice(&dev);
                if (dev)
                  dev->CreateBuffer(&bd, nullptr, &created[i]);
              }
              if (dev)
                dev->Release();
              {
                std::lock_guard lock(g_pipeMutex);
                auto& e = g_vbClones[vbKey];
                e.refreshed = frame;
                for (int i = 0; i < 4; ++i)
                  if (created[i]) {
                    if (!e.clone[i]) {
                      e.clone[i] = created[i];
                      copies[i] = created[i];
                      copies[i]->AddRef();
                    } else {
                      created[i]->Release();
                    }
                  }
              }
              for (int i = 0; i < 4; ++i)
                if (copies[i] && srcs[i])
                  getContextProcs(ctx)->CopyResource(ctx, copies[i], srcs[i]);
            }
            for (int i = 0; i < 4; ++i)
              if (copies[i])
                copies[i]->Release();
          }
          for (int i = 0; i < 4; ++i)
            if (srcs[i])
              srcs[i]->Release();
          ReplayDraw r;
          ctx->VSGetShader(&r.vs, nullptr, nullptr);
          ctx->IAGetVertexBuffers(0, 4, r.geo.vb, r.geo.stride, r.geo.offset);
          ctx->IAGetIndexBuffer(&r.geo.ib, &r.geo.ibFormat, &r.geo.ibOffset);
          ctx->IAGetPrimitiveTopology(&r.geo.topology);
          ctx->IAGetInputLayout(&r.il);
          r.geo.indexed = indexed;
          r.geo.count = count;
          r.geo.startIndex = startIndex;
          r.geo.baseVertex = baseVertex;
          r.geo.vbKey = vbKey;
          std::memcpy(r.params, snap, 80);
          r.frame = frame;
          std::lock_guard lock(g_pipeMutex);
          auto key = std::make_tuple(vbKey, startIndex, baseVertex);
          auto it = g_replayDraws.find(key);
          if (it != g_replayDraws.end())
            releaseReplayDraw(it->second);
          if (g_replayDraws.size() < 32 || it != g_replayDraws.end())
            g_replayDraws[key] = r;
          else
            releaseReplayDraw(r);
        }
      }
    }
    if (cutinInjTestMode() && haveParams &&
        g_injTestThisFrame.fetch_add(1, std::memory_order_relaxed) < 4) {
      auto procs = getContextProcs(ctx);
      const int mode = cutinInjTestMode();
      bool did = false;
      if (mode == 1) {
        if (indexed)
          procs->DrawIndexed(ctx, count, startIndex, baseVertex);
        else
          procs->Draw(ctx, count, startIndex);
        did = true;
      }
      if (mode >= 2 && !g_injectCb) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
          D3D11_BUFFER_DESC desc = {};
          desc.ByteWidth = 96;
          desc.Usage = D3D11_USAGE_DYNAMIC;
          desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
          desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
          std::lock_guard lock(g_pipeMutex);
          if (!g_injectCb)
            dev->CreateBuffer(&desc, nullptr, &g_injectCb);
          dev->Release();
        }
      }
      if (mode == 2 && g_injectCb) {
        float params[24] = {};
        std::memcpy(params, snap, 16);
        float S2[16];
        std::memcpy(S2, reinterpret_cast<const float*>(snap) + 4, 64);
        for (int r = 0; r < 2; ++r) {
          for (int c = 0; c < 3; ++c)
            S2[r * 4 + c] *= 60.0f;
          S2[r * 4 + 3] = 0.0f;
        }
        S2[8] = 0; S2[9] = 0; S2[10] = 0; S2[11] = 0.3f;
        S2[12] = 0; S2[13] = 0; S2[14] = 0; S2[15] = 1;
        std::memcpy(params + 4, S2, 64);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(ctx->Map(g_injectCb, 0, D3D11_MAP_WRITE_DISCARD, 0,
                               &mapped))) {
          std::memcpy(mapped.pData, params, 96);
          ctx->Unmap(g_injectCb, 0);
          ID3D11Buffer* savedCb = nullptr;
          ctx->VSGetConstantBuffers(0, 1, &savedCb);
          ctx->VSSetConstantBuffers(0, 1, &g_injectCb);
          if (indexed)
            procs->DrawIndexed(ctx, count, startIndex, baseVertex);
          else
            procs->Draw(ctx, count, startIndex);
          ctx->VSSetConstantBuffers(0, 1, &savedCb);
          if (savedCb)
            savedCb->Release();
          did = true;
        }
      } else if (mode == 3 && g_injectCb) {
        std::lock_guard lock(g_pipeMutex);
        if (!g_replayDraws.empty()) {
          const ReplayDraw* best = nullptr;
          for (auto& [key, r] : g_replayDraws)
            if (!best || r.geo.count > best->geo.count)
              best = &r;
          // §33o: blob matrix from the RECORDING's own healthy S — success
          // renders the giant shadow through recorded geometry, completing
          // the bisect (mode 2 proved the cb path with live geometry).
          float params[24] = {};
          std::memcpy(params, best->params, 16);
          float S2[16];
          std::memcpy(S2, reinterpret_cast<const float*>(best->params) + 4,
                      64);
          for (int r2 = 0; r2 < 2; ++r2) {
            for (int c = 0; c < 3; ++c)
              S2[r2 * 4 + c] *= 60.0f;
            S2[r2 * 4 + 3] = 0.0f;
          }
          S2[8] = 0; S2[9] = 0; S2[10] = 0; S2[11] = 0.3f;
          S2[12] = 0; S2[13] = 0; S2[14] = 0; S2[15] = 1;
          std::memcpy(params + 4, S2, 64);
          bool cbOk = false;
          D3D11_MAPPED_SUBRESOURCE mapped = {};
          if (SUCCEEDED(ctx->Map(g_injectCb, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                 &mapped))) {
            std::memcpy(mapped.pData, params, 96);
            ctx->Unmap(g_injectCb, 0);
            cbOk = true;
          }
          ID3D11Buffer* savedCb = nullptr;
          if (cbOk) {
            ctx->VSGetConstantBuffers(0, 1, &savedCb);
            ctx->VSSetConstantBuffers(0, 1, &g_injectCb);
          }
          ID3D11Buffer* savedVb[4] = {};
          UINT ss[4] = {}, so[4] = {};
          ctx->IAGetVertexBuffers(0, 4, savedVb, ss, so);
          ID3D11Buffer* savedIb = nullptr;
          DXGI_FORMAT sf = DXGI_FORMAT_UNKNOWN;
          UINT sio = 0;
          ctx->IAGetIndexBuffer(&savedIb, &sf, &sio);
          ID3D11InputLayout* sil = nullptr;
          ctx->IAGetInputLayout(&sil);
          ID3D11VertexShader* svs = nullptr;
          ctx->VSGetShader(&svs, nullptr, nullptr);
          ctx->IASetVertexBuffers(0, 4,
            const_cast<ID3D11Buffer* const*>(best->geo.vb),
            best->geo.stride, best->geo.offset);
          if (best->geo.indexed)
            ctx->IASetIndexBuffer(best->geo.ib, best->geo.ibFormat,
                                  best->geo.ibOffset);
          if (best->il) ctx->IASetInputLayout(best->il);
          if (best->vs) ctx->VSSetShader(best->vs, nullptr, 0);
          if (best->geo.indexed)
            procs->DrawIndexed(ctx, best->geo.count, best->geo.startIndex,
                               best->geo.baseVertex);
          else
            procs->Draw(ctx, best->geo.count, best->geo.startIndex);
          ctx->IASetVertexBuffers(0, 4, savedVb, ss, so);
          ctx->IASetIndexBuffer(savedIb, sf, sio);
          if (sil) { ctx->IASetInputLayout(sil); sil->Release(); }
          if (svs) { ctx->VSSetShader(svs, nullptr, 0); svs->Release(); }
          for (auto* b : savedVb) if (b) b->Release();
          if (savedIb) savedIb->Release();
          if (savedCb) {
            ctx->VSSetConstantBuffers(0, 1, &savedCb);
            savedCb->Release();
          }
          did = true;
        }
      }
      if (did &&
          g_injTestLogs.fetch_add(1, std::memory_order_relaxed) % 120 == 0)
        log("CUTIN_INJTEST mode=", mode, " count=", count);
    }
    std::lock_guard lock(g_shadowTraceMutex);
    g_frameShadowVBs.insert(vbKey);
    auto& mx = g_vbEverCast[vbKey];
    if (count > mx) mx = count;
    if (haveParams) {
      auto& cs = g_casterShadow[vbKey];
      std::memcpy(cs.diffuse, snap, 16);
      std::memcpy(cs.S, snap + 16, 64);
      cs.frame = frame;
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
  CasterShadow cs;
  bool haveShadowMtx = false;
  {
    std::lock_guard lock(g_shadowTraceMutex);
    everCast = g_vbEverCast.count(vbKey) != 0;
    castThisFrame = g_frameShadowVBs.count(vbKey) != 0;
    auto it = g_casterShadow.find(vbKey);
    if (it != g_casterShadow.end() && it->second.frame == frame) {
      cs = it->second;
      haveShadowMtx = true;
    }
  }
  // §33q: do NOT require everCast up front — the cut-in attacker's cinematic
  // mesh is a separate, never-cast VB, which this filter structurally
  // excluded through the whole investigation. Character skin (26048) draws
  // are eligible regardless; everCast still gates the outline fallback.
  if (castThisFrame) {
    if (!everCast)
      return;
    // Reference caster drawn in both passes: derive (or verify) this frame's
    // light-VP from S = L x W. Skip hidden casters (near-zero scale).
    if (!haveShadowMtx)
      return;
    uint32_t snapSize = 0;
    uint8_t snap[kCbSnapBytes];
    float W[16];
    if (!readCb0Snap(ctx, &snapSize, snap) || !extractModel(snapSize, snap, W))
      return;
    if (std::fabs(mtxDet3(W)) < 1e-12f)
      return;
    float Wi[16], L[16];
    if (!mtxInvert(W, Wi))
      return;
    mtxMul(cs.S, Wi, L);
    std::lock_guard lock(g_pipeMutex);
    {
      ColorW& cw = g_colorW[vbKey];
      std::memcpy(cw.W, W, sizeof(W));
      cw.frame = frame;
    }
    if (g_light.valid && g_light.frame == frame) {
      // Second valid caster this frame: cross-check that the light-VP is
      // global, not per-caster.
      float pred[16];
      mtxMul(g_light.L, W, pred);
      const float err = mtxMaxDiff(pred, cs.S);
      if (g_lvpChecks.fetch_add(1, std::memory_order_relaxed) % 240 == 0)
        log("CUTIN_LVP check frame=", frame, " err=", err);
    } else {
      std::memcpy(g_light.L, L, sizeof(L));
      std::memcpy(g_light.diffuse, cs.diffuse, sizeof(cs.diffuse));
      g_light.frame = frame;
      g_light.valid = true;
      if (g_lvpDerives.fetch_add(1, std::memory_order_relaxed) % 600 == 0)
        log("CUTIN_LVP derive frame=", frame,
            " vb=", reinterpret_cast<void*>(vbKey),
            " r3=", L[12], ",", L[13], ",", L[14], ",", L[15]);
    }
    return;
  }

  // Decoupled mesh: visible this frame but absent from the shadow map. Record
  // it (geometry bindings + current Model) for injection into the NEXT frame's
  // shadow pass.
  uint32_t snapSize = 0;
  uint8_t snap[kCbSnapBytes];
  float W[16];
  if (!readCb0Snap(ctx, &snapSize, snap) || !extractModel(snapSize, snap, W))
    return;
  // Character materials only: field meshes (768/880-byte $Globals) also
  // decouple when the lighting bounds cull them, and injecting scenery would
  // paint large wrong shadows.
  if (snapSize != 26048 && !(snapSize == 256 && everCast))
    return;
  if (std::fabs(mtxDet3(W)) < 1e-12f)   // hidden mesh, nothing to cast
    return;
  // §33r: battle-HUD character portraits use the skin material too and sit at
  // exactly y=0 (world translations like (0,0,5.5)) — they flooded the queue
  // and crowded out the attacker. Arena characters always have |y| > 0.
  if (std::fabs(W[7]) < 1e-6f)
    return;
  // §35: capture this cut-in character's ModelViewProj (26048 $Globals +160)
  // for the contact-blob overlay — projects the character to screen space.
  if (snapSize == 26048 && arlandInCinematicBattle())
    captureCutinMVP(reinterpret_cast<const float*>(snap + 160), W);
  PendingInject e;
  ctx->IAGetVertexBuffers(0, 4, e.vb, e.stride, e.offset);
  ctx->IAGetIndexBuffer(&e.ib, &e.ibFormat, &e.ibOffset);
  ctx->IAGetPrimitiveTopology(&e.topology);
  e.indexed = indexed;
  e.count = count;
  e.startIndex = startIndex;
  e.baseVertex = baseVertex;
  e.vbKey = vbKey;
  e.cb0Size = snapSize;
  std::memcpy(e.W, W, sizeof(W));
  // §41 V2: also record the color draw's OWN input layout + vertex shader.
  // The piggybacked shadow-proxy IL/VS expects a different vertex format and
  // rasterizes zero fragments from this VB (root cause A); the re-draw must
  // use the pipeline that actually made these vertices visible.
  if (cutinInjectV2Enabled()) {
    ctx->IAGetInputLayout(&e.il);
    ctx->VSGetShader(&e.vs, nullptr, nullptr);
  }
  // The outline (edge) pass draws the mesh FIRST each frame; every §30d/§30e
  // injection only ever carried outline geometry (cb0_size=256 in all logs) —
  // likely an inverted hull the caster's backface culling rejects entirely.
  // Prefer the skin material's geometry: replace a queued outline entry with
  // the 26048-material draw of the same VB; drop other duplicates.
  bool queued = false;
  {
    std::lock_guard lock(g_pipeMutex);
    auto existing = std::find_if(g_injectNext.begin(), g_injectNext.end(),
      [&](const PendingInject& p) { return p.vbKey == vbKey; });
    if (existing != g_injectNext.end()) {
      if (existing->cb0Size == 256 && snapSize == 26048) {
        releasePendingRefs(*existing);
        *existing = e;
        queued = true;
      }
    } else if (g_injectNext.size() < 16) {
      g_injectNext.push_back(e);
      queued = true;
    }
  }
  if (!queued) {
    releasePendingRefs(e);
    return;
  }
  if (g_queueLogs.fetch_add(1, std::memory_order_relaxed) % 240 == 0)
    log("CUTIN_QUEUE vb=", reinterpret_cast<void*>(vbKey), " count=", count,
        " cb0_size=", snapSize, " frame=", frame,
        " Wt=", W[3], ",", W[7], ",", W[11]);
}

// Per-draw recon logging (ARLAND_CUTIN_RECON=1): classify the draw — shadow
// pass, color draw of a mesh that also cast this frame (the reference caster
// for any later matrix derivation), color draw of a decoupled mesh (the broken
// close-up geometry), or a big color mesh that never cast — then log every VS
// cbuffer slot's buffer pointer, size, and last-written contents, and dump the
// bound VS bytecode for offline constant-layout analysis.
bool reconAllow(int category) {
  static mutex bucketMutex;
  static uint64_t bucket[4] = {};
  static int count[4] = {};
  static const int quota[4] = {2, 2, 4, 1};   // per 500 ms window
  const uint64_t now = GetTickCount64() / 500;
  std::lock_guard lock(bucketMutex);
  if (bucket[category] != now) {
    bucket[category] = now;
    count[category] = 0;
  }
  return count[category]++ < quota[category];
}

void cutinRecon(ID3D11DeviceContext* ctx, bool indexed, UINT count) {
  if (!cutinReconEnabled() || count < 300)
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
  if (!shadowPass && !colorMain)
    return;

  ID3D11Buffer* vb = nullptr;
  UINT stride = 0, offset = 0;
  ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
  const uintptr_t vbKey = reinterpret_cast<uintptr_t>(vb);
  if (vb) vb->Release();
  if (!vbKey)
    return;

  int category;
  const char* role;
  if (shadowPass) {
    std::lock_guard lock(g_shadowTraceMutex);
    g_frameShadowVBs.insert(vbKey);
    auto& mx = g_vbEverCast[vbKey];
    if (count > mx) mx = count;
    category = 0;
    role = "shadow";
  } else {
    bool everCast, castThisFrame;
    {
      std::lock_guard lock(g_shadowTraceMutex);
      everCast = g_vbEverCast.count(vbKey) != 0;
      castThisFrame = g_frameShadowVBs.count(vbKey) != 0;
    }
    if (everCast && castThisFrame) { category = 1; role = "colorcast"; }
    else if (everCast)             { category = 2; role = "decoup"; }
    else                           { category = 3; role = "colornew"; }
  }
  if (!reconAllow(category))
    return;

  ID3D11VertexShader* vs = nullptr;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  ID3D11InputLayout* il = nullptr;
  ctx->IAGetInputLayout(&il);
  ID3D11Buffer* cb[14] = {};
  ctx->VSGetConstantBuffers(0, 14, cb);

  log("CUTIN_RECON pass=", role, " frame=",
      g_reconFrame.load(std::memory_order_relaxed),
      " ms=", GetTickCount64(), " cin=", arlandInCinematicBattle(),
      " vb=", reinterpret_cast<void*>(vbKey), " n=", count,
      " indexed=", indexed, " vs=", static_cast<void*>(vs),
      " il=", static_cast<void*>(il));
  for (int i = 0; i < 14; ++i) {
    if (!cb[i])
      continue;
    D3D11_BUFFER_DESC desc = {};
    cb[i]->GetDesc(&desc);
    CbSnap snap;
    bool haveSnap = false;
    {
      std::lock_guard lock(g_cbSnapMutex);
      auto it = g_cbSnaps.find(cb[i]);
      if (it != g_cbSnaps.end()) {
        snap = it->second;
        haveSnap = true;
      }
    }
    if (haveSnap) {
      char floats[512];
      int off = 0;
      const float* f = reinterpret_cast<const float*>(snap.data);
      const uint32_t n = std::min<uint32_t>(
        std::min<uint32_t>(snap.size, sizeof(snap.data)) / 4, 20);
      for (uint32_t k = 0; k < n && off < int(sizeof(floats)) - 16; ++k)
        off += std::snprintf(floats + off, sizeof(floats) - off,
                             "%s%.6g", k ? "," : "", f[k]);
      log("CUTIN_RECON_SLOT pass=", role,
          " vb=", reinterpret_cast<void*>(vbKey), " s=", i,
          " cb=", static_cast<void*>(cb[i]), " size=", desc.ByteWidth,
          " seq=", snap.seq, " f=", floats);
    } else {
      log("CUTIN_RECON_SLOT pass=", role,
          " vb=", reinterpret_cast<void*>(vbKey), " s=", i,
          " cb=", static_cast<void*>(cb[i]), " size=", desc.ByteWidth,
          " snap=none");
    }
  }
  if (vs)
    dumpVsOnce(vs, role);

  for (auto* b : cb) if (b) b->Release();
  if (vs) vs->Release();
  if (il) il->Release();
}

// Per-frame reset (called from Present): the "cast this frame" set starts
// fresh, and the meshes recorded during this frame's color pass become the
// injection batch for the next frame's shadow pass.
void cutinShadowPresent() {
  static std::atomic<bool> envLogged{false};
  if (!envLogged.exchange(true)) {
    const char* raw = std::getenv("ARLAND_CUTIN_INJTEST");
    log("CUTIN_ENV fix=", cutinShadowFixEnabled(),
        " injtest_raw=", raw ? raw : "(null)",
        " injtest=", cutinInjTestMode(),
        " blocker=", cutinBlockerMode(),
        " recon=", cutinReconEnabled(),
        " maptrace=", cutinMapTraceEnabled(),
        " readback=", cutinMapReadbackEnabled(),
        " lighttrace=", cutinLightTraceEnabled(),
        " recvtrace=", cutinRecvTraceEnabled());
  }
  flushMapEvents();
  // Advance the frame counter for the map-trace/readback path too — it drives
  // the readback's "wait N frames" logic. (Previously only incremented in the
  // fix-only body below, so map-trace-only froze it at 0 and the readback
  // could never complete.)
  if (!cutinShadowFixEnabled() && !cutinReconEnabled())
    g_reconFrame.fetch_add(1, std::memory_order_relaxed);
  consumeMapReadback();
  if (!cutinShadowFixEnabled() && !cutinReconEnabled())
    return;
  // §33u: on a scene (re)build, purge cross-scene caches — proxy pairings,
  // recordings, clones, per-cb rot caches and the derived light-VP all
  // reference geometry the previous scene owned. Stale pairings sent the
  // cut-in shadow to wrong/off-screen positions after the first scene.
  {
    static uint32_t seenGen = 0;
    const uint32_t gen = arlandSceneGeneration();
    if (gen != seenGen) {
      seenGen = gen;
      std::lock_guard lock(g_pipeMutex);
      g_colorToProxy.clear();
      for (auto& [k, r] : g_replayDraws)
        releaseReplayDraw(r);
      g_replayDraws.clear();
      for (auto& [k, e] : g_vbClones)
        for (int i = 0; i < 4; ++i)
          if (e.clone[i]) e.clone[i]->Release();
      g_vbClones.clear();
      g_colorW.clear();
      g_healthyByCb.clear();
      g_charRot.valid = false;
      g_light.valid = false;
      log("CUTIN_SCENE_RESET gen=", gen);
    }
  }
  {
    const uint32_t heals =
      g_healsThisFrame.exchange(0, std::memory_order_relaxed);
    const uint32_t healedDraws =
      g_healedDrawsThisFrame.exchange(0, std::memory_order_relaxed);
    static std::atomic<uint32_t> flowLogs{0};
    if (heals &&
        flowLogs.fetch_add(1, std::memory_order_relaxed) % 60 == 0) {
      size_t distinct = 0;
      const uint64_t f = g_reconFrame.load(std::memory_order_relaxed);
      {
        std::lock_guard lock(g_pipeMutex);
        for (auto& e : g_healedCbs)
          if (f - e.second <= 1)
            ++distinct;
      }
      log("CUTIN_HEALFLOW heals=", heals, " distinct_cbs=", distinct,
          " healed_draws=", healedDraws);
    }
  }
  g_reconFrame.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(g_shadowTraceMutex);
    g_frameShadowVBs.clear();
  }
  std::lock_guard lock(g_pipeMutex);
  releasePendingList(g_injectReady);   // leftovers: no shadow pass consumed them
  g_injectReady.swap(g_injectNext);
  g_injectedDsvTextures.clear();       // §43: fresh per-frame per-map arming
  g_healedSlices.clear();
  g_blockerHijacks.store(0, std::memory_order_relaxed);
  g_injTestThisFrame.store(0, std::memory_order_relaxed);
  // Drop replay recordings that stopped refreshing (battle over, scene change).
  const uint64_t now = g_reconFrame.load(std::memory_order_relaxed);
  for (auto it = g_vbClones.begin(); it != g_vbClones.end();) {
    if (now - it->second.refreshed > 900) {
      for (int i = 0; i < 4; ++i)
        if (it->second.clone[i])
          it->second.clone[i]->Release();
      it = g_vbClones.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_replayDraws.begin(); it != g_replayDraws.end();) {
    if (now - it->second.frame > 900) {
      releaseReplayDraw(it->second);
      it = g_replayDraws.erase(it);
    } else {
      ++it;
    }
  }
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

// §41 V2: capture the object-independent world->shadow-UV matrix from the
// ACTUAL ground receive draw. PSSGLightModelViewProjTex (@752 of the 880-byte
// receiver material) maps that mesh's LOCAL vertex to shadow-map UV (it bakes
// the mesh's Model — the receiver VS does dp4 o4, v0_local, cb0[47..50]), so
// TexWorld = Tex x Model(@0)^-1 is the world->UV mapping the receiver really
// samples with. Qualifying draw: battle state active, VS cb0 ByteWidth==880,
// a shadow SRV bound. Largest element count this frame wins (a small prop's
// receiver cannot displace the arena ground's).
void captureRecvTexWorld(ID3D11DeviceContext* context, UINT count) {
  if (!cutinInjectV2Enabled())
    return;
  const char* st = arlandBattleStateName();
  if (!st)
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
  // §43 routing diagnostic: the SRV scan runs for EVERY qualifying 880 draw
  // (not just the frame's largest) so we can log which shadow-map TEXTURE each
  // ground draw actually samples — the FLATZ experiment proved the injected
  // caster is abundantly present in the readback map (recvMapTex) while the
  // visible ground stays shadowless, i.e. the ground samples a DIFFERENT map.
  // GROUND_MAP groundSrvTex vs INJECT_MAP dsvTex/recvMapTex settles routing.
  bool samplesShadow = false;
  void* groundSrvTex = nullptr;    // identity only, logged then released
  ID3D11ShaderResourceView* srvs[16] = {};
  context->PSGetShaderResources(0, 16, srvs);
  for (ID3D11ShaderResourceView* srv : srvs)
    if (srv && !samplesShadow) {
      std::lock_guard lock(g_shadowTraceMutex);
      if (isShadowSrvLocked(srv)) {
        samplesShadow = true;
        ID3D11Resource* sres = nullptr;
        srv->GetResource(&sres);
        if (sres) {
          groundSrvTex = sres;
          sres->Release();
        }
      }
    }
  for (ID3D11ShaderResourceView* srv : srvs)
    if (srv)
      srv->Release();
  if (!samplesShadow) {
    cb->Release();
    return;
  }
  {
    static std::mutex gmMutex;
    static std::unordered_map<std::string, uint64_t> gmTick;
    const uint64_t tick = GetTickCount64();
    char keyc[64];
    std::snprintf(keyc, sizeof(keyc), "%s/%u", st, count);
    bool doLog = false;
    {
      std::lock_guard<std::mutex> lock(gmMutex);
      auto& t = gmTick[keyc];
      if (tick - t > 500 || t == 0) { t = tick; doLog = true; }
    }
    if (doLog)
      log("GROUND_MAP state=", st, " count=", count,
          " groundSrvTex=", groundSrvTex,
          " recvMapTex=", g_recvMapTexPtr.load(std::memory_order_relaxed));
  }
  const uint64_t frame = g_reconFrame.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_texWorldMutex);
    if (g_texWorldFrame == frame && count <= g_texWorldBestCount) {
      cb->Release();
      return;   // a bigger receiver already won this frame
    }
  }
  // §41b: the scene (incl. this receive draw) is recorded on a DEFERRED
  // context, where a staging-copy readback of the bound cbuffer is impossible
  // (and readVsCbufferBytes correctly refuses). But every context is hooked,
  // so the Map/UpdateSubresource hooks have already snapshotted the LAST WRITE
  // to this exact buffer object into g_cbSnaps — read Model@0 and
  // PSSGLightModelViewProjTex@752 from that CPU-side snapshot instead. This
  // both works on any context AND is guaranteed to be the payload of the
  // buffer bound at THIS draw (correct object, not "last 880 write of the
  // frame"). Immediate-context staging read kept only as a fallback.
  const bool immediate =
      context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
  uint8_t win[816];
  bool haveWin = false;
  const char* winSrc = "snap";
  {
    std::lock_guard lock(g_cbSnapMutex);
    auto it = g_cbSnaps.find(cb);
    if (it != g_cbSnaps.end() && it->second.size == 880) {
      static_assert(kCbSnapBytes >= 816, "snapshot must cover @752 matrix");
      std::memcpy(win, it->second.data, sizeof(win));
      haveWin = true;
    }
  }
  if (!haveWin && immediate) {
    haveWin = readVsCbufferBytes(context, 0, 0, 816, win);
    winSrc = "staging";
  }
  cb->Release();
  if (!haveWin) {
    static std::atomic<uint64_t> missLogs{0};
    if (missLogs.fetch_add(1, std::memory_order_relaxed) % 600 == 0)
      log("CUTIN_V2_TEX miss ctx=", immediate ? "immediate" : "deferred",
          " count=", count, " (no cb snapshot for bound 880)");
    return;
  }
  float M[16], T[16], Mi[16], TW[16];
  std::memcpy(M, win, 64);          // Model @0
  std::memcpy(T, win + 752, 64);    // PSSGLightModelViewProjTex @752
  if (!mtxInvert(M, Mi))
    return;
  mtxMul(T, Mi, TW);                // world -> shadow UV
  // §42: fold the receiver shader's v-flip (samples at 1 - v) into row 1:
  // row1' = e3 - row1. Exact for w=1 points since TW row3 = (0,0,0,1).
  const bool mirrorY = cutinInjectMirrorYEnabled();
  if (mirrorY) {
    TW[4] = -TW[4];
    TW[5] = -TW[5];
    TW[6] = -TW[6];
    TW[7] = 1.0f - TW[7];
  }
  {
    std::lock_guard<std::mutex> lock(g_texWorldMutex);
    g_texWorldFrame = frame;
    g_texWorldBestCount = count;
    std::memcpy(g_texWorld, TW, 64);
  }
  g_texWorldValid.store(true, std::memory_order_relaxed);
  static std::atomic<uint64_t> capLogs{0};
  if (capLogs.fetch_add(1, std::memory_order_relaxed) % 240 == 0)
    log("CUTIN_V2_TEX capture count=", count,
        " ctx=", immediate ? "immediate" : "deferred", " src=", winSrc,
        " mirrorY=", mirrorY ? 1 : 0,
        " Mt=", M[3], ",", M[7], ",", M[11],
        " TWr0=", TW[0], ",", TW[1], ",", TW[2], ",", TW[3],
        " TWr3=", TW[12], ",", TW[13], ",", TW[14], ",", TW[15]);
}

// §32b draw-time gate-hold (the write-path-independent net). Runs on every
// draw during a cinematic battle state when ARLAND_CUTIN_GATE_HOLD is on:
// if the draw's VS cb0 is an 880-byte receiver material AND the shadow SRV is
// bound, record a 16-byte box UpdateSubresource over [832,848) setting the VS
// `diffuse` to (1,1,1,1) immediately before the draw. min(diffuse.w,diffuse.x)
// then clears the 0.75 shadow-fade threshold and the receiver PS's shadow
// block executes. Catches ALL write-path scenarios: unseen write path, stale
// pre-cinematic content re-bound, deferred-context recording. Idempotent, 16
// bytes/draw on at most a handful of receiver draws per frame.
void gateHoldAtDraw(ID3D11DeviceContext* context) {
  if (!cutinGateHoldEnabled() || !cutinGateDrawEnabled())
    return;
  if (!arlandInCinematicBattle())
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
  // Log the last CPU-side write's @832 (when we have one) so the run shows
  // what the gate value WAS at draw time — 0.7 proves the stale/unseen-path
  // theory; a missing snapshot proves the write path bypasses all cb hooks.
  {
    static std::atomic<uint64_t> dl{0};
    const uint64_t dk = GetTickCount64() / 500;
    if (dl.exchange(dk) != dk) {
      float d[4] = {};
      bool haveSnap = false;
      {
        std::lock_guard lock(g_cbSnapMutex);
        auto it = g_cbSnaps.find(cb);
        if (it != g_cbSnaps.end() && it->second.size == 880) {
          std::memcpy(d, it->second.data + 832, 16);
          haveSnap = true;
        }
      }
      const char* st = arlandBattleStateName();  // capture once (race)
      log("GATE_DRAW res=", static_cast<void*>(cb),
          " state=", st ? st : "-", " snap=", haveSnap ? 1 : 0,
          " @832=", d[0], ",", d[1], ",", d[2], ",", d[3]);
    }
  }
  // Force the gate open for this draw: partial 16-byte update of the bound
  // DEFAULT buffer, recorded in-order before the draw on THIS context (legal
  // on deferred contexts; DXVK implements 11.1 partial-cb-update semantics).
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
  {
    static std::atomic<uint64_t> t{0};
    const uint64_t k = GetTickCount64() / 500;
    if (t.exchange(k) != k)
      log("GATE_HOLD draw res=", static_cast<void*>(cb),
          " diffuse@832 -> 1.0 (gate opened at draw)");
  }
  cb->Release();
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

  // §27 decisive probe: for FIELD/GROUND-material draws (VS cb0 = 768 no-shadow
  // or 880 shadow-receiving) during battle, log state + which material + whether
  // a shadow SRV is bound. Tells us definitively whether the cut-in ground is
  // drawn with the shadow family (880) or the no-shadow family (768), and if it
  // samples — the fact that decides whether "draw the ground with the shadow
  // family" is the right lever. Throttled per (state, size).
  {
    const char* st = arlandBattleStateName();
    if (st) {
      ID3D11Buffer* vscb = nullptr;
      context->VSGetConstantBuffers(0, 1, &vscb);
      uint32_t cbSize = 0;
      if (vscb) {
        D3D11_BUFFER_DESC bd = {};
        vscb->GetDesc(&bd);
        cbSize = bd.ByteWidth;
        vscb->Release();
      }
      if (cbSize == 768 || cbSize == 880) {
        static std::mutex m;
        static std::unordered_map<std::string, uint64_t> lastTick;
        const uint64_t tick = GetTickCount64();
        char keyc[48];
        std::snprintf(keyc, sizeof(keyc), "%s/%u", st, cbSize);
        bool doLog = false;
        { std::lock_guard<std::mutex> lock(m);
          auto& t = lastTick[keyc];
          if (tick - t > 500 || t == 0) { t = tick; doLog = true; } }
        if (doLog)
          log("GROUND_DRAW state=", st, " material=", cbSize,
              " (", (cbSize == 880 ? "shadow-recv" : "no-shadow"),
              ") shadowSrvBound=", samplesShadow);
      }
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

// §32c receiver-constants probe: on draws whose PS samples the shadow map,
// dump the PS (once) and log its bound constant buffers' latest CPU writes,
// tagged with the battle state. A phase-level "stop resolving shadows" switch
// must show up as a per-state diff here (or in the PS itself changing).
void traceShadowRecvConstants(ID3D11DeviceContext* context) {
  if (!cutinRecvTraceEnabled() && !cutinMapTraceEnabled())
    return;
  const char* state = arlandBattleStateName();
  if (!state)
    return;
  static mutex recvMutex;
  static const char* lastState = nullptr;
  static int logged = 0;
  {
    std::lock_guard lock(recvMutex);
    if (lastState != state) {
      lastState = state;
      logged = 0;
    }
    if (logged >= 4)
      return;
  }
  ID3D11ShaderResourceView* srvs[16] = {};
  context->PSGetShaderResources(0, 16, srvs);
  bool samplesShadow = false;
  int shadowSlot = -1;
  for (int i = 0; i < 16; ++i)
    if (srvs[i]) {
      std::lock_guard lock(g_shadowTraceMutex);
      if (isShadowSrvLocked(srvs[i])) {
        samplesShadow = true;
        shadowSlot = i;
        break;
      }
    }
  if (samplesShadow && cutinMapTraceEnabled() && shadowSlot >= 0) {
    ID3D11Resource* rres = nullptr;
    srvs[shadowSlot]->GetResource(&rres);
    if (rres) {
      recordMapEvent('R', rres, "shadow_map", context);
      rres->Release();
    }
  }
  for (ID3D11ShaderResourceView* srv : srvs)
    if (srv)
      srv->Release();
  if (!samplesShadow || !cutinRecvTraceEnabled())
    return;
  {
    std::lock_guard lock(recvMutex);
    if (lastState != state || logged >= 4)
      return;
    ++logged;
  }
  ID3D11PixelShader* ps = nullptr;
  context->PSGetShader(&ps, nullptr, nullptr);
  if (ps)
    dumpPsOnce(ps, "recv");
  ID3D11Buffer* cbs[4] = {};
  context->PSGetConstantBuffers(0, 4, cbs);
  for (int i = 0; i < 4; ++i) {
    if (!cbs[i])
      continue;
    CbSnap snap;
    bool have = false;
    {
      std::lock_guard lock(g_cbSnapMutex);
      auto it = g_cbSnaps.find(cbs[i]);
      if (it != g_cbSnaps.end()) {
        snap = it->second;
        have = true;
      }
    }
    if (!have) {
      log("CUTIN_RECV state=", state, " ps=", reinterpret_cast<void*>(ps),
        " slot=", i, " cb=", reinterpret_cast<void*>(cbs[i]), " snap=none");
      continue;
    }
    const float* f = reinterpret_cast<const float*>(snap.data);
    log("CUTIN_RECV state=", state, " ps=", reinterpret_cast<void*>(ps),
      " srv_slot=", shadowSlot, " slot=", i, " size=", snap.size,
      " f0=", f[0], ",", f[1], ",", f[2], ",", f[3],
      " f4=", f[4], ",", f[5], ",", f[6], ",", f[7],
      " f8=", f[8], ",", f[9], ",", f[10], ",", f[11]);
    // Receiver material: PSSGLightModelViewProjTex at +752 is the matrix the
    // shadow resolve projects through — the prime phase-desync suspect.
    if (snap.size == 880 || snap.size == 768) {
      const float* tm = reinterpret_cast<const float*>(snap.data + 752);
      const float* ts = reinterpret_cast<const float*>(snap.data + 816);
      const float* lp = reinterpret_cast<const float*>(snap.data + 832);
      log("CUTIN_RECV_TEXMTX state=", state,
        " cb=", reinterpret_cast<void*>(cbs[i]),
        " r0=", tm[0], ",", tm[1], ",", tm[2], ",", tm[3],
        " r1=", tm[4], ",", tm[5], ",", tm[6], ",", tm[7],
        " r2=", tm[8], ",", tm[9], ",", tm[10], ",", tm[11],
        " r3=", tm[12], ",", tm[13], ",", tm[14], ",", tm[15],
        " tapScale=", ts[0],
        " shadowLPos=", lp[0], ",", lp[1], ",", lp[2]);
    }
  }
  for (ID3D11Buffer* cb : cbs)
    if (cb)
      cb->Release();
  if (ps)
    ps->Release();
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

  // §32b: an 880 receiver material created WITH initial data (transient
  // per-frame cb pattern) bypasses both the Map and UpdateSubresource hooks —
  // trace it, and hold the shadow gate open in the initial payload too.
  D3D11_SUBRESOURCE_DATA gateInit;
  uint8_t gateInitCopy[880];
  if (pDesc && pData && pData->pSysMem &&
      (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
      pDesc->ByteWidth >= 768 && pDesc->ByteWidth <= 1024) {
    gateTraceCbWrite("create", pData->pSysMem, pDesc->ByteWidth, nullptr);
    if (cutinGateHoldEnabled() && pDesc->ByteWidth == 880 &&
        arlandInCinematicBattle()) {
      std::memcpy(gateInitCopy, pData->pSysMem, 880);
      if (gateHoldPatch(gateInitCopy, 880)) {
        gateInit = *pData;
        gateInit.pSysMem = gateInitCopy;
        pData = &gateInit;
        static std::atomic<uint64_t> t{0};
        const uint64_t k = GetTickCount64() / 500;
        if (t.exchange(k) != k)
          log("GATE_HOLD create diffuse@832 -> 1.0 (gate opened)");
      }
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
  if (cbSnapEnabled() && SUCCEEDED(hr) && ppBuffer && *ppBuffer &&
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
  if (cutinReconEnabled() && SUCCEEDED(hr) && ppVertexShader && *ppVertexShader &&
      pShaderBytecode && BytecodeLength && BytecodeLength <= (1u << 20))
    recordVsBytecode(*ppVertexShader, pShaderBytecode, BytecodeLength);
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
  if ((cutinReconEnabled() || cutinRecvTraceEnabled()) && SUCCEEDED(hr) &&
      ppPixelShader && *ppPixelShader &&
      pShaderBytecode && BytecodeLength && BytecodeLength <= (1u << 20))
    recordPsBytecode(*ppPixelShader, pShaderBytecode, BytecodeLength);
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
  if ((cutinMapTraceEnabled() || cutinMapReadbackEnabled()) &&
      SUCCEEDED(hr) && pDesc &&
      pDesc->Width == 1024 && pDesc->Height == 1024 &&
      pDesc->Format == DXGI_FORMAT_R24G8_TYPELESS &&
      pDesc->Usage != D3D11_USAGE_STAGING && ppTexture && *ppTexture) {
    std::lock_guard lock(g_mapTraceMutex);
    mapTraceId(*ppTexture, "shadow_map_created");
    g_shadowMapTexSet.insert(*ppTexture);
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

  if (cutinMapTraceEnabled() && (isTracedShadowMapTex(pDstResource) ||
                                 isTracedShadowMapTex(pSrcResource)))
    recordMapEvent('P', pDstResource, "shadow_map", pContext, pSrcResource);

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
  if ((cutinMapTraceEnabled() || cutinMapReadbackEnabled()) &&
      (isTracedShadowMapTex(pDstResource) ||
       isTracedShadowMapTex(pSrcResource))) {
    recordMapEvent('Q', pDstResource, "shadow_map", pContext, pSrcResource);
    if (cutinMapReadbackEnabled() && isTracedShadowMapTex(pDstResource))
      rememberRecvMap(pDstResource);
  }
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

  const void* effectiveData = pData;
  uint8_t dimHoldCopy[16];
  uint8_t gateHoldCopy[880];
  // §32b blind-spot check: the gate-hold below only sees full-buffer writes
  // (no box, subresource 0). If the engine updates the 880 receiver with a BOX
  // (partial or full-extent), it would silently bypass every cb hook — log it.
  if (cutinGateHoldEnabled() && pData && (pBox || Subresource != 0)) {
    D3D11_BUFFER_DESC boxDesc = {};
    if (isConstantBuffer(pResource, &boxDesc) && boxDesc.ByteWidth >= 768 &&
        boxDesc.ByteWidth <= 1024) {
      static std::atomic<uint64_t> t{0};
      const uint64_t k = GetTickCount64() / 300;
      if (t.exchange(k) != k) {
        const char* boxSt = arlandBattleStateName();  // capture once (race)
        log("GATE_TRACE path=update_box res=", pResource,
            " size=", boxDesc.ByteWidth, " sub=", Subresource,
            " box=", pBox ? int64_t(pBox->left) : -1,
            "..", pBox ? int64_t(pBox->right) : -1,
            " state=", boxSt ? boxSt : "-",
            " cine=", arlandInCinematicBattle());
      }
    }
  }
  if (cbSnapEnabled() && pData && !pBox && Subresource == 0) {
    D3D11_BUFFER_DESC desc = {};
    if (isConstantBuffer(pResource, &desc)) {
      snapCbWrite(pResource, pData, desc.ByteWidth);
      gateTraceCbWrite("update", pData, desc.ByteWidth, pResource);
      lightDiagLog(pData, desc.ByteWidth);   // see the 880 receiver's constants
      captureRecvTex(pData, desc.ByteWidth);
      logSmallCbChange(pData, desc.ByteWidth, pResource);
      // §22 fix: hold the faded light $Params at 1.0 during the cut-in. pData is
      // const (DEFAULT buffer), so patch a copy and pass that to the real call.
      if (cutinDimHoldEnabled() && desc.ByteWidth == 16 &&
          arlandInCinematicBattle()) {
        std::memcpy(dimHoldCopy, pData, 16);
        if (dimHoldPatch(dimHoldCopy, 16)) {
          effectiveData = dimHoldCopy;
          static std::atomic<uint64_t> t{0};
          const uint64_t k = GetTickCount64() / 500;
          if (t.exchange(k) != k)
            log("DIM_HOLD update res=", pResource, " -> 1.0");
        }
      }
      // §32 fix: OPEN the shadow-reception gate on the 880 receiver material by
      // holding its VS `diffuse` (byte 832) at 1.0 during cinematic states.
      if (cutinGateHoldEnabled() && desc.ByteWidth == 880 &&
          arlandInCinematicBattle()) {
        // Log the incoming diffuse so we can confirm it is the faded 0.7.
        static std::atomic<uint64_t> tl{0};
        const uint64_t kl = GetTickCount64() / 500;
        if (tl.exchange(kl) != kl) {
          const float* d = reinterpret_cast<const float*>(
            static_cast<const uint8_t*>(pData) + 832);
          log("GATE_DIFFUSE res=", pResource, " @832=", d[0], ",", d[1], ",",
              d[2], ",", d[3], " gate=", 2.5f - 2.0f * (d[0] < d[3] ? d[0] : d[3]));
        }
        std::memcpy(gateHoldCopy, pData, 880);
        if (gateHoldPatch(gateHoldCopy, 880)) {
          effectiveData = gateHoldCopy;
          static std::atomic<uint64_t> t{0};
          const uint64_t k = GetTickCount64() / 500;
          if (t.exchange(k) != k)
            log("GATE_HOLD update res=", pResource, " diffuse@832 -> 1.0 (gate opened)");
        }
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
  traceShadowDraw(pContext, false, VertexCount, 1);
  traceShadowReceive(pContext);
  captureRecvTexWorld(pContext, VertexCount);
  gateHoldAtDraw(pContext);
  traceShadowRecvConstants(pContext);
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

  cutinRecon(pContext, false, VertexCount);
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
  captureRecvTexWorld(pContext, IndexCount);
  gateHoldAtDraw(pContext);
  traceShadowRecvConstants(pContext);
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
  cutinRecon(pContext, true, IndexCount);
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
  captureRecvTexWorld(pContext, VertexCountPerInstance);
  gateHoldAtDraw(pContext);
  traceShadowRecvConstants(pContext);
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
  captureRecvTexWorld(pContext, IndexCountPerInstance);
  gateHoldAtDraw(pContext);
  traceShadowRecvConstants(pContext);
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
  // §20 experiment: during a cut-in, substitute the receiver's shadow map with a
  // uniform 1x1 map so sample_c returns a constant (no per-pixel shadow). If the
  // ground brightens, the shadow term is the darkener (§19).
  if (cutinShadowOffEnabled() && arlandInCinematicBattle() &&
      ppShaderResourceViews && NumViews > 0 && NumViews <= 16) {
    ID3D11ShaderResourceView* lit = getLitShadowSrv(pContext);  // cached; no lock held
    if (lit) {
      ID3D11ShaderResourceView* subst[16];
      bool anyShadow = false;
      {
        std::lock_guard<mutex> lock(g_shadowTraceMutex);
        for (UINT i = 0; i < NumViews; i++) {
          subst[i] = ppShaderResourceViews[i];
          if (subst[i] && isShadowSrvLocked(subst[i])) {
            subst[i] = lit;
            anyShadow = true;
          }
        }
      }
      if (anyShadow) {
        procs->PSSetShaderResources(pContext, StartSlot, NumViews, subst);
        return;
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
  recordMapEvent('X', pCommandList, "cmdlist", pContext);
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
